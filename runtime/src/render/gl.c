/* OpenGL 2.1 / OpenGL ES 1.1 (fixed function) backend for the PC and Android targets.
 * Mirrors what the GE can do: vertex colours or a modulated texture, four blend
 * equations, stencil masks. */
#include "render.h"

#include "plat_gl.h"

#include <stdlib.h>
#include <string.h>

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef APIENTRY
#define APIENTRY GL_APIENTRY
#endif

static GLuint *g_tex;
static uint32_t g_tex_count;
static int g_stencil;          /* current mask nesting level */
static uint8_t *g_scratch;     /* per-vertex colours after a colour transform */
static size_t g_scratch_cap;

/* Blend layers need framebuffer objects (GL 3.0 / ARB_framebuffer_object) and
 * glBlendFuncSeparate (GL 1.4), both loaded at init; without them layers are off. */
/* Own pointer types: the desktop and ES headers name them differently (…OESPROC). */
static void(APIENTRY *p_glBlendFuncSeparate)(GLenum, GLenum, GLenum, GLenum);
static void(APIENTRY *p_glGenFramebuffers)(GLsizei, GLuint *);
static void(APIENTRY *p_glDeleteFramebuffers)(GLsizei, const GLuint *);
static void(APIENTRY *p_glBindFramebuffer)(GLenum, GLuint);
static void(APIENTRY *p_glFramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
static GLenum(APIENTRY *p_glCheckFramebufferStatus)(GLenum);
static void(APIENTRY *p_glGenRenderbuffers)(GLsizei, GLuint *);
static void(APIENTRY *p_glDeleteRenderbuffers)(GLsizei, const GLuint *);
static void(APIENTRY *p_glBindRenderbuffer)(GLenum, GLuint);
static void(APIENTRY *p_glRenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
static void(APIENTRY *p_glFramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
static bool g_layers_ok;
static float g_screen_w = PLAT_SCREEN_W, g_screen_h = PLAT_SCREEN_H; /* logical screen of the movie */

#define MAX_LAYERS 4
typedef struct {
    GLuint fbo, tex, rb;
    GLint w, h;
    int saved_stencil;
} gl_layer;
static gl_layer g_layers[MAX_LAYERS];
static int g_layer_depth;
static GLint g_viewport[4]; /* viewport of the window framebuffer, captured per frame */

/* Every p_gl* pointer has the size and representation of SDL_FunctionPointer. */
static bool load_proc(SDL_FunctionPointer *dst, const char *name)
{
    *dst = SDL_GL_GetProcAddress(name);
    return *dst != NULL;
}

static void load_gl_procs(void)
{
#define LOAD(name) load_proc((SDL_FunctionPointer *)&p_##name, PLAT_GL_EXT_NAME(name))
    g_layers_ok = LOAD(glBlendFuncSeparate) && LOAD(glGenFramebuffers) && LOAD(glDeleteFramebuffers) &&
                  LOAD(glBindFramebuffer) && LOAD(glFramebufferTexture2D) && LOAD(glCheckFramebufferStatus) &&
                  LOAD(glGenRenderbuffers) && LOAD(glDeleteRenderbuffers) && LOAD(glBindRenderbuffer) &&
                  LOAD(glRenderbufferStorage) && LOAD(glFramebufferRenderbuffer);
#undef LOAD
    if (!g_layers_ok) plat_log("render: no framebuffer objects; blend modes are applied per shape");
}

/* Colour uses (src, dst); alpha always accumulates coverage, so a layer holds
 * premultiplied colour with correct alpha. */
static void blend_func(GLenum src, GLenum dst)
{
    if (p_glBlendFuncSeparate) {
        p_glBlendFuncSeparate(src, dst, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    } else {
        glBlendFunc(src, dst);
    }
}

static void free_layers(void)
{
    for (int i = 0; i < MAX_LAYERS; i++) {
        gl_layer *l = &g_layers[i];
        if (l->fbo) p_glDeleteFramebuffers(1, &l->fbo);
        if (l->rb) p_glDeleteRenderbuffers(1, &l->rb);
        if (l->tex) glDeleteTextures(1, &l->tex);
        memset(l, 0, sizeof *l);
    }
    g_layer_depth = 0;
}

static void expand_texels(const swf_texture *t, const uint8_t *lin, uint8_t *rgba)
{
    uint32_t n = (uint32_t)t->width * t->height;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t *o = rgba + i * 4;
        uint16_t v;
        switch (t->format) {
        case SWF_TEX_CLUT8:
            memcpy(o, t->clut + (lin[i] < t->clut_count ? lin[i] : 0) * 4, 4);
            break;
        case SWF_TEX_8888:
            memcpy(o, lin + i * 4, 4);
            break;
        case SWF_TEX_565:
            v = (uint16_t)(lin[i * 2] | (lin[i * 2 + 1] << 8));
            o[0] = (uint8_t)((v & 31) * 255 / 31);
            o[1] = (uint8_t)(((v >> 5) & 63) * 255 / 63);
            o[2] = (uint8_t)(((v >> 11) & 31) * 255 / 31);
            o[3] = 255;
            break;
        case SWF_TEX_5551:
            v = (uint16_t)(lin[i * 2] | (lin[i * 2 + 1] << 8));
            o[0] = (uint8_t)((v & 31) * 255 / 31);
            o[1] = (uint8_t)(((v >> 5) & 31) * 255 / 31);
            o[2] = (uint8_t)(((v >> 10) & 31) * 255 / 31);
            o[3] = (v >> 15) ? 255 : 0;
            break;
        case SWF_TEX_4444:
            v = (uint16_t)(lin[i * 2] | (lin[i * 2 + 1] << 8));
            o[0] = (uint8_t)((v & 15) * 17);
            o[1] = (uint8_t)(((v >> 4) & 15) * 17);
            o[2] = (uint8_t)(((v >> 8) & 15) * 17);
            o[3] = (uint8_t)((v >> 12) * 17);
            break;
        }
    }
}

static uint8_t *unswizzle(const uint8_t *src, uint32_t width_bytes, uint32_t height)
{
    uint8_t *out = malloc((size_t)width_bytes * height);
    if (!out) return NULL;
    const uint8_t *s = src;
    for (uint32_t by = 0; by < height / 8; by++)
        for (uint32_t bx = 0; bx < width_bytes / 16; bx++)
            for (uint32_t row = 0; row < 8; row++) {
                memcpy(out + (by * 8 + row) * width_bytes + bx * 16, s, 16);
                s += 16;
            }
    return out;
}

plat_result swf_render_init(const swf_assets *a)
{
    swf_render_shutdown();
    load_gl_procs();
    g_tex_count = a->texture_count;
    g_tex = calloc(g_tex_count ? g_tex_count : 1, sizeof *g_tex);
    if (!g_tex) return PLAT_ERR_NO_MEMORY;
    if (g_tex_count) glGenTextures((GLsizei)g_tex_count, g_tex);
    for (uint32_t i = 0; i < g_tex_count; i++) {
        const swf_texture *t = &a->textures[i];
        uint32_t bpp = t->format == SWF_TEX_CLUT8 ? 8 : t->format == SWF_TEX_8888 ? 32 : 16;
        const uint8_t *lin = t->data;
        uint8_t *tmp = NULL;
        if (t->flags & SWF_TEX_SWIZZLED) {
            tmp = unswizzle(t->data, t->width * bpp / 8, t->height);
            if (!tmp) return PLAT_ERR_NO_MEMORY;
            lin = tmp;
        }
        uint8_t *rgba = malloc((size_t)t->width * t->height * 4);
        if (!rgba) {
            free(tmp);
            return PLAT_ERR_NO_MEMORY;
        }
        expand_texels(t, lin, rgba);
        glBindTexture(GL_TEXTURE_2D, g_tex[i]);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, t->width, t->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        GLint wrap = (t->flags & SWF_TEX_REPEAT) ? GL_REPEAT : GL_CLAMP_TO_EDGE;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
        free(rgba);
        free(tmp);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        plat_log("render: texture upload failed (GL error 0x%x)", err);
        return PLAT_ERR_BACKEND;
    }
    return PLAT_OK;
}

void swf_render_shutdown(void)
{
    if (g_tex && g_tex_count) glDeleteTextures((GLsizei)g_tex_count, g_tex);
    free(g_tex);
    g_tex = NULL;
    g_tex_count = 0;
    if (g_layers_ok) free_layers();
    free(g_scratch);
    g_scratch = NULL;
    g_scratch_cap = 0;
}

void swf_render_begin(const swf_assets *a)
{
    g_screen_w = a->screen_w;
    g_screen_h = a->screen_h;
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, g_screen_w, g_screen_h, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    blend_func(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_STENCIL_TEST);
    glStencilMask(0xFF);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    g_stencil = 0;
    g_layer_depth = 0;
    glGetIntegerv(GL_VIEWPORT, g_viewport);
}

void swf_render_end(void)
{
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_STENCIL_TEST);
}

static void set_blend(uint8_t blend)
{
    switch (blend) {
    case SWF_BLEND_ADD: blend_func(GL_SRC_ALPHA, GL_ONE); break;
    case SWF_BLEND_MULTIPLY: blend_func(GL_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA); break;
    case SWF_BLEND_SCREEN: blend_func(GL_ONE, GL_ONE_MINUS_SRC_COLOR); break;
    default: blend_func(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); break;
    }
}

static void load_matrix(const swf_matrix *m, float unit)
{
    GLfloat g[16] = {m->a * unit, m->b * unit, 0, 0, m->c * unit, m->d * unit, 0, 0, 0, 0, 1, 0, m->tx, m->ty, 0, 1};
    glLoadMatrixf(g);
}

static const uint8_t *transformed_colors(const swf_draw *d, const swf_cxform *cx)
{
    size_t need = (size_t)d->vertex_count * 4;
    if (need > g_scratch_cap) {
        uint8_t *n = realloc(g_scratch, need);
        if (!n) return NULL;
        g_scratch = n;
        g_scratch_cap = need;
    }
    const uint8_t *v = d->vertices;
    for (uint32_t i = 0; i < d->vertex_count; i++)
        swf_cxform_apply(cx, v + i * SWF_SOLID_VERTEX_SIZE, g_scratch + i * 4);
    return g_scratch;
}

void swf_render_shape(const swf_assets *a, uint32_t shape, const swf_matrix *m, const swf_cxform *cx,
                      uint8_t blend, bool strokes)
{
    if (shape >= a->shape_count) return;
    const swf_shape *s = &a->shapes[shape];
    set_blend(blend);
    glEnableClientState(GL_VERTEX_ARRAY);
    for (uint32_t i = 0; i < s->draw_count; i++) {
        const swf_draw *d = &a->draws[s->first_draw + i];
        if (!strokes && (d->flags & SWF_DRAW_STROKE)) continue;
        load_matrix(m, (float)(1u << d->shift));
        const uint8_t *v = d->vertices;
        if (!d->textured) {
            glDisable(GL_TEXTURE_2D);
            glDisableClientState(GL_TEXTURE_COORD_ARRAY);
            glEnableClientState(GL_COLOR_ARRAY);
            bool plain = swf_cxform_is_identity_mul(cx) && !swf_cxform_has_add(cx);
            const uint8_t *cols = plain ? v : transformed_colors(d, cx);
            if (!cols) continue;
            glColorPointer(4, GL_UNSIGNED_BYTE, plain ? SWF_SOLID_VERTEX_SIZE : 4, cols);
            glVertexPointer(2, GL_SHORT, SWF_SOLID_VERTEX_SIZE, v + 4);
        } else {
            glDisableClientState(GL_COLOR_ARRAY);
            glEnable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, g_tex[d->texture]);
            GLint f = (d->flags & SWF_DRAW_SMOOTH) ? GL_LINEAR : GL_NEAREST;
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
            glColor4f(cx->mul[0], cx->mul[1], cx->mul[2], cx->mul[3]);
            glEnableClientState(GL_TEXTURE_COORD_ARRAY);
            glTexCoordPointer(2, GL_FLOAT, SWF_TEX_VERTEX_SIZE, v);
            glVertexPointer(2, GL_SHORT, SWF_TEX_VERTEX_SIZE, v + 8);
        }
        glDrawElements(GL_TRIANGLES, (GLsizei)d->index_count, GL_UNSIGNED_SHORT, d->indices);
        if (d->textured && swf_cxform_has_add(cx)) {
            /* Additive colour-transform term: a second pass adds max(add, 0) weighted by
             * the texel's coverage, the same two-pass scheme the GE uses. Negative add
             * terms are not representable this way and are dropped. */
            if (p_glBlendFuncSeparate) {
                p_glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ZERO, GL_ONE);
            } else {
                glBlendFunc(GL_SRC_ALPHA, GL_ONE);
            }
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
            glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_REPLACE);
            glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_RGB, GL_PRIMARY_COLOR);
            glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_MODULATE);
            glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_ALPHA, GL_TEXTURE);
            glTexEnvi(GL_TEXTURE_ENV, GL_SRC1_ALPHA, GL_PRIMARY_COLOR);
            float add[3];
            for (int k = 0; k < 3; k++) add[k] = cx->add[k] > 0 ? cx->add[k] / 255.0f : 0.0f;
            glColor4f(add[0], add[1], add[2], cx->mul[3]);
            glDrawElements(GL_TRIANGLES, (GLsizei)d->index_count, GL_UNSIGNED_SHORT, d->indices);
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
            set_blend(blend);
        }
    }
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisable(GL_TEXTURE_2D);
}

/* Quad as a triangle fan (no GL_QUADS or glBegin on ES); uv may be NULL. */
static void draw_quad(const GLfloat xy[8], const GLfloat *uv)
{
    glDisableClientState(GL_COLOR_ARRAY);
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, xy);
    if (uv) {
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glTexCoordPointer(2, GL_FLOAT, 0, uv);
    } else {
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    }
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
}

void swf_render_rect(const swf_matrix *m, const int32_t r[4], uint32_t rgba, const swf_cxform *cx)
{
    uint8_t in[4] = {(uint8_t)rgba, (uint8_t)(rgba >> 8), (uint8_t)(rgba >> 16), (uint8_t)(rgba >> 24)}, c[4];
    swf_cxform_apply(cx, in, c);
    load_matrix(m, 1.0f);
    glDisable(GL_TEXTURE_2D);
    set_blend(0);
    glColor4ub(c[0], c[1], c[2], c[3]);
    const GLfloat xy[8] = {(float)r[0], (float)r[1], (float)r[2], (float)r[1],
                           (float)r[2], (float)r[3], (float)r[0], (float)r[3]};
    draw_quad(xy, NULL);
}

void swf_render_glyph_quad(const swf_assets *a, const swf_matrix *m, const float src[4], const float dst[4],
                           uint32_t rgba, const swf_cxform *cx)
{
    const swf_texture *t = &a->textures[a->device_font.texture];
    uint8_t in[4] = {(uint8_t)rgba, (uint8_t)(rgba >> 8), (uint8_t)(rgba >> 16), (uint8_t)(rgba >> 24)}, c[4];
    swf_cxform_apply(cx, in, c);
    load_matrix(m, 1.0f);
    set_blend(0);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, g_tex[a->device_font.texture]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glColor4ub(c[0], c[1], c[2], c[3]);
    float u0 = src[0] / t->width, v0 = src[1] / t->height;
    float u1 = (src[0] + src[2]) / t->width, v1 = (src[1] + src[3]) / t->height;
    const GLfloat uv[8] = {u0, v0, u1, v0, u1, v1, u0, v1};
    const GLfloat xy[8] = {dst[0], dst[1], dst[0] + dst[2], dst[1], dst[0] + dst[2], dst[1] + dst[3],
                           dst[0], dst[1] + dst[3]};
    draw_quad(xy, uv);
    glDisable(GL_TEXTURE_2D);
}

/* Stencil masks: level n content is drawn where stencil == n. A mask raises the covered
 * pixels from n to n+1 (EQUAL test, so overlapping mask triangles count once). */
void swf_render_mask_begin(void)
{
    glEnable(GL_STENCIL_TEST);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glStencilFunc(GL_EQUAL, g_stencil, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);
}

void swf_render_mask_end(void)
{
    g_stencil++;
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glStencilFunc(GL_EQUAL, g_stencil, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
}

void swf_render_mask_pop_begin(void)
{
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glStencilFunc(GL_EQUAL, g_stencil, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_DECR);
}

void swf_render_mask_pop_end(void)
{
    g_stencil--;
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    if (g_stencil == 0) {
        glDisable(GL_STENCIL_TEST);
    } else {
        glStencilFunc(GL_EQUAL, g_stencil, 0xFF);
    }
}

static void apply_stencil_state(void)
{
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    if (g_stencil == 0) {
        glDisable(GL_STENCIL_TEST);
    } else {
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(GL_EQUAL, g_stencil, 0xFF);
    }
}

static bool layer_alloc(gl_layer *l, GLint w, GLint h)
{
    if (l->fbo && l->w == w && l->h == h) return true;
    if (!l->fbo) {
        p_glGenFramebuffers(1, &l->fbo);
        p_glGenRenderbuffers(1, &l->rb);
        glGenTextures(1, &l->tex);
    }
    glBindTexture(GL_TEXTURE_2D, l->tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    p_glBindRenderbuffer(GL_RENDERBUFFER, l->rb);
    p_glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
    p_glBindRenderbuffer(GL_RENDERBUFFER, 0);
    p_glBindFramebuffer(GL_FRAMEBUFFER, l->fbo);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, l->tex, 0);
    /* Packed depth-stencil on both attachment points (GL_DEPTH_STENCIL_ATTACHMENT is not on ES). */
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, l->rb);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, l->rb);
    bool ok = p_glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    l->w = ok ? w : 0;
    l->h = ok ? h : 0;
    return ok;
}

static void bind_target(int depth)
{
    if (depth == 0) {
        p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(g_viewport[0], g_viewport[1], g_viewport[2], g_viewport[3]);
    } else {
        const gl_layer *l = &g_layers[depth - 1];
        p_glBindFramebuffer(GL_FRAMEBUFFER, l->fbo);
        glViewport(0, 0, l->w, l->h);
    }
}

bool swf_render_layer_begin(void)
{
    if (!g_layers_ok || g_layer_depth == MAX_LAYERS || g_viewport[2] <= 0 || g_viewport[3] <= 0) return false;
    gl_layer *l = &g_layers[g_layer_depth];
    if (!layer_alloc(l, g_viewport[2], g_viewport[3])) {
        plat_log("render: blend layer framebuffer incomplete; blend modes are applied per shape");
        g_layers_ok = false;
        bind_target(g_layer_depth);
        return false;
    }
    l->saved_stencil = g_stencil;
    g_layer_depth++;
    bind_target(g_layer_depth);
    glClearColor(0, 0, 0, 0);
    glClearStencil(0);
    glStencilMask(0xFF);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    g_stencil = 0;
    apply_stencil_state();
    return true;
}

void swf_render_layer_end(uint8_t blend)
{
    if (g_layer_depth == 0) return;
    gl_layer *l = &g_layers[--g_layer_depth];
    bind_target(g_layer_depth);
    g_stencil = l->saved_stencil;
    apply_stencil_state();
    /* The layer is premultiplied, so each mode uses its premultiplied form. */
    switch (blend) {
    case SWF_BLEND_ADD: blend_func(GL_ONE, GL_ONE); break;
    case SWF_BLEND_MULTIPLY: blend_func(GL_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA); break;
    case SWF_BLEND_SCREEN: blend_func(GL_ONE, GL_ONE_MINUS_SRC_COLOR); break;
    default: blend_func(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); break;
    }
    glLoadIdentity();
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, l->tex);
    glColor4f(1, 1, 1, 1);
    /* Texture rows start at the bottom of the viewport; the projection puts y = 0 at the top. */
    const GLfloat uv[8] = {0, 1, 1, 1, 1, 0, 0, 0};
    const GLfloat xy[8] = {0, 0, g_screen_w, 0, g_screen_w, g_screen_h, 0, g_screen_h};
    draw_quad(xy, uv);
    glDisable(GL_TEXTURE_2D);
    set_blend(0);
}

bool swf_render_read_rgb(uint8_t *rgb, int w, int h)
{
    if (w <= 0 || h <= 0) return false;
    GLint vp[4];
    glGetIntegerv(GL_VIEWPORT, vp);
    if (vp[2] <= 0 || vp[3] <= 0) return false;
    uint8_t *full = malloc((size_t)vp[2] * vp[3] * 4);
    if (!full) return false;
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(vp[0], vp[1], vp[2], vp[3], GL_RGBA, GL_UNSIGNED_BYTE, full);
    bool ok = glGetError() == GL_NO_ERROR;
    for (int y = 0; ok && y < h; y++) {
        int sy = vp[3] - 1 - (int)((y + 0.5f) * vp[3] / h);
        for (int x = 0; x < w; x++) {
            int sx = (int)((x + 0.5f) * vp[2] / w);
            memcpy(rgb + ((size_t)y * w + x) * 3, full + ((size_t)sy * vp[2] + sx) * 4, 3);
        }
    }
    free(full);
    return ok;
}
