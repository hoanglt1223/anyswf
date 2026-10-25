/* Unit tests for run-time pieces that need no compiled movie: anyswf.toml parsing, the
 * sound mixer and the drawing API tessellator. */
#include "display/drawing.h"
#include "swfrt/config.h"
#include "swfrt/mixer.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
            failures++;                                                           \
        }                                                                         \
    } while (0)

static void test_config(void)
{
    swf_config c;
    swf_config_defaults(&c);
    CHECK(c.keys[0] == 38 && c.keys[4] == 255 && c.keys[5] == 32 && c.cursor_visible);
    const char *text = "# build settings\n"
                       "id = \"MYGAME\"\n"
                       "title = \"My game\"\n"
                       "[android]\n"
                       "touch_keys = [\"left\", \"right\"]\n"
                       "[stage]\n"
                       "scale = \"exactFit\"   # comment\n"
                       "[cursor]\n"
                       "speed = 400\n"
                       "visible = false\n"
                       "[keys]\n"
                       "cross = \"space\"\n"
                       "square = \"a\"\n"
                       "l = 65\n"
                       "start = \"none\"\n";
    CHECK(swf_config_parse(&c, text, strlen(text)));
    CHECK(c.scale_mode == SWF_SCALE_EXACT_FIT && c.cursor_speed == 400.0f && !c.cursor_visible);
    CHECK(c.keys[4] == 32 && c.keys[6] == 'A' && c.keys[8] == 65 && c.keys[10] == 0);
    swf_config d;
    swf_config_defaults(&d);
    const char *bad = "[keys]\ncross = \"nokey\"\n[cursor]\nspeed = fast\n[nosuch]\nx = 1\n[stage]\nscale = \"showAll\"\n";
    CHECK(!swf_config_parse(&d, bad, strlen(bad)));
    CHECK(d.keys[4] == 255 && d.cursor_speed == 240.0f && d.scale_mode == SWF_SCALE_SHOW_ALL);
}

static void test_mixer(void)
{
    enum { FRAMES = 2205 };
    static int16_t pcm[FRAMES];
    for (int i = 0; i < FRAMES; i++) pcm[i] = (int16_t)(10000 * sin(i * 0.12));
    swf_sound snd = {SWF_SOUND_PCM16, 1, 22050, FRAMES, (const uint8_t *)pcm, sizeof pcm};
    swf_assets a;
    memset(&a, 0, sizeof a);
    a.sounds = &snd;
    a.sound_count = 1;
    swf_mixer *m = swf_mixer_new(&a);
    CHECK(m != NULL);
    if (!m) return;
    CHECK(swf_mixer_play(m, 1, 0, 0, 1, 1, 1) == 0); /* no such sound */
    uint32_t v = swf_mixer_play(m, 0, 0, 0, 1, 1.0f, 0.0f);
    CHECK(v != 0 && swf_mixer_active(m, v));
    static int16_t out[4410 * 2];
    swf_mixer_render(out, 2205, m);
    int peak_l = 0, peak_r = 0;
    for (int i = 0; i < 2205; i++) {
        if (abs(out[i * 2]) > peak_l) peak_l = abs(out[i * 2]);
        if (abs(out[i * 2 + 1]) > peak_r) peak_r = abs(out[i * 2 + 1]);
    }
    CHECK(peak_l > 8000 && peak_r == 0);
    CHECK(swf_mixer_position(m, v) > 1000);
    uint32_t ids[4];
    CHECK(swf_mixer_poll_finished(m, ids, 4) == 0);
    swf_mixer_render(out, 4410, m); /* 0.1 s at 22 kHz is 4410 output frames */
    CHECK(swf_mixer_poll_finished(m, ids, 4) == 1 && ids[0] == v && !swf_mixer_active(m, v));

    /* Two plays last twice as long; a stopped voice never reports finishing. */
    uint32_t w = swf_mixer_play(m, 0, 0, 0, 2, 1, 1);
    swf_mixer_render(out, 4410, m);
    CHECK(swf_mixer_poll_finished(m, ids, 4) == 0);
    swf_mixer_render(out, 4410, m);
    swf_mixer_render(out, 100, m);
    CHECK(swf_mixer_poll_finished(m, ids, 4) == 1 && ids[0] == w);
    uint32_t s = swf_mixer_play(m, 0, 0, 0, 1, 1, 1);
    swf_mixer_stop(m, s);
    swf_mixer_render(out, 4410, m);
    CHECK(swf_mixer_poll_finished(m, ids, 4) == 0 && !swf_mixer_active(m, s));
    bool silent = true;
    for (int i = 0; i < 4410 * 2; i++) silent = silent && out[i] == 0;
    CHECK(silent);
    /* Every voice can be busy at once; the next play is refused. */
    for (int i = 0; i < SWF_MIXER_VOICES; i++) CHECK(swf_mixer_play(m, 0, 0, 0, 1, 1, 1) != 0);
    CHECK(swf_mixer_play(m, 0, 0, 0, 1, 1, 1) == 0);
    swf_mixer_stop_all(m);
    swf_mixer_free(m);
}

static double mesh_area(swf_drawing *d)
{
    const swf_mesh_vertex *v;
    const uint16_t *ix;
    uint32_t n;
    swf_drawing_mesh(d, &v, &ix, &n);
    double area = 0;
    for (uint32_t i = 0; i + 2 < n; i += 3) {
        const swf_mesh_vertex *p = &v[ix[i]], *q = &v[ix[i + 1]], *r = &v[ix[i + 2]];
        area += fabs((q->x - p->x) * (r->y - p->y) - (r->x - p->x) * (q->y - p->y)) / 2;
    }
    return area;
}

static void rect_path(swf_drawing *d, float x0, float y0, float x1, float y1)
{
    swf_drawing_move_to(d, x0, y0);
    swf_drawing_line_to(d, x1, y0);
    swf_drawing_line_to(d, x1, y1);
    swf_drawing_line_to(d, x0, y1);
    swf_drawing_line_to(d, x0, y0);
}

static void test_drawing(void)
{
    swf_drawing *d = swf_drawing_new();
    swf_fill_style red = {0};
    red.rgba = 0xFF0000FFu;
    CHECK(swf_drawing_begin_fill(d, &red));
    rect_path(d, 0, 0, 2000, 2000);
    rect_path(d, 500, 500, 1500, 1500); /* a hole under the even-odd rule */
    CHECK(swf_drawing_begin_fill(d, NULL));
    CHECK(fabs(mesh_area(d) - (2000.0 * 2000 - 1000.0 * 1000)) < 1.0);
    CHECK(swf_drawing_hit(d, 100, 100) && !swf_drawing_hit(d, 1000, 1000) && !swf_drawing_hit(d, 2500, 100));
    float b[4];
    CHECK(swf_drawing_bounds(d, &SWF_MATRIX_IDENTITY, b) && b[0] == 0 && b[2] == 2000);

    /* A self-intersecting bow tie: two triangles, each 1/4 of the square. */
    swf_drawing_clear(d);
    swf_drawing_begin_fill(d, &red);
    swf_drawing_move_to(d, 0, 0);
    swf_drawing_line_to(d, 1000, 1000);
    swf_drawing_line_to(d, 1000, 0);
    swf_drawing_line_to(d, 0, 1000);
    swf_drawing_begin_fill(d, NULL);
    CHECK(fabs(mesh_area(d) - 500000.0) < 1.0);

    /* An unfinished fill is closed automatically; a line adds stroke triangles and
     * widens the bounds by half its width. */
    swf_drawing_clear(d);
    swf_drawing_line_style(d, true, 200, 0xFF000000u);
    swf_drawing_begin_fill(d, &red);
    swf_drawing_line_to(d, 1000, 0);
    swf_drawing_line_to(d, 1000, 1000);
    CHECK(mesh_area(d) > 500000.0);
    CHECK(swf_drawing_bounds(d, &SWF_MATRIX_IDENTITY, b) && b[0] == -100 && b[3] == 1100);
    swf_drawing *c = swf_drawing_clone(d);
    CHECK(c && fabs(mesh_area(c) - mesh_area(d)) < 1.0);
    swf_drawing_free(c);
    swf_drawing_free(d);
}

int main(void)
{
    test_config();
    test_mixer();
    test_drawing();
    if (failures) {
        fprintf(stderr, "test_builtins: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_builtins: all passed\n");
    return 0;
}
