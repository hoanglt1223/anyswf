#include "display_internal.h"

#include "../render/render.h"
#include "drawing.h"

#include <stdlib.h>
#include <string.h>

#define MAX_MASK_NEST 16

plat_result swf_movie_init(swf_movie *m, const swf_assets *a)
{
    memset(m, 0, sizeof *m);
    m->assets = a;
    swf_dobj *root = calloc(1, sizeof *root);
    if (!root) return PLAT_ERR_NO_MEMORY;
    root->kind = SWF_CHAR_SPRITE;
    root->index = 0;
    root->matrix = SWF_MATRIX_IDENTITY;
    root->cxform = SWF_CXFORM_IDENTITY;
    root->visible = true;
    root->playing = true;
    root->name = "";
    root->button_state = 1;
    m->root = root;
    swf_exec_link(m, root);
    return PLAT_OK;
}

void swf_movie_free(swf_movie *m)
{
    m->on_event = NULL;
    m->hold_removed = false;
    m->deferring = false;
    swf_flush_graveyard(m);
    swf_dobj_free(m, m->root);
    m->root = NULL;
}

/* AVM1 frame order (Ruffle `Avm1::run_frame`): every sprite on the execution list,
 * newest first, steps its timeline; clips created during the walk start next tick. */
void swf_movie_advance(swf_movie *m)
{
    m->tick++;
    if (m->root->frame == 0) {
        swf_clip_enter_first_frame(m, m->root);
        return;
    }
    m->deferring = true;
    for (swf_dobj *c = m->exec_head; c;) {
        swf_dobj *next = c->exec_next;
        if (!c->removed && c->born_tick < m->tick && c->frame > 0) {
            if (m->on_event) m->on_event(m, c, SWF_EV_ENTER_FRAME, m->user);
            if (c->playing) swf_clip_next_frame(m, c);
        }
        c = next;
    }
    m->deferring = false;
    if (!m->hold_removed) swf_flush_graveyard(m);
}

int swf_movie_update(swf_movie *m, double dt, int max_frames)
{
    double period = 1.0 / m->assets->frame_rate;
    m->accum += dt;
    int n = 0;
    while (m->accum >= period && n < max_frames) {
        m->accum -= period;
        swf_movie_advance(m);
        n++;
    }
    /* Do not build up a backlog the device can never catch up with. */
    if (m->accum > period * max_frames) m->accum = 0;
    return n;
}

swf_matrix swf_movie_stage_matrix(const swf_assets *a)
{
    float s = a->target_scale / 20.0f;
    float w = (a->stage[2] - a->stage[0]) * s;
    float h = (a->stage[3] - a->stage[1]) * s;
    swf_matrix m = {s, 0, 0, s, 0, 0};
    m.tx = (a->screen_w - w) * 0.5f - a->stage[0] * s;
    m.ty = (a->screen_h - h) * 0.5f - a->stage[1] * s;
    return m;
}

swf_matrix swf_movie_view_matrix(const swf_movie *m)
{
    const swf_assets *a = m->assets;
    if (m->scale_mode == SWF_SCALE_SHOW_ALL) return swf_movie_stage_matrix(a);
    /* Assets are converted for "show all" (target_scale); the other modes rescale that. */
    float sw = (float)(a->stage[2] - a->stage[0]), sh = (float)(a->stage[3] - a->stage[1]);
    if (sw <= 0 || sh <= 0) return swf_movie_stage_matrix(a);
    float sx, sy;
    switch (m->scale_mode) {
    case SWF_SCALE_EXACT_FIT:
        sx = (float)a->screen_w / sw;
        sy = (float)a->screen_h / sh;
        break;
    case SWF_SCALE_NO_BORDER:
        sx = sy = ((float)a->screen_w / sw > (float)a->screen_h / sh) ? (float)a->screen_w / sw : (float)a->screen_h / sh;
        break;
    default: /* no scale */
        sx = sy = 1.0f / 20.0f;
        break;
    }
    swf_matrix r = {sx, 0, 0, sy, 0, 0};
    r.tx = ((float)a->screen_w - sw * sx) * 0.5f - a->stage[0] * sx;
    r.ty = ((float)a->screen_h - sh * sy) * 0.5f - a->stage[1] * sy;
    return r;
}

typedef struct mask_entry {
    const swf_dobj *obj;
    swf_matrix world;
    int32_t clip_depth;
} mask_entry;

static void render_obj(swf_movie *m, const swf_dobj *o, const swf_matrix *wm, const swf_cxform *wcx,
                       uint8_t blend, bool as_mask);

static void render_children(swf_movie *m, const swf_dobj *first, const swf_matrix *wm, const swf_cxform *wcx,
                            uint8_t blend, bool as_mask)
{
    mask_entry masks[MAX_MASK_NEST];
    int n = 0;
    for (const swf_dobj *c = first; c; c = c->next) {
        while (n > 0 && masks[n - 1].clip_depth < c->depth) {
            n--;
            swf_render_mask_pop_begin();
            render_obj(m, masks[n].obj, &masks[n].world, &SWF_CXFORM_IDENTITY, 0, true);
            swf_render_mask_pop_end();
        }
        swf_matrix cm = swf_matrix_mul(wm, &c->matrix);
        if (c->clip_depth > 0 && !as_mask) {
            if (n == MAX_MASK_NEST) {
                swf_warn_once(m, SWF_WARN_DEPTH_LIMIT, "render: masks nested deeper than %d are ignored", MAX_MASK_NEST);
                continue;
            }
            swf_render_mask_begin();
            render_obj(m, c, &cm, &SWF_CXFORM_IDENTITY, 0, true);
            swf_render_mask_end();
            masks[n++] = (mask_entry){c, cm, c->clip_depth};
            continue;
        }
        if (!c->visible && !as_mask) continue;
        swf_cxform cc = swf_cxform_mul(wcx, &c->cxform);
        uint8_t cb = c->blend;
        if (cb != SWF_BLEND_NORMAL && cb != 1 && cb != SWF_BLEND_LAYER && cb != SWF_BLEND_MULTIPLY &&
            cb != SWF_BLEND_SCREEN && cb != SWF_BLEND_ADD) {
            swf_warn_once(m, SWF_WARN_BLEND, "render: blend mode %u is drawn as normal", (unsigned)cb);
            cb = SWF_BLEND_NORMAL;
        }
        if (cb > 1 && !as_mask && swf_render_layer_begin()) {
            render_obj(m, c, &cm, &cc, SWF_BLEND_NORMAL, false);
            swf_render_layer_end(cb);
            continue;
        }
        render_obj(m, c, &cm, &cc, cb > 1 ? cb : blend, as_mask);
    }
    while (n > 0) {
        n--;
        swf_render_mask_pop_begin();
        render_obj(m, masks[n].obj, &masks[n].world, &SWF_CXFORM_IDENTITY, 0, true);
        swf_render_mask_pop_end();
    }
}

static void render_button(swf_movie *m, const swf_dobj *o, const swf_matrix *wm, const swf_cxform *wcx,
                          uint8_t blend, bool as_mask)
{
    const swf_assets *a = m->assets;
    const swf_button *b = &a->buttons[o->index];
    /* Records are drawn in depth order; they are stored in tag order, which Flash
     * authoring tools emit sorted, so draw them as stored. */
    for (uint16_t i = 0; i < b->record_count; i++) {
        const swf_button_record *r = &b->records[i];
        if (!(r->states & o->button_state)) continue;
        const swf_char *ch = swf_assets_char(a, r->char_id);
        if (!ch) continue;
        swf_matrix rm = {r->matrix[0], r->matrix[1], r->matrix[2], r->matrix[3], r->matrix[4], r->matrix[5]};
        swf_matrix cm = swf_matrix_mul(wm, &rm);
        swf_cxform rc;
        for (int k = 0; k < 4; k++) {
            rc.mul[k] = r->cxform[k] / 256.0f;
            rc.add[k] = r->cxform[4 + k];
        }
        swf_cxform cc = swf_cxform_mul(wcx, &rc);
        swf_dobj tmp = {0};
        tmp.kind = ch->kind;
        tmp.index = ch->index;
        tmp.char_id = r->char_id;
        tmp.matrix = SWF_MATRIX_IDENTITY;
        tmp.cxform = SWF_CXFORM_IDENTITY;
        tmp.visible = true;
        tmp.button_state = 1;
        if (ch->kind == SWF_CHAR_SPRITE) {
            swf_warn_once(m, SWF_WARN_BUTTON_SPRITE, "render: sprites inside button states are not drawn yet");
            continue;
        }
        render_obj(m, &tmp, &cm, &cc, r->blend > 1 ? r->blend : blend, as_mask);
    }
}

static void render_obj(swf_movie *m, const swf_dobj *o, const swf_matrix *wm, const swf_cxform *wcx,
                       uint8_t blend, bool as_mask)
{
    const swf_assets *a = m->assets;
    switch (o->kind) {
    case SWF_CHAR_SHAPE:
    case SWF_CHAR_TEXT:
        swf_render_shape(a, o->index, wm, wcx, blend, !as_mask);
        break;
    case SWF_CHAR_MORPH:
        swf_render_shape(a, swf_assets_morph_shape(a, o->index, o->ratio), wm, wcx, blend, !as_mask);
        break;
    case SWF_CHAR_SPRITE:
        if (o->drawing) swf_drawing_render(o->drawing, wm, wcx, blend, !as_mask);
        render_children(m, o->children, wm, wcx, blend, as_mask);
        break;
    case SWF_CHAR_BUTTON:
        render_button(m, o, wm, wcx, blend, as_mask);
        break;
    case SWF_CHAR_EDIT_TEXT:
        swf_render_edit_text(m, o, wm, wcx);
        break;
    default:
        break;
    }
}

void swf_movie_render(swf_movie *m)
{
    swf_matrix stage = swf_movie_view_matrix(m);
    swf_render_begin(m->assets);
    swf_matrix wm = swf_matrix_mul(&stage, &m->root->matrix);
    render_children(m, m->root->children, &wm, &m->root->cxform, 0, false);
    swf_render_end();
}
