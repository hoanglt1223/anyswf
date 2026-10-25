#include "plat_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

bool plat__app_id_is_valid(const char *app_id)
{
    if (!app_id) return false;
    size_t n = strlen(app_id);
    if (n == 0 || n > PLAT_APP_ID_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        char c = app_id[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

bool plat_path_is_valid(const char *rel)
{
    if (!rel) return false;
    size_t n = strlen(rel);
    if (n == 0 || n >= PLAT_PATH_MAX) return false;

    size_t seg_start = 0;
    for (size_t i = 0; i <= n; i++) {
        char c = rel[i];
        if (c == '\\' || c == ':' || (c > 0 && c < 0x20)) return false;
        if (c == '/' || c == '\0') {
            size_t len = i - seg_start;
            if (len == 0) return false;
            if (len == 1 && rel[seg_start] == '.') return false;
            if (len == 2 && rel[seg_start] == '.' && rel[seg_start + 1] == '.') return false;
            seg_start = i + 1;
        }
    }
    return true;
}

void plat__apply_deadzone(float *x, float *y)
{
    float mag = sqrtf(*x * *x + *y * *y);
    if (mag < PLAT_STICK_DEADZONE) {
        *x = 0.0f;
        *y = 0.0f;
        return;
    }
    float clamped = mag > 1.0f ? 1.0f : mag;
    float scale = (clamped - PLAT_STICK_DEADZONE) / (1.0f - PLAT_STICK_DEADZONE) / mag;
    *x *= scale;
    *y *= scale;
}

void plat__input_update(plat_input *state, uint32_t held, float stick_x, float stick_y)
{
    uint32_t prev = state->held;
    plat__apply_deadzone(&stick_x, &stick_y);
    state->held = held;
    state->pressed = held & ~prev;
    state->released = prev & ~held;
    state->stick_x = stick_x;
    state->stick_y = stick_y;
}

plat_result plat__full_path(plat_root root, const char *rel, char *buf, size_t buf_size)
{
    if (!plat_path_is_valid(rel) || !buf) return PLAT_ERR_INVALID;
    if (root != PLAT_ROOT_DATA && root != PLAT_ROOT_USER) return PLAT_ERR_INVALID;
    const char *dir = plat__root_dir(root);
    if (!dir) return PLAT_ERR_STATE;
    size_t dl = strlen(dir), rl = strlen(rel);
    if (dl + rl + 1 > buf_size) return PLAT_ERR_INVALID;
    memcpy(buf, dir, dl);
    memcpy(buf + dl, rel, rl + 1);
    return PLAT_OK;
}

plat_result plat_file_read_all(plat_root root, const char *rel, void **data, size_t *size)
{
    if (!data || !size) return PLAT_ERR_INVALID;
    *data = NULL;
    *size = 0;

    plat_file *f;
    plat_result r = plat_file_open(root, rel, PLAT_OPEN_READ, &f);
    if (r != PLAT_OK) return r;

    int64_t len = plat_file_size(f);
    if (len < 0 || (uint64_t)len >= (uint64_t)SIZE_MAX) {
        plat_file_close(f);
        return len < 0 ? (plat_result)len : PLAT_ERR_NO_MEMORY;
    }
    char *buf = malloc((size_t)len + 1);
    if (!buf) {
        plat_file_close(f);
        return PLAT_ERR_NO_MEMORY;
    }
    int64_t got = plat_file_read(f, buf, (size_t)len);
    plat_result cr = plat_file_close(f);
    if (got != len || cr != PLAT_OK) {
        free(buf);
        return got < 0 ? (plat_result)got : PLAT_ERR_IO;
    }
    buf[len] = '\0';
    *data = buf;
    *size = (size_t)len;
    return PLAT_OK;
}

plat_result plat_file_write_all(plat_root root, const char *rel, const void *data, size_t size)
{
    if (root != PLAT_ROOT_USER) return PLAT_ERR_INVALID;
    if (!data && size > 0) return PLAT_ERR_INVALID;
    if (!plat_path_is_valid(rel)) return PLAT_ERR_INVALID;

    char tmp_rel[PLAT_PATH_MAX];
    size_t rl = strlen(rel);
    if (rl + 4 >= sizeof tmp_rel) return PLAT_ERR_INVALID;
    memcpy(tmp_rel, rel, rl);
    memcpy(tmp_rel + rl, ".tmp", 5);

    char tmp_full[PLAT_PATH_MAX * 2], final_full[PLAT_PATH_MAX * 2];
    plat_result r = plat__full_path(root, tmp_rel, tmp_full, sizeof tmp_full);
    if (r == PLAT_OK) r = plat__full_path(root, rel, final_full, sizeof final_full);
    if (r != PLAT_OK) return r;

    plat_file *f;
    r = plat_file_open(root, tmp_rel, PLAT_OPEN_WRITE, &f);
    if (r != PLAT_OK) return r;
    int64_t put = size ? plat_file_write(f, data, size) : 0;
    plat_result cr = plat_file_close(f);
    if (put != (int64_t)size || cr != PLAT_OK) {
        plat__remove_full(tmp_full);
        return put < 0 ? (plat_result)put : (cr != PLAT_OK ? cr : PLAT_ERR_IO);
    }

    r = plat__replace_file(tmp_full, final_full);
    if (r != PLAT_OK) plat__remove_full(tmp_full);
    return r;
}

plat_result plat_file_remove(plat_root root, const char *rel)
{
    if (root != PLAT_ROOT_USER) return PLAT_ERR_INVALID;
    char full[PLAT_PATH_MAX * 2];
    plat_result r = plat__full_path(root, rel, full, sizeof full);
    if (r != PLAT_OK) return r;
    return plat__remove_full(full);
}

static const struct {
    const char *name;
    const char *label;
    uint8_t code;
} key_names[] = {
    {"backspace", "BKSP", 8}, {"tab", "TAB", 9},      {"enter", "ENTER", 13},  {"shift", "SHIFT", 16},
    {"control", "CTRL", 17},  {"alt", "ALT", 18},     {"escape", "ESC", 27},   {"space", "SPACE", 32},
    {"pageup", "PGUP", 33},   {"pagedown", "PGDN", 34}, {"end", "END", 35},    {"home", "HOME", 36},
    {"left", "LEFT", 37},     {"up", "UP", 38},       {"right", "RIGHT", 39},  {"down", "DOWN", 40},
    {"insert", "INS", 45},    {"delete", "DEL", 46},
};

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static bool name_eq(const char *a, const char *b)
{
    while (*a && *b && lower((unsigned char)*a) == lower((unsigned char)*b)) a++, b++;
    return *a == 0 && *b == 0;
}

int plat_key_from_name(const char *name)
{
    if (!name || !*name) return -1;
    int c = lower((unsigned char)name[0]);
    if (!name[1]) {
        if (c >= 'a' && c <= 'z') return c - 32;
        if (c >= '0' && c <= '9') return c;
    }
    if (c == 'f' && name[1] >= '1' && name[1] <= '9') {
        int n = name[1] - '0';
        if (name[2] >= '0' && name[2] <= '9' && !name[3]) n = n * 10 + (name[2] - '0');
        else if (name[2]) n = 0;
        if (n >= 1 && n <= 12) return 111 + n;
    }
    for (size_t i = 0; i < sizeof key_names / sizeof key_names[0]; i++)
        if (name_eq(name, key_names[i].name)) return key_names[i].code;
    if (name_eq(name, "ctrl")) return 17;
    if (name_eq(name, "esc")) return 27;
    if (name_eq(name, "return")) return 13;
    return -1;
}

const char *plat_key_label(int code)
{
    static const char alnum[36][2] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "A", "B",
                                      "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N",
                                      "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z"};
    static const char fkeys[12][4] = {"F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12"};
    if (code >= '0' && code <= '9') return alnum[code - '0'];
    if (code >= 'A' && code <= 'Z') return alnum[10 + code - 'A'];
    if (code >= 112 && code <= 123) return fkeys[code - 112];
    for (size_t i = 0; i < sizeof key_names / sizeof key_names[0]; i++)
        if (key_names[i].code == code) return key_names[i].label;
    return "?";
}

void plat__letterbox(int win_w, int win_h, int scr_w, int scr_h, int vp[4])
{
    if (win_w <= 0 || win_h <= 0 || scr_w <= 0 || scr_h <= 0) {
        vp[0] = vp[1] = vp[2] = vp[3] = 0;
        return;
    }
    /* Integer cross-multiplication: the exact fit stays exact (no off-by-one bars). */
    long long w = win_w, h = (long long)win_w * scr_h / scr_w;
    if (h > win_h) {
        h = win_h;
        w = (long long)win_h * scr_w / scr_h;
    }
    vp[2] = (int)w;
    vp[3] = (int)h;
    vp[0] = (win_w - vp[2]) / 2;
    vp[1] = (win_h - vp[3]) / 2;
}

void plat__touch_layout(const uint8_t *codes, int count, int win_w, int win_h, float rects[][4])
{
    float s = (float)(win_w < win_h ? win_w : win_h) * 0.14f;
    float margin = s * 0.35f, step = s * 1.15f;
    float cx = margin + 1.5f * s, cy = (float)win_h - margin - 1.5f * s;
    int other = 0;
    for (int i = 0; i < count; i++) {
        float x, y;
        switch (codes[i]) {
        case 37: x = cx - 1.5f * s; y = cy - 0.5f * s; break;
        case 38: x = cx - 0.5f * s; y = cy - 1.5f * s; break;
        case 39: x = cx + 0.5f * s; y = cy - 0.5f * s; break;
        case 40: x = cx - 0.5f * s; y = cy + 0.5f * s; break;
        default:
            x = (float)win_w - margin - (float)(other % 3 + 1) * step;
            y = (float)win_h - margin - (float)(other / 3 + 1) * step;
            other++;
            break;
        }
        rects[i][0] = x;
        rects[i][1] = y;
        rects[i][2] = s;
        rects[i][3] = s;
    }
}

int plat__touch_hit(const float rects[][4], int count, float x, float y)
{
    for (int i = 0; i < count; i++)
        if (x >= rects[i][0] && x < rects[i][0] + rects[i][2] && y >= rects[i][1] && y < rects[i][1] + rects[i][3])
            return i;
    return -1;
}
