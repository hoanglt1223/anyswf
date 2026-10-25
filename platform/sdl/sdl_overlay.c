/* Keyboard mapping and on-screen touch buttons for the SDL backend. */
#include "plat_gl.h"
#include "sdl_internal.h"

int plat__flash_key_from_scancode(SDL_Scancode sc)
{
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return 'A' + (sc - SDL_SCANCODE_A);
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return '1' + (sc - SDL_SCANCODE_1);
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) return 112 + (sc - SDL_SCANCODE_F1);
    if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9) return 97 + (sc - SDL_SCANCODE_KP_1);
    switch (sc) {
    case SDL_SCANCODE_0: return '0';
    case SDL_SCANCODE_KP_0: return 96;
    case SDL_SCANCODE_BACKSPACE: return 8;
    case SDL_SCANCODE_TAB: return 9;
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_KP_ENTER: return 13;
    case SDL_SCANCODE_LSHIFT:
    case SDL_SCANCODE_RSHIFT: return 16;
    case SDL_SCANCODE_LCTRL:
    case SDL_SCANCODE_RCTRL: return 17;
    case SDL_SCANCODE_LALT:
    case SDL_SCANCODE_RALT: return 18;
    case SDL_SCANCODE_PAUSE: return 19;
    case SDL_SCANCODE_CAPSLOCK: return 20;
    case SDL_SCANCODE_ESCAPE: return 27;
    case SDL_SCANCODE_SPACE: return 32;
    case SDL_SCANCODE_PAGEUP: return 33;
    case SDL_SCANCODE_PAGEDOWN: return 34;
    case SDL_SCANCODE_END: return 35;
    case SDL_SCANCODE_HOME: return 36;
    case SDL_SCANCODE_LEFT: return 37;
    case SDL_SCANCODE_UP: return 38;
    case SDL_SCANCODE_RIGHT: return 39;
    case SDL_SCANCODE_DOWN: return 40;
    case SDL_SCANCODE_INSERT: return 45;
    case SDL_SCANCODE_DELETE: return 46;
    case SDL_SCANCODE_KP_MULTIPLY: return 106;
    case SDL_SCANCODE_KP_PLUS: return 107;
    case SDL_SCANCODE_KP_MINUS: return 109;
    case SDL_SCANCODE_KP_PERIOD: return 110;
    case SDL_SCANCODE_KP_DIVIDE: return 111;
    case SDL_SCANCODE_SEMICOLON: return 186;
    case SDL_SCANCODE_EQUALS: return 187;
    case SDL_SCANCODE_COMMA: return 188;
    case SDL_SCANCODE_MINUS: return 189;
    case SDL_SCANCODE_PERIOD: return 190;
    case SDL_SCANCODE_SLASH: return 191;
    case SDL_SCANCODE_GRAVE: return 192;
    case SDL_SCANCODE_LEFTBRACKET: return 219;
    case SDL_SCANCODE_BACKSLASH: return 220;
    case SDL_SCANCODE_RIGHTBRACKET: return 221;
    case SDL_SCANCODE_APOSTROPHE: return 222;
    default: return 0;
    }
}

static uint8_t keys[PLAT_TOUCH_KEYS_MAX];
static int key_count;
static bool visible;

void plat__overlay_init(const uint8_t *codes, int count)
{
    key_count = 0;
    for (int i = 0; i < count && i < PLAT_TOUCH_KEYS_MAX; i++) keys[key_count++] = codes[i];
#if defined(SDL_PLATFORM_ANDROID)
    visible = key_count > 0;
#else
    visible = false;
#endif
}

void plat__overlay_touched(void)
{
    if (key_count > 0) visible = true;
}

int plat__overlay_hit(float x, float y, int win_w, int win_h)
{
    if (!visible) return 0;
    float rects[PLAT_TOUCH_KEYS_MAX][4];
    plat__touch_layout(keys, key_count, win_w, win_h, rects);
    int i = plat__touch_hit((const float(*)[4])rects, key_count, x, y);
    return i < 0 ? 0 : keys[i];
}

/* 3x5 pixel glyphs, rows top to bottom, 3 bits per row with the left pixel highest. */
#define G(a, b, c, d, e) (uint16_t)((a << 12) | (b << 9) | (c << 6) | (d << 3) | e)
static const uint16_t font_alpha[26] = {
    G(2, 5, 7, 5, 5), G(6, 5, 6, 5, 6), G(3, 4, 4, 4, 3), G(6, 5, 5, 5, 6), G(7, 4, 6, 4, 7),
    G(7, 4, 6, 4, 4), G(3, 4, 5, 5, 3), G(5, 5, 7, 5, 5), G(7, 2, 2, 2, 7), G(1, 1, 1, 5, 2),
    G(5, 5, 6, 5, 5), G(4, 4, 4, 4, 7), G(5, 7, 7, 5, 5), G(6, 5, 5, 5, 5), G(2, 5, 5, 5, 2),
    G(6, 5, 6, 4, 4), G(2, 5, 5, 6, 3), G(6, 5, 6, 5, 5), G(3, 4, 2, 1, 6), G(7, 2, 2, 2, 2),
    G(5, 5, 5, 5, 7), G(5, 5, 5, 5, 2), G(5, 5, 7, 7, 5), G(5, 5, 2, 5, 5), G(5, 5, 2, 2, 2),
    G(7, 1, 2, 4, 7)};
static const uint16_t font_digit[10] = {
    G(7, 5, 5, 5, 7), G(2, 6, 2, 2, 7), G(6, 1, 2, 4, 7), G(6, 1, 2, 1, 6), G(5, 5, 7, 1, 1),
    G(7, 4, 6, 1, 6), G(3, 4, 7, 5, 7), G(7, 1, 2, 2, 2), G(7, 5, 7, 5, 7), G(7, 5, 7, 1, 6)};
static const uint16_t font_unknown = G(6, 1, 2, 0, 2);
#undef G

static uint16_t glyph(char c)
{
    if (c >= 'A' && c <= 'Z') return font_alpha[c - 'A'];
    if (c >= '0' && c <= '9') return font_digit[c - '0'];
    return font_unknown;
}

#define MAX_TRIS 2048
static GLfloat verts[MAX_TRIS * 6];
static int tri_count;

static void tri(float x0, float y0, float x1, float y1, float x2, float y2)
{
    if (tri_count >= MAX_TRIS) return;
    GLfloat *v = verts + tri_count++ * 6;
    v[0] = x0, v[1] = y0, v[2] = x1, v[3] = y1, v[4] = x2, v[5] = y2;
}

static void quad(float x, float y, float w, float h)
{
    tri(x, y, x + w, y, x + w, y + h);
    tri(x, y, x + w, y + h, x, y + h);
}

static void flush(float r, float g, float b, float a)
{
    if (!tri_count) return;
    glColor4f(r, g, b, a);
    glVertexPointer(2, GL_FLOAT, 0, verts);
    glDrawArrays(GL_TRIANGLES, 0, tri_count * 3);
    tri_count = 0;
}

static void label(const char *text, const float r[4])
{
    int n = (int)SDL_strlen(text);
    /* Glyph cell is 4x5 (one column of spacing); fit the label into 70% of the button. */
    float px = r[2] * 0.7f / (float)(n * 4 - 1);
    if (px > r[3] * 0.08f) px = r[3] * 0.08f;
    float x = r[0] + (r[2] - px * (float)(n * 4 - 1)) * 0.5f, y = r[1] + (r[3] - px * 5) * 0.5f;
    for (int i = 0; i < n; i++, x += px * 4) {
        uint16_t g = glyph(text[i]);
        for (int row = 0; row < 5; row++)
            for (int col = 0; col < 3; col++)
                if (g & (1u << ((4 - row) * 3 + (2 - col)))) quad(x + px * (float)col, y + px * (float)row, px, px);
    }
}

static void arrow(int code, const float r[4])
{
    float cx = r[0] + r[2] * 0.5f, cy = r[1] + r[3] * 0.5f, k = r[2] * 0.25f;
    switch (code) {
    case 37: tri(cx - k, cy, cx + k, cy - k, cx + k, cy + k); break;
    case 38: tri(cx, cy - k, cx + k, cy + k, cx - k, cy + k); break;
    case 39: tri(cx + k, cy, cx - k, cy + k, cx - k, cy - k); break;
    default: tri(cx, cy + k, cx - k, cy - k, cx + k, cy - k); break;
    }
}

void plat__overlay_draw(int win_w, int win_h)
{
    if (!visible || win_w <= 0 || win_h <= 0) return;
    float rects[PLAT_TOUCH_KEYS_MAX][4];
    plat__touch_layout(keys, key_count, win_w, win_h, rects);

    glViewport(0, 0, win_w, win_h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, win_w, win_h, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_TEXTURE_2D);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    for (int i = 0; i < key_count; i++) quad(rects[i][0], rects[i][1], rects[i][2], rects[i][3]);
    flush(0.1f, 0.1f, 0.1f, 0.35f);
    for (int i = 0; i < key_count; i++) {
        if (keys[i] >= 37 && keys[i] <= 40) arrow(keys[i], rects[i]);
        else label(plat_key_label(keys[i]), rects[i]);
    }
    flush(1.0f, 1.0f, 1.0f, 0.75f);
    glDisableClientState(GL_VERTEX_ARRAY);
}
