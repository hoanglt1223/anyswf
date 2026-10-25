/* MovieClip methods beyond timeline navigation: attachMovie, duplicateMovieClip,
 * createEmptyMovieClip, createTextField, removeMovieClip, depths, bounds, coordinate
 * conversion, hitTest, dragging and the drawing API. Semantics follow Ruffle's
 * avm1/globals/movie_clip.rs (MIT/Apache-2.0). */
#include "../avm1/vm_internal.h"
#include "../display/display_internal.h"
#include "../display/drawing.h"

#include <math.h>

#define AVM_MAX_DEPTH 2130690045
#define AVM_MAX_REMOVE_DEPTH 2130706416
#define INVALID_BOUNDS 6710886.4

static swf_dobj *this_sprite(swf_val this_)
{
    swf_dobj *d = clip_of(this_);
    return d && d->kind == SWF_CHAR_SPRITE && !d->removed ? d : NULL;
}

static swf_dobj *this_display(swf_val this_)
{
    swf_dobj *d = clip_of(this_);
    return d && !d->removed ? d : NULL;
}

static bool name_from(swf_ctx *cx, swf_dobj *d, swf_val v)
{
    char *u = str_to_utf8(to_string(cx, v), NULL);
    if (!u) return false;
    bool ok = swf_dobj_set_name(d, u);
    free(u);
    return ok;
}

/* Copies an init object's properties onto a new clip, in reverse enumeration order as
 * Flash does (setters can observe it). */
static void apply_init_object(swf_ctx *cx, swf_obj *target, swf_val init)
{
    if (init.t != SWF_T_OBJ) return;
    uint32_t n;
    swf_str **keys = obj_keys(cx, init.u.o, false, &n);
    if (!keys) return;
    for (int64_t i = (int64_t)n - 1; i >= 0; i--) obj_set(cx, target, keys[i], obj_get(cx, init.u.o, keys[i]));
    free(keys);
}

/* Finishes an object created by script: places it, then (for clips with a registered
 * class) copies the init object and runs the constructor, like Ruffle's
 * construct_as_avm1_object with Instantiator::Avm1. */
static swf_obj *place_scripted(swf_ctx *cx, swf_dobj *parent, swf_dobj *o, swf_val init)
{
    swf_vm *vm = cx->vm;
    bool saved = vm->instantiating;
    vm->instantiating = true;
    swf_clip_place(vm->movie, parent, o);
    vm->instantiating = saved;
    swf_obj *obj = clip_object(vm, o);
    apply_init_object(cx, obj, init);
    swf_obj *ctor = vm_registered_class(vm, o);
    if (ctor) func_construct_on(cx, ctor, obj, 0, NULL);
    return obj;
}

static int64_t find_export(const swf_assets *a, swf_str *name)
{
    char *u = str_to_utf8(name, NULL);
    if (!u) return -1;
    int64_t found = -1;
    for (uint32_t i = 0; i < a->export_count && found < 0; i++) {
        const char *e = a->exports[i].name, *p = u;
        while (*e && *p && (*e | 0x20) == (*p | 0x20)) e++, p++;
        if (!*e && !*p) found = a->exports[i].char_id;
    }
    free(u);
    return found;
}

static swf_val mc_attach_movie(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    swf_vm *vm = cx->vm;
    swf_dobj *d = this_sprite(this_);
    if (!d || argc < 3) return swf_undef();
    swf_str *id = to_string(cx, argv[0]);
    int64_t depth = (int64_t)to_int32(cx, argv[2]) + AVM_DEPTH_BIAS;
    if (depth < 0 || depth > AVM_MAX_DEPTH) return swf_undef();
    int64_t ch = find_export(vm->movie->assets, id);
    if (ch < 0) return swf_undef();
    swf_dobj *o = swf_dobj_create(vm->movie, (uint16_t)ch, 0, (int32_t)depth);
    if (!o) return swf_undef();
    if (!name_from(cx, o, argv[1])) {
        swf_dobj_free(NULL, o);
        return swf_undef();
    }
    return swf_objv(place_scripted(cx, d, o, arg(argc, argv, 3)));
}

static swf_val mc_create_empty(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    swf_vm *vm = cx->vm;
    swf_dobj *d = this_sprite(this_);
    if (!d || argc < 2) return swf_undef();
    int64_t depth = (int64_t)to_int32(cx, argv[1]) + AVM_DEPTH_BIAS;
    swf_dobj *o = swf_dobj_create(vm->movie, 0, SWF_CHAR_SPRITE, (int32_t)depth);
    if (!o) return swf_undef();
    if (!name_from(cx, o, argv[0])) {
        swf_dobj_free(NULL, o);
        return swf_undef();
    }
    return swf_objv(place_scripted(cx, d, o, swf_undef()));
}

swf_dobj *textfield_create(swf_ctx *cx, swf_dobj *parent, swf_val name, int32_t depth, double x, double y,
                           double w, double h);

static swf_val mc_create_text_field(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    swf_dobj *d = this_sprite(this_);
    if (!d) return swf_undef();
    double depth = to_number(cx, arg(argc, argv, 1));
    double v[4];
    for (int i = 0; i < 4; i++) v[i] = to_int32(cx, arg(argc, argv, 2 + (uint32_t)i));
    swf_dobj *t = textfield_create(cx, d, arg(argc, argv, 0), (int32_t)(f64_to_i32(depth) + AVM_DEPTH_BIAS), v[0],
                                   v[1], v[2], v[3]);
    if (!t || cx->version < 8) return swf_undef();
    return swf_objv(clip_object(cx->vm, t));
}

/* CloneSprite semantics (Ruffle clone_sprite): `depth` is already biased. */
static swf_dobj *clone(swf_ctx *cx, swf_dobj *src, swf_val name, int64_t depth, swf_val init)
{
    swf_vm *vm = cx->vm;
    swf_dobj *parent = src->parent;
    if (!parent || parent->kind != SWF_CHAR_SPRITE || depth < 0 || depth > AVM_MAX_DEPTH) return NULL;
    swf_dobj *o;
    if (src->char_id) o = swf_dobj_create(vm->movie, src->char_id, 0, (int32_t)depth);
    else if (src->kind == SWF_CHAR_SPRITE || src->kind == SWF_CHAR_EDIT_TEXT)
        o = swf_dobj_create(vm->movie, 0, src->kind, (int32_t)depth);
    else return NULL;
    if (!o) return NULL;
    if (!name_from(cx, o, name)) {
        swf_dobj_free(NULL, o);
        return NULL;
    }
    o->matrix = src->matrix;
    o->cxform = src->cxform;
    if (o->kind == SWF_CHAR_SPRITE) {
        o->clip_action_first = src->clip_action_first;
        o->clip_action_count = src->clip_action_count;
        if (src->drawing) o->drawing = swf_drawing_clone(src->drawing);
    }
    if (src->edit && o->kind == SWF_CHAR_EDIT_TEXT) {
        swf_edit_text *e = malloc(sizeof *e);
        if (e) {
            *e = *src->edit;
            e->var_name = NULL;
            e->initial_text = NULL;
            size_t n = strlen(src->edit->var_name), m = strlen(src->edit->initial_text);
            char *vn = malloc(n + 1), *it = malloc(m + 1);
            if (vn && it) {
                memcpy(vn, src->edit->var_name, n + 1);
                memcpy(it, src->edit->initial_text, m + 1);
                e->var_name = vn;
                e->initial_text = it;
                o->edit = e;
            } else {
                free(vn);
                free(it);
                free(e);
            }
        }
    }
    place_scripted(cx, parent, o, init);
    return o;
}

static swf_val mc_duplicate(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    swf_dobj *d = this_display(this_);
    if (!d || argc < 1) return swf_undef();
    int64_t depth = (int64_t)to_int32(cx, arg(argc, argv, 1)) + AVM_DEPTH_BIAS;
    swf_val init = argc > 2 ? swf_objv(to_object_or_bare(cx, argv[2])) : swf_undef();
    swf_dobj *o = clone(cx, d, argv[0], depth, init);
    if (cx->version < 6 || !o) return swf_undef();
    return swf_objv(clip_object(cx->vm, o));
}

void swf_act_clone_sprite(swf_ctx *cx, swf_val source, swf_val target, swf_val depth)
{
    swf_dobj *base = ctx_target(cx) ? ctx_target(cx) : clip_root(cx->vm);
    swf_dobj *src = resolve_target_clip(cx, base, source, false);
    if (!src) return;
    (void)clone(cx, src, target, (int64_t)to_int32(cx, depth), swf_undef());
}

static void remove_display(swf_vm *vm, swf_dobj *d)
{
    if (d->depth >= AVM_DEPTH_BIAS && (int64_t)d->depth < AVM_MAX_REMOVE_DEPTH && !d->removed && d->parent)
        swf_clip_remove_child(vm->movie, d->parent, d->depth);
}

static swf_val mc_remove(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)argc; (void)argv;
    swf_dobj *d = this_display(this_);
    if (d) remove_display(cx->vm, d);
    return swf_undef();
}

static swf_val mc_swap_depths(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    swf_vm *vm = cx->vm;
    swf_dobj *d = this_display(this_);
    if (!d || !d->parent) return swf_undef();
    swf_dobj *parent = d->parent;
    swf_val a = arg(argc, argv, 0);
    int64_t depth;
    if (a.t == SWF_T_NUM) {
        depth = (int64_t)f64_to_i32(a.u.n) + AVM_DEPTH_BIAS;
    } else {
        swf_dobj *t = resolve_target_clip(cx, d, a, false);
        if (!t || t->parent != parent || t->removed) return swf_undef();
        depth = t->depth;
    }
    if (depth < 0 || depth > AVM_MAX_DEPTH || depth == d->depth) return swf_undef();
    swf_dobj *other = swf_clip_child_at(parent, (int32_t)depth);
    int32_t old = d->depth;
    swf_clip_detach(parent, d);
    if (other) {
        swf_clip_detach(parent, other);
        other->depth = old;
        other->from_timeline = false;
        swf_clip_insert(vm->movie, parent, other);
    }
    d->depth = (int32_t)depth;
    d->from_timeline = false;
    swf_clip_insert(vm->movie, parent, d);
    return swf_undef();
}

static swf_val mc_next_depth(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)cx; (void)argc; (void)argv;
    swf_dobj *d = this_sprite(this_);
    if (!d) return swf_undef();
    int64_t hi = 0;
    for (swf_dobj *c = d->children; c; c = c->next)
        if (c->depth > hi) hi = c->depth;
    int64_t r = hi - (AVM_DEPTH_BIAS - 1);
    return swf_num((double)(r < 0 ? 0 : r));
}

static swf_val mc_instance_at(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    swf_dobj *d = this_sprite(this_);
    if (!d || argc < 1) return swf_undef();
    int32_t depth = (int32_t)((uint32_t)to_int32(cx, argv[0]) + AVM_DEPTH_BIAS);
    swf_dobj *c = swf_clip_child_at(d, depth);
    if (!c) return swf_undef();
    return swf_objv(clip_object(cx->vm, c));
}

static swf_val mc_swf_version(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)argc; (void)argv;
    return this_display(this_) ? swf_num(cx->vm->version) : swf_undef();
}

/* --- coordinates and bounds ---------------------------------------------------------- */

static swf_obj *rect_object(swf_ctx *cx, double x0, double x1, double y0, double y1)
{
    swf_vm *vm = cx->vm;
    swf_obj *o = obj_new(vm, vm->object_proto);
    obj_set(cx, o, atom_ascii(vm, "xMin"), swf_num(x0));
    obj_set(cx, o, atom_ascii(vm, "xMax"), swf_num(x1));
    obj_set(cx, o, atom_ascii(vm, "yMin"), swf_num(y0));
    obj_set(cx, o, atom_ascii(vm, "yMax"), swf_num(y1));
    return o;
}

static swf_val mc_get_bounds(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    swf_vm *vm = cx->vm;
    swf_dobj *d = this_display(this_);
    if (!d) return swf_undef();
    swf_dobj *t = argc ? resolve_target_clip(cx, d, argv[0], false) : d;
    if (!t) return swf_undef();
    float b[4];
    if (!swf_dobj_bounds(vm->movie, d, &SWF_MATRIX_IDENTITY, b))
        return swf_objv(rect_object(cx, INVALID_BOUNDS, INVALID_BOUNDS, INVALID_BOUNDS, INVALID_BOUNDS));
    if (t != d) {
        swf_matrix to_global = swf_dobj_world_matrix(d), tw = swf_dobj_world_matrix(t), inv;
        if (!swf_matrix_invert(&tw, &inv)) inv = SWF_MATRIX_IDENTITY;
        swf_matrix m = swf_matrix_mul(&inv, &to_global);
        const float xs[2] = {b[0], b[2]}, ys[2] = {b[1], b[3]};
        float o[4] = {0};
        for (int i = 0; i < 4; i++) {
            float x = m.a * xs[i & 1] + m.c * ys[i >> 1] + m.tx, y = m.b * xs[i & 1] + m.d * ys[i >> 1] + m.ty;
            if (i == 0 || x < o[0]) o[0] = x;
            if (i == 0 || y < o[1]) o[1] = y;
            if (i == 0 || x > o[2]) o[2] = x;
            if (i == 0 || y > o[3]) o[3] = y;
        }
        memcpy(b, o, sizeof o);
    }
    return swf_objv(rect_object(cx, roundf(b[0]) / 20.0, roundf(b[2]) / 20.0, roundf(b[1]) / 20.0,
                                roundf(b[3]) / 20.0));
}

static bool point_xy(swf_ctx *cx, swf_val p, double *x, double *y)
{
    if (p.t != SWF_T_OBJ) return false;
    swf_prop *px = obj_own(cx->vm, p.u.o, atom_ascii(cx->vm, "x"));
    swf_prop *py = obj_own(cx->vm, p.u.o, atom_ascii(cx->vm, "y"));
    if (!px || !py || px->getter || py->getter || px->val.t != SWF_T_NUM || py->val.t != SWF_T_NUM) return false;
    *x = px->val.u.n;
    *y = py->val.u.n;
    return true;
}

static swf_val convert_point(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv, bool to_global)
{
    swf_dobj *d = this_display(this_);
    double x, y;
    if (!d || !point_xy(cx, arg(argc, argv, 0), &x, &y)) return swf_undef();
    swf_matrix w = swf_dobj_world_matrix(d), m;
    if (to_global) m = w;
    else if (!swf_matrix_invert(&w, &m)) return swf_undef();
    double tx = x * 20.0, ty = y * 20.0;
    double gx = m.a * tx + m.c * ty + m.tx, gy = m.b * tx + m.d * ty + m.ty;
    obj_set(cx, argv[0].u.o, atom_ascii(cx->vm, "x"), swf_num(round(gx) / 20.0));
    obj_set(cx, argv[0].u.o, atom_ascii(cx->vm, "y"), swf_num(round(gy) / 20.0));
    return swf_undef();
}

static swf_val mc_local_to_global(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    return convert_point(cx, this_, argc, argv, true);
}

static swf_val mc_global_to_local(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    return convert_point(cx, this_, argc, argv, false);
}

static swf_val mc_hit_test(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    swf_vm *vm = cx->vm;
    swf_dobj *d = this_display(this_);
    if (!d) return swf_bool(false);
    if (argc > 1) {
        double x = to_number(cx, argv[0]), y = to_number(cx, argv[1]);
        bool shape = argc > 2 && to_bool_v(cx->version, argv[2]);
        if (!isfinite(x) || !isfinite(y)) return swf_bool(false);
        /* The point is in _root coordinates. */
        const swf_matrix *r = &clip_root(vm)->matrix;
        float px = (float)(r->a * x * 20 + r->c * y * 20 + r->tx), py = (float)(r->b * x * 20 + r->d * y * 20 + r->ty);
        if (!shape) {
            float b[4];
            swf_matrix w = swf_dobj_world_matrix(d);
            return swf_bool(swf_dobj_bounds(vm->movie, d, &w, b) && px >= b[0] && px <= b[2] && py >= b[1] &&
                            py <= b[3]);
        }
        return swf_bool(swf_dobj_hit(vm->movie, d, px, py, true));
    }
    if (argc == 1) {
        swf_dobj *o = resolve_target_clip(cx, d, argv[0], false);
        if (!o || o->removed) return swf_bool(false);
        float a[4], b[4];
        swf_matrix wa = swf_dobj_world_matrix(d), wb = swf_dobj_world_matrix(o);
        if (!swf_dobj_bounds(vm->movie, d, &wa, a) || !swf_dobj_bounds(vm->movie, o, &wb, b)) return swf_bool(false);
        return swf_bool(a[0] <= b[2] && b[0] <= a[2] && a[1] <= b[3] && b[1] <= a[3]);
    }
    return swf_bool(false);
}

/* --- dragging ---------------------------------------------------------------------------- */

static void start_drag(swf_ctx *cx, swf_dobj *d, bool lock, bool constrain, const double r[4])
{
    float rect[4] = {0};
    for (int i = 0; i < 4 && constrain; i++) rect[i] = isfinite(r[i]) ? (float)(r[i] * 20.0) : 0.0f;
    mouse_start_drag(cx->vm, d, lock, constrain, rect);
}

static swf_val mc_start_drag(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    swf_dobj *d = this_display(this_);
    if (!d) return swf_undef();
    bool lock = argc > 0 && to_bool_v(cx->version, argv[0]);
    double r[4] = {0};
    for (int i = 0; i < 4; i++) r[i] = to_number(cx, arg(argc, argv, 1 + (uint32_t)i));
    start_drag(cx, d, lock, argc > 1, r);
    return swf_undef();
}

static swf_val mc_stop_drag(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)this_; (void)argc; (void)argv;
    mouse_stop_drag(cx->vm);
    return swf_undef();
}

/* StartDrag action: target, lock, constrain flag, then (if set) x1, y1, x2, y2. */
void swf_dyn_start_drag(swf_ctx *cx, swf_val *S, int *sp)
{
    swf_val target = *sp > 0 ? S[--*sp] : swf_undef();
    swf_val lock = *sp > 0 ? S[--*sp] : swf_undef();
    swf_val constrain = *sp > 0 ? S[--*sp] : swf_undef();
    double r[4] = {0};
    bool c = to_int32(cx, constrain) != 0;
    if (c) {
        /* Pushed as x1, y1, x2, y2: y2 is on top. */
        for (int i = 3; i >= 0; i--) r[i] = *sp > 0 ? to_number(cx, S[--*sp]) : 0;
    }
    swf_dobj *base = ctx_target(cx) ? ctx_target(cx) : clip_root(cx->vm);
    swf_dobj *d = resolve_target_clip(cx, base, target, true);
    if (d) start_drag(cx, d, to_bool_v(cx->version, lock), c, r);
}

void swf_act_end_drag(swf_ctx *cx)
{
    mouse_stop_drag(cx->vm);
}

/* --- drawing API --------------------------------------------------------------------- */

static swf_drawing *drawing_of(swf_vm *vm, swf_dobj *d)
{
    if (!d->drawing) {
        d->drawing = swf_drawing_new();
        if (!d->drawing) vm_abort(vm, "out of memory in the drawing API");
    }
    return d->drawing;
}

static void check(swf_vm *vm, bool ok)
{
    if (!ok) vm_abort(vm, "out of memory in the drawing API");
}

static uint32_t rgba_of(swf_ctx *cx, swf_val rgb, swf_val alpha, bool have_alpha)
{
    uint32_t c = to_uint32(cx, rgb);
    double a = have_alpha ? to_number(cx, alpha) : 100.0;
    if (!(a >= 0)) a = 0;  /* NaN and negatives */
    if (a > 100) a = 100;
    uint32_t a8 = (uint32_t)(a / 100.0 * 255.0);
    return ((c >> 16) & 0xFF) | (c & 0xFF00) | ((c & 0xFF) << 16) | (a8 << 24);
}

static swf_val mc_line_style(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    swf_dobj *d = this_sprite(this_);
    if (!d) return swf_undef();
    swf_drawing *dr = drawing_of(cx->vm, d);
    if (argc < 1) {
        check(cx->vm, swf_drawing_line_style(dr, false, 0, 0));
        return swf_undef();
    }
    double w = to_number(cx, argv[0]);
    if (!(w >= 0)) w = 0;
    if (w > 255) w = 255;
    uint32_t rgba = argc > 1 ? rgba_of(cx, argv[1], arg(argc, argv, 2), argc > 2) : 0xFF000000u;
    check(cx->vm, swf_drawing_line_style(dr, true, (float)(w * 20.0), rgba));
    return swf_undef();
}

static swf_val mc_begin_fill(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    swf_dobj *d = this_sprite(this_);
    if (!d) return swf_undef();
    swf_drawing *dr = drawing_of(cx->vm, d);
    if (argc < 1) {
        check(cx->vm, swf_drawing_begin_fill(dr, NULL));
        return swf_undef();
    }
    swf_fill_style fs = {0};
    fs.rgba = rgba_of(cx, argv[0], arg(argc, argv, 1), argc > 1);
    check(cx->vm, swf_drawing_begin_fill(dr, &fs));
    return swf_undef();
}

static double num_prop(swf_ctx *cx, swf_obj *o, const char *name)
{
    return to_number(cx, obj_get(cx, o, atom_ascii(cx->vm, name)));
}

static swf_val mc_begin_gradient_fill(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    swf_vm *vm = cx->vm;
    swf_dobj *d = this_sprite(this_);
    if (!d) return swf_undef();
    swf_drawing *dr = drawing_of(vm, d);
    if (argc < 1 || argv[0].t == SWF_T_UNDEF) {
        check(vm, swf_drawing_begin_fill(dr, NULL));
        return swf_undef();
    }
    if (argc < 5 || argc > 8 || (argc > 5 && cx->version < 8)) return swf_undef();
    swf_str *type = to_string(cx, argv[0]);
    bool radial = str_eq_ascii(type, "radial");
    if (!radial && !str_eq_ascii(type, "linear")) return swf_undef();
    swf_obj *colors = to_object_or_bare(cx, argv[1]), *alphas = to_object_or_bare(cx, argv[2]),
            *ratios = to_object_or_bare(cx, argv[3]), *mo = to_object_or_bare(cx, argv[4]);
    uint32_t n = to_uint32(cx, obj_get(cx, colors, vm->s.length));
    uint32_t na = to_uint32(cx, obj_get(cx, alphas, vm->s.length));
    uint32_t nr = to_uint32(cx, obj_get(cx, ratios, vm->s.length));
    if (na < n) n = na;
    if (nr < n) n = nr;
    if (n > SWF_GRADIENT_MAX) n = SWF_GRADIENT_MAX;
    if (n == 0) return swf_undef();
    swf_fill_style fs = {0};
    fs.gradient = true;
    fs.radial = radial;
    fs.stops = (uint8_t)n;
    for (uint32_t i = 0; i < n; i++) {
        swf_str *k = atom_int(vm, i);
        fs.stop_rgba[i] = rgba_of(cx, obj_get(cx, colors, k), obj_get(cx, alphas, k), true);
        double r = to_number(cx, obj_get(cx, ratios, k));
        fs.ratio[i] = (uint8_t)(r >= 255 ? 255 : r > 0 ? r : 0);
    }
    fs.rgba = fs.stop_rgba[0];
    swf_matrix gm;
    if (str_eq_ascii(to_string(cx, obj_get(cx, mo, atom_ascii(vm, "matrixType"))), "box")) {
        double w = num_prop(cx, mo, "w"), h = num_prop(cx, mo, "h"), r = num_prop(cx, mo, "r");
        double x = num_prop(cx, mo, "x"), y = num_prop(cx, mo, "y");
        double sx = w / 1638.4, sy = h / 1638.4;
        gm = (swf_matrix){(float)(cos(r) * sx), (float)(sin(r) * sx), (float)(-sin(r) * sy), (float)(cos(r) * sy),
                          (float)((x + w / 2) * 20.0), (float)((y + h / 2) * 20.0)};
    } else {
        gm = (swf_matrix){(float)num_prop(cx, mo, "a"), (float)num_prop(cx, mo, "b"), (float)num_prop(cx, mo, "c"),
                          (float)num_prop(cx, mo, "d"), (float)(num_prop(cx, mo, "tx") * 20.0),
                          (float)(num_prop(cx, mo, "ty") * 20.0)};
    }
    if (!swf_matrix_invert(&gm, &fs.inverse)) fs.gradient = false;
    check(vm, swf_drawing_begin_fill(dr, &fs));
    return swf_undef();
}

static swf_val mc_begin_bitmap_fill(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)argc; (void)argv;
    swf_dobj *d = this_sprite(this_);
    if (!d) return swf_undef();
    vm_warn(cx->vm, WARN_BITMAP_FILL, "avm1: beginBitmapFill is not supported; the fill is drawn grey");
    swf_fill_style fs = {0};
    fs.rgba = 0xFF808080u;
    check(cx->vm, swf_drawing_begin_fill(drawing_of(cx->vm, d), &fs));
    return swf_undef();
}

static swf_val mc_end_fill(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)argc; (void)argv;
    swf_dobj *d = this_sprite(this_);
    if (d) check(cx->vm, swf_drawing_begin_fill(drawing_of(cx->vm, d), NULL));
    return swf_undef();
}

static swf_val mc_move_to(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    swf_dobj *d = this_sprite(this_);
    if (!d || argc < 2) return swf_undef();
    double x = to_number(cx, argv[0]), y = to_number(cx, argv[1]);
    check(cx->vm, swf_drawing_move_to(drawing_of(cx->vm, d), (float)(x * 20), (float)(y * 20)));
    return swf_undef();
}

static swf_val mc_line_to(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    swf_dobj *d = this_sprite(this_);
    if (!d || argc < 2) return swf_undef();
    double x = to_number(cx, argv[0]), y = to_number(cx, argv[1]);
    check(cx->vm, swf_drawing_line_to(drawing_of(cx->vm, d), (float)(x * 20), (float)(y * 20)));
    return swf_undef();
}

static swf_val mc_curve_to(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    swf_dobj *d = this_sprite(this_);
    if (!d || argc < 4) return swf_undef();
    double v[4];
    for (int i = 0; i < 4; i++) v[i] = to_number(cx, argv[i]) * 20.0;
    check(cx->vm, swf_drawing_curve_to(drawing_of(cx->vm, d), (float)v[0], (float)v[1], (float)v[2], (float)v[3]));
    return swf_undef();
}

static swf_val mc_clear(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)cx; (void)argc; (void)argv;
    swf_dobj *d = this_sprite(this_);
    if (d && d->drawing) swf_drawing_clear(d->drawing);
    return swf_undef();
}

static swf_val mc_set_mask(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)this_; (void)argc; (void)argv;
    vm_warn(cx->vm, WARN_SETMASK, "avm1: MovieClip.setMask is not supported yet; content is drawn unmasked");
    return swf_bool(true);
}

static swf_val mc_noop(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)argc; (void)argv; (void)this_;
    vm_warn(cx->vm, WARN_GETURL, "avm1: loadMovie/loadVariables/getURL ignored (network features are out of scope)");
    return swf_undef();
}

void builtins_movieclip(swf_vm *vm)
{
    swf_obj *p = vm->clip_proto;
    const uint16_t A = ATTR_DONT_ENUM | ATTR_DONT_DELETE;
    native_method(vm, p, "attachMovie", mc_attach_movie, A);
    native_method(vm, p, "createEmptyMovieClip", mc_create_empty, A | ATTR_VERSION_6);
    native_method(vm, p, "createTextField", mc_create_text_field, A | ATTR_VERSION_6);
    native_method(vm, p, "duplicateMovieClip", mc_duplicate, A);
    native_method(vm, p, "removeMovieClip", mc_remove, A);
    native_method(vm, p, "swapDepths", mc_swap_depths, A);
    native_method(vm, p, "getNextHighestDepth", mc_next_depth, A | ATTR_VERSION_7);
    native_method(vm, p, "getInstanceAtDepth", mc_instance_at, A | ATTR_VERSION_7);
    native_method(vm, p, "getSWFVersion", mc_swf_version, A);
    native_method(vm, p, "getBounds", mc_get_bounds, A);
    native_method(vm, p, "getRect", mc_get_bounds, A | ATTR_VERSION_8);
    native_method(vm, p, "localToGlobal", mc_local_to_global, A);
    native_method(vm, p, "globalToLocal", mc_global_to_local, A);
    native_method(vm, p, "hitTest", mc_hit_test, A);
    native_method(vm, p, "startDrag", mc_start_drag, A);
    native_method(vm, p, "stopDrag", mc_stop_drag, A);
    native_method(vm, p, "lineStyle", mc_line_style, A | ATTR_VERSION_6);
    native_method(vm, p, "beginFill", mc_begin_fill, A | ATTR_VERSION_6);
    native_method(vm, p, "beginGradientFill", mc_begin_gradient_fill, A | ATTR_VERSION_6);
    native_method(vm, p, "beginBitmapFill", mc_begin_bitmap_fill, A | ATTR_VERSION_8);
    native_method(vm, p, "endFill", mc_end_fill, A | ATTR_VERSION_6);
    native_method(vm, p, "moveTo", mc_move_to, A | ATTR_VERSION_6);
    native_method(vm, p, "lineTo", mc_line_to, A | ATTR_VERSION_6);
    native_method(vm, p, "curveTo", mc_curve_to, A | ATTR_VERSION_6);
    native_method(vm, p, "clear", mc_clear, A | ATTR_VERSION_6);
    native_method(vm, p, "setMask", mc_set_mask, A | ATTR_VERSION_6);
    native_method(vm, p, "loadMovie", mc_noop, A);
    native_method(vm, p, "unloadMovie", mc_noop, A);
    native_method(vm, p, "loadVariables", mc_noop, A);
    native_method(vm, p, "getURL", mc_noop, A);
}
