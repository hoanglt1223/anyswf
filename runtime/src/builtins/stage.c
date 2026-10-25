/* Stage (size, scaleMode, align, listeners) and Color (setRGB/setTransform on a clip's
 * colour transform), following Ruffle's avm1/globals/stage.rs and color.rs
 * (MIT/Apache-2.0). */
#include "../avm1/vm_internal.h"

#include <math.h>

#define DD (ATTR_DONT_ENUM | ATTR_DONT_DELETE)

/* --- Stage ------------------------------------------------------------------------------ */

static swf_val stage_width(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)this_; (void)argc; (void)argv;
    const swf_movie *m = cx->vm->movie;
    if (m->scale_mode == SWF_SCALE_NO_SCALE) return swf_num(m->assets->screen_w);
    return swf_num((m->assets->stage[2] - m->assets->stage[0]) / 20.0);
}

static swf_val stage_height(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)this_; (void)argc; (void)argv;
    const swf_movie *m = cx->vm->movie;
    if (m->scale_mode == SWF_SCALE_NO_SCALE) return swf_num(m->assets->screen_h);
    return swf_num((m->assets->stage[3] - m->assets->stage[1]) / 20.0);
}

static const char *const SCALE_NAMES[] = {"showAll", "exactFit", "noBorder", "noScale"};

static swf_val stage_get_scale(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)this_; (void)argc; (void)argv;
    uint8_t s = cx->vm->movie->scale_mode;
    return swf_strv(atom_ascii(cx->vm, SCALE_NAMES[s < 4 ? s : 0]));
}

static swf_val stage_set_scale(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)this_;
    if (argc < 1) return swf_undef();
    swf_str *s = to_string(cx, argv[0]);
    for (uint8_t i = 0; i < 4; i++) {
        swf_str *n = atom_ascii(cx->vm, SCALE_NAMES[i]);
        if (str_eq_nocase(s, n)) {
            cx->vm->movie->scale_mode = i;
            break;
        }
    }
    return swf_undef();
}

static swf_val stage_get_align(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)this_; (void)argc; (void)argv;
    return swf_strv(cx->vm->s.empty);
}

static swf_val stage_true(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)cx; (void)this_; (void)argc; (void)argv;
    return swf_bool(true);
}

static swf_val stage_ignore(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)cx; (void)this_; (void)argc; (void)argv;
    return swf_undef();
}

static void prop(swf_vm *vm, swf_obj *o, const char *name, swf_native_fn get, swf_native_fn set)
{
    obj_add_property(vm, o, atom_ascii(vm, name), func_new_native(vm, get, NULL, NULL),
                     set ? func_new_native(vm, set, NULL, NULL) : NULL, DD);
}

/* --- Color ------------------------------------------------------------------------------- */

static swf_dobj *color_target(swf_ctx *cx, swf_val this_)
{
    swf_obj *o = to_object(cx, this_);
    if (!o) return NULL;
    swf_val t = obj_get(cx, o, atom_ascii(cx->vm, "target"));
    swf_dobj *base = ctx_base(cx) ? ctx_base(cx) : clip_root(cx->vm);
    if (is_nullish(t)) return NULL;
    swf_dobj *d = resolve_target_clip(cx, base, t, false);
    return d && !d->removed ? d : NULL;
}

static swf_val color_ctor(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    swf_obj *o = to_object(cx, this_);
    if (o) {
        obj_define(cx->vm, o, atom_ascii(cx->vm, "target"), arg(argc, argv, 0), ATTR_DONT_ENUM | ATTR_DONT_DELETE |
                                                                                    ATTR_READ_ONLY);
    }
    return swf_undef();
}

static swf_val color_get_rgb(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)argc; (void)argv;
    swf_dobj *d = color_target(cx, this_);
    if (!d) return swf_undef();
    int r = (int)d->cxform.add[0], g = (int)d->cxform.add[1], b = (int)d->cxform.add[2];
    return swf_num((double)(((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF)));
}

static swf_val color_set_rgb(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    swf_dobj *d = color_target(cx, this_);
    if (!d || argc < 1) return swf_undef();
    uint32_t rgb = (uint32_t)to_int32(cx, argv[0]);
    d->cxform.mul[0] = d->cxform.mul[1] = d->cxform.mul[2] = 0;
    d->cxform.add[0] = (float)((rgb >> 16) & 0xFF);
    d->cxform.add[1] = (float)((rgb >> 8) & 0xFF);
    d->cxform.add[2] = (float)(rgb & 0xFF);
    return swf_undef();
}

static const char *const CT_MUL[] = {"ra", "ga", "ba", "aa"};
static const char *const CT_ADD[] = {"rb", "gb", "bb", "ab"};

static swf_val color_get_transform(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)argc; (void)argv;
    swf_dobj *d = color_target(cx, this_);
    if (!d) return swf_undef();
    swf_obj *o = obj_new(cx->vm, cx->vm->object_proto);
    for (int i = 0; i < 4; i++) {
        obj_set(cx, o, atom_ascii(cx->vm, CT_MUL[i]), swf_num(round(d->cxform.mul[i] * 256.0) / 2.56));
        obj_set(cx, o, atom_ascii(cx->vm, CT_ADD[i]), swf_num(d->cxform.add[i]));
    }
    return swf_objv(o);
}

static swf_val color_set_transform(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    swf_dobj *d = color_target(cx, this_);
    if (!d || argc < 1 || argv[0].t != SWF_T_OBJ) return swf_undef();
    swf_obj *t = argv[0].u.o;
    for (int i = 0; i < 4; i++) {
        swf_val m = obj_get(cx, t, atom_ascii(cx->vm, CT_MUL[i]));
        swf_val a = obj_get(cx, t, atom_ascii(cx->vm, CT_ADD[i]));
        /* Flash keeps the multiplier in 8.8 fixed point and the offset as an integer. */
        if (m.t != SWF_T_UNDEF) d->cxform.mul[i] = (float)((int16_t)f64_to_i32(to_number(cx, m) * 2.56)) / 256.0f;
        if (a.t != SWF_T_UNDEF) d->cxform.add[i] = (float)(int16_t)f64_to_i32(to_number(cx, a));
    }
    return swf_undef();
}

void builtins_stage(swf_vm *vm)
{
    swf_obj *s = obj_new(vm, vm->object_proto);
    vm->stage_obj = s;
    obj_define_ascii(vm, vm->global, "Stage", swf_objv(s), ATTR_DONT_ENUM);
    prop(vm, s, "width", stage_width, NULL);
    prop(vm, s, "height", stage_height, NULL);
    prop(vm, s, "scaleMode", stage_get_scale, stage_set_scale);
    prop(vm, s, "align", stage_get_align, stage_ignore);
    prop(vm, s, "showMenu", stage_true, stage_ignore);
    broadcaster_init(vm, s);

    swf_obj *cp = obj_new(vm, vm->object_proto);
    vm->color_proto = cp;
    swf_obj *c = func_new_native(vm, color_ctor, NULL, cp);
    c->u.fn.ctor = NULL;
    obj_define_ascii(vm, vm->global, "Color", swf_objv(c), ATTR_DONT_ENUM);
    const uint16_t A = DD | ATTR_READ_ONLY;
    native_method(vm, cp, "getRGB", color_get_rgb, A);
    native_method(vm, cp, "setRGB", color_set_rgb, A);
    native_method(vm, cp, "getTransform", color_get_transform, A);
    native_method(vm, cp, "setTransform", color_set_transform, A);
}
