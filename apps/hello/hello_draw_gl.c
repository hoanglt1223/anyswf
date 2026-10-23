#include "hello_draw.h"
#include "platform.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

void hello_draw_triangle(const hello_vertex v[3])
{
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, PLAT_SCREEN_W, PLAT_SCREEN_H, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glShadeModel(GL_SMOOTH);

    glBegin(GL_TRIANGLES);
    for (int i = 0; i < 3; i++) {
        glColor3ub(v[i].r, v[i].g, v[i].b);
        glVertex2f(v[i].x, v[i].y);
    }
    glEnd();
}

bool hello_read_pixel(int x, int y, uint8_t rgb[3])
{
    GLint vp[4];
    glGetIntegerv(GL_VIEWPORT, vp);
    if (vp[2] <= 0 || vp[3] <= 0) return false;

    /* Sample the centre of the logical pixel; GL rows start at the bottom. */
    int px = (int)((x + 0.5f) * vp[2] / PLAT_SCREEN_W);
    int py = vp[3] - 1 - (int)((y + 0.5f) * vp[3] / PLAT_SCREEN_H);
    uint8_t rgba[4];
    glReadPixels(px, py, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    if (glGetError() != GL_NO_ERROR) return false;
    rgb[0] = rgba[0];
    rgb[1] = rgba[1];
    rgb[2] = rgba[2];
    return true;
}
