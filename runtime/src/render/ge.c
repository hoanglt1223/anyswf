/* PSP GE backend (sceGu). Asset vertex and texture formats are the GE's native ones:
 * solid vertices are COLOR_8888|VERTEX_16BIT, textured ones TEXTURE_32BITF|VERTEX_16BIT,
 * indices 16-bit, textures in GU_PSM_* order with optional swizzling.
 *
 * In transform mode the GE reads 16-bit positions as value / 32768, so the model matrix
 * carries a 32768 * (1 << shift) twip scale. The 8888 framebuffer's alpha byte is the
 * stencil buffer: the stencil test stays enabled with KEEP so normal draws never write
 * it, and masks use INCR/DECR on it like the GL backend. */
#include "render.h"
#include "swfrt/profiler.h"
#include "texture_cache.h"

#include <pspge.h>
#include <pspgu.h>
#include <pspkernel.h>

#include <string.h>

/* Vertices built per frame (colour-transformed copies, rects, glyph quads). The GE reads
 * them while the list runs, so they live until plat_frame_end syncs; the ring restarts
 * at the next swf_render_begin. */
#define RING_SIZE (1024 * 1024)
static uint8_t __attribute__((aligned(64))) g_ring[RING_SIZE];
static size_t g_ring_used;
static bool g_ring_warned;
static int g_stencil;

typedef struct {
    float u, v;
    float x, y, z;
} tex_vertex_f;

typedef struct {
    uint32_t color;
    float x, y, z;
} color_vertex_f;

static void *ring_alloc(size_t size)
{
    size = (size + 15) & ~(size_t)15;
    if (size > RING_SIZE - g_ring_used) {
        if (!g_ring_warned) {
            plat_log("render: per-frame vertex buffer full (%u bytes); draws dropped", (unsigned)RING_SIZE);
            g_ring_warned = true;
        }
        return NULL;
    }
    void *p = g_ring + g_ring_used;
    g_ring_used += size;
    return p;
}

static void ring_flush(void *p, size_t size)
{
    uintptr_t start = (uintptr_t)p & ~(uintptr_t)63, end = ((uintptr_t)p + size + 63) & ~(uintptr_t)63;
    sceKernelDcacheWritebackRange((void *)start, (unsigned int)(end - start));
}

static uint32_t abgr(const uint8_t c[4])
{
    return (uint32_t)c[0] | ((uint32_t)c[1] << 8) | ((uint32_t)c[2] << 16) | ((uint32_t)c[3] << 24);
}

static uint8_t unit_to_byte(float f)
{
    if (f <= 0) return 0;
    if (f >= 1) return 255;
    return (uint8_t)(f * 255.0f + 0.5f);
}

/* VRAM left after the platform's frame and depth buffers holds resident textures. */
static swf_texcache *g_cache;
static uint8_t *g_vram_arena;
static size_t g_vram_size;

static void vram_upload(void *dst, const void *src, size_t size)
{
    /* Through the uncached VRAM mirror, so the GE sees the bytes without a cache flush. */
    memcpy((void *)((uintptr_t)dst | 0x40000000u), src, size);
}

plat_result swf_render_init(const swf_assets *a)
{
    if (!g_vram_arena) {
        /* guGetStaticVramTexture returns the absolute address of the next free byte. */
        uint8_t *next = (uint8_t *)guGetStaticVramTexture(0, 0, GU_PSM_8888);
        uintptr_t end = (uintptr_t)sceGeEdramGetAddr() + sceGeEdramGetSize();
        next = (uint8_t *)(((uintptr_t)next + 15) & ~(uintptr_t)15);
        if ((uintptr_t)next < end) {
            g_vram_arena = next;
            g_vram_size = end - (uintptr_t)next;
        }
    }
    swf_texcache_free(g_cache);
    g_cache = g_vram_arena ? swf_texcache_new(g_vram_arena, g_vram_size, a->texture_count, vram_upload) : NULL;
    if (!g_cache) plat_log("render: no VRAM texture cache; textures are read from RAM");
    /* Texture, CLUT, vertex and index data were read into cached RAM; the GE reads memory
     * directly. */
    sceKernelDcacheWritebackAll();
    g_ring_warned = false;
    return PLAT_OK;
}

void swf_render_shutdown(void)
{
    swf_texcache_free(g_cache);
    g_cache = NULL;
}

static void set_blend(uint8_t blend)
{
    switch (blend) {
    case SWF_BLEND_ADD: sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_FIX, 0, 0xFFFFFF); break;
    /* As a source factor, GU_SRC_COLOR (0) selects the destination colour. */
    case SWF_BLEND_MULTIPLY: sceGuBlendFunc(GU_ADD, GU_SRC_COLOR, GU_ONE_MINUS_SRC_ALPHA, 0, 0); break;
    case SWF_BLEND_SCREEN: sceGuBlendFunc(GU_ADD, GU_FIX, GU_ONE_MINUS_SRC_COLOR, 0xFFFFFF, 0); break;
    default: sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0); break;
    }
}

static void apply_stencil(void)
{
    sceGuStencilFunc(GU_EQUAL, g_stencil, 0xFF);
    sceGuStencilOp(GU_KEEP, GU_KEEP, GU_KEEP);
}

void swf_render_begin(const swf_assets *a)
{
    (void)a;
    g_ring_used = 0;
    g_stencil = 0;
    swf_texcache_frame(g_cache);
    swf_texcache_stats st;
    swf_texcache_get_stats(g_cache, &st);
    swf_prof.texture_used = st.used;
    swf_prof.texture_capacity = st.capacity;
    swf_prof.texture_uploads = st.uploads;
    swf_prof.texture_evictions = st.evictions;
    swf_prof.texture_misses = st.misses;
    ScePspFMatrix4 proj = {{2.0f / PLAT_SCREEN_W, 0, 0, 0},
                           {0, -2.0f / PLAT_SCREEN_H, 0, 0},
                           {0, 0, -1, 0},
                           {-1, 1, 0, 1}};
    ScePspFMatrix4 ident = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
    sceGuSetMatrix(GU_PROJECTION, &proj);
    sceGuSetMatrix(GU_VIEW, &ident);
    sceGuSetMatrix(GU_MODEL, &ident);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_CULL_FACE);
    sceGuEnable(GU_BLEND);
    set_blend(0);
    sceGuEnable(GU_STENCIL_TEST);
    apply_stencil();
    sceGuPixelMask(0);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);
    sceGuDisable(GU_TEXTURE_2D);
}

void swf_render_end(void)
{
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDisable(GU_STENCIL_TEST);
    set_blend(0);
}

static void load_matrix(const swf_matrix *m, float unit)
{
    ScePspFMatrix4 g = {{m->a * unit, m->b * unit, 0, 0},
                        {m->c * unit, m->d * unit, 0, 0},
                        {0, 0, 1, 0},
                        {m->tx, m->ty, 0, 1}};
    sceGuSetMatrix(GU_MODEL, &g);
}

static void bind_texture(const swf_assets *a, uint32_t index, bool smooth)
{
    const swf_texture *t = &a->textures[index];
    if (t->format == SWF_TEX_CLUT8) {
        sceGuClutMode(GU_PSM_8888, 0, 0xFF, 0);
        /* sceGuClutLoad counts blocks of 8 entries. */
        sceGuClutLoad((t->clut_count + 7) / 8, t->clut);
        sceGuTexMode(GU_PSM_T8, 0, 0, (t->flags & SWF_TEX_SWIZZLED) ? 1 : 0);
    } else {
        sceGuTexMode(t->format, 0, 0, (t->flags & SWF_TEX_SWIZZLED) ? 1 : 0);
    }
    swf_texcache_stats before;
    swf_texcache_get_stats(g_cache, &before);
    const void *data = swf_texcache_use(g_cache, index, t->data, t->data_size);
    swf_texcache_stats after;
    swf_texcache_get_stats(g_cache, &after);
    if (after.uploads != before.uploads) sceGuTexFlush(); /* the address may have held another texture */
    sceGuTexImage(0, t->width, t->height, t->width, data);
    int wrap = (t->flags & SWF_TEX_REPEAT) ? GU_REPEAT : GU_CLAMP;
    sceGuTexWrap(wrap, wrap);
    int f = smooth ? GU_LINEAR : GU_NEAREST;
    sceGuTexFilter(f, f);
    sceGuEnable(GU_TEXTURE_2D);
}

static const void *transformed_vertices(const swf_draw *d, const swf_cxform *cx)
{
    size_t size = (size_t)d->vertex_count * SWF_SOLID_VERTEX_SIZE;
    uint8_t *out = ring_alloc(size);
    if (!out) return NULL;
    memcpy(out, d->vertices, size);
    for (uint32_t i = 0; i < d->vertex_count; i++) {
        uint8_t *v = out + i * SWF_SOLID_VERTEX_SIZE;
        uint8_t c[4];
        swf_cxform_apply(cx, v, c);
        memcpy(v, c, 4);
    }
    ring_flush(out, size);
    return out;
}

void swf_render_shape(const swf_assets *a, uint32_t shape, const swf_matrix *m, const swf_cxform *cx,
                      uint8_t blend, bool strokes)
{
    if (shape >= a->shape_count) return;
    const swf_shape *s = &a->shapes[shape];
    set_blend(blend);
    bool plain = swf_cxform_is_identity_mul(cx) && !swf_cxform_has_add(cx);
    for (uint32_t i = 0; i < s->draw_count; i++) {
        const swf_draw *d = &a->draws[s->first_draw + i];
        if (!strokes && (d->flags & SWF_DRAW_STROKE)) continue;
        load_matrix(m, 32768.0f * (float)(1u << d->shift));
        if (!d->textured) {
            const void *v = plain ? d->vertices : transformed_vertices(d, cx);
            if (!v) continue;
            sceGuDisable(GU_TEXTURE_2D);
            sceGuDrawArray(GU_TRIANGLES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_INDEX_16BIT | GU_TRANSFORM_3D,
                           (int)d->index_count, d->indices, v);
            swf_prof.draws++;
            swf_prof.tris += d->index_count / 3;
            continue;
        }
        bind_texture(a, d->texture, (d->flags & SWF_DRAW_SMOOTH) != 0);
        const int vtype = GU_TEXTURE_32BITF | GU_VERTEX_16BIT | GU_INDEX_16BIT | GU_TRANSFORM_3D;
        uint8_t mul[4] = {unit_to_byte(cx->mul[0]), unit_to_byte(cx->mul[1]), unit_to_byte(cx->mul[2]),
                          unit_to_byte(cx->mul[3])};
        sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
        sceGuColor(abgr(mul));
        sceGuDrawArray(GU_TRIANGLES, vtype, (int)d->index_count, d->indices, d->vertices);
        swf_prof.draws++;
        swf_prof.tris += d->index_count / 3;
        if (swf_cxform_has_add(cx)) {
            /* Additive colour-transform term as a second pass: TFX_BLEND with the
             * environment and vertex colours both set to max(add, 0) yields that colour
             * with the texel's alpha times the alpha multiplier. Negative add terms are
             * not representable this way and are dropped. */
            uint8_t add[4];
            for (int k = 0; k < 3; k++) add[k] = cx->add[k] > 0 ? unit_to_byte(cx->add[k] / 255.0f) : 0;
            add[3] = mul[3];
            sceGuTexFunc(GU_TFX_BLEND, GU_TCC_RGBA);
            sceGuTexEnvColor(abgr(add) & 0xFFFFFF);
            sceGuColor(abgr(add));
            sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_FIX, 0, 0xFFFFFF);
            sceGuDrawArray(GU_TRIANGLES, vtype, (int)d->index_count, d->indices, d->vertices);
            sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
            set_blend(blend);
        }
        sceGuDisable(GU_TEXTURE_2D);
    }
}

void swf_render_mesh(const swf_matrix *m, const swf_mesh_vertex *v, uint32_t vertex_count, const uint16_t *idx,
                     uint32_t index_count, const swf_cxform *cx, uint8_t blend)
{
    if (!vertex_count || !index_count) return;
    size_t vsize = (size_t)vertex_count * sizeof(swf_mesh_vertex), isize = (size_t)index_count * 2;
    const void *verts = v;
    if (swf_cxform_is_identity_mul(cx) && !swf_cxform_has_add(cx)) {
        /* Same layout as the GE vertex: draw the caller's data in place. */
        ring_flush((void *)v, vsize);
        ring_flush((void *)idx, isize);
    } else {
        swf_mesh_vertex *out = ring_alloc(vsize);
        if (!out) return;
        for (uint32_t i = 0; i < vertex_count; i++) {
            uint32_t c = v[i].rgba;
            uint8_t in[4] = {(uint8_t)c, (uint8_t)(c >> 8), (uint8_t)(c >> 16), (uint8_t)(c >> 24)}, o[4];
            swf_cxform_apply(cx, in, o);
            out[i] = (swf_mesh_vertex){abgr(o), v[i].x, v[i].y, 0};
        }
        ring_flush(out, vsize);
        verts = out;
    }
    const uint16_t *ix = idx;
    load_matrix(m, 1.0f);
    set_blend(blend);
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDrawArray(GU_TRIANGLES, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_INDEX_16BIT | GU_TRANSFORM_3D,
                   (int)index_count, ix, verts);
    swf_prof.draws++;
    swf_prof.tris += index_count / 3;
}

void swf_render_rect(const swf_matrix *m, const int32_t r[4], uint32_t rgba, const swf_cxform *cx)
{
    uint8_t in[4] = {(uint8_t)rgba, (uint8_t)(rgba >> 8), (uint8_t)(rgba >> 16), (uint8_t)(rgba >> 24)}, c[4];
    swf_cxform_apply(cx, in, c);
    color_vertex_f *v = ring_alloc(6 * sizeof *v);
    if (!v) return;
    uint32_t col = abgr(c);
    float x0 = (float)r[0], y0 = (float)r[1], x1 = (float)r[2], y1 = (float)r[3];
    const float xy[6][2] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y0}, {x1, y1}, {x0, y1}};
    for (int i = 0; i < 6; i++) v[i] = (color_vertex_f){col, xy[i][0], xy[i][1], 0};
    ring_flush(v, 6 * sizeof *v);
    load_matrix(m, 1.0f);
    set_blend(0);
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDrawArray(GU_TRIANGLES, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D, 6, NULL, v);
}

void swf_render_glyph_quad(const swf_assets *a, const swf_matrix *m, const float src[4], const float dst[4],
                           uint32_t rgba, const swf_cxform *cx)
{
    const swf_texture *t = &a->textures[a->device_font.texture];
    uint8_t in[4] = {(uint8_t)rgba, (uint8_t)(rgba >> 8), (uint8_t)(rgba >> 16), (uint8_t)(rgba >> 24)}, c[4];
    swf_cxform_apply(cx, in, c);
    tex_vertex_f *v = ring_alloc(6 * sizeof *v);
    if (!v) return;
    float u0 = src[0] / t->width, v0 = src[1] / t->height;
    float u1 = (src[0] + src[2]) / t->width, v1 = (src[1] + src[3]) / t->height;
    float x0 = dst[0], y0 = dst[1], x1 = dst[0] + dst[2], y1 = dst[1] + dst[3];
    const tex_vertex_f q[6] = {{u0, v0, x0, y0, 0}, {u1, v0, x1, y0, 0}, {u1, v1, x1, y1, 0},
                               {u0, v0, x0, y0, 0}, {u1, v1, x1, y1, 0}, {u0, v1, x0, y1, 0}};
    memcpy(v, q, sizeof q);
    ring_flush(v, sizeof q);
    load_matrix(m, 1.0f);
    set_blend(0);
    bind_texture(a, a->device_font.texture, false);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuColor(abgr(c));
    sceGuDrawArray(GU_TRIANGLES, GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_3D, 6, NULL, v);
    sceGuDisable(GU_TEXTURE_2D);
}

/* Mask shapes only touch the stencil: the fixed blend keeps the destination colour. */
static void mask_write(int op)
{
    sceGuStencilFunc(GU_EQUAL, g_stencil, 0xFF);
    sceGuStencilOp(GU_KEEP, GU_KEEP, op);
    sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0, 0xFFFFFF);
}

void swf_render_mask_begin(void)
{
    mask_write(GU_INCR);
}

void swf_render_mask_end(void)
{
    g_stencil++;
    apply_stencil();
    set_blend(0);
}

void swf_render_mask_pop_begin(void)
{
    mask_write(GU_DECR);
}

void swf_render_mask_pop_end(void)
{
    g_stencil--;
    apply_stencil();
    set_blend(0);
}

/* The GE has render-to-texture, but the framebuffer alpha byte is the stencil, so a
 * layer could not carry coverage; blended objects use per-shape GE blending instead. */
bool swf_render_layer_begin(void)
{
    return false;
}

void swf_render_layer_end(uint8_t blend)
{
    (void)blend;
}

bool swf_render_read_rgb(uint8_t *rgb, int w, int h)
{
    (void)rgb;
    (void)w;
    (void)h;
    return false;
}
