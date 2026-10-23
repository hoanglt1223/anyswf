/* Hello triangle + tone: proves video, input, audio, files and clock on each backend.
 * PC: --frames N exits after N frames; --headless hides the window and uses the
 * silent dummy audio driver; --smoke is headless, runs 120 frames and checks the
 * rendered pixels, audio progress and the save-file round trip (exit code 1 on failure). */
#include "hello_draw.h"
#include "platform.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __PSP__
#include <pspkernel.h>
PSP_MODULE_INFO("anyswf_hello", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-1024);

/* Allegrex has no LL/SC, so C11 atomics may lower to libatomic calls the SDK
 * lacks. Aligned word loads/stores are single-copy atomic on it, and each
 * shared value here has a single writer. */
typedef volatile uint32_t shared_u32;
#define SHARED_INIT(p, v) (*(p) = (v))
#define SHARED_LOAD(p) (*(p))
#define SHARED_STORE(p, v) (*(p) = (v))
#else
#include <stdatomic.h>
typedef atomic_uint shared_u32;
#define SHARED_INIT(p, v) atomic_init((p), (v))
#define SHARED_LOAD(p) atomic_load_explicit((p), memory_order_relaxed)
#define SHARED_STORE(p, v) atomic_store_explicit((p), (v), memory_order_relaxed)
#endif

#define APP_ID "ANYSWFHELLO"
#define CLEAR_RGB 0x202830u
#define TONE_HZ 440.0f
#define TONE_GAIN 0.2f
#define SMOKE_FRAMES 120
#define TWO_PI 6.28318530718f

typedef struct tone {
    float phase;
    uint32_t frames_local;
    shared_u32 muted;    /* written only by the main thread */
    shared_u32 frames;   /* written only by the audio thread */
} tone;

static void tone_fill(int16_t *out, int frames, void *user)
{
    tone *t = user;
    bool muted = SHARED_LOAD(&t->muted) != 0;
    const float step = TWO_PI * TONE_HZ / PLAT_AUDIO_RATE;
    for (int i = 0; i < frames; i++) {
        int16_t s = muted ? 0 : (int16_t)(sinf(t->phase) * TONE_GAIN * 32767.0f);
        out[2 * i] = s;
        out[2 * i + 1] = s;
        t->phase += step;
        if (t->phase >= TWO_PI) t->phase -= TWO_PI;
    }
    t->frames_local += (uint32_t)frames;
    SHARED_STORE(&t->frames, t->frames_local);
}

/* Increments a counter in the user data dir; exercises read_all/write_all. */
static bool bump_run_counter(void)
{
    long runs = 0;
    void *data;
    size_t size;
    plat_result r = plat_file_read_all(PLAT_ROOT_USER, "runs.txt", &data, &size);
    if (r == PLAT_OK) {
        runs = strtol(data, NULL, 10);
        free(data);
    } else if (r != PLAT_ERR_NOT_FOUND) {
        plat_log("hello: reading runs.txt failed (%d)", (int)r);
        return false;
    }
    char text[32];
    int len = snprintf(text, sizeof text, "%ld\n", runs + 1);
    r = plat_file_write_all(PLAT_ROOT_USER, "runs.txt", text, (size_t)len);
    if (r != PLAT_OK) {
        plat_log("hello: writing runs.txt failed (%d)", (int)r);
        return false;
    }
    void *back;
    r = plat_file_read_all(PLAT_ROOT_USER, "runs.txt", &back, &size);
    bool same = r == PLAT_OK && size == (size_t)len && memcmp(back, text, size) == 0;
    if (r == PLAT_OK) free(back);
    if (!same) {
        plat_log("hello: runs.txt read-back mismatch (%d)", (int)r);
        return false;
    }
    plat_log("hello: run #%ld", runs + 1);
    return true;
}

static bool near_rgb(const uint8_t got[3], uint32_t want, int tol)
{
    int w[3] = {(int)(want >> 16) & 0xFF, (int)(want >> 8) & 0xFF, (int)want & 0xFF};
    for (int i = 0; i < 3; i++)
        if (abs((int)got[i] - w[i]) > tol) return false;
    return true;
}

static bool check_frame(void)
{
    uint8_t corner[3], centre[3];
    if (!hello_read_pixel(2, 2, corner) || !hello_read_pixel(240, 162, centre)) {
        plat_log("smoke: pixel read-back unavailable");
        return false;
    }
    bool ok = true;
    if (!near_rgb(corner, CLEAR_RGB, 2)) {
        plat_log("smoke: corner is %u,%u,%u, expected clear colour", corner[0], corner[1], corner[2]);
        ok = false;
    }
    /* Centroid of a red/green/blue triangle interpolates to one third of each. */
    if (!near_rgb(centre, 0x555555u, 12)) {
        plat_log("smoke: centroid is %u,%u,%u, expected ~85,85,85", centre[0], centre[1], centre[2]);
        ok = false;
    }
    return ok;
}

static bool parse_args(int argc, char **argv, long *max_frames, bool *smoke, bool *headless)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--smoke") == 0) {
            *smoke = true;
            *headless = true;
            *max_frames = SMOKE_FRAMES;
        } else if (strcmp(argv[i], "--headless") == 0) {
            *headless = true;
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            char *end;
            *max_frames = strtol(argv[++i], &end, 10);
            if (*end != '\0' || *max_frames <= 0) return false;
        } else {
            return false;
        }
    }
    return true;
}

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

int main(int argc, char **argv)
{
    long max_frames = 0;
    bool smoke = false, headless = false;
    if (!parse_args(argc, argv, &max_frames, &smoke, &headless)) {
        fprintf(stderr, "usage: hello [--headless] [--frames N | --smoke]\n");
        return 2;
    }

    const plat_config cfg = {.app_id = APP_ID, .title = "AnySWF hello", .window_scale = 2,
                             .vsync = true, .headless = headless};
    plat_result r = plat_init(&cfg);
    if (r != PLAT_OK) {
        fprintf(stderr, "hello: plat_init failed (%d)\n", (int)r);
        return 1;
    }

    bool ok = bump_run_counter();

    static tone t;
    SHARED_INIT(&t.muted, 0u);
    SHARED_INIT(&t.frames, 0u);
    r = plat_audio_start(tone_fill, &t);
    if (r != PLAT_OK) {
        plat_log("hello: audio start failed (%d)", (int)r);
        ok = false;
    }

    float ox = 0.0f, oy = 0.0f;
    long frame = 0;
    uint64_t start = plat_time_us(), last_report = start;
    while (plat_frame_begin(CLEAR_RGB)) {
        plat_input in;
        plat_input_get(&in);
        if (in.pressed & PLAT_BTN_CROSS)
            SHARED_STORE(&t.muted, SHARED_LOAD(&t.muted) ? 0u : 1u);
        float dx = in.stick_x + ((in.held & PLAT_BTN_RIGHT) ? 1.0f : 0.0f) - ((in.held & PLAT_BTN_LEFT) ? 1.0f : 0.0f);
        float dy = in.stick_y + ((in.held & PLAT_BTN_DOWN) ? 1.0f : 0.0f) - ((in.held & PLAT_BTN_UP) ? 1.0f : 0.0f);
        ox = clampf(ox + dx * 3.0f, -150.0f, 150.0f);
        oy = clampf(oy + dy * 3.0f, -56.0f, 56.0f);

        const hello_vertex tri[3] = {
            {240.0f + ox, 56.0f + oy, 255, 0, 0},
            {330.0f + ox, 216.0f + oy, 0, 255, 0},
            {150.0f + ox, 216.0f + oy, 0, 0, 255},
        };
        hello_draw_triangle(tri);
        if (smoke && frame == max_frames - 1 && !check_frame()) ok = false;
        plat_frame_end();
        frame++;

        uint64_t now = plat_time_us();
        if (now - last_report >= 2000000u) {
            plat_log("hello: %.1f fps", frame * 1e6 / (double)(now - start));
            last_report = now;
        }
        if (max_frames > 0 && frame >= max_frames) break;
    }

    double elapsed = (plat_time_us() - start) / 1e6;
    unsigned audio_frames = SHARED_LOAD(&t.frames);
    plat_audio_stop();
    plat_shutdown();

    plat_log("hello: %ld frames in %.2f s (%.1f fps), %u audio frames", frame, elapsed,
             elapsed > 0 ? frame / elapsed : 0.0, audio_frames);
    if (!smoke) return ok ? 0 : 1;

    if (frame < max_frames) {
        plat_log("smoke: quit after %ld of %ld frames", frame, max_frames);
        ok = false;
    }
    /* Device start-up latency eats some of the run; a quarter of real time proves the stream flows. */
    if (audio_frames == 0 || audio_frames < elapsed * PLAT_AUDIO_RATE / 4) {
        plat_log("smoke: audio produced %u frames in %.2f s", audio_frames, elapsed);
        ok = false;
    }
    plat_log("smoke: %s", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
