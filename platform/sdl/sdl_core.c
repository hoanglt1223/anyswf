#include "plat_internal.h"
#include "plat_gl.h"
#include "sdl_internal.h"

#include <stdarg.h>

static SDL_Window *window;
static SDL_GLContext gl_context;
static SDL_Gamepad *gamepad;
static plat_input input_state;
static bool quit_requested;
static bool in_frame;
static bool vsync_on;
static Uint64 next_frame_ns;
static int screen_w, screen_h;
static int viewport[4];   /* letterboxed frame viewport: x, y from the top left, w, h */
static int window_px_w, window_px_h;

#define MAX_FINGERS 10
static struct finger {
    SDL_FingerID id;
    float x, y;   /* window pixels */
    bool used;
} fingers[MAX_FINGERS];

static void open_first_gamepad(void)
{
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    if (!ids) return;
    for (int i = 0; i < count && !gamepad; i++) gamepad = SDL_OpenGamepad(ids[i]);
    SDL_free(ids);
}

static char win_title[128];
static int win_scale, win_msaa;
static SDL_WindowFlags win_flags;

/* Window and GL context for the current screen size; falls back to no MSAA. */
static bool create_window(void)
{
    /* Stencil matches the PSP 8888 framebuffer, which renderers use for masks. */
#if PLAT_GL_ES
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    for (;;) {
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, win_msaa ? 1 : 0);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, win_msaa);
        window = SDL_CreateWindow(win_title, screen_w * win_scale, screen_h * win_scale, win_flags);
        if (window) gl_context = SDL_GL_CreateContext(window);
        if (window && gl_context) break;
        if (win_msaa == 0) {
            plat_log("plat: %s failed: %s", window ? "SDL_GL_CreateContext" : "SDL_CreateWindow", SDL_GetError());
            if (window) SDL_DestroyWindow(window);
            window = NULL;
            return false;
        }
        plat_log("plat: %dx MSAA unavailable (%s); continuing without", win_msaa, SDL_GetError());
        if (window) SDL_DestroyWindow(window);
        window = NULL;
        win_msaa = 0;
    }
    plat_set_vsync(vsync_on);
    return true;
}

plat_result plat_set_screen_size(int w, int h)
{
    if (!window) return PLAT_ERR_STATE;
    if (w <= 0 || h <= 0 || w > PLAT_SCREEN_MAX || h > PLAT_SCREEN_MAX) return PLAT_ERR_INVALID;
    if (w == screen_w && h == screen_h) return PLAT_OK;
    screen_w = w;
    screen_h = h;
#if defined(SDL_PLATFORM_ANDROID)
    return PLAT_OK; /* full screen: only the viewport changes */
#else
    if (!(win_flags & SDL_WINDOW_HIDDEN)) {
        if (!SDL_SetWindowSize(window, w * win_scale, h * win_scale))
            plat_log("plat: SDL_SetWindowSize failed: %s", SDL_GetError());
        SDL_SyncWindow(window);
        return PLAT_OK;
    }
    /* Hidden (headless) windows may render into a fixed-size surface (the offscreen driver
     * does), so they are recreated at the new size instead. */
    SDL_GL_DestroyContext(gl_context);
    gl_context = NULL;
    SDL_DestroyWindow(window);
    window = NULL;
    return create_window() ? PLAT_OK : PLAT_ERR_BACKEND;
#endif
}

plat_result plat_init(const plat_config *cfg)
{
    if (window) return PLAT_ERR_STATE;
    if (!cfg || !plat__app_id_is_valid(cfg->app_id)) return PLAT_ERR_INVALID;

    int sw = cfg->screen_w ? cfg->screen_w : PLAT_SCREEN_W;
    int sh = cfg->screen_h ? cfg->screen_h : PLAT_SCREEN_H;
    if (sw <= 0 || sh <= 0 || sw > PLAT_SCREEN_MAX || sh > PLAT_SCREEN_MAX) return PLAT_ERR_INVALID;
    if (cfg->touch_key_count < 0 || cfg->touch_key_count > PLAT_TOUCH_KEYS_MAX ||
        (cfg->touch_key_count > 0 && !cfg->touch_keys))
        return PLAT_ERR_INVALID;

    plat_result r = plat__files_init(cfg->app_id);
    if (r != PLAT_OK) return r;
    screen_w = sw;
    screen_h = sh;

    /* Fingers are handled here (touch buttons, then the pointer), not as synthetic mouse. */
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    /* Audio opens later in plat_audio_start; the hint must be set before then. */
    if (cfg->headless) SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        plat_log("plat: SDL_Init failed: %s", SDL_GetError());
        plat__files_shutdown();
        return PLAT_ERR_BACKEND;
    }

    win_scale = cfg->window_scale > 0 ? cfg->window_scale : 2;
    win_flags = SDL_WINDOW_OPENGL | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (cfg->headless) win_flags |= SDL_WINDOW_HIDDEN;
    else win_flags |= SDL_WINDOW_RESIZABLE;
#if defined(SDL_PLATFORM_ANDROID)
    win_flags |= SDL_WINDOW_FULLSCREEN;
#endif
    SDL_strlcpy(win_title, cfg->title ? cfg->title : cfg->app_id, sizeof win_title);
    win_msaa = cfg->msaa > 0 ? (cfg->msaa > 16 ? 16 : cfg->msaa) : 0;
    vsync_on = cfg->vsync;
    if (!create_window()) goto fail;
    open_first_gamepad();

    quit_requested = false;
    in_frame = false;
    SDL_zero(input_state);
    SDL_zero(fingers);
    plat__overlay_init(cfg->touch_keys, cfg->touch_key_count);
    plat_log("plat: %dx%d screen, %dx MSAA, SDL %d.%d.%d, GL %s (%s)", screen_w, screen_h, win_msaa, SDL_VERSIONNUM_MAJOR(SDL_GetVersion()),
             SDL_VERSIONNUM_MINOR(SDL_GetVersion()), SDL_VERSIONNUM_MICRO(SDL_GetVersion()),
             (const char *)glGetString(GL_VERSION), (const char *)glGetString(GL_RENDERER));
    return PLAT_OK;

fail:
    if (gl_context) SDL_GL_DestroyContext(gl_context);
    gl_context = NULL;
    if (window) SDL_DestroyWindow(window);
    window = NULL;
    SDL_Quit();
    plat__files_shutdown();
    return PLAT_ERR_BACKEND;
}

void plat_shutdown(void)
{
    plat_audio_stop();
    if (gamepad) SDL_CloseGamepad(gamepad);
    gamepad = NULL;
    if (gl_context) SDL_GL_DestroyContext(gl_context);
    gl_context = NULL;
    if (window) SDL_DestroyWindow(window);
    window = NULL;
    SDL_Quit();
    plat__files_shutdown();
}

void plat_set_vsync(bool on)
{
    if (!gl_context) return;
    vsync_on = on;
    next_frame_ns = SDL_GetTicksNS();
    if (!SDL_GL_SetSwapInterval(on ? 1 : 0))
        plat_log("plat: SDL_GL_SetSwapInterval(%d) failed: %s", on ? 1 : 0, SDL_GetError());
}

static void handle_event(const SDL_Event *ev)
{
    switch (ev->type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        quit_requested = true;
        break;
    case SDL_EVENT_KEY_DOWN:
        if (ev->key.key == SDLK_ESCAPE) quit_requested = true;
        break;
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_UP: {
        plat__overlay_touched();
        int free_slot = -1, slot = -1;
        for (int i = 0; i < MAX_FINGERS; i++) {
            if (fingers[i].used && fingers[i].id == ev->tfinger.fingerID) slot = i;
            else if (!fingers[i].used && free_slot < 0) free_slot = i;
        }
        if (ev->type == SDL_EVENT_FINGER_UP) {
            if (slot >= 0) fingers[slot].used = false;
            break;
        }
        if (slot < 0) slot = free_slot;
        if (slot < 0) break;
        fingers[slot].used = true;
        fingers[slot].id = ev->tfinger.fingerID;
        fingers[slot].x = ev->tfinger.x * (float)window_px_w;
        fingers[slot].y = ev->tfinger.y * (float)window_px_h;
        break;
    }
    case SDL_EVENT_FINGER_CANCELED:
        SDL_zero(fingers);
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
        if (!gamepad) gamepad = SDL_OpenGamepad(ev->gdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (gamepad && SDL_GetGamepadID(gamepad) == ev->gdevice.which) {
            SDL_CloseGamepad(gamepad);
            gamepad = NULL;
            open_first_gamepad();
        }
        break;
    default:
        break;
    }
}

static void sample_input(void)
{
    static const struct { SDL_Scancode key; uint32_t btn; } keymap[] = {
        {SDL_SCANCODE_UP, PLAT_BTN_UP},       {SDL_SCANCODE_DOWN, PLAT_BTN_DOWN},
        {SDL_SCANCODE_LEFT, PLAT_BTN_LEFT},   {SDL_SCANCODE_RIGHT, PLAT_BTN_RIGHT},
        {SDL_SCANCODE_Z, PLAT_BTN_CROSS},     {SDL_SCANCODE_X, PLAT_BTN_CIRCLE},
        {SDL_SCANCODE_A, PLAT_BTN_SQUARE},    {SDL_SCANCODE_S, PLAT_BTN_TRIANGLE},
        {SDL_SCANCODE_Q, PLAT_BTN_L},         {SDL_SCANCODE_W, PLAT_BTN_R},
        {SDL_SCANCODE_RETURN, PLAT_BTN_START}, {SDL_SCANCODE_RSHIFT, PLAT_BTN_SELECT},
    };
    static const struct { SDL_GamepadButton pad; uint32_t btn; } padmap[] = {
        {SDL_GAMEPAD_BUTTON_DPAD_UP, PLAT_BTN_UP},
        {SDL_GAMEPAD_BUTTON_DPAD_DOWN, PLAT_BTN_DOWN},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, PLAT_BTN_LEFT},
        {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, PLAT_BTN_RIGHT},
        {SDL_GAMEPAD_BUTTON_SOUTH, PLAT_BTN_CROSS},
        {SDL_GAMEPAD_BUTTON_EAST, PLAT_BTN_CIRCLE},
        {SDL_GAMEPAD_BUTTON_WEST, PLAT_BTN_SQUARE},
        {SDL_GAMEPAD_BUTTON_NORTH, PLAT_BTN_TRIANGLE},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, PLAT_BTN_L},
        {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, PLAT_BTN_R},
        {SDL_GAMEPAD_BUTTON_START, PLAT_BTN_START},
        {SDL_GAMEPAD_BUTTON_BACK, PLAT_BTN_SELECT},
    };

    uint32_t held = 0;
    const bool *keys = SDL_GetKeyboardState(NULL);
    for (size_t i = 0; i < SDL_arraysize(keymap); i++)
        if (keys[keymap[i].key]) held |= keymap[i].btn;

    /* IJKL stands in for the analog nub when no gamepad is attached. */
    float sx = (float)keys[SDL_SCANCODE_L] - (float)keys[SDL_SCANCODE_J];
    float sy = (float)keys[SDL_SCANCODE_K] - (float)keys[SDL_SCANCODE_I];

    if (gamepad) {
        for (size_t i = 0; i < SDL_arraysize(padmap); i++)
            if (SDL_GetGamepadButton(gamepad, padmap[i].pad)) held |= padmap[i].btn;
        float gx = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
        float gy = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
        if (gx * gx + gy * gy > sx * sx + sy * sy) {
            sx = gx;
            sy = gy;
        }
    }
    plat__input_update(&input_state, held, SDL_clamp(sx, -1.0f, 1.0f), SDL_clamp(sy, -1.0f, 1.0f));

    SDL_memset(input_state.keys, 0, sizeof input_state.keys);
    for (int sc = 0; sc < SDL_SCANCODE_COUNT; sc++) {
        if (!keys[sc]) continue;
        int code = plat__flash_key_from_scancode((SDL_Scancode)sc);
        if (code > 0) plat__key_set(input_state.keys, code);
    }

    /* Pointer: the first finger that is not on a touch button, else the mouse. */
    float px = 0, py = 0;
    bool down = false, have = false;
    for (int i = 0; i < MAX_FINGERS; i++) {
        if (!fingers[i].used) continue;
        int code = plat__overlay_hit(fingers[i].x, fingers[i].y, window_px_w, window_px_h);
        if (code > 0) {
            plat__key_set(input_state.keys, code);
        } else if (!have) {
            px = fingers[i].x;
            py = fingers[i].y;
            down = have = true;
        }
    }
    if (!have) {
        SDL_MouseButtonFlags mb = SDL_GetMouseState(&px, &py);
        float density = SDL_GetWindowPixelDensity(window);
        if (density > 0) {
            px *= density;
            py *= density;
        }
        down = (mb & SDL_BUTTON_LMASK) != 0;
        have = SDL_GetMouseFocus() == window;
    }
    if (viewport[2] > 0 && viewport[3] > 0) {
        input_state.pointer_x = (px - (float)viewport[0]) * (float)screen_w / (float)viewport[2];
        input_state.pointer_y = (py - (float)viewport[1]) * (float)screen_h / (float)viewport[3];
    }
    input_state.pointer_in = have && input_state.pointer_x >= 0 && input_state.pointer_y >= 0 &&
                             input_state.pointer_x < (float)screen_w && input_state.pointer_y < (float)screen_h;
    input_state.pointer_down = down;
}

bool plat_frame_begin(uint32_t clear_rgb)
{
    if (!window) return false;
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) handle_event(&ev);
    if (quit_requested) return false;

    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    window_px_w = w;
    window_px_h = h;
    plat__letterbox(w, h, screen_w, screen_h, viewport);
    sample_input();

    glViewport(0, 0, w, h);
    glClearColor(((clear_rgb >> 16) & 0xFF) / 255.0f, ((clear_rgb >> 8) & 0xFF) / 255.0f,
                 (clear_rgb & 0xFF) / 255.0f, 1.0f);
    glClearStencil(0);
    glStencilMask(0xFF);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glViewport(viewport[0], h - viewport[1] - viewport[3], viewport[2], viewport[3]);
    in_frame = true;
    return true;
}

void plat_frame_end(void)
{
    if (!in_frame) return;
    in_frame = false;
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) plat_log("plat: GL error 0x%04x during frame", (unsigned)err);
    plat__overlay_draw(window_px_w, window_px_h);
    SDL_GL_SwapWindow(window);

    if (!vsync_on) return;
    const Uint64 period = SDL_NS_PER_SECOND / PLAT_FRAME_HZ;
    Uint64 now = SDL_GetTicksNS();
    next_frame_ns += period;
    if (next_frame_ns > now) {
        SDL_DelayPrecise(next_frame_ns - now);
    } else if (now - next_frame_ns > period) {
        next_frame_ns = now;   /* fell behind (stall, debugger); do not try to catch up */
    }
}

void plat_input_get(plat_input *out)
{
    if (out) *out = input_state;
}

uint64_t plat_time_us(void)
{
    return SDL_GetTicksNS() / 1000u;
}

void plat_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    SDL_LogMessageV(SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_INFO, fmt, ap);
    va_end(ap);
}
