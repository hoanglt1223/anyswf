/* Plays a converted movie (assets.bin, plus compiled scripts once linked).
 * PC: anyswf-player [assets.bin] [--headless] [--frames N] [--screenshot out.png]
 *       [--msaa N] [--touch-keys k1,k2,...] [--config anyswf.toml] [--input SCRIPT]
 *       [--save-dir DIR] [--wav out.wav]
 *   Without a path it loads assets.bin from the executable's folder (the APK assets on
 *   Android, Contents/Resources in a macOS app bundle).
 *   --screenshot runs exactly N SWF frames (default 1), one per display frame, then
 *   writes the last one as PNG at the movie's screen size; it implies --headless and
 *   hides the cursor.
 *   --msaa sets the multisample count (default 4 in a window, 0 headless).
 *   --touch-keys lists keys for on-screen touch buttons (names as plat_key_from_name).
 *   Headless runs are deterministic: one SWF frame per display frame, the clock
 *   (getTimer, intervals) advances by the SWF frame period, and sound is mixed on the
 *   game thread (written to --wav when given) instead of the audio device.
 *   --input F:BUTTONS[@X/Y],... holds BUTTONS (up, down, left, right, cross, circle,
 *   square, triangle, l, r, start, select joined by '+', or none) from display frame F
 *   on, with the cursor at screen pixel X/Y when given.
 *   anyswf.toml defaults to the file next to assets.bin.
 * PSP: loads assets.bin and anyswf.toml from the EBOOT folder and plays until exit.
 * Game builds (cmake/anyswf-game.cmake) define ANYSWF_APP_ID (save folder) and ANYSWF_TITLE. */
#include "platform.h"
#include "render/render.h"
#include "swfrt/avm1.h"
#include "swfrt/mixer.h"
#include "swfrt/movie.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__ANDROID__)
#include <SDL3/SDL_main.h> /* SDLActivity calls SDL_main */
#endif

#ifndef ANYSWF_APP_ID
#define ANYSWF_APP_ID "ANYSWFPLAYER"
#endif
#ifndef ANYSWF_TITLE
#define ANYSWF_TITLE "AnySWF"
#endif

#ifdef __PSP__
#include <pspkernel.h>
PSP_MODULE_INFO("anyswf_player", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-1024);
#else
#include "png_write.h"
#endif

#define MAX_INPUT_STEPS 256

typedef struct input_step {
    long frame;
    uint32_t held;
    bool has_cursor;
    float x, y;
} input_step;

typedef struct args {
    const char *assets;
    const char *screenshot;
    const char *config;
    const char *save_dir;
    const char *wav;
    long frames;
    bool headless;
    int msaa; /* -1 = default */
    uint8_t touch_keys[PLAT_TOUCH_KEYS_MAX];
    int touch_key_count;
    input_step steps[MAX_INPUT_STEPS];
    int nsteps;
} args;

static bool parse_buttons(const char *s, size_t n, uint32_t *held)
{
    static const char *const NAMES[] = {"up", "down", "left", "right", "cross", "circle",
                                        "square", "triangle", "l", "r", "start", "select"};
    *held = 0;
    while (n > 0) {
        const char *plus = memchr(s, '+', n);
        size_t len = plus ? (size_t)(plus - s) : n;
        bool found = len == 4 && strncmp(s, "none", 4) == 0;
        for (int i = 0; i < 12 && !found; i++) {
            if (strlen(NAMES[i]) == len && strncmp(s, NAMES[i], len) == 0) {
                *held |= 1u << i;
                found = true;
            }
        }
        if (!found) return false;
        s += len;
        n -= len;
        if (n) {
            s++;
            n--;
        }
    }
    return true;
}

static bool parse_input(const char *spec, args *a)
{
    const char *p = spec;
    while (*p) {
        if (a->nsteps == MAX_INPUT_STEPS) return false;
        input_step *st = &a->steps[a->nsteps];
        memset(st, 0, sizeof *st);
        char *end;
        st->frame = strtol(p, &end, 10);
        if (end == p || *end != ':' || st->frame < 1) return false;
        p = end + 1;
        size_t n = strcspn(p, ",@");
        if (!parse_buttons(p, n, &st->held)) return false;
        p += n;
        if (*p == '@') {
            st->x = strtof(p + 1, &end);
            if (*end != '/') return false;
            st->y = strtof(end + 1, &end);
            st->has_cursor = true;
            p = end;
        }
        if (*p == ',') p++;
        else if (*p) return false;
        if (a->nsteps && st->frame < a->steps[a->nsteps - 1].frame) return false;
        a->nsteps++;
    }
    return true;
}

/* "left,right,space" -> key codes; false on an unknown name or too many keys. */
static bool parse_touch_keys(const char *list, args *a)
{
    char name[16];
    size_t n = 0;
    for (const char *p = list;; p++) {
        if (*p == ',' || *p == '\0') {
            name[n] = '\0';
            int code = plat_key_from_name(name);
            if (code < 0 || a->touch_key_count == PLAT_TOUCH_KEYS_MAX) {
                fprintf(stderr, "player: bad --touch-keys entry '%s'\n", name);
                return false;
            }
            a->touch_keys[a->touch_key_count++] = (uint8_t)code;
            n = 0;
            if (*p == '\0') return true;
        } else if (n + 1 < sizeof name) {
            name[n++] = *p;
        } else {
            fprintf(stderr, "player: --touch-keys entry too long\n");
            return false;
        }
    }
}

static bool parse_args(int argc, char **argv, args *a)
{
    memset(a, 0, sizeof *a);
    a->msaa = -1;
    for (int i = 1; i < argc; i++) {
        bool more = i + 1 < argc;
        if (strcmp(argv[i], "--headless") == 0) {
            a->headless = true;
        } else if (strcmp(argv[i], "--frames") == 0 && more) {
            char *end;
            a->frames = strtol(argv[++i], &end, 10);
            if (*end || a->frames <= 0) return false;
        } else if (strcmp(argv[i], "--msaa") == 0 && more) {
            char *end;
            long v = strtol(argv[++i], &end, 10);
            if (*end || v < 0 || v > 16) return false;
            a->msaa = (int)v;
        } else if (strcmp(argv[i], "--touch-keys") == 0 && more) {
            if (!parse_touch_keys(argv[++i], a)) return false;
        } else if (strcmp(argv[i], "--screenshot") == 0 && more) {
            a->screenshot = argv[++i];
            a->headless = true;
        } else if (strcmp(argv[i], "--config") == 0 && more) {
            a->config = argv[++i];
        } else if (strcmp(argv[i], "--save-dir") == 0 && more) {
            a->save_dir = argv[++i];
        } else if (strcmp(argv[i], "--wav") == 0 && more) {
            a->wav = argv[++i];
        } else if (strcmp(argv[i], "--input") == 0 && more) {
            if (!parse_input(argv[++i], a)) return false;
        } else if (argv[i][0] != '-' && !a->assets) {
            a->assets = argv[i];
        } else {
            return false;
        }
    }
    if (a->screenshot && a->frames == 0) a->frames = 1;
    return true;
}

#ifndef __PSP__
/* PC builds load from any path (the platform file API is rooted for portability). */
static void *read_path(const char *path, size_t *size, bool aligned)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    void *buf = NULL;
    long n = -1;
    if (fseek(f, 0, SEEK_END) == 0) n = ftell(f);
    if (n >= 0 && fseek(f, 0, SEEK_SET) == 0) {
        buf = aligned ? swf_aligned_alloc((size_t)n + 1) : malloc((size_t)n + 1);
        if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
            if (aligned) swf_aligned_free(buf);
            else free(buf);
            buf = NULL;
        }
    }
    fclose(f);
    if (buf) *size = (size_t)n;
    return buf;
}

static plat_result load_path(swf_assets *out, const char *path)
{
    size_t size = 0;
    void *buf = read_path(path, &size, true);
    if (!buf) {
        plat_log("player: cannot read %s", path);
        return PLAT_ERR_NOT_FOUND;
    }
    plat_result r = swf_assets_parse(out, buf, size);
    if (r != PLAT_OK) {
        swf_aligned_free(buf);
        return r;
    }
    out->file = buf;
    out->file_size = size;
    return PLAT_OK;
}
#endif

static void load_config(const args *a, swf_config *cfg)
{
    swf_config_defaults(cfg);
    char *text = NULL;
    size_t size = 0;
#ifdef __PSP__
    (void)a;
    void *data;
    if (plat_file_read_all(PLAT_ROOT_DATA, "anyswf.toml", &data, &size) == PLAT_OK) text = data;
#else
    char path[1024];
    const char *p = a->config;
    if (!p && a->assets) {
        const char *slash = strrchr(a->assets, '/');
        const char *bslash = strrchr(a->assets, '\\');
        if (bslash && (!slash || bslash > slash)) slash = bslash;
        size_t dir = slash ? (size_t)(slash - a->assets + 1) : 0;
        if (dir + 12 < sizeof path) {
            memcpy(path, a->assets, dir);
            memcpy(path + dir, "anyswf.toml", 12);
            p = path;
        }
    }
    if (p) {
        text = read_path(p, &size, false);
    } else {
        void *data;
        if (plat_file_read_all(PLAT_ROOT_DATA, "anyswf.toml", &data, &size) == PLAT_OK) text = data;
    }
    if (!text && a->config) plat_log("player: cannot read %s; using defaults", a->config);
#endif
    if (text) {
        if (!swf_config_parse(cfg, text, size)) plat_log("player: anyswf.toml has errors; see above");
        free(text);
    }
}

static void current_input(const args *a, long frame, swf_input *in)
{
    plat_input pi;
    plat_input_get(&pi);
    memset(in, 0, sizeof *in);
    if (a->nsteps == 0) {
        in->held = pi.held;
        in->stick_x = pi.stick_x;
        in->stick_y = pi.stick_y;
        memcpy(in->keys, pi.keys, sizeof in->keys);
        in->has_pointer = pi.pointer_in;
        in->pointer_down = pi.pointer_down;
        in->pointer_x = pi.pointer_x;
        in->pointer_y = pi.pointer_y;
        return;
    }
    for (int i = 0; i < a->nsteps && a->steps[i].frame <= frame; i++) {
        in->held = a->steps[i].held;
        if (a->steps[i].has_cursor) {
            in->has_cursor = true;
            in->cursor_x = a->steps[i].x;
            in->cursor_y = a->steps[i].y;
        }
    }
}

#ifndef __PSP__
typedef struct wav_out {
    FILE *f;
    uint32_t frames;
} wav_out;

static void wav_header(FILE *f, uint32_t frames)
{
    uint32_t data = frames * 4, rate = PLAT_AUDIO_RATE, byte_rate = rate * 4;
    uint8_t h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0};
    memcpy(h + 4, (uint32_t[]){36 + data}, 4);
    memcpy(h + 24, &rate, 4);
    memcpy(h + 28, &byte_rate, 4);
    h[32] = 4;
    h[34] = 16;
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &data, 4);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, sizeof h, f);
}
#endif

int main(int argc, char **argv)
{
    args a;
    if (!parse_args(argc, argv, &a)) {
        fprintf(stderr, "usage: anyswf-player [assets.bin] [--headless] [--frames N] [--screenshot out.png]\n"
                        "       [--msaa N] [--touch-keys k1,k2,...] [--config anyswf.toml]\n"
                        "       [--input F:BUTTONS[@X/Y],...] [--save-dir DIR] [--wav out.wav]\n");
        return 2;
    }
    const plat_config pcfg = {.app_id = ANYSWF_APP_ID,
                              .title = ANYSWF_TITLE,
                              .window_scale = a.headless ? 1 : 2,
                              .vsync = !a.screenshot,
                              .headless = a.headless,
                              .msaa = a.msaa >= 0 ? a.msaa : (a.headless ? 0 : 4),
                              .touch_keys = a.touch_keys,
                              .touch_key_count = a.touch_key_count,
                              .user_dir = a.save_dir};
    plat_result r = plat_init(&pcfg);
    if (r != PLAT_OK) {
        fprintf(stderr, "player: plat_init failed (%d)\n", (int)r);
        return 1;
    }
    swf_config cfg;
    load_config(&a, &cfg);
    if (a.screenshot) cfg.cursor_visible = false;
    swf_assets assets;
#ifdef __PSP__
    r = swf_assets_load(&assets, PLAT_ROOT_DATA, a.assets ? a.assets : "assets.bin");
#else
    r = a.assets ? load_path(&assets, a.assets) : swf_assets_load(&assets, PLAT_ROOT_DATA, "assets.bin");
#endif
    if (r != PLAT_OK) {
        plat_shutdown();
        return 1;
    }
    int status = 0;
    swf_movie movie;
    if (plat_set_screen_size(assets.screen_w, assets.screen_h) != PLAT_OK || swf_render_init(&assets) != PLAT_OK ||
        swf_movie_init(&movie, &assets) != PLAT_OK) {
        swf_render_shutdown();
        swf_assets_free(&assets);
        plat_shutdown();
        return 1;
    }
    swf_vm *vm = swf_vm_new(&movie, &SWF_GAME_CODE);
    if (!vm) {
        swf_movie_free(&movie);
        swf_render_shutdown();
        swf_assets_free(&assets);
        plat_shutdown();
        return 1;
    }
    swf_vm_set_config(vm, &cfg);
    swf_mixer *mixer = assets.sound_count ? swf_mixer_new(&assets) : NULL;
    if (assets.sound_count && !mixer) plat_log("player: out of memory for the sound mixer; playing silently");
    swf_vm_set_mixer(vm, mixer);
    bool live_audio = false;
    if (mixer && !a.headless) live_audio = plat_audio_start(swf_mixer_render, mixer) == PLAT_OK;
#ifndef __PSP__
    wav_out wav = {0};
    if (a.wav) {
        wav.f = fopen(a.wav, "wb");
        if (!wav.f) plat_log("player: cannot write %s", a.wav);
        else wav_header(wav.f, 0);
    }
    static int16_t audio_buf[PLAT_AUDIO_RATE / 4 * 2];
    double audio_due = 0;
#endif
    swf_vm_run_init_actions(vm);
    uint32_t bg = assets.background;
    uint32_t clear = ((bg & 0xFF) << 16) | (bg & 0xFF00) | ((bg >> 16) & 0xFF);
    long shown = 0;
    uint64_t start = plat_time_us(), last = start;
    double period_ms = 1000.0 / assets.frame_rate;
    while (plat_frame_begin(clear)) {
        swf_input in;
        current_input(&a, shown + 1, &in);
        uint64_t now = plat_time_us();
        double dt = a.headless ? period_ms / 1000.0 : (now - last) / 1e6;
        last = now;
        swf_vm_set_time(vm, a.headless ? (uint64_t)(shown * period_ms) : (now - start) / 1000);
        swf_vm_input(vm, &in, dt);
        if (a.headless) {
            /* Headless runs are deterministic: one SWF frame per displayed frame. */
            swf_movie_advance(&movie);
        } else if (shown == 0) {
            swf_movie_advance(&movie);
        } else {
            swf_movie_update(&movie, dt, 4);
        }
        swf_vm_run_queue(vm);
        /* Timers run after the frame, on the clock at its end (as Ruffle's player does). */
        swf_vm_set_time(vm, a.headless ? (uint64_t)((shown + 1) * period_ms) : (plat_time_us() - start) / 1000);
        swf_vm_run_timers(vm);
        swf_vm_after_frame(vm);
        swf_vm_collect(vm);
        swf_movie_render(&movie);
        swf_vm_draw_overlay(vm);
        shown++;
#ifndef __PSP__
        if (mixer && !live_audio) {
            audio_due += PLAT_AUDIO_RATE / (double)assets.frame_rate;
            int n = (int)audio_due;
            audio_due -= n;
            while (n > 0) {
                int chunk = n < PLAT_AUDIO_RATE / 4 ? n : PLAT_AUDIO_RATE / 4;
                swf_mixer_render(audio_buf, chunk, mixer);
                if (wav.f) {
                    fwrite(audio_buf, 4, (size_t)chunk, wav.f);
                    wav.frames += (uint32_t)chunk;
                }
                n -= chunk;
            }
        }
        if (a.screenshot && shown >= a.frames) {
            int w = assets.screen_w, h = assets.screen_h;
            uint8_t *rgb = malloc((size_t)w * h * 3);
            if (!rgb || !swf_render_read_rgb(rgb, w, h) || !png_write_rgb(a.screenshot, rgb, w, h)) {
                plat_log("player: cannot write %s", a.screenshot);
                status = 1;
            }
            free(rgb);
            plat_frame_end();
            break;
        }
#endif
        plat_frame_end();
        if (a.frames > 0 && shown >= a.frames) break;
    }
    if (live_audio) plat_audio_stop();
#ifndef __PSP__
    if (wav.f) {
        wav_header(wav.f, wav.frames);
        if (fclose(wav.f) != 0) status = 1;
    }
#endif
    swf_vm_free(vm);
    swf_mixer_free(mixer);
    swf_movie_free(&movie);
    swf_render_shutdown();
    swf_assets_free(&assets);
    plat_shutdown();
    return status;
}
