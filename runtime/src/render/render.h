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

/* Reads back the frame being drawn as RGB (480x272, top row first). False if unsupported. */
bool swf_render_read_rgb(uint8_t *rgb);

#endif
