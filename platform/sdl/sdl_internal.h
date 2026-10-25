/* Shared between the SDL backend files. */
#ifndef PLAT_SDL_INTERNAL_H
#define PLAT_SDL_INTERNAL_H

#include "plat_internal.h"

#include <SDL3/SDL.h>

/* Flash key code for a keyboard scancode, 0 if it has none. */
int plat__flash_key_from_scancode(SDL_Scancode sc);

/* On-screen touch buttons. They stay hidden until the screen is first touched, except on
 * Android where they always show. */
void plat__overlay_init(const uint8_t *codes, int count);
void plat__overlay_touched(void);
/* Flash key code of the visible button under (x, y) in window pixels, 0 if none. */
int plat__overlay_hit(float x, float y, int win_w, int win_h);
/* Draws the buttons over the finished frame; leaves the viewport at the full window. */
void plat__overlay_draw(int win_w, int win_h);

#endif
