/* Bounds and hit tests of display objects (getBounds, _width, hitTest, button picking).
 * Shapes are tested against their tessellated fill and stroke triangles, which is what
 * the renderer draws, so no separate outline data is needed. */
#include "display_internal.h"
#include "drawing.h"

#include <math.h>
#include <string.h>

swf_matrix swf_dobj_world_matrix(const swf_dobj *o)
{
    swf_matrix m = o->matrix;
    for (const swf_dobj *p = o->parent; p; p = p->parent) m = swf_matrix_mul(&p->matrix, &m);
    return m;
}

bool swf_matrix_invert(const swf_matrix *m, swf_matrix *out)
{
    double det = (double)m->a * m->d - (double)m->b * m->c;
    if (det == 0 || !isfinite(det)) return false;
    double ia = m->d / det, ib = -m->b / det, ic = -m->c / det, id = m->a / det;
    out->a = (float)ia;
    out->b = (float)ib;
    out->c = (float)ic;
    out->d = (float)id;
    out->tx = (float)(-(ia * m->tx + ic * m->ty));
    out->ty = (float)(-(ib * m->tx + id * m->ty));
    return true;
}

static void include_rect(const swf_matrix *m, const float r[4], float b[4], bool *any)
{
    const float xs[2] = {r[0], r[2]}, ys[2] = {r[1], r[3]};
    for (int i = 0; i < 4; i++) {
        float x = xs[i & 1], y = ys[i >> 1];
        float tx = m->a * x + m->c * y + m->tx, ty = m->b * x + m->d * y + m->ty;
        if (!*any) {
            b[0] = b[2] = tx;
            b[1] = b[3] = ty;
            *any = true;
            continue;
        }
        if (tx < b[0]) b[0] = tx;
        if (ty < b[1]) b[1] = ty;
        if (tx > b[2]) b[2] = tx;
        if (ty > b[3]) b[3] = ty;
    }
}

static void merge(float b[4], const float c[4], bool *any)
{
    if (!*any) {
        memcpy(b, c, 4 * sizeof *b);
        *any = true;
        return;
    }
    if (c[0] < b[0]) b[0] = c[0];
    if (c[1] < b[1]) b[1] = c[1];
    if (c[2] > b[2]) b[2] = c[2];
    if (c[3] > b[3]) b[3] = c[3];
}

static int32_t shape_of(const swf_movie *m, const swf_dobj *o)
{
    const swf_assets *a = m->assets;
    if (o->kind == SWF_CHAR_SHAPE || o->kind == SWF_CHAR_TEXT) return o->index < a->shape_count ? (int32_t)o->index : -1;
    if (o->kind == SWF_CHAR_MORPH) return (int32_t)swf_assets_morph_shape(a, o->index, o->ratio);
    return -1;
}

static bool button_bounds(const swf_movie *m, const swf_dobj *o, const swf_matrix *mat, float b[4], uint8_t states)
{
    const swf_assets *a = m->assets;
    const swf_button *bt = &a->buttons[o->index];
    bool any = false;
    for (uint16_t i = 0; i < bt->record_count; i++) {
        const swf_button_record *r = &bt->records[i];
        if (!(r->states & states)) continue;
        const swf_char *ch = swf_assets_char(a, r->char_id);
        if (!ch) continue;
        swf_matrix rm = {r->matrix[0], r->matrix[1], r->matrix[2], r->matrix[3], r->matrix[4], r->matrix[5]};
        swf_matrix cm = swf_matrix_mul(mat, &rm);
        swf_dobj tmp = {0};
        tmp.kind = ch->kind;
        tmp.index = ch->index;
        tmp.matrix = SWF_MATRIX_IDENTITY;
        float cb[4];
        if (ch->kind != SWF_CHAR_SPRITE && ch->kind != SWF_CHAR_BUTTON && swf_dobj_bounds(m, &tmp, &cm, cb))
            merge(b, cb, &any);
    }
    return any;
}

bool swf_dobj_bounds(const swf_movie *m, const swf_dobj *o, const swf_matrix *mat, float b[4])
{
    const swf_assets *a = m->assets;
    bool any = false;
    int32_t shape = shape_of(m, o);
    if (shape >= 0 && (uint32_t)shape < a->shape_count) {
        const int32_t *r = a->shapes[shape].bounds;
        const float rf[4] = {(float)r[0], (float)r[1], (float)r[2], (float)r[3]};
        include_rect(mat, rf, b, &any);
    } else if (o->kind == SWF_CHAR_EDIT_TEXT) {
        const int32_t *r = swf_dobj_edit(m, o)->bounds;
        const float rf[4] = {(float)r[0], (float)r[1], (float)r[2], (float)r[3]};
        include_rect(mat, rf, b, &any);
    } else if (o->kind == SWF_CHAR_BUTTON) {
        float bb[4];
        /* The up state is what a button shows at rest. */
        if (button_bounds(m, o, mat, bb, o->button_state ? o->button_state : 1)) merge(b, bb, &any);
    }
    if (o->drawing) {
        float db[4];
        if (swf_drawing_bounds(o->drawing, mat, db)) merge(b, db, &any);
    }
    for (const swf_dobj *c = o->children; c; c = c->next) {
        swf_matrix cm = swf_matrix_mul(mat, &c->matrix);
        float cb[4];
        if (swf_dobj_bounds(m, c, &cm, cb)) merge(b, cb, &any);
    }
    return any;
}

static int16_t rd16s(const uint8_t *p)
{
    return (int16_t)(uint16_t)(p[0] | (p[1] << 8));
}

static bool tri_hit(float px, float py, const float t[6])
{
    float d1 = (px - t[2]) * (t[1] - t[3]) - (t[0] - t[2]) * (py - t[3]);
    float d2 = (px - t[4]) * (t[3] - t[5]) - (t[2] - t[4]) * (py - t[5]);
    float d3 = (px - t[0]) * (t[5] - t[1]) - (t[4] - t[0]) * (py - t[1]);
    bool neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
    return !(neg && pos);
}

/* Point in shape-local twips against every triangle the renderer would draw. */
static bool shape_hit(const swf_assets *a, uint32_t shape, float x, float y)
{
    const swf_shape *s = &a->shapes[shape];
    if (x < s->bounds[0] || x > s->bounds[2] || y < s->bounds[1] || y > s->bounds[3]) return false;
    for (uint32_t k = 0; k < s->draw_count; k++) {
        const swf_draw *d = &a->draws[s->first_draw + k];
        uint32_t stride = d->textured ? SWF_TEX_VERTEX_SIZE : SWF_SOLID_VERTEX_SIZE;
        uint32_t off = d->textured ? 8 : 4;
        float unit = (float)(1u << d->shift);
        const uint8_t *vb = d->vertices;
        for (uint32_t i = 0; i + 2 < d->index_count; i += 3) {
            float t[6];
            for (int j = 0; j < 3; j++) {
                const uint8_t *v = vb + (size_t)d->indices[i + j] * stride + off;
                t[j * 2] = rd16s(v) * unit;
                t[j * 2 + 1] = rd16s(v + 2) * unit;
            }
            if (tri_hit(x, y, t)) return true;
        }
    }
    return false;
}

static bool hit_local(const swf_movie *m, const swf_dobj *o, const swf_matrix *world, float x, float y, bool shape);

static bool button_hit(const swf_movie *m, const swf_dobj *o, const swf_matrix *world, float x, float y, bool shape)
{
    const swf_assets *a = m->assets;
    const swf_button *bt = &a->buttons[o->index];
    for (uint16_t i = 0; i < bt->record_count; i++) {
        const swf_button_record *r = &bt->records[i];
        if (!(r->states & 8)) continue; /* hit-test state */
        const swf_char *ch = swf_assets_char(a, r->char_id);
        if (!ch) continue;
        swf_matrix rm = {r->matrix[0], r->matrix[1], r->matrix[2], r->matrix[3], r->matrix[4], r->matrix[5]};
        swf_matrix cm = swf_matrix_mul(world, &rm);
        swf_dobj tmp = {0};
        tmp.kind = ch->kind;
        tmp.index = ch->index;
        tmp.matrix = SWF_MATRIX_IDENTITY;
        if (ch->kind != SWF_CHAR_BUTTON && hit_local(m, &tmp, &cm, x, y, shape)) return true;
    }
    return false;
}

static bool hit_local(const swf_movie *m, const swf_dobj *o, const swf_matrix *world, float x, float y, bool shape)
{
    const swf_assets *a = m->assets;
    if (o->kind == SWF_CHAR_BUTTON) return button_hit(m, o, world, x, y, shape);
    if (!shape || o->kind == SWF_CHAR_EDIT_TEXT) {
        float b[4];
        if (!swf_dobj_bounds(m, o, world, b)) return false;
        return x >= b[0] && x <= b[2] && y >= b[1] && y <= b[3];
    }
    swf_matrix inv;
    int32_t s = shape_of(m, o);
    if (s >= 0 && (uint32_t)s < a->shape_count) {
        if (!swf_matrix_invert(world, &inv)) return false;
        float lx = inv.a * x + inv.c * y + inv.tx, ly = inv.b * x + inv.d * y + inv.ty;
        return shape_hit(a, (uint32_t)s, lx, ly);
    }
    if (o->drawing && swf_matrix_invert(world, &inv)) {
        float lx = inv.a * x + inv.c * y + inv.tx, ly = inv.b * x + inv.d * y + inv.ty;
        if (swf_drawing_hit(o->drawing, lx, ly)) return true;
    }
    for (const swf_dobj *c = o->children; c; c = c->next) {
        if (c->clip_depth > 0) continue; /* masks themselves are not hit */
        swf_matrix cm = swf_matrix_mul(world, &c->matrix);
        if (hit_local(m, c, &cm, x, y, shape)) return true;
    }
    return false;
}

bool swf_dobj_hit(const swf_movie *m, const swf_dobj *o, float x, float y, bool shape)
{
    swf_matrix w = swf_dobj_world_matrix(o);
    return hit_local(m, o, &w, x, y, shape);
}
