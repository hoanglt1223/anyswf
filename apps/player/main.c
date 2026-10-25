/* Plays a converted movie (assets.bin, plus compiled scripts once linked).
 * PC: anyswf-player [assets.bin] [--headless] [--frames N] [--screenshot out.png]
 *                   [--msaa N] [--touch-keys k1,k2,...]
 *   Without a path it loads assets.bin from the executable's folder (the APK assets on
 *   Android, Contents/Resources in a macOS app bundle).
 *   --screenshot runs exactly N SWF frames (default 1), one per display frame, then
 *   writes the last one as PNG at the movie's screen size; it implies --headless.
 *   --msaa sets the multisample count (default 4 in a window, 0 headless).
 *   --touch-keys lists keys for on-screen touch buttons (names as plat_key_from_name).
 * PSP: loads assets.bin from the EBOOT folder and plays until exit.
 * Game builds (cmake/anyswf-game.cmake) define ANYSWF_APP_ID (save folder) and ANYSWF_TITLE. */
#include "platform.h"
#include "render/render.h"
#include "swfrt/avm1.h"
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

typedef struct args {
    const char *assets;
    const char *screenshot;
    long frames;
    bool headless;
    int msaa; /* -1 = default */
    uint8_t touch_keys[PLAT_TOUCH_KEYS_MAX];
    int touch_key_count;
} args;

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
        if (strcmp(argv[i], "--headless") == 0) {
            a->headless = true;
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            char *end;
            a->frames = strtol(argv[++i], &end, 10);
            if (*end || a->frames <= 0) return false;
        } else if (strcmp(argv[i], "--msaa") == 0 && i + 1 < argc) {
            char *end;
            long v = strtol(argv[++i], &end, 10);
            if (*end || v < 0 || v > 16) return false;
            a->msaa = (int)v;
        } else if (strcmp(argv[i], "--touch-keys") == 0 && i + 1 < argc) {
            if (!parse_touch_keys(argv[++i], a)) return false;
        } else if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
            a->screenshot = argv[++i];
            a->headless = true;
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
static plat_result load_path(swf_assets *out, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        plat_log("player: cannot open %s", path);
        return PLAT_ERR_NOT_FOUND;
    }
    plat_result r = PLAT_ERR_IO;
    void *buf = NULL;
    long size = -1;
    if (fseek(f, 0, SEEK_END) == 0) size = ftell(f);
    if (size > 0 && fseek(f, 0, SEEK_SET) == 0 && (buf = swf_aligned_alloc((size_t)size)) != NULL &&
        fread(buf, 1, (size_t)size, f) == (size_t)size) {
        r = swf_assets_parse(out, buf, (size_t)size);
        if (r == PLAT_OK) {
            out->file = buf;
            out->file_size = (size_t)size;
            buf = NULL;
        }
    }
    fclose(f);
    swf_aligned_free(buf);
    return r;
}
#endif

int main(int argc, char **argv)
{
    args a;
    if (!parse_args(argc, argv, &a)) {
        fprintf(stderr, "usage: anyswf-player [assets.bin] [--headless] [--frames N] [--screenshot out.png]\n"
                        "                     [--msaa N] [--touch-keys k1,k2,...]\n");
        return 2;
    }
    const plat_config cfg = {.app_id = ANYSWF_APP_ID,
                             .title = ANYSWF_TITLE,
                             .window_scale = a.headless ? 1 : 2,
                             .vsync = !a.screenshot,
                             .headless = a.headless,
                             .msaa = a.msaa >= 0 ? a.msaa : (a.headless ? 0 : 4),
                             .touch_keys = a.touch_keys,
                             .touch_key_count = a.touch_key_count};
    plat_result r = plat_init(&cfg);
    if (r != PLAT_OK) {
        fprintf(stderr, "player: plat_init failed (%d)\n", (int)r);
        return 1;
    }
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
    swf_vm_run_init_actions(vm);
    uint32_t bg = assets.background;
    uint32_t clear = ((bg & 0xFF) << 16) | (bg & 0xFF00) | ((bg >> 16) & 0xFF);
    long shown = 0;
    uint64_t last = plat_time_us();
    while (plat_frame_begin(clear)) {
        if (a.screenshot || a.headless) {
            /* Headless runs are deterministic: one SWF frame per displayed frame. */
            swf_movie_advance(&movie);
        } else {
            uint64_t now = plat_time_us();
            if (shown == 0) swf_movie_advance(&movie);
            else swf_movie_update(&movie, (now - last) / 1e6, 4);
            last = now;
        }
        swf_vm_run_queue(vm);
        swf_vm_collect(vm);
        swf_movie_render(&movie);
        shown++;
#ifndef __PSP__
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
    swf_movie_free(&movie);
    swf_vm_free(vm);
    swf_render_shutdown();
    swf_assets_free(&assets);
    plat_shutdown();
    return status;
}
