/* anyswf.toml: a small TOML subset (sections, `key = value` with strings, numbers and
 * booleans, `#` comments) holding the per-game settings in swfrt/config.h. The same file
 * is the `anyswf build --config` file: its top-level keys and [android] table are build
 * settings (swf2c/src/anyswf/config.rs) and are skipped here. */
#include "swfrt/config.h"

#include "platform.h"
#include "swfrt/movie.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define KEY_MOUSE 255

static const char *const BUTTONS[SWF_CONFIG_BUTTONS] = {"up", "down", "left", "right", "cross", "circle",
                                                        "square", "triangle", "l", "r", "start", "select"};

void swf_config_defaults(swf_config *c)
{
    memset(c, 0, sizeof *c);
    static const uint8_t KEYS[SWF_CONFIG_BUTTONS] = {
        38, 40, 37, 39,      /* D-pad: arrow keys */
        KEY_MOUSE, 32,       /* Cross: mouse button, Circle: space */
        'Z', 'X',            /* Square, Triangle */
        16, 17,              /* L: shift, R: control */
        13, 27               /* Start: enter, Select: escape */
    };
    memcpy(c->keys, KEYS, sizeof KEYS);
    c->scale_mode = SWF_SCALE_SHOW_ALL;
    c->cursor_speed = 240.0f;
    c->cursor_accel = 1.5f;
    c->cursor_visible = true;
}

typedef struct value {
    enum { V_STRING, V_NUMBER, V_BOOL } kind;
    char str[64];
    double num;
    bool b;
} value;

static bool streq_ci(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    return *a == *b;
}

static int key_code(const value *v)
{
    if (v->kind == V_NUMBER) return v->num >= 1 && v->num <= 254 && v->num == (int)v->num ? (int)v->num : -1;
    if (v->kind != V_STRING) return -1;
    static const struct { const char *name; int code; } NAMES[] = {
        {"none", 0}, {"mouse", KEY_MOUSE}, {"up", 38}, {"down", 40}, {"left", 37}, {"right", 39},
        {"space", 32}, {"enter", 13}, {"escape", 27}, {"shift", 16}, {"control", 17}, {"ctrl", 17},
        {"alt", 18}, {"tab", 9}, {"backspace", 8}, {"delete", 46}, {"insert", 45}, {"home", 36},
        {"end", 35}, {"pgup", 33}, {"pgdn", 34}};
    for (size_t i = 0; i < sizeof NAMES / sizeof NAMES[0]; i++)
        if (streq_ci(v->str, NAMES[i].name)) return NAMES[i].code;
    if (strlen(v->str) == 1) {
        int ch = toupper((unsigned char)v->str[0]);
        if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) return ch;
    }
    return -1;
}

static bool apply(swf_config *c, const char *section, const char *key, const value *v)
{
    if (strcmp(section, "stage") == 0 && strcmp(key, "scale") == 0 && v->kind == V_STRING) {
        static const char *const MODES[] = {"showAll", "exactFit", "noBorder", "noScale"};
        for (uint8_t i = 0; i < 4; i++)
            if (streq_ci(v->str, MODES[i])) {
                c->scale_mode = i;
                return true;
            }
        return false;
    }
    if (strcmp(section, "cursor") == 0) {
        if (strcmp(key, "speed") == 0 && v->kind == V_NUMBER && v->num > 0 && v->num <= 5000) {
            c->cursor_speed = (float)v->num;
            return true;
        }
        if (strcmp(key, "accel") == 0 && v->kind == V_NUMBER && v->num >= 0 && v->num <= 20) {
            c->cursor_accel = (float)v->num;
            return true;
        }
        if (strcmp(key, "visible") == 0 && v->kind == V_BOOL) {
            c->cursor_visible = v->b;
            return true;
        }
        return false;
    }
    if (strcmp(section, "keys") == 0) {
        for (int i = 0; i < SWF_CONFIG_BUTTONS; i++) {
            if (strcmp(key, BUTTONS[i]) != 0) continue;
            int k = key_code(v);
            if (k < 0) return false;
            c->keys[i] = (uint8_t)k;
            return true;
        }
        return false;
    }
    if (strcmp(section, "debug") == 0 && strcmp(key, "profiler") == 0 && v->kind == V_BOOL) {
        c->profiler = v->b;
        return true;
    }
    return false;
}

static const char *skip_ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    return p;
}

bool swf_config_parse(swf_config *c, const char *text, size_t len)
{
    bool ok = true;
    char section[32] = "";
    const char *p = text, *end = text + len;
    for (int line = 1; p < end; line++) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        if (!eol) eol = end;
        const char *q = skip_ws(p, eol);
        const char *next = eol < end ? eol + 1 : end;
        if (q == eol || *q == '#' || *q == '\r') {
            p = next;
            continue;
        }
        if (*q == '[') {
            const char *close = memchr(q, ']', (size_t)(eol - q));
            size_t n = close ? (size_t)(close - q - 1) : 0;
            if (!close || n == 0 || n >= sizeof section) {
                plat_log("anyswf.toml:%d: bad section header", line);
                ok = false;
                section[0] = 0;
            } else {
                memcpy(section, q + 1, n);
                section[n] = 0;
            }
            p = next;
            continue;
        }
        char key[32];
        size_t kn = 0;
        while (q < eol && (isalnum((unsigned char)*q) || *q == '_' || *q == '-') && kn + 1 < sizeof key) key[kn++] = *q++;
        key[kn] = 0;
        if (kn > 0 && (section[0] == 0 || strcmp(section, "android") == 0)) {
            /* Build settings (any TOML value, arrays included); not ours to check. */
            p = next;
            continue;
        }
        q = skip_ws(q, eol);
        value v;
        memset(&v, 0, sizeof v);
        bool good = kn > 0 && q < eol && *q == '=';
        if (good) {
            q = skip_ws(q + 1, eol);
            if (q < eol && *q == '"') {
                size_t n = 0;
                q++;
                while (q < eol && *q != '"' && n + 1 < sizeof v.str) {
                    if (*q == '\\' && q + 1 < eol) q++;
                    v.str[n++] = *q++;
                }
                good = q < eol && *q == '"';
                v.str[n] = 0;
                v.kind = V_STRING;
                q++;
            } else if (eol - q >= 4 && strncmp(q, "true", 4) == 0) {
                v.kind = V_BOOL;
                v.b = true;
                q += 4;
            } else if (eol - q >= 5 && strncmp(q, "false", 5) == 0) {
                v.kind = V_BOOL;
                v.b = false;
                q += 5;
            } else {
                char num[32];
                size_t n = 0;
                while (q < eol && n + 1 < sizeof num && strchr("+-.0123456789eE", *q)) num[n++] = *q++;
                num[n] = 0;
                char *e;
                v.num = strtod(num, &e);
                v.kind = V_NUMBER;
                good = n > 0 && *e == 0;
            }
            q = skip_ws(q, eol);
            good = good && (q == eol || *q == '#' || *q == '\r');
        }
        if (!good) {
            plat_log("anyswf.toml:%d: cannot parse this line", line);
            ok = false;
        } else if (!apply(c, section, key, &v)) {
            plat_log("anyswf.toml:%d: unknown setting or bad value for [%s] %s", line, section, key);
            ok = false;
        }
        p = next;
    }
    return ok;
}
