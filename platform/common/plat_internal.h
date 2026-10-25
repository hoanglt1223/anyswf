/* Shared between backends and the common code; not part of the public API. */
#ifndef PLAT_INTERNAL_H
#define PLAT_INTERNAL_H

#include "platform.h"

#define PLAT_STICK_DEADZONE 0.2f

bool plat__app_id_is_valid(const char *app_id);

/* Radial dead zone, rescaled so output still spans the full -1..1 range. */
void plat__apply_deadzone(float *x, float *y);

/* Derives pressed/released edges from the previous held mask. */
void plat__input_update(plat_input *state, uint32_t held, float stick_x, float stick_y);

/* Largest rectangle of the scr_w:scr_h aspect ratio centred in a win_w x win_h window,
 * as x, y (from the top left), w, h in window pixels. */
void plat__letterbox(int win_w, int win_h, int scr_w, int scr_h, int vp[4]);

/* Touch button rectangles (x, y, w, h in window pixels) for the given key codes: arrow keys
 * form a d-pad at the bottom left, the others a grid at the bottom right. */
void plat__touch_layout(const uint8_t *codes, int count, int win_w, int win_h, float rects[][4]);
/* Index of the button containing (x, y), or -1. */
int plat__touch_hit(const float rects[][4], int count, float x, float y);

static inline void plat__key_set(uint32_t keys[8], int code)
{
    if (code >= 0 && code < 256) keys[code >> 5] |= 1u << (code & 31);
}

/* Validates rel and joins it onto the root directory. */
plat_result plat__full_path(plat_root root, const char *rel, char *buf, size_t buf_size);

/* Backend hooks. */
plat_result plat__files_init(const char *app_id);   /* resolves roots, creates the USER dir */
void plat__files_shutdown(void);
const char *plat__root_dir(plat_root root);         /* ends with '/', NULL before init */
plat_result plat__replace_file(const char *from_full, const char *to_full);
plat_result plat__remove_full(const char *full);

#endif
