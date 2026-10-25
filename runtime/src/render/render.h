/* Renderer backend interface: one implementation per target (render/gl.c on PC,
 * render/ge.c on PSP). Matrices map twips to screen pixels. */
#ifndef SWFRT_RENDER_H
#define SWFRT_RENDER_H

#include "swfrt/movie.h"

/* Uploads (PC) or prepares (PSP) every texture of the asset file. */
plat_result swf_render_init(const swf_assets *a);
void swf_render_shutdown(void);

/* Called between plat_frame_begin and plat_frame_end. */
void swf_render_begin(const swf_assets *a);
void swf_render_end(void);

/* Draws a shape. `strokes` false skips stroke draws (used when the shape is a mask). */
void swf_render_shape(const swf_assets *a, uint32_t shape, const swf_matrix *m, const swf_cxform *cx,
                      uint8_t blend, bool strokes);

/* Vertex of a mesh built at run time (drawing API, cursor): colour as RGBA bytes (R
 * lowest), position in twips, z = 0. The layout is the GE's COLOR_8888 | VERTEX_32BITF,
 * so meshes without a colour transform are drawn in place on PSP. */
typedef struct swf_mesh_vertex {
    uint32_t rgba;
    float x, y, z;
} swf_mesh_vertex;

/* Draws indexed triangles built at run time. The data must stay unchanged until the frame
 * has been presented (the GE may read it in place). */
void swf_render_mesh(const swf_matrix *m, const swf_mesh_vertex *v, uint32_t vertex_count, const uint16_t *idx,
                     uint32_t index_count, const swf_cxform *cx, uint8_t blend);

/* Draws a solid rectangle in twips (edit-text backgrounds and borders). */
void swf_render_rect(const swf_matrix *m, const int32_t rect[4], uint32_t rgba, const swf_cxform *cx);

/* Draws one textured quad from the device-font atlas: `src` in atlas pixels (x, y, w, h),
 * `dst` in twips relative to `m`. */
void swf_render_glyph_quad(const swf_assets *a, const swf_matrix *m, const float src[4], const float dst[4],
                           uint32_t rgba, const swf_cxform *cx);

/* Masks: draw mask shapes between begin/end, then the masked content, then pop once the
 * masked depth range ends. Nesting uses increasing stencil levels. */
void swf_render_mask_begin(void);
void swf_render_mask_end(void);
void swf_render_mask_pop_begin(void);
void swf_render_mask_pop_end(void);

/* Blend layers: a display object with a blend mode is drawn into an offscreen layer
 * (cleared to transparent, premultiplied alpha), which layer_end composites onto the
 * previous target with `blend`. layer_begin returns false when no layer is available
 * (backend without render targets, or nesting too deep); the caller then draws the
 * object directly, passing the blend mode to each swf_render_shape. */
bool swf_render_layer_begin(void);
void swf_render_layer_end(uint8_t blend);

/* Reads back the frame being drawn as w x h RGB (normally the movie's logical screen),
 * top row first, resampling the viewport if its size differs. False if unsupported. */
bool swf_render_read_rgb(uint8_t *rgb, int w, int h);

#endif
