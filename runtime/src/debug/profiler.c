/* Frame profiler (see swfrt/profiler.h). Stage times are averaged over one-second
 * windows; the overlay shows the last window, so it is readable at 60 fps. */
#include "swfrt/profiler.h"

#include "../render/render.h"

#include <stdio.h>
#include <string.h>

#if defined(__PSP__)
#include <malloc.h>
#endif

#define HEAP_BUDGET (40u * 1024 * 1024) /* PSP-3000 user memory left for a game */

swf_profiler swf_prof;

static uint64_t g_start[SWF_PROF_STAGES];
static uint64_t g_sum[SWF_PROF_STAGES];
static uint64_t g_window_start, g_frame_start, g_frame_max;
static uint32_t g_frames, g_draws, g_tris, g_draws_max, g_tris_max;
static bool g_warned_heap;

/* Last finished window, for the overlay. */
static struct {
    double fps, stage_ms[SWF_PROF_STAGES], frame_max_ms;
    uint32_t draws, tris;
} g_shown;

static const char *const STAGE_NAMES[SWF_PROF_STAGES] = {"script", "timeline", "render", "present"};

void swf_prof_begin(int stage)
{
    uint64_t now = plat_time_us();
    if (stage == 0 && g_frame_start == 0) g_frame_start = now;
    g_start[stage] = now;
}

void swf_prof_end(int stage)
{
    g_sum[stage] += plat_time_us() - g_start[stage];
}

size_t swf_prof_heap_used(void)
{
#if defined(__PSP__)
    struct mallinfo mi = mallinfo();
    return (size_t)mi.uordblks;
#else
    return 0;
#endif
}

void swf_prof_frame_done(void)
{
    uint64_t now = plat_time_us();
    if (g_frame_start && now - g_frame_start > g_frame_max) g_frame_max = now - g_frame_start;
    g_frame_start = 0;
    g_frames++;
    g_draws += swf_prof.draws;
    g_tris += swf_prof.tris;
    if (swf_prof.draws > g_draws_max) g_draws_max = swf_prof.draws;
    if (swf_prof.tris > g_tris_max) g_tris_max = swf_prof.tris;
    swf_prof.draws = swf_prof.tris = 0;
    if (g_window_start == 0) g_window_start = now;
    uint64_t span = now - g_window_start;
    if (span < 1000000) return;

    g_shown.fps = g_frames * 1e6 / (double)span;
    for (int i = 0; i < SWF_PROF_STAGES; i++) g_shown.stage_ms[i] = g_sum[i] / 1000.0 / g_frames;
    g_shown.frame_max_ms = g_frame_max / 1000.0;
    g_shown.draws = g_draws / g_frames;
    g_shown.tris = g_tris / g_frames;
    size_t heap = swf_prof_heap_used();
    if (swf_prof.log) {
        plat_log("prof: %.1f fps, script %.2f, timeline %.2f, render %.2f, present %.2f ms, worst frame %.1f ms; "
                 "%u draws (max %u), %u tris (max %u); heap %u KB, script heap %u KB, textures %u/%u KB "
                 "(%u uploads, %u evictions, %u from RAM)",
                 g_shown.fps, g_shown.stage_ms[0], g_shown.stage_ms[1], g_shown.stage_ms[2], g_shown.stage_ms[3],
                 g_shown.frame_max_ms, (unsigned)g_shown.draws, (unsigned)g_draws_max, (unsigned)g_shown.tris,
                 (unsigned)g_tris_max, (unsigned)(heap / 1024), (unsigned)(swf_prof.script_bytes / 1024),
                 (unsigned)(swf_prof.texture_used / 1024), (unsigned)(swf_prof.texture_capacity / 1024),
                 (unsigned)swf_prof.texture_uploads, (unsigned)swf_prof.texture_evictions,
                 (unsigned)swf_prof.texture_misses);
    }
    if (heap > HEAP_BUDGET && !g_warned_heap) {
        g_warned_heap = true;
        plat_log("prof: heap use %u KB is over the %u KB budget for a PSP-3000 game", (unsigned)(heap / 1024),
                 (unsigned)(HEAP_BUDGET / 1024));
    }
    memset(g_sum, 0, sizeof g_sum);
    g_window_start = now;
    g_frames = g_draws = g_tris = g_draws_max = g_tris_max = 0;
    g_frame_max = 0;
}

static float text(const swf_assets *a, float x, float y, const char *s)
{
    const swf_device_font *f = &a->device_font;
    for (; *s; s++) {
        unsigned ch = (unsigned char)*s;
        if (ch < f->first || ch >= (unsigned)f->first + f->count) ch = '?';
        uint32_t i = ch - f->first;
        if (ch != ' ') {
            float src[4] = {(float)(i % f->cols * f->cell), (float)(i / f->cols * f->cell), (float)f->cell,
                            (float)f->cell};
            float dst[4] = {x, y, (float)f->cell, (float)f->cell};
            swf_render_glyph_quad(a, &SWF_MATRIX_IDENTITY, src, dst, 0xFFFFFFFFu, &SWF_CXFORM_IDENTITY);
        }
        x += f->advances[i];
    }
    return x;
}

void swf_prof_draw(const swf_assets *a)
{
    if (!swf_prof.overlay || !a->has_device_font) return;
    char lines[4][96];
    snprintf(lines[0], sizeof lines[0], "%.1f FPS  worst %.1f ms", g_shown.fps, g_shown.frame_max_ms);
    int n = snprintf(lines[1], sizeof lines[1], "ms");
    for (int i = 0; i < SWF_PROF_STAGES && n > 0 && (size_t)n < sizeof lines[1]; i++)
        n += snprintf(lines[1] + n, sizeof lines[1] - (size_t)n, " %s %.1f", STAGE_NAMES[i], g_shown.stage_ms[i]);
    snprintf(lines[2], sizeof lines[2], "%u draws  %u tris", (unsigned)g_shown.draws, (unsigned)g_shown.tris);
    snprintf(lines[3], sizeof lines[3], "heap %uK  script %uK  tex %u/%uK", (unsigned)(swf_prof_heap_used() / 1024),
             (unsigned)(swf_prof.script_bytes / 1024), (unsigned)(swf_prof.texture_used / 1024),
             (unsigned)(swf_prof.texture_capacity / 1024));
    float h = (float)a->device_font.cell + 2;
    int32_t bg[4] = {0, 0, 300, (int32_t)(4 * h + 4)};
    swf_render_rect(&SWF_MATRIX_IDENTITY, bg, 0xB0000000u, &SWF_CXFORM_IDENTITY);
    for (int i = 0; i < 4; i++) text(a, 3, 2 + i * h, lines[i]);
}
