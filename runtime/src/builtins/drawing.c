/* MovieClip drawing API geometry (see display/drawing.h). The path bookkeeping follows
 * Ruffle's core/src/drawing.rs (MIT/Apache-2.0): fills auto-close, lines drawn while a
 * fill is open are kept until the fill ends and then drawn above it. */
#include "../display/drawing.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MAX_MESH_VERTICES 65535u
#define HEAVY_EDGES 2000u        /* warn once when a drawing gets this complex */
#define MAX_INTERSECT_EDGES 4096u
#define HAIRLINE 20.0f           /* zero-width lines draw one pixel wide */

typedef struct dpath {
    bool fill;
    swf_fill_style style;
    float width;
    uint32_t rgba;
    float *pt;        /* x, y pairs */
    uint8_t *mv;      /* 1 where a subpath starts */
    uint32_t n, cap;
} dpath;

typedef struct dpath_list {
    dpath *p;
    uint32_t n, cap;
} dpath_list;

struct swf_drawing {
    dpath_list paths;    /* finished, in drawing order */
    dpath fill;
    bool has_fill;
    dpath_list pending;  /* lines finished while the fill was open */
    dpath line;
    bool has_line;
    float cx, cy;        /* pen */
    float fsx, fsy;      /* start of the current fill subpath */
    float bounds[4];
    bool has_bounds;
    bool dirty;
    bool warned;
    swf_mesh_vertex *v;
    uint32_t nv, cap_v;
    uint16_t *ix;        /* every triangle, in drawing order */
    uint32_t ni, cap_i;
    uint16_t *fx;        /* fill triangles only (the drawing used as a mask) */
    uint32_t nf, cap_f;
    bool overflow;
};

/* --- path storage -------------------------------------------------------------------- */

static void path_free(dpath *p)
{
    free(p->pt);
    free(p->mv);
    memset(p, 0, sizeof *p);
}

static bool path_push(dpath *p, float x, float y, bool move)
{
    if (p->n == p->cap) {
        uint32_t cap = p->cap ? p->cap * 2 : 16;
        float *pt = realloc(p->pt, (size_t)cap * 2 * sizeof *pt);
        if (!pt) return false;
        p->pt = pt;
        uint8_t *mv = realloc(p->mv, cap);
        if (!mv) return false;
        p->mv = mv;
        p->cap = cap;
    }
    p->pt[p->n * 2] = x;
    p->pt[p->n * 2 + 1] = y;
    p->mv[p->n] = move ? 1 : 0;
    p->n++;
    return true;
}

static bool path_copy(dpath *dst, const dpath *src)
{
    *dst = *src;
    dst->pt = NULL;
    dst->mv = NULL;
    dst->cap = 0;
    dst->n = 0;
    for (uint32_t i = 0; i < src->n; i++)
        if (!path_push(dst, src->pt[i * 2], src->pt[i * 2 + 1], src->mv[i])) {
            path_free(dst);
            return false;
        }
    return true;
}

static bool list_push(dpath_list *l, const dpath *p)
{
    if (l->n == l->cap) {
        uint32_t cap = l->cap ? l->cap * 2 : 8;
        dpath *n = realloc(l->p, (size_t)cap * sizeof *n);
        if (!n) return false;
        l->p = n;
        l->cap = cap;
    }
    l->p[l->n++] = *p;
    return true;
}

static void list_free(dpath_list *l)
{
    for (uint32_t i = 0; i < l->n; i++) path_free(&l->p[i]);
    free(l->p);
    memset(l, 0, sizeof *l);
}

/* --- public state changes -------------------------------------------------------------- */

swf_drawing *swf_drawing_new(void)
{
    return calloc(1, sizeof(swf_drawing));
}

void swf_drawing_free(swf_drawing *d)
{
    if (!d) return;
    swf_drawing_clear(d);
    free(d->v);
    free(d->ix);
    free(d->fx);
    free(d);
}

void swf_drawing_clear(swf_drawing *d)
{
    list_free(&d->paths);
    list_free(&d->pending);
    path_free(&d->fill);
    path_free(&d->line);
    d->has_fill = d->has_line = false;
    d->cx = d->cy = d->fsx = d->fsy = 0;
    d->has_bounds = false;
    d->dirty = true;
}

static void grow_bounds(swf_drawing *d, float x, float y, float r)
{
    if (!d->has_bounds) {
        d->bounds[0] = x - r;
        d->bounds[1] = y - r;
        d->bounds[2] = x + r;
        d->bounds[3] = y + r;
        d->has_bounds = true;
        return;
    }
    if (x - r < d->bounds[0]) d->bounds[0] = x - r;
    if (y - r < d->bounds[1]) d->bounds[1] = y - r;
    if (x + r > d->bounds[2]) d->bounds[2] = x + r;
    if (y + r > d->bounds[3]) d->bounds[3] = y + r;
}

static bool close_path(swf_drawing *d)
{
    if (!d->has_fill || (d->cx == d->fsx && d->cy == d->fsy)) return true;
    bool ok = path_push(&d->fill, d->fsx, d->fsy, false);
    if (d->has_line) ok = path_push(&d->line, d->fsx, d->fsy, false) && ok;
    d->dirty = true;
    return ok;
}

bool swf_drawing_begin_fill(swf_drawing *d, const swf_fill_style *style)
{
    bool ok = close_path(d);
    if (d->has_fill) {
        ok = list_push(&d->paths, &d->fill) && ok;
        memset(&d->fill, 0, sizeof d->fill);
        d->has_fill = false;
    }
    for (uint32_t i = 0; i < d->pending.n; i++) ok = list_push(&d->paths, &d->pending.p[i]) && ok;
    d->pending.n = 0;
    if (d->has_line) {
        dpath next = {0};
        next.width = d->line.width;
        next.rgba = d->line.rgba;
        ok = list_push(&d->paths, &d->line) && ok;
        ok = path_push(&next, d->cx, d->cy, true) && ok;
        d->line = next;
    }
    if (style) {
        memset(&d->fill, 0, sizeof d->fill);
        d->fill.fill = true;
        d->fill.style = *style;
        ok = path_push(&d->fill, d->cx, d->cy, true) && ok;
        d->has_fill = true;
    }
    d->fsx = d->cx;
    d->fsy = d->cy;
    d->dirty = true;
    return ok;
}

bool swf_drawing_line_style(swf_drawing *d, bool on, float width, uint32_t rgba)
{
    bool ok = true;
    if (d->has_line) {
        ok = list_push(d->has_fill ? &d->pending : &d->paths, &d->line);
        if (!ok) path_free(&d->line);
        memset(&d->line, 0, sizeof d->line);
        d->has_line = false;
    }
    if (on) {
        d->line.width = width;
        d->line.rgba = rgba;
        ok = path_push(&d->line, d->cx, d->cy, true) && ok;
        d->has_line = true;
    }
    d->dirty = true;
    return ok;
}

bool swf_drawing_move_to(swf_drawing *d, float x, float y)
{
    bool ok = close_path(d);
    d->fsx = x;
    d->fsy = y;
    if (d->has_fill) ok = path_push(&d->fill, x, y, true) && ok;
    if (d->has_line) ok = path_push(&d->line, x, y, true) && ok;
    d->cx = x;
    d->cy = y;
    d->dirty = true;
    return ok;
}

static bool add_point(swf_drawing *d, float x, float y)
{
    bool ok = true;
    if (d->has_fill) ok = path_push(&d->fill, x, y, false);
    if (d->has_line) ok = path_push(&d->line, x, y, false) && ok;
    return ok;
}

bool swf_drawing_line_to(swf_drawing *d, float x, float y)
{
    float r = d->has_line ? (d->line.width > 0 ? d->line.width : HAIRLINE) * 0.5f : 0.0f;
    if (d->cx == d->fsx && d->cy == d->fsy) grow_bounds(d, d->cx, d->cy, r);
    grow_bounds(d, x, y, r);
    bool ok = add_point(d, x, y);
    d->cx = x;
    d->cy = y;
    d->dirty = true;
    return ok;
}

bool swf_drawing_curve_to(swf_drawing *d, float cx, float cy, float x, float y)
{
    float r = d->has_line ? (d->line.width > 0 ? d->line.width : HAIRLINE) * 0.5f : 0.0f;
    float x0 = d->cx, y0 = d->cy;
    if (x0 == d->fsx && y0 == d->fsy) grow_bounds(d, x0, y0, r);
    /* Subdivide by the control polygon length: about one segment per 5 pixels. */
    float len = hypotf(cx - x0, cy - y0) + hypotf(x - cx, y - cy);
    int segs = (int)(len / 100.0f);
    if (segs < 4) segs = 4;
    if (segs > 32) segs = 32;
    bool ok = true;
    for (int i = 1; i <= segs; i++) {
        float t = (float)i / segs, u = 1.0f - t;
        float px = u * u * x0 + 2 * u * t * cx + t * t * x;
        float py = u * u * y0 + 2 * u * t * cy + t * t * y;
        grow_bounds(d, px, py, r);
        ok = add_point(d, px, py) && ok;
    }
    d->cx = x;
    d->cy = y;
    d->dirty = true;
    return ok;
}

swf_drawing *swf_drawing_clone(const swf_drawing *s)
{
    if (!s) return NULL;
    swf_drawing *d = swf_drawing_new();
    if (!d) return NULL;
    bool ok = true;
    for (uint32_t i = 0; i < s->paths.n && ok; i++) {
        dpath p;
        ok = path_copy(&p, &s->paths.p[i]) && list_push(&d->paths, &p);
    }
    for (uint32_t i = 0; i < s->pending.n && ok; i++) {
        dpath p;
        ok = path_copy(&p, &s->pending.p[i]) && list_push(&d->pending, &p);
    }
    if (ok && s->has_fill) ok = path_copy(&d->fill, &s->fill);
    if (ok && s->has_line) ok = path_copy(&d->line, &s->line);
    if (!ok) {
        swf_drawing_free(d);
        return NULL;
    }
    d->has_fill = s->has_fill;
    d->has_line = s->has_line;
    d->cx = s->cx;
    d->cy = s->cy;
    d->fsx = s->fsx;
    d->fsy = s->fsy;
    memcpy(d->bounds, s->bounds, sizeof d->bounds);
    d->has_bounds = s->has_bounds;
    d->dirty = true;
    return d;
}

/* --- tessellation ------------------------------------------------------------------- */

typedef struct edge {
    float x0, y0, x1, y1; /* y0 < y1 */
    int dir;
} edge;

static uint32_t gradient_color(const swf_fill_style *s, float x, float y)
{
    if (!s->gradient || s->stops == 0) return s->rgba;
    const swf_matrix *m = &s->inverse;
    float gx = m->a * x + m->c * y + m->tx, gy = m->b * x + m->d * y + m->ty;
    float t = s->radial ? sqrtf(gx * gx + gy * gy) / 16384.0f : (gx + 16384.0f) / 32768.0f;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    float r = t * 255.0f;
    if (r <= s->ratio[0]) return s->stop_rgba[0];
    for (uint8_t i = 1; i < s->stops; i++) {
        if (r <= s->ratio[i]) {
            float span = (float)(s->ratio[i] - s->ratio[i - 1]);
            float f = span > 0 ? (r - s->ratio[i - 1]) / span : 1.0f;
            uint32_t a = s->stop_rgba[i - 1], b = s->stop_rgba[i], out = 0;
            for (int k = 0; k < 4; k++) {
                float ca = (float)((a >> (8 * k)) & 0xFF), cb = (float)((b >> (8 * k)) & 0xFF);
                out |= (uint32_t)(ca + (cb - ca) * f + 0.5f) << (8 * k);
            }
            return out;
        }
    }
    return s->stop_rgba[s->stops - 1];
}

static float gradient_step(const swf_fill_style *s)
{
    if (!s->gradient) return 0;
    const swf_matrix *m = &s->inverse;
    float norm = fmaxf(fmaxf(fabsf(m->a), fabsf(m->b)), fmaxf(fabsf(m->c), fabsf(m->d)));
    if (!(norm > 0)) return 0;
    float step = 2048.0f / norm;
    return step < 40.0f ? 40.0f : step;
}

static bool mesh_room(swf_drawing *d, uint32_t nv, uint32_t ni, bool fill)
{
    if (d->nv + nv > MAX_MESH_VERTICES) {
        d->overflow = true;
        return false;
    }
    if (d->nv + nv > d->cap_v) {
        uint32_t cap = d->cap_v ? d->cap_v : 256;
        while (cap < d->nv + nv) cap *= 2;
        swf_mesh_vertex *v = realloc(d->v, (size_t)cap * sizeof *v);
        if (!v) return false;
        d->v = v;
        d->cap_v = cap;
    }
    if (d->ni + ni > d->cap_i) {
        uint32_t cap = d->cap_i ? d->cap_i : 512;
        while (cap < d->ni + ni) cap *= 2;
        uint16_t *ix = realloc(d->ix, (size_t)cap * sizeof *ix);
        if (!ix) return false;
        d->ix = ix;
        d->cap_i = cap;
    }
    if (fill && d->nf + ni > d->cap_f) {
        uint32_t cap = d->cap_f ? d->cap_f : 512;
        while (cap < d->nf + ni) cap *= 2;
        uint16_t *fx = realloc(d->fx, (size_t)cap * sizeof *fx);
        if (!fx) return false;
        d->fx = fx;
        d->cap_f = cap;
    }
    return true;
}

static void emit_quad(swf_drawing *d, const float xy[8], const swf_fill_style *fs, uint32_t rgba, bool fill)
{
    if (!mesh_room(d, 4, 6, fill)) return;
    uint16_t b = (uint16_t)d->nv;
    for (int i = 0; i < 4; i++) {
        float x = xy[i * 2], y = xy[i * 2 + 1];
        d->v[d->nv++] = (swf_mesh_vertex){.rgba = fs ? gradient_color(fs, x, y) : rgba, .x = x, .y = y};
    }
    const uint16_t q[6] = {b, (uint16_t)(b + 1), (uint16_t)(b + 2), b, (uint16_t)(b + 2), (uint16_t)(b + 3)};
    memcpy(d->ix + d->ni, q, sizeof q);
    d->ni += 6;
    if (fill) {
        memcpy(d->fx + d->nf, q, sizeof q);
        d->nf += 6;
    }
}

static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

typedef struct span {
    float xa, xb, mid;
    int dir;
} span;

static int cmp_span(const void *a, const void *b)
{
    return cmp_float(&((const span *)a)->mid, &((const span *)b)->mid);
}

static float edge_x(const edge *e, float y)
{
    float t = (y - e->y0) / (e->y1 - e->y0);
    return e->x0 + (e->x1 - e->x0) * t;
}

/* Even-odd fill by horizontal bands: band edges are every vertex y and every edge
 * crossing, so inside a band no two edges cross and consecutive edges bound trapezoids. */
static void tessellate_fill(swf_drawing *d, const dpath *p)
{
    uint32_t ne = 0;
    edge *edges = malloc((size_t)(p->n + 1) * sizeof *edges);
    if (!edges) return;
    for (uint32_t s = 0; s < p->n;) {
        uint32_t e = s + 1;
        while (e < p->n && !p->mv[e]) e++;
        for (uint32_t i = s; i < e; i++) {
            uint32_t j = i + 1 < e ? i + 1 : s;
            float x0 = p->pt[i * 2], y0 = p->pt[i * 2 + 1], x1 = p->pt[j * 2], y1 = p->pt[j * 2 + 1];
            if (y0 == y1) continue;
            edges[ne++] = y0 < y1 ? (edge){x0, y0, x1, y1, 1} : (edge){x1, y1, x0, y0, -1};
        }
        s = e;
    }
    if (ne < 2) {
        free(edges);
        return;
    }
    if (ne >= HEAVY_EDGES && !d->warned) {
        d->warned = true;
        plat_log("drawing: %u edges tessellated on the device; heavy drawing API use is slow on PSP", (unsigned)ne);
    }
    size_t ycap = (size_t)ne * 2 + 16, ny = 0;
    float *ys = malloc(ycap * sizeof *ys);
    span *spans = malloc((size_t)ne * sizeof *spans);
    if (!ys || !spans) {
        free(ys);
        free(spans);
        free(edges);
        return;
    }
    float ymin = edges[0].y0, ymax = edges[0].y1;
    for (uint32_t i = 0; i < ne; i++) {
        ys[ny++] = edges[i].y0;
        ys[ny++] = edges[i].y1;
        if (edges[i].y0 < ymin) ymin = edges[i].y0;
        if (edges[i].y1 > ymax) ymax = edges[i].y1;
    }
    /* Gradients are shaded per vertex, so gradient fills get a grid fine enough for the
     * colour to change smoothly: 1/16 of the gradient square per cell. */
    float step = gradient_step(&p->style);
    uint32_t extra = step > 0 ? (uint32_t)((ymax - ymin) / step) : 0;
    if (extra > 64) extra = 64, step = (ymax - ymin) / 64;
    if (extra) {
        float *n = realloc(ys, (ny + extra + 1) * sizeof *n);
        if (n) {
            ys = n;
            ycap = ny + extra + 1;
            for (uint32_t k = 1; k <= extra; k++) ys[ny++] = ymin + step * (float)k;
        }
    }
    if (ne <= MAX_INTERSECT_EDGES) {
        for (uint32_t i = 0; i < ne; i++) {
            for (uint32_t j = i + 1; j < ne; j++) {
                const edge *a = &edges[i], *b = &edges[j];
                float lo = a->y0 > b->y0 ? a->y0 : b->y0, hi = a->y1 < b->y1 ? a->y1 : b->y1;
                if (hi <= lo) continue;
                float da = edge_x(a, lo) - edge_x(b, lo), db = edge_x(a, hi) - edge_x(b, hi);
                if ((da < 0 && db > 0) || (da > 0 && db < 0)) {
                    float y = lo + (hi - lo) * da / (da - db);
                    if (ny == ycap) {
                        ycap *= 2;
                        float *n = realloc(ys, ycap * sizeof *n);
                        if (!n) break;
                        ys = n;
                    }
                    ys[ny++] = y;
                }
            }
        }
    }
    qsort(ys, ny, sizeof *ys, cmp_float);
    const swf_fill_style *fs = &p->style;
    for (size_t k = 0; k + 1 < ny; k++) {
        float ya = ys[k], yb = ys[k + 1];
        if (yb - ya < 0.01f) continue;
        uint32_t ns = 0;
        for (uint32_t i = 0; i < ne; i++) {
            const edge *e = &edges[i];
            if (e->y0 > ya || e->y1 < yb) continue;
            float xa = edge_x(e, ya), xb = edge_x(e, yb);
            spans[ns++] = (span){xa, xb, (xa + xb) * 0.5f, e->dir};
        }
        qsort(spans, ns, sizeof *spans, cmp_span);
        for (uint32_t i = 0; i + 1 < ns; i += 2) {
            const span *l = &spans[i], *r = &spans[i + 1];
            float w = fmaxf(r->xa - l->xa, r->xb - l->xb);
            uint32_t cols = step > 0 && w > step ? (uint32_t)ceilf(w / step) : 1;
            if (cols > 64) cols = 64;
            for (uint32_t c = 0; c < cols; c++) {
                float t0 = (float)c / cols, t1 = (float)(c + 1) / cols;
                const float q[8] = {l->xa + (r->xa - l->xa) * t0, ya, l->xa + (r->xa - l->xa) * t1, ya,
                                    l->xb + (r->xb - l->xb) * t1, yb, l->xb + (r->xb - l->xb) * t0, yb};
                emit_quad(d, q, fs->gradient ? fs : NULL, fs->rgba, true);
            }
        }
    }
    free(spans);
    free(ys);
    free(edges);
}

static void emit_disc(swf_drawing *d, float x, float y, float r, uint32_t rgba)
{
    int segs = (int)(r / 10.0f);
    if (segs < 8) segs = 8;
    if (segs > 16) segs = 16;
    if (!mesh_room(d, (uint32_t)segs + 1, (uint32_t)segs * 3, false)) return;
    uint16_t c = (uint16_t)d->nv;
    d->v[d->nv++] = (swf_mesh_vertex){.rgba = rgba, .x = x, .y = y};
    for (int i = 0; i < segs; i++) {
        float a = 6.2831853f * i / segs;
        d->v[d->nv++] = (swf_mesh_vertex){.rgba = rgba, .x = x + r * cosf(a), .y = y + r * sinf(a)};
    }
    for (int i = 0; i < segs; i++) {
        d->ix[d->ni++] = c;
        d->ix[d->ni++] = (uint16_t)(c + 1 + i);
        d->ix[d->ni++] = (uint16_t)(c + 1 + (i + 1) % segs);
    }
}

static void tessellate_line(swf_drawing *d, const dpath *p, bool close_to_fill)
{
    float half = (p->width > 0 ? p->width : HAIRLINE) * 0.5f;
    bool joins = half >= 20.0f;
    for (uint32_t i = 0; i < p->n; i++) {
        float x1 = p->pt[i * 2], y1 = p->pt[i * 2 + 1];
        bool start = p->mv[i] != 0;
        if (!start && i > 0) {
            float x0 = p->pt[i * 2 - 2], y0 = p->pt[i * 2 - 1];
            float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy);
            if (len > 0) {
                float nx = -dy / len * half, ny = dx / len * half;
                const float q[8] = {x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny};
                emit_quad(d, q, NULL, p->rgba, false);
            }
        }
        /* Round joins and caps; a lone move point draws nothing. */
        bool next_seg = i + 1 < p->n && !p->mv[i + 1];
        if (joins && (!start || next_seg)) emit_disc(d, x1, y1, half, p->rgba);
    }
    if (close_to_fill && p->n) {
        float x0 = p->pt[(p->n - 1) * 2], y0 = p->pt[(p->n - 1) * 2 + 1];
        float x1 = d->fsx, y1 = d->fsy;
        float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy);
        if (len > 0) {
            float nx = -dy / len * half, ny = dx / len * half;
            const float q[8] = {x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny};
            emit_quad(d, q, NULL, p->rgba, false);
        }
    }
}

static void rebuild(swf_drawing *d)
{
    d->nv = d->ni = d->nf = 0;
    d->overflow = false;
    for (uint32_t i = 0; i < d->paths.n; i++) {
        const dpath *p = &d->paths.p[i];
        if (p->fill) tessellate_fill(d, p);
        else tessellate_line(d, p, false);
    }
    if (d->has_fill) tessellate_fill(d, &d->fill);
    for (uint32_t i = 0; i < d->pending.n; i++) tessellate_line(d, &d->pending.p[i], d->has_fill);
    if (d->has_line) tessellate_line(d, &d->line, d->has_fill);
    if (d->overflow && !d->warned) {
        d->warned = true;
        plat_log("drawing: more than %u vertices; the rest of the drawing is not shown", MAX_MESH_VERTICES);
    }
    d->dirty = false;
}

void swf_drawing_render(swf_drawing *d, const swf_matrix *m, const swf_cxform *cx, uint8_t blend, bool strokes)
{
    if (d->dirty) rebuild(d);
    if (strokes) swf_render_mesh(m, d->v, d->nv, d->ix, d->ni, cx, blend);
    else swf_render_mesh(m, d->v, d->nv, d->fx, d->nf, cx, blend);
}

void swf_drawing_mesh(swf_drawing *d, const swf_mesh_vertex **v, const uint16_t **idx, uint32_t *index_count)
{
    if (d->dirty) rebuild(d);
    *v = d->v;
    *idx = d->ix;
    *index_count = d->ni;
}

uint32_t swf_drawing_triangles(const swf_drawing *d)
{
    return d->ni / 3;
}

bool swf_drawing_bounds(const swf_drawing *d, const swf_matrix *m, float out[4])
{
    if (!d->has_bounds) return false;
    const float xs[2] = {d->bounds[0], d->bounds[2]}, ys[2] = {d->bounds[1], d->bounds[3]};
    for (int i = 0; i < 4; i++) {
        float x = xs[i & 1], y = ys[i >> 1];
        float tx = m->a * x + m->c * y + m->tx, ty = m->b * x + m->d * y + m->ty;
        if (i == 0 || tx < out[0]) out[0] = tx;
        if (i == 0 || ty < out[1]) out[1] = ty;
        if (i == 0 || tx > out[2]) out[2] = tx;
        if (i == 0 || ty > out[3]) out[3] = ty;
    }
    return true;
}

/* --- hit testing ----------------------------------------------------------------------- */

static bool fill_hit(const dpath *p, float x, float y)
{
    bool inside = false;
    for (uint32_t s = 0; s < p->n;) {
        uint32_t e = s + 1;
        while (e < p->n && !p->mv[e]) e++;
        for (uint32_t i = s; i < e; i++) {
            uint32_t j = i + 1 < e ? i + 1 : s;
            float x0 = p->pt[i * 2], y0 = p->pt[i * 2 + 1], x1 = p->pt[j * 2], y1 = p->pt[j * 2 + 1];
            if ((y0 > y) != (y1 > y) && x < x0 + (x1 - x0) * (y - y0) / (y1 - y0)) inside = !inside;
        }
        s = e;
    }
    return inside;
}

static bool line_hit(const dpath *p, float x, float y)
{
    float half = (p->width > 0 ? p->width : HAIRLINE) * 0.5f;
    for (uint32_t i = 1; i < p->n; i++) {
        if (p->mv[i]) continue;
        float x0 = p->pt[i * 2 - 2], y0 = p->pt[i * 2 - 1], x1 = p->pt[i * 2], y1 = p->pt[i * 2 + 1];
        float dx = x1 - x0, dy = y1 - y0, l2 = dx * dx + dy * dy;
        float t = l2 > 0 ? ((x - x0) * dx + (y - y0) * dy) / l2 : 0;
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        float px = x0 + dx * t - x, py = y0 + dy * t - y;
        if (px * px + py * py <= half * half) return true;
    }
    return false;
}

bool swf_drawing_hit(const swf_drawing *d, float x, float y)
{
    if (!d->has_bounds || x < d->bounds[0] || x > d->bounds[2] || y < d->bounds[1] || y > d->bounds[3]) return false;
    for (uint32_t i = 0; i < d->paths.n; i++) {
        const dpath *p = &d->paths.p[i];
        if (p->fill ? fill_hit(p, x, y) : line_hit(p, x, y)) return true;
    }
    if (d->has_fill && fill_hit(&d->fill, x, y)) return true;
    for (uint32_t i = 0; i < d->pending.n; i++)
        if (line_hit(&d->pending.p[i], x, y)) return true;
    return d->has_line && line_hit(&d->line, x, y);
}
