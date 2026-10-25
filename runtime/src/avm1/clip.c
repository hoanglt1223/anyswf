/* Script objects for display objects (MovieClip, Button, TextField) and their display
 * properties (_x, _alpha, ...), following Ruffle's avm1 stage_object and
 * display_object property tables. */
#include "vm_internal.h"

#include <math.h>
#include <stdio.h>

#define PI 3.14159265358979323846

swf_dobj *clip_root(swf_vm *vm)
{
    return vm->movie->root;
}

swf_dobj *clip_parent(swf_vm *vm, swf_dobj *d)
{
    (void)vm;
    return d ? d->parent : NULL;
}

bool clip_removed(swf_dobj *d)
{
    return d == NULL || d->removed;
}

swf_dobj *clip_of(swf_val v)
{
    return v.t == SWF_T_OBJ && v.u.o->cls == OC_CLIP ? v.u.o->u.clip : NULL;
}

static bool scriptable(const swf_dobj *d)
{
    return d->kind == SWF_CHAR_SPRITE || d->kind == SWF_CHAR_BUTTON || d->kind == SWF_CHAR_EDIT_TEXT;
}

swf_obj *clip_new_object(swf_vm *vm, swf_dobj *d)
{
    swf_obj *proto = vm->clip_proto;
    if (d->kind == SWF_CHAR_BUTTON && vm->button_proto) proto = vm->button_proto;
    else if (d->kind == SWF_CHAR_EDIT_TEXT && vm->textfield_proto) proto = vm->textfield_proto;
    else if (d->kind == SWF_CHAR_SPRITE) {
        swf_obj *ctor = vm_registered_class(vm, d);
        if (ctor) {
            swf_prop *p = obj_own(vm, ctor, vm->s.prototype);
            if (p && p->val.t == SWF_T_OBJ) proto = p->val.u.o;
        }
    }
    swf_obj *o = obj_new_class(vm, proto, OC_CLIP);
    o->u.clip = d;
    d->script_object = o;
    return o;
}

swf_obj *clip_object(swf_vm *vm, swf_dobj *d)
{
    if (!d) return obj_new_bare(vm);
    /* Shapes and other graphics have no script object; Flash hands out the parent. */
    while (d && !scriptable(d) && d->parent) d = d->parent;
    if (d->script_object) return d->script_object;
    return clip_new_object(vm, d);
}

/* Decodes one UTF-8 sequence as a UTF-16 unit; characters outside the BMP never match
 * an identifier unit-for-unit here, so they map to a non-character. */
static uint16_t utf8_unit(const unsigned char **p)
{
    uint32_t c = *(*p)++;
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (extra) c &= 0x3Fu >> extra;
    for (int k = 0; k < extra && **p; k++) c = (c << 6) | (*(*p)++ & 0x3F);
    return c >= 0x10000 ? 0xFFFF : (uint16_t)c;
}

swf_dobj *clip_child_by_name(swf_vm *vm, swf_dobj *d, const swf_str *name, uint32_t from, uint32_t to)
{
    if (!d) return NULL;
    for (swf_dobj *c = d->children; c; c = c->next) {
        if (!c->name || !c->name[0]) continue;
        const unsigned char *p = (const unsigned char *)c->name;
        uint32_t i = from;
        for (; i < to && *p; i++) {
            uint16_t a = name->ch[i], b = utf8_unit(&p);
            if (a != b && (vm->case_sensitive || swf_char_lower(a) != swf_char_lower(b))) break;
        }
        /* With duplicate names the lowest depth wins. */
        if (i == to && !*p) return c;
    }
    return NULL;
}

static void path_rec(swf_vm *vm, swf_dobj *d, char *buf, size_t cap, size_t *len, bool slash)
{
    if (!d->parent) {
        const char *r = slash ? "" : "_level0";
        size_t n = strlen(r);
        if (*len + n < cap) {
            memcpy(buf + *len, r, n);
            *len += n;
        }
        return;
    }
    path_rec(vm, d->parent, buf, cap, len, slash);
    const char *name = d->name && d->name[0] ? d->name : "";
    char tmp[32];
    if (!name[0]) {
        snprintf(tmp, sizeof tmp, "instance%u", (unsigned)(d->depth + 16384));
        name = tmp;
    }
    size_t n = strlen(name);
    if (*len + n + 1 < cap) {
        buf[(*len)++] = slash ? '/' : '.';
        memcpy(buf + *len, name, n);
        *len += n;
    }
}

swf_str *clip_path(swf_vm *vm, swf_dobj *d)
{
    char buf[1024];
    size_t len = 0;
    path_rec(vm, d, buf, sizeof buf, &len, false);
    return str_from_utf8(vm, buf, len);
}

static swf_str *target_path(swf_vm *vm, swf_dobj *d)
{
    char buf[1024];
    size_t len = 0;
    path_rec(vm, d, buf, sizeof buf, &len, true);
    if (len == 0) return str_from_ascii(vm, "/");
    return str_from_utf8(vm, buf, len);
}

static double mscale_x(const swf_matrix *m) { return sqrt((double)m->a * m->a + (double)m->b * m->b); }
static double mscale_y(const swf_matrix *m) { return sqrt((double)m->c * m->c + (double)m->d * m->d); }

static void set_scale_rot(swf_dobj *d, double sx, double sy, double rot_deg)
{
    double r = rot_deg * PI / 180.0;
    d->matrix.a = (float)(sx * cos(r));
    d->matrix.b = (float)(sx * sin(r));
    d->matrix.c = (float)(-sy * sin(r));
    d->matrix.d = (float)(sy * cos(r));
}

static double rotation_deg(const swf_matrix *m)
{
    return atan2(m->b, m->a) * 180.0 / PI;
}

static double extent(swf_vm *vm, swf_dobj *d, bool width)
{
    float b[4];
    if (!swf_dobj_bounds(vm->movie, d, &d->matrix, b)) return 0;
    double v = width ? (double)b[2] - b[0] : (double)b[3] - b[1];
    return round(v) / 20.0;
}

enum {
    P_X, P_Y, P_XSCALE, P_YSCALE, P_CURRENTFRAME, P_TOTALFRAMES, P_ALPHA, P_VISIBLE, P_WIDTH, P_HEIGHT,
    P_ROTATION, P_TARGET, P_FRAMESLOADED, P_NAME, P_DROPTARGET, P_URL, P_HIGHQUALITY, P_FOCUSRECT,
    P_SOUNDBUFTIME, P_QUALITY, P_XMOUSE, P_YMOUSE, P_COUNT
};

static const char *const PROP_NAMES[P_COUNT] = {
    "_x", "_y", "_xscale", "_yscale", "_currentframe", "_totalframes", "_alpha", "_visible", "_width", "_height",
    "_rotation", "_target", "_framesloaded", "_name", "_droptarget", "_url", "_highquality", "_focusrect",
    "_soundbuftime", "_quality", "_xmouse", "_ymouse"};

static int prop_index(const swf_str *name)
{
    if (name->len < 2 || name->ch[0] != '_') return -1;
    for (int i = 0; i < P_COUNT; i++) {
        const char *p = PROP_NAMES[i];
        size_t n = strlen(p);
        if (name->len != n) continue;
        bool eq = true;
        /* Display property names are never case sensitive. */
        for (size_t k = 0; k < n && eq; k++) {
            uint16_t c = name->ch[k];
            if (c >= 'A' && c <= 'Z') c = (uint16_t)(c + 32);
            if (c != (unsigned char)p[k]) eq = false;
        }
        if (eq) return i;
    }
    return -1;
}

swf_val clip_get_property_index(swf_ctx *cx, swf_dobj *d, int index)
{
    swf_vm *vm = cx->vm;
    switch (index) {
    /* Flash positions are whole twips. */
    case P_X: return swf_num(round(d->matrix.tx) / 20.0);
    case P_Y: return swf_num(round(d->matrix.ty) / 20.0);
    case P_XSCALE: return swf_num(mscale_x(&d->matrix) * 100.0);
    case P_YSCALE: return swf_num(mscale_y(&d->matrix) * 100.0);
    case P_CURRENTFRAME: return swf_num(d->kind == SWF_CHAR_SPRITE ? (d->frame ? d->frame : 1) : 1);
    case P_TOTALFRAMES:
    case P_FRAMESLOADED:
        return swf_num(d->kind == SWF_CHAR_SPRITE ? swf_clip_sprite(vm->movie, d)->frame_count : 1);
    case P_ALPHA: return swf_num(d->cxform.mul[3] * 100.0);
    case P_VISIBLE: return swf_bool(d->visible);
    case P_WIDTH: return swf_num(extent(vm, d, true));
    case P_HEIGHT: return swf_num(extent(vm, d, false));
    case P_ROTATION: return swf_num(rotation_deg(&d->matrix));
    case P_TARGET: return swf_strv(target_path(vm, d));
    case P_NAME: return swf_strv(str_from_utf8(vm, d->name ? d->name : "", d->name ? strlen(d->name) : 0));
    case P_DROPTARGET:
        /* What lies under the cursor besides this clip: valid while dragging and right
         * after the drop, which is when scripts read it. */
        return swf_strv(mouse_drop_target(vm, d));
    case P_URL: return swf_strv(vm->s.empty);
    case P_HIGHQUALITY: return swf_num(1);
    case P_FOCUSRECT: return swf_bool(true);
    case P_SOUNDBUFTIME: return swf_num(5);
    case P_QUALITY: return swf_strv(atom_ascii(vm, "HIGH"));
    case P_XMOUSE:
    case P_YMOUSE: {
        swf_matrix w = swf_dobj_world_matrix(d), inv;
        if (!swf_matrix_invert(&w, &inv)) return swf_num(0);
        double x = inv.a * vm->mouse_x + inv.c * vm->mouse_y + inv.tx;
        double y = inv.b * vm->mouse_x + inv.d * vm->mouse_y + inv.ty;
        return swf_num(round(index == P_XMOUSE ? x : y) / 20.0);
    }
    default: return swf_undef();
    }
}

void clip_set_property_index(swf_ctx *cx, swf_dobj *d, int index, swf_val v)
{
    double n;
    switch (index) {
    case P_X:
        n = to_number(cx, v);
        if (isfinite(n)) d->matrix.tx = (float)round(n * 20.0);
        break;
    case P_Y:
        n = to_number(cx, v);
        if (isfinite(n)) d->matrix.ty = (float)round(n * 20.0);
        break;
    case P_XSCALE:
        n = to_number(cx, v);
        if (isfinite(n)) set_scale_rot(d, n / 100.0, mscale_y(&d->matrix), rotation_deg(&d->matrix));
        break;
    case P_YSCALE:
        n = to_number(cx, v);
        if (isfinite(n)) set_scale_rot(d, mscale_x(&d->matrix), n / 100.0, rotation_deg(&d->matrix));
        break;
    case P_ROTATION:
        n = to_number(cx, v);
        if (isfinite(n)) {
            n = fmod(n, 360.0);
            if (n > 180.0) n -= 360.0;
            else if (n < -180.0) n += 360.0;
            set_scale_rot(d, mscale_x(&d->matrix), mscale_y(&d->matrix), n);
        }
        break;
    case P_ALPHA:
        n = to_number(cx, v);
        if (isfinite(n)) d->cxform.mul[3] = (float)(n / 100.0);
        break;
    case P_VISIBLE:
        n = to_number(cx, v);
        if (!isnan(n)) d->visible = n != 0.0;
        break;
    case P_WIDTH:
    case P_HEIGHT: {
        n = to_number(cx, v);
        double cur = extent(cx->vm, d, index == P_WIDTH);
        if (isfinite(n) && n >= 0 && cur > 0) {
            double sx = mscale_x(&d->matrix), sy = mscale_y(&d->matrix);
            if (index == P_WIDTH) sx *= n / cur;
            else sy *= n / cur;
            set_scale_rot(d, sx, sy, rotation_deg(&d->matrix));
        }
        break;
    }
    case P_NAME: {
        char *u = str_to_utf8(to_string(cx, v), NULL);
        if (!u || !swf_dobj_set_name(d, u)) vm_abort(cx->vm, "out of memory renaming a clip");
        free(u);
        break;
    }
    default:
        break;
    }
}

bool clip_get_display_prop(swf_ctx *cx, swf_dobj *d, swf_str *name, swf_val *out)
{
    swf_vm *vm = cx->vm;
    if (name->len && name->ch[0] == '_' && cx->version >= 5) {
        bool cs = vm->case_sensitive;
        if (cs ? str_eq_ascii(name, "_root") : str_eq_nocase(name, atom_ascii(vm, "_root"))) {
            *out = swf_objv(clip_object(vm, clip_root(vm)));
            return true;
        }
        if (cs ? str_eq_ascii(name, "_parent") : str_eq_nocase(name, atom_ascii(vm, "_parent"))) {
            *out = d->parent ? swf_objv(clip_object(vm, d->parent)) : swf_undef();
            return true;
        }
        if (cx->version >= 6 && (cs ? str_eq_ascii(name, "_global") : str_eq_nocase(name, vm->s._global))) {
            *out = swf_objv(vm->global);
            return true;
        }
        if (name->len == 7 && (cs ? str_eq_ascii(name, "_level0") : str_eq_nocase(name, vm->s._level0))) {
            *out = swf_objv(clip_object(vm, clip_root(vm)));
            return true;
        }
    }
    swf_dobj *child = clip_child_by_name(vm, d, name, 0, name->len);
    if (child) {
        *out = swf_objv(clip_object(vm, child));
        return true;
    }
    int idx = prop_index(name);
    if (idx >= 0) {
        *out = clip_get_property_index(cx, d, idx);
        return true;
    }
    return false;
}

bool clip_set_display_prop(swf_ctx *cx, swf_dobj *d, swf_str *name, swf_val v)
{
    int idx = prop_index(name);
    if (idx < 0) return false;
    switch (idx) {
    case P_CURRENTFRAME: case P_TOTALFRAMES: case P_FRAMESLOADED: case P_TARGET: case P_URL: case P_DROPTARGET:
    case P_XMOUSE: case P_YMOUSE:
        return true; /* read-only */
    default:
        clip_set_property_index(cx, d, idx, v);
        return true;
    }
}

void clip_goto(swf_ctx *cx, swf_dobj *d, swf_val frame, bool play, uint32_t scene_bias)
{
    swf_vm *vm = cx->vm;
    swf_dobj *clip = d;
    int64_t f = -1;
    if (frame.t == SWF_T_NUM && frame.u.n == trunc(frame.u.n)) {
        f = f64_to_i32(frame.u.n);
    } else {
        swf_str *s = to_string(cx, frame);
        /* "path:frame" targets another clip. */
        int64_t sep = -1;
        for (int64_t i = (int64_t)s->len - 1; i >= 0; i--)
            if (s->ch[i] == ':' || s->ch[i] == '.') {
                sep = i;
                break;
            }
        uint32_t fs = 0;
        if (sep >= 0) {
            swf_obj *o = resolve_target_path(cx, clip_root(vm), clip_object(vm, d), s, 0, (uint32_t)sep, true, true);
            clip = o && o->cls == OC_CLIP ? o->u.clip : NULL;
            fs = (uint32_t)sep + 1;
        }
        if (!clip || clip->kind != SWF_CHAR_SPRITE) return;
        swf_str *fstr = str_sub(vm, s, fs, s->len);
        double n = parse_float_impl(fstr->ch, fstr->len, true);
        if (!isnan(n) && fstr->len) {
            f = f64_to_i32(n);
        } else {
            char *u = str_to_utf8(fstr, NULL);
            if (!u) return;
            int64_t lf = swf_clip_find_label(vm->movie, clip, u);
            free(u);
            if (lf < 0) return;
            f = lf;
        }
    }
    if (clip->kind != SWF_CHAR_SPRITE) return;
    int32_t fr = (int32_t)(uint32_t)((uint32_t)(int32_t)f - 1u);
    fr = (int32_t)((uint32_t)fr + scene_bias);
    fr = fr == INT32_MAX ? INT32_MAX : fr + 1;
    if (fr <= 0) return;
    if (play) swf_clip_play(clip);
    else swf_clip_stop(clip);
    swf_clip_goto(vm->movie, clip, (uint32_t)fr);
}

/* --- MovieClip prototype (navigation only; the rest belongs to the built-ins phase) --- */

static swf_dobj *this_clip(swf_val this_)
{
    swf_dobj *d = clip_of(this_);
    return d && d->kind == SWF_CHAR_SPRITE ? d : NULL;
}

static swf_val mc_play(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)cx; (void)argc; (void)argv;
    swf_dobj *d = this_clip(this_);
    if (d) swf_clip_play(d);
    return swf_undef();
}

static swf_val mc_stop(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)cx; (void)argc; (void)argv;
    swf_dobj *d = this_clip(this_);
    if (d) swf_clip_stop(d);
    return swf_undef();
}

static swf_val mc_goto_play(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    swf_dobj *d = this_clip(this_);
    if (d) clip_goto(cx, d, arg(argc, argv, 0), true, 0);
    return swf_undef();
}

static swf_val mc_goto_stop(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    swf_dobj *d = this_clip(this_);
    if (d) clip_goto(cx, d, arg(argc, argv, 0), false, 0);
    return swf_undef();
}

static swf_val mc_next(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)argc; (void)argv;
    swf_dobj *d = this_clip(this_);
    if (d) {
        swf_clip_stop(d);
        if (d->frame < swf_clip_total_frames(cx->vm->movie, d)) swf_clip_goto(cx->vm->movie, d, d->frame + 1);
    }
    return swf_undef();
}

static swf_val mc_prev(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)argc; (void)argv;
    swf_dobj *d = this_clip(this_);
    if (d) {
        swf_clip_stop(d);
        if (d->frame > 1) swf_clip_goto(cx->vm->movie, d, d->frame - 1);
    }
    return swf_undef();
}

static swf_val mc_get_depth(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)cx; (void)argc; (void)argv;
    swf_dobj *d = clip_of(this_);
    return d ? swf_num(d->depth - 16384) : swf_undef();
}

static swf_val mc_bytes(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)cx; (void)argc; (void)argv; (void)this_;
    return swf_num((double)cx->vm->movie->assets->swf_size);
}

void clip_init_builtins(swf_vm *vm)
{
    swf_obj *p = obj_new(vm, vm->object_proto);
    vm->clip_proto = p;
    const uint16_t A = ATTR_DONT_ENUM | ATTR_DONT_DELETE;
    native_method(vm, p, "play", mc_play, A);
    native_method(vm, p, "stop", mc_stop, A);
    native_method(vm, p, "gotoAndPlay", mc_goto_play, A);
    native_method(vm, p, "gotoAndStop", mc_goto_stop, A);
    native_method(vm, p, "nextFrame", mc_next, A);
    native_method(vm, p, "prevFrame", mc_prev, A);
    native_method(vm, p, "getDepth", mc_get_depth, A);
    native_method(vm, p, "getBytesLoaded", mc_bytes, A);
    native_method(vm, p, "getBytesTotal", mc_bytes, A);
    make_class(vm, "MovieClip", NULL, NULL, p);
}
