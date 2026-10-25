/* MovieClip drawing API content: paths recorded in the order Flash keeps them (fills,
 * the lines drawn while a fill was open, then the open line) and tessellated on the
 * device into one coloured triangle mesh when the drawing changes. Fills use trapezoid
 * decomposition, so holes and self-intersections follow the fill rule; strokes are
 * quads with round joins. Gradient fills are evaluated per vertex. Implementation:
 * builtins/drawing.c. Coordinates are twips in the clip's own space. */
#ifndef SWFRT_DRAWING_H
#define SWFRT_DRAWING_H

#include "../render/render.h"
#include "swfrt/movie.h"

#define SWF_GRADIENT_MAX 15

typedef struct swf_fill_style {
    bool gradient;
    bool radial;
    uint32_t rgba;                  /* solid colour, RGBA bytes (R lowest) */
    uint8_t stops;
    uint8_t ratio[SWF_GRADIENT_MAX];
    uint32_t stop_rgba[SWF_GRADIENT_MAX];
    swf_matrix inverse;             /* clip twips -> gradient square (-16384..16384) */
} swf_fill_style;

swf_drawing *swf_drawing_new(void);
void swf_drawing_free(swf_drawing *d);
void swf_drawing_clear(swf_drawing *d);
/* Mutators return false when memory runs out (the command is dropped). */
bool swf_drawing_begin_fill(swf_drawing *d, const swf_fill_style *style);   /* NULL ends the fill */
bool swf_drawing_line_style(swf_drawing *d, bool on, float width, uint32_t rgba);
bool swf_drawing_move_to(swf_drawing *d, float x, float y);
bool swf_drawing_line_to(swf_drawing *d, float x, float y);
bool swf_drawing_curve_to(swf_drawing *d, float cx, float cy, float x, float y);
/* Copies the content (duplicateMovieClip). NULL when out of memory or empty. */
swf_drawing *swf_drawing_clone(const swf_drawing *d);

void swf_drawing_render(swf_drawing *d, const swf_matrix *m, const swf_cxform *cx, uint8_t blend, bool strokes);
/* Bounds of the content (strokes included) under `m`; false when empty. */
bool swf_drawing_bounds(const swf_drawing *d, const swf_matrix *m, float out[4]);
bool swf_drawing_hit(const swf_drawing *d, float x, float y);
/* Triangles of the last tessellation; for the profiler. */
uint32_t swf_drawing_triangles(const swf_drawing *d);

#endif
