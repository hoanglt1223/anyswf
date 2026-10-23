#include "display_internal.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* Frame scripts run after the frame's display commands; a frame rarely has more than a
 * couple, so a small fixed queue suffices and overflow is reported. */
#define MAX_FRAME_ACTIONS 32

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static float rdf(const uint8_t *p)
{
    uint32_t u = rd32(p);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static const swf_sprite *sprite_of(const swf_movie *m, const swf_dobj *clip)
{
    return &m->assets->sprites[clip->index];
}

uint32_t swf_clip_total_frames(const swf_movie *m, const swf_dobj *clip)
{
    return clip->kind == SWF_CHAR_SPRITE ? sprite_of(m, clip)->frame_count : 1;
}

typedef struct place_args {
    uint16_t flags;
    int32_t depth;
    uint16_t char_id;
    swf_matrix matrix;
    swf_cxform cxform;
    uint16_t ratio;
    const char *name;
    uint16_t clip_depth;
    uint32_t ca_first, ca_count;
    uint8_t blend, visible;
} place_args;

/* Commands were validated at load time, so parsing here only follows the layout. */
static const uint8_t *read_place(const swf_assets *a, const uint8_t *p, place_args *pa)
{
    pa->flags = rd16(p);
    pa->depth = rd16(p + 2);
    p += 4;
    if (pa->flags & SWF_PLACE_CHARACTER) {
        pa->char_id = rd16(p);
        p += 2;
    }
    if (pa->flags & SWF_PLACE_MATRIX) {
        pa->matrix = (swf_matrix){rdf(p), rdf(p + 4), rdf(p + 8), rdf(p + 12), rdf(p + 16), rdf(p + 20)};
        p += 24;
    }
    if (pa->flags & SWF_PLACE_CXFORM) {
        for (int i = 0; i < 4; i++) {
            pa->cxform.mul[i] = (int16_t)rd16(p + i * 2) / 256.0f;
            pa->cxform.add[i] = (int16_t)rd16(p + 8 + i * 2);
        }
        p += 16;
    }
    if (pa->flags & SWF_PLACE_RATIO) {
        pa->ratio = rd16(p);
        p += 2;
    }
    if (pa->flags & SWF_PLACE_NAME) {
        pa->name = swf_assets_string(a, rd32(p));
        p += 4;
    }
    if (pa->flags & SWF_PLACE_CLIP_DEPTH) {
        pa->clip_depth = rd16(p);
        p += 2;
    }
    if (pa->flags & SWF_PLACE_CLIP_ACTIONS) {
        pa->ca_first = rd32(p);
        pa->ca_count = rd16(p + 4);
        p += 6;
    }
    if (pa->flags & SWF_PLACE_BLEND) pa->blend = *p++;
    if (pa->flags & SWF_PLACE_CACHE) p++;
    if (pa->flags & SWF_PLACE_VISIBLE) pa->visible = *p++;
    return p;
}

static void apply_fields(swf_dobj *o, const place_args *pa)
{
    if (pa->flags & SWF_PLACE_MATRIX) o->matrix = pa->matrix;
    if (pa->flags & SWF_PLACE_CXFORM) o->cxform = pa->cxform;
    if (pa->flags & SWF_PLACE_RATIO) o->ratio = pa->ratio;
    if (pa->flags & SWF_PLACE_NAME) o->name = pa->name;
    if (pa->flags & SWF_PLACE_CLIP_DEPTH) o->clip_depth = pa->clip_depth;
    if (pa->flags & SWF_PLACE_BLEND) o->blend = pa->blend;
    if (pa->flags & SWF_PLACE_VISIBLE) o->visible = pa->visible != 0;
}

static void do_place(swf_movie *m, swf_dobj *clip, const place_args *pa)
{
    swf_dobj *existing = swf_clip_child_at(clip, pa->depth);
    bool move = (pa->flags & SWF_PLACE_MOVE) != 0;
    bool has_char = (pa->flags & SWF_PLACE_CHARACTER) != 0;
    if (existing && move) {
        if (has_char && existing->char_id != pa->char_id) {
            const swf_char *ch = swf_assets_char(m->assets, pa->char_id);
            if (!ch) return;
            if (ch->kind == SWF_CHAR_SPRITE || existing->kind == SWF_CHAR_SPRITE) {
                swf_dobj *o = swf_dobj_new(m, pa->char_id, pa->depth);
                if (!o) return;
                o->matrix = existing->matrix;
                o->cxform = existing->cxform;
                o->name = existing->name;
                o->clip_depth = existing->clip_depth;
                o->from_timeline = true;
                o->place_frame = clip->frame;
                apply_fields(o, pa);
                swf_clip_insert(clip, o);
                if (o->kind == SWF_CHAR_SPRITE) swf_clip_enter_first_frame(m, o);
                return;
            }
            existing->char_id = pa->char_id;
            existing->kind = ch->kind;
            existing->index = ch->index;
        }
        apply_fields(existing, pa);
        return;
    }
    if (existing || !has_char) return; /* Flash ignores a place on an occupied depth */
    swf_dobj *o = swf_dobj_new(m, pa->char_id, pa->depth);
    if (!o) return;
    o->from_timeline = true;
    o->place_frame = clip->frame;
    apply_fields(o, pa);
    if (pa->flags & SWF_PLACE_CLIP_ACTIONS) {
        o->clip_action_first = pa->ca_first;
        o->clip_action_count = pa->ca_count;
    }
    swf_clip_insert(clip, o);
    if (o->kind == SWF_CHAR_SPRITE) swf_clip_enter_first_frame(m, o);
}

/* Applies one frame (0-based) of a clip's timeline. Scripts and sounds only fire when
 * `live` is set: frames skipped over by a goto change the display list only. */
static void apply_frame(swf_movie *m, swf_dobj *clip, uint32_t f, bool live)
{
    const swf_assets *a = m->assets;
    const swf_sprite *s = sprite_of(m, clip);
    const uint8_t *p = a->cmds + s->frame_offsets[f];
    const uint8_t *end = a->cmds + s->frame_offsets[f + 1];
    uint32_t actions[MAX_FRAME_ACTIONS];
    int n_actions = 0;
    while (p < end) {
        uint8_t op = *p++;
        switch (op) {
        case SWF_CMD_PLACE: {
            place_args pa = {0};
            pa.cxform = SWF_CXFORM_IDENTITY;
            pa.matrix = SWF_MATRIX_IDENTITY;
            p = read_place(a, p, &pa);
            do_place(m, clip, &pa);
            break;
        }
        case SWF_CMD_REMOVE:
            swf_clip_remove_child(m, clip, rd16(p));
            p += 2;
            break;
        case SWF_CMD_ACTION:
            if (live) {
                if (n_actions < MAX_FRAME_ACTIONS)
                    actions[n_actions++] = rd32(p);
                else
                    plat_log("timeline: more than %d scripts in one frame; extra ones skipped", MAX_FRAME_ACTIONS);
            }
            p += 4;
            break;
        case SWF_CMD_START_SOUND:
            if (live && m->on_sound) m->on_sound(m, clip, p, m->user);
            p += 16;
            break;
        case SWF_CMD_STREAM_BLOCK:
            if (live && m->on_sound) m->on_sound(m, clip, p - 1, m->user);
            p += 4;
            break;
        default:
            return; /* unreachable after validation */
        }
    }
    if (n_actions && !m->on_action) {
        swf_warn_once(m, SWF_WARN_ACTIONS, "timeline: frame scripts present but no compiled code is linked");
        return;
    }
    for (int i = 0; i < n_actions; i++) m->on_action(m, clip, actions[i], m->user);
}

/* Runs only the scripts and sounds of a frame (after a rewind rebuilt its display list). */
static void run_frame_scripts(swf_movie *m, swf_dobj *clip, uint32_t f)
{
    const swf_assets *a = m->assets;
    const swf_sprite *s = sprite_of(m, clip);
    const uint8_t *p = a->cmds + s->frame_offsets[f];
    const uint8_t *end = a->cmds + s->frame_offsets[f + 1];
    while (p < end) {
        uint8_t op = *p++;
        if (op == SWF_CMD_PLACE) {
            place_args pa = {0};
            p = read_place(a, p, &pa);
        } else if (op == SWF_CMD_REMOVE) {
            p += 2;
        } else if (op == SWF_CMD_ACTION) {
            if (m->on_action)
                m->on_action(m, clip, rd32(p), m->user);
            else
                swf_warn_once(m, SWF_WARN_ACTIONS, "timeline: frame scripts present but no compiled code is linked");
            p += 4;
        } else if (op == SWF_CMD_START_SOUND) {
            if (m->on_sound) m->on_sound(m, clip, p, m->user);
            p += 16;
        } else {
            if (m->on_sound) m->on_sound(m, clip, p - 1, m->user);
            p += 4;
        }
    }
}

void swf_clip_enter_first_frame(swf_movie *m, swf_dobj *clip)
{
    clip->frame = 1;
    apply_frame(m, clip, 0, true);
}

/* Display state a timeline reaches at some frame, rebuilt from frame 1. */
typedef struct plan_entry {
    int32_t depth;
    uint16_t char_id;
    uint32_t place_frame;
    place_args pa; /* every field set since the instance was placed */
} plan_entry;

typedef struct plan {
    plan_entry *e;
    int n, cap;
} plan;

static plan_entry *plan_find(plan *p, int32_t depth)
{
    for (int i = 0; i < p->n; i++)
        if (p->e[i].depth == depth) return &p->e[i];
    return NULL;
}

static plan_entry *plan_add(plan *p)
{
    if (p->n == p->cap) {
        int cap = p->cap ? p->cap * 2 : 16;
        plan_entry *n = realloc(p->e, (size_t)cap * sizeof *n);
        if (!n) return NULL;
        p->e = n;
        p->cap = cap;
    }
    return &p->e[p->n++];
}

static void merge_args(place_args *dst, const place_args *src)
{
    uint16_t f = src->flags;
    if (f & SWF_PLACE_MATRIX) dst->matrix = src->matrix;
    if (f & SWF_PLACE_CXFORM) dst->cxform = src->cxform;
    if (f & SWF_PLACE_RATIO) dst->ratio = src->ratio;
    if (f & SWF_PLACE_NAME) dst->name = src->name;
    if (f & SWF_PLACE_CLIP_DEPTH) dst->clip_depth = src->clip_depth;
    if (f & SWF_PLACE_CLIP_ACTIONS) {
        dst->ca_first = src->ca_first;
        dst->ca_count = src->ca_count;
    }
    if (f & SWF_PLACE_BLEND) dst->blend = src->blend;
    if (f & SWF_PLACE_VISIBLE) dst->visible = src->visible;
    dst->flags |= f & (uint16_t)~SWF_PLACE_MOVE;
}

static bool build_plan(swf_movie *m, const swf_dobj *clip, uint32_t frame, plan *p)
{
    const swf_assets *a = m->assets;
    const swf_sprite *s = sprite_of(m, clip);
    for (uint32_t f = 0; f < frame; f++) {
        const uint8_t *c = a->cmds + s->frame_offsets[f];
        const uint8_t *end = a->cmds + s->frame_offsets[f + 1];
        while (c < end) {
            uint8_t op = *c++;
            if (op == SWF_CMD_PLACE) {
                place_args pa = {0};
                pa.matrix = SWF_MATRIX_IDENTITY;
                pa.cxform = SWF_CXFORM_IDENTITY;
                c = read_place(a, c, &pa);
                plan_entry *e = plan_find(p, pa.depth);
                bool has_char = (pa.flags & SWF_PLACE_CHARACTER) != 0;
                if (e && (pa.flags & SWF_PLACE_MOVE)) {
                    if (has_char && e->char_id != pa.char_id) {
                        e->char_id = pa.char_id;
                        e->place_frame = f + 1;
                    }
                    merge_args(&e->pa, &pa);
                } else if (!e && has_char) {
                    e = plan_add(p);
                    if (!e) return false;
                    e->depth = pa.depth;
                    e->char_id = pa.char_id;
                    e->place_frame = f + 1;
                    e->pa = pa;
                    e->pa.flags &= (uint16_t)~SWF_PLACE_MOVE;
                }
            } else if (op == SWF_CMD_REMOVE) {
                plan_entry *e = plan_find(p, rd16(c));
                if (e) *e = p->e[--p->n];
                c += 2;
            } else {
                c += op == SWF_CMD_START_SOUND ? 16 : 4;
            }
        }
    }
    return true;
}

/* Rewinds to an earlier frame the way Flash does: instances that exist at the target
 * frame from the same PlaceObject are kept (sprites keep playing), the rest are
 * removed, missing ones are created. */
static void goto_backward(swf_movie *m, swf_dobj *clip, uint32_t frame)
{
    plan p = {0};
    if (!build_plan(m, clip, frame, &p)) {
        plat_log("timeline: out of memory rewinding a clip");
        free(p.e);
        return;
    }
    clip->frame = frame;
    swf_dobj **pp = &clip->children;
    while (*pp) {
        swf_dobj *c = *pp;
        if (!c->from_timeline) {
            pp = &c->next;
            continue;
        }
        plan_entry *e = plan_find(&p, c->depth);
        if (e && e->char_id == c->char_id && e->place_frame == c->place_frame) {
            c->matrix = SWF_MATRIX_IDENTITY;
            c->cxform = SWF_CXFORM_IDENTITY;
            c->ratio = 0;
            apply_fields(c, &e->pa);
            e->char_id = 0; /* consumed */
            pp = &c->next;
        } else {
            *pp = c->next;
            swf_dobj_free(c);
        }
    }
    for (int i = 0; i < p.n; i++) {
        plan_entry *e = &p.e[i];
        if (e->char_id == 0) continue;
        place_args pa = e->pa;
        pa.depth = e->depth;
        pa.char_id = e->char_id;
        pa.flags |= SWF_PLACE_CHARACTER;
        clip->frame = e->place_frame;
        do_place(m, clip, &pa);
    }
    clip->frame = frame;
    free(p.e);
    run_frame_scripts(m, clip, frame - 1);
}

void swf_clip_goto(swf_movie *m, swf_dobj *clip, uint32_t frame)
{
    if (clip->kind != SWF_CHAR_SPRITE) return;
    uint32_t total = sprite_of(m, clip)->frame_count;
    if (frame < 1) frame = 1;
    if (frame > total) frame = total;
    if (frame == clip->frame) return;
    if (frame < clip->frame) {
        goto_backward(m, clip, frame);
        return;
    }
    for (uint32_t f = clip->frame + 1; f <= frame; f++) {
        clip->frame = f;
        apply_frame(m, clip, f - 1, f == frame);
    }
}

void swf_clip_next_frame(swf_movie *m, swf_dobj *clip)
{
    uint32_t total = sprite_of(m, clip)->frame_count;
    if (total <= 1) return;
    if (clip->frame >= total)
        swf_clip_goto(m, clip, 1);
    else
        swf_clip_goto(m, clip, clip->frame + 1);
}

int64_t swf_clip_find_label(const swf_movie *m, const swf_dobj *clip, const char *label)
{
    if (clip->kind != SWF_CHAR_SPRITE) return -1;
    const swf_sprite *s = sprite_of(m, clip);
    for (uint32_t i = 0; i < s->label_count; i++) {
        const char *a = s->labels[i].name, *b = label;
        /* Frame labels are case-insensitive in AVM1. */
        while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) a++, b++;
        if (*a == 0 && *b == 0) return (int64_t)s->labels[i].frame + 1;
    }
    return -1;
}

void swf_clip_play(swf_dobj *clip) { clip->playing = true; }
void swf_clip_stop(swf_dobj *clip) { clip->playing = false; }
