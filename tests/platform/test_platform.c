/* Platform layer unit tests (host backends). Uses internal hooks so no window is needed. */
#include "plat_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                   \
        }                                                                 \
    } while (0)

static void test_paths(void)
{
    CHECK(plat_path_is_valid("save.dat"));
    CHECK(plat_path_is_valid("game-1/so/high_scores.sol"));
    CHECK(plat_path_is_valid(".hidden"));
    CHECK(!plat_path_is_valid(NULL));
    CHECK(!plat_path_is_valid(""));
    CHECK(!plat_path_is_valid("/abs"));
    CHECK(!plat_path_is_valid("dir/"));
    CHECK(!plat_path_is_valid("a//b"));
    CHECK(!plat_path_is_valid("../escape"));
    CHECK(!plat_path_is_valid("a/../b"));
    CHECK(!plat_path_is_valid("./a"));
    CHECK(!plat_path_is_valid("a\\b"));
    CHECK(!plat_path_is_valid("ms0:/x"));
    CHECK(!plat_path_is_valid("tab\there"));

    char long_path[PLAT_PATH_MAX + 1];
    memset(long_path, 'a', PLAT_PATH_MAX);
    long_path[PLAT_PATH_MAX] = '\0';
    CHECK(!plat_path_is_valid(long_path));
    long_path[PLAT_PATH_MAX - 1] = '\0';
    CHECK(plat_path_is_valid(long_path));
}

static void test_app_id(void)
{
    CHECK(plat__app_id_is_valid("ANYSWF"));
    CHECK(plat__app_id_is_valid("my_game-2"));
    CHECK(!plat__app_id_is_valid(NULL));
    CHECK(!plat__app_id_is_valid(""));
    CHECK(!plat__app_id_is_valid("a/b"));
    CHECK(!plat__app_id_is_valid("dot.ted"));
    CHECK(!plat__app_id_is_valid("0123456789012345678901234567890123"));
}

static int near(float a, float b)
{
    return fabsf(a - b) < 1e-4f;
}

static void test_deadzone_and_edges(void)
{
    float x = 0.1f, y = -0.1f;
    plat__apply_deadzone(&x, &y);
    CHECK(x == 0.0f && y == 0.0f);

    x = 1.0f, y = 0.0f;
    plat__apply_deadzone(&x, &y);
    CHECK(near(x, 1.0f) && y == 0.0f);

    x = 0.0f, y = -0.6f;
    plat__apply_deadzone(&x, &y);
    CHECK(x == 0.0f && near(y, -0.5f));

    x = 1.0f, y = 1.0f;   /* square corner of a raw stick clamps to the unit circle */
    plat__apply_deadzone(&x, &y);
    CHECK(near(sqrtf(x * x + y * y), 1.0f));

    plat_input in = {0};
    plat__input_update(&in, PLAT_BTN_CROSS | PLAT_BTN_UP, 0.0f, 0.0f);
    CHECK(in.held == (PLAT_BTN_CROSS | PLAT_BTN_UP));
    CHECK(in.pressed == (PLAT_BTN_CROSS | PLAT_BTN_UP));
    CHECK(in.released == 0);
    plat__input_update(&in, PLAT_BTN_UP | PLAT_BTN_START, 0.0f, 0.0f);
    CHECK(in.pressed == PLAT_BTN_START);
    CHECK(in.released == PLAT_BTN_CROSS);
    plat__input_update(&in, PLAT_BTN_UP | PLAT_BTN_START, 0.0f, 0.0f);
    CHECK(in.pressed == 0 && in.released == 0);
}

static void test_files(void)
{
    CHECK(plat__files_init("bad/id") == PLAT_ERR_INVALID);
    plat_result r = plat__files_init("anyswf-test");
    CHECK(r == PLAT_OK);
    if (r != PLAT_OK) return;

    void *data = NULL;
    size_t size = 0;
    plat_file_remove(PLAT_ROOT_USER, "roundtrip.bin");
    CHECK(plat_file_read_all(PLAT_ROOT_USER, "roundtrip.bin", &data, &size) == PLAT_ERR_NOT_FOUND);
    CHECK(data == NULL && size == 0);

    const char first[] = "first version, longer than the second";
    const char second[] = "second";
    CHECK(plat_file_write_all(PLAT_ROOT_USER, "roundtrip.bin", first, sizeof first - 1) == PLAT_OK);
    CHECK(plat_file_write_all(PLAT_ROOT_USER, "roundtrip.bin", second, sizeof second - 1) == PLAT_OK);
    CHECK(plat_file_read_all(PLAT_ROOT_USER, "roundtrip.bin", &data, &size) == PLAT_OK);
    CHECK(size == sizeof second - 1 && data && memcmp(data, second, size) == 0);
    CHECK(data && ((char *)data)[size] == '\0');
    free(data);

    CHECK(plat_file_read_all(PLAT_ROOT_USER, "roundtrip.bin.tmp", &data, &size) == PLAT_ERR_NOT_FOUND);

    plat_file *f = NULL;
    CHECK(plat_file_open(PLAT_ROOT_USER, "roundtrip.bin", PLAT_OPEN_READ, &f) == PLAT_OK);
    if (f) {
        char buf[4];
        CHECK(plat_file_size(f) == 6);
        CHECK(plat_file_seek(f, 3) == PLAT_OK);
        CHECK(plat_file_read(f, buf, sizeof buf) == 3);
        CHECK(memcmp(buf, "ond", 3) == 0);
        CHECK(plat_file_seek(f, -1) == PLAT_ERR_INVALID);
        CHECK(plat_file_close(f) == PLAT_OK);
    }

    CHECK(plat_mkdir(PLAT_ROOT_USER, "sub") == PLAT_OK);
    CHECK(plat_mkdir(PLAT_ROOT_USER, "sub") == PLAT_OK);
    CHECK(plat_file_write_all(PLAT_ROOT_USER, "sub/a.txt", "x", 1) == PLAT_OK);
    CHECK(plat_file_write_all(PLAT_ROOT_USER, "empty.txt", NULL, 0) == PLAT_OK);
    CHECK(plat_file_read_all(PLAT_ROOT_USER, "empty.txt", &data, &size) == PLAT_OK && size == 0);
    free(data);

    CHECK(plat_file_write_all(PLAT_ROOT_DATA, "nope.txt", "x", 1) == PLAT_ERR_INVALID);
    CHECK(plat_file_open(PLAT_ROOT_DATA, "nope.txt", PLAT_OPEN_WRITE, &f) == PLAT_ERR_INVALID);
    CHECK(plat_file_write_all(PLAT_ROOT_USER, "../escape.txt", "x", 1) == PLAT_ERR_INVALID);
    CHECK(plat_mkdir(PLAT_ROOT_DATA, "sub") == PLAT_ERR_INVALID);

    CHECK(plat_file_remove(PLAT_ROOT_USER, "sub/a.txt") == PLAT_OK);
    CHECK(plat_file_remove(PLAT_ROOT_USER, "sub/a.txt") == PLAT_ERR_NOT_FOUND);
    CHECK(plat_file_remove(PLAT_ROOT_USER, "empty.txt") == PLAT_OK);
    CHECK(plat_file_remove(PLAT_ROOT_USER, "roundtrip.bin") == PLAT_OK);

    plat__files_shutdown();
    CHECK(plat_file_read_all(PLAT_ROOT_USER, "roundtrip.bin", &data, &size) == PLAT_ERR_STATE);
}

static void test_key_names(void)
{
    CHECK(plat_key_from_name("a") == 65);
    CHECK(plat_key_from_name("Z") == 90);
    CHECK(plat_key_from_name("7") == '7');
    CHECK(plat_key_from_name("LEFT") == 37);
    CHECK(plat_key_from_name("down") == 40);
    CHECK(plat_key_from_name("Space") == 32);
    CHECK(plat_key_from_name("enter") == 13 && plat_key_from_name("return") == 13);
    CHECK(plat_key_from_name("ctrl") == 17 && plat_key_from_name("control") == 17);
    CHECK(plat_key_from_name("f1") == 112 && plat_key_from_name("F12") == 123);
    CHECK(plat_key_from_name("f13") == -1 && plat_key_from_name("f1x") == -1);
    CHECK(plat_key_from_name("") == -1 && plat_key_from_name(NULL) == -1);
    CHECK(plat_key_from_name("lefty") == -1);
    CHECK(strcmp(plat_key_label(37), "LEFT") == 0);
    CHECK(strcmp(plat_key_label('Q'), "Q") == 0);
    CHECK(strcmp(plat_key_label(123), "F12") == 0);
    CHECK(strcmp(plat_key_label(200), "?") == 0);

    plat_input in;
    memset(&in, 0, sizeof in);
    plat__key_set(in.keys, 32);
    plat__key_set(in.keys, 255);
    plat__key_set(in.keys, 256);
    CHECK(plat_key_held(&in, 32) && plat_key_held(&in, 255));
    CHECK(!plat_key_held(&in, 33) && !plat_key_held(&in, 256) && !plat_key_held(&in, -1));
}

static void test_letterbox(void)
{
    int vp[4];
    plat__letterbox(960, 544, 480, 272, vp);
    CHECK(vp[0] == 0 && vp[1] == 0 && vp[2] == 960 && vp[3] == 544);
    plat__letterbox(1920, 544, 480, 272, vp); /* wide window: bars left and right */
    CHECK(vp[0] == 480 && vp[1] == 0 && vp[2] == 960 && vp[3] == 544);
    plat__letterbox(550, 1000, 550, 400, vp); /* tall window: bars above and below */
    CHECK(vp[0] == 0 && vp[1] == 300 && vp[2] == 550 && vp[3] == 400);
    plat__letterbox(0, 100, 480, 272, vp);
    CHECK(vp[2] == 0 && vp[3] == 0);
}

static void test_touch_layout(void)
{
    const uint8_t codes[] = {37, 38, 39, 40, 32, 'Z'};
    float r[6][4];
    plat__touch_layout(codes, 6, 1000, 500, r);
    float s = 500 * 0.14f;
    for (int i = 0; i < 6; i++) {
        CHECK(near(r[i][2], s) && near(r[i][3], s));
        CHECK(r[i][0] >= 0 && r[i][1] >= 0 && r[i][0] + r[i][2] <= 1000 && r[i][1] + r[i][3] <= 500);
    }
    CHECK(r[0][0] < r[1][0] && r[1][0] < r[2][0]); /* left, up, right across the d-pad */
    CHECK(r[1][1] < r[0][1] && r[0][1] < r[3][1]); /* up above left above down */
    CHECK(r[4][0] > 500 && r[5][0] < r[4][0]);     /* other keys bottom right, filling leftwards */
    /* No two buttons overlap. */
    for (int i = 0; i < 6; i++)
        for (int j = i + 1; j < 6; j++)
            CHECK(r[i][0] + r[i][2] <= r[j][0] || r[j][0] + r[j][2] <= r[i][0] ||
                  r[i][1] + r[i][3] <= r[j][1] || r[j][1] + r[j][3] <= r[i][1]);
    CHECK(plat__touch_hit((const float(*)[4])r, 6, r[4][0] + 1, r[4][1] + 1) == 4);
    CHECK(plat__touch_hit((const float(*)[4])r, 6, 500, 10) == -1);
}

static void test_clock(void)
{
    uint64_t a = plat_time_us();
    uint64_t b = plat_time_us();
    CHECK(b >= a);
}

int main(void)
{
    test_paths();
    test_app_id();
    test_deadzone_and_edges();
    test_files();
    test_key_names();
    test_letterbox();
    test_touch_layout();
    test_clock();
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("platform tests passed\n");
    return 0;
}
