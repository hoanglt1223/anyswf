/* Backend-specific drawing for the hello app; rendering proper lands in the runtime renderer. */
#ifndef HELLO_DRAW_H
#define HELLO_DRAW_H

#include <stdbool.h>
#include <stdint.h>

typedef struct hello_vertex {
    float x, y;       /* screen pixels, 480x272 */
    uint8_t r, g, b;
} hello_vertex;

void hello_draw_triangle(const hello_vertex v[3]);

/* Reads a pixel of the frame being drawn (before plat_frame_end).
 * Returns false where the backend cannot read back. */
bool hello_read_pixel(int x, int y, uint8_t rgb[3]);

#endif
