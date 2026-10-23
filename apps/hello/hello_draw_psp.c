#include "hello_draw.h"

#include <pspgu.h>

typedef struct gu_vertex {
    unsigned int color;   /* ABGR */
    float x, y, z;
} gu_vertex;

void hello_draw_triangle(const hello_vertex v[3])
{
    gu_vertex *gv = sceGuGetMemory(3 * sizeof(gu_vertex));
    for (int i = 0; i < 3; i++) {
        gv[i].color = 0xFF000000u | ((unsigned)v[i].b << 16) | ((unsigned)v[i].g << 8) | v[i].r;
        gv[i].x = v[i].x;
        gv[i].y = v[i].y;
        gv[i].z = 0.0f;
    }
    sceGuDrawArray(GU_TRIANGLES, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 3, NULL, gv);
}

bool hello_read_pixel(int x, int y, uint8_t rgb[3])
{
    (void)x;
    (void)y;
    (void)rgb;
    return false;
}
