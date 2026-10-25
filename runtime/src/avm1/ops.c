/* AVM1 operators and the remaining actions, each matching the action of the same name in
 * Ruffle's avm1/activation.rs. Binary operators take (left, right) in stack order: left
 * is the deeper operand. */
#include "vm_internal.h"

#include <math.h>
#include <stdio.h>

bool swf_to_bool(swf_ctx *cx, swf_val v)
{
    return to_bool_v(cx->version, v);
}

swf_val swf_op_add(swf_ctx *cx, swf_val l, swf_val r)
{
    double a = to_number(cx, l);
    return swf_num(a + to_number(cx, r));
}

swf_val swf_op_add2(swf_ctx *cx, swf_val l, swf_val r)
{
    /* Ruffle evaluates the right operand's primitive first. */
    swf_val b = to_primitive(cx, r);
    swf_val a = to_primitive(cx, l);
    if (a.t == SWF_T_STR || b.t == SWF_T_STR) {
        swf_str *sa = to_string(cx, a), *sb = to_string(cx, b);
        return swf_strv(str_concat(cx->vm, sa, sb));
    }
    double y = to_number(cx, b);
    double x = to_number(cx, a);
    return swf_num(x + y);
}

swf_val swf_op_sub(swf_ctx *cx, swf_val l, swf_val r)
{
    double b = to_number(cx, r);
    return swf_num(to_number(cx, l) - b);
}

swf_val swf_op_mul(swf_ctx *cx, swf_val l, swf_val r)
{
    double a = to_number(cx, l);
    return swf_num(a * to_number(cx, r));
}

swf_val swf_op_div(swf_ctx *cx, swf_val l, swf_val r)
{
    double b = to_number(cx, r);
    double a = to_number(cx, l);
    if (b == 0.0 && cx->version < 5) return swf_strv(atom_ascii(cx->vm, "#ERROR#"));
    return swf_num(a / b);
}

swf_val swf_op_mod(swf_ctx *cx, swf_val l, swf_val r)
{
    double b = to_number(cx, r);
    double a = to_number(cx, l);
    return swf_num(fmod(a, b));
}

swf_val swf_op_inc(swf_ctx *cx, swf_val a) { return swf_num(to_number(cx, a) + 1.0); }
swf_val swf_op_dec(swf_ctx *cx, swf_val a) { return swf_num(to_number(cx, a) - 1.0); }

swf_val swf_op_equals(swf_ctx *cx, swf_val l, swf_val r)
{
    double b = to_number(cx, r);
    double a = to_number(cx, l);
    return swf_bool(a == b);
}

swf_val swf_op_equals2(swf_ctx *cx, swf_val l, swf_val r)
{
    return swf_bool(abstract_eq(cx, l, r));
}

swf_val swf_op_strict_equals(swf_ctx *cx, swf_val l, swf_val r)
{
    (void)cx;
    return swf_bool(strict_eq(r, l));
}

swf_val swf_op_less(swf_ctx *cx, swf_val l, swf_val r)
{
    double a = to_number(cx, l);
    double b = to_number(cx, r);
    return swf_bool(a < b);
}

swf_val swf_op_less2(swf_ctx *cx, swf_val l, swf_val r)
{
    return abstract_lt(cx, l, r);
}

swf_val swf_op_greater(swf_ctx *cx, swf_val l, swf_val r)
{
    return abstract_lt(cx, r, l);
}

swf_val swf_op_and(swf_ctx *cx, swf_val l, swf_val r)
{
    return swf_bool(to_bool_v(cx->version, l) && to_bool_v(cx->version, r));
}

swf_val swf_op_or(swf_ctx *cx, swf_val l, swf_val r)
{
    return swf_bool(to_bool_v(cx->version, l) || to_bool_v(cx->version, r));
}

swf_val swf_op_not(swf_ctx *cx, swf_val a)
{
    return swf_bool(!to_bool_v(cx->version, a));
}

swf_val swf_op_bitand(swf_ctx *cx, swf_val l, swf_val r)
{
    int32_t a = to_int32(cx, r);
    return swf_num((double)(to_int32(cx, l) & a));
}

swf_val swf_op_bitor(swf_ctx *cx, swf_val l, swf_val r)
{
    int32_t a = to_int32(cx, r);
    return swf_num((double)(to_int32(cx, l) | a));
}

swf_val swf_op_bitxor(swf_ctx *cx, swf_val l, swf_val r)
{
    int32_t a = to_int32(cx, r);
    return swf_num((double)(to_int32(cx, l) ^ a));
}

swf_val swf_op_lshift(swf_ctx *cx, swf_val l, swf_val r)
{
    int32_t a = to_int32(cx, r) & 31;
    int32_t b = to_int32(cx, l);
    return swf_num((double)(int32_t)((uint32_t)b << a));
}

swf_val swf_op_rshift(swf_ctx *cx, swf_val l, swf_val r)
{
    uint32_t a = to_uint32(cx, r) & 31;
    int32_t b = to_int32(cx, l);
    return swf_num((double)(b >> a));
}

swf_val swf_op_urshift(swf_ctx *cx, swf_val l, swf_val r)
{
    uint32_t a = to_uint32(cx, r) & 31;
    uint32_t b = to_uint32(cx, l) >> a;
    /* SWF 8 and 9 give a signed result. */
    if (cx->version == 8 || cx->version == 9) return swf_num((double)(int32_t)b);
    return swf_num((double)b);
}

swf_val swf_op_string_add(swf_ctx *cx, swf_val l, swf_val r)
{
    swf_str *a = to_string(cx, r);
    swf_str *b = to_string(cx, l);
    return swf_strv(str_concat(cx->vm, b, a));
}

swf_val swf_op_string_equals(swf_ctx *cx, swf_val l, swf_val r)
{
    swf_str *a = to_string(cx, r);
    swf_str *b = to_string(cx, l);
    return swf_bool(str_eq(b, a));
}

swf_val swf_op_string_less(swf_ctx *cx, swf_val l, swf_val r)
{
    swf_str *a = to_string(cx, r);
    swf_str *b = to_string(cx, l);
    return swf_bool(str_cmp(b, a) < 0);
}

swf_val swf_op_string_greater(swf_ctx *cx, swf_val l, swf_val r)
{
    swf_str *b = to_string(cx, l);
    swf_str *a = to_string(cx, r);
    return swf_bool(str_cmp(b, a) > 0);
}

swf_val swf_op_string_length(swf_ctx *cx, swf_val a)
{
    return swf_num(to_string(cx, a)->len);
}

swf_val swf_op_mb_string_length(swf_ctx *cx, swf_val a)
{
    return swf_num(to_string(cx, a)->len);
}

static swf_val extract(swf_ctx *cx, swf_val sv, swf_val index, swf_val count)
{
    int32_t len = to_int32(cx, count);
    int32_t start = to_int32(cx, index);
    swf_str *s = to_string(cx, sv);
    uint32_t st = start >= 1 ? (uint32_t)start - 1 : 0;
    uint32_t end = s->len;
    if (len >= 0 && (uint64_t)st + (uint64_t)len <= s->len) end = st + (uint32_t)len;
    if (st > end) st = end;
    return swf_strv(str_sub(cx->vm, s, st, end));
}

swf_val swf_op_string_extract(swf_ctx *cx, swf_val s, swf_val index, swf_val count)
{
    return extract(cx, s, index, count);
}

swf_val swf_op_mb_string_extract(swf_ctx *cx, swf_val s, swf_val index, swf_val count)
{
    return extract(cx, s, index, count);
}

swf_val swf_op_ascii_to_char(swf_ctx *cx, swf_val a)
{
    uint16_t c = f64_to_u16(to_number(cx, a));
    if (c == 0) return swf_strv(cx->vm->s.empty);
    if (cx->version >= 6 && c >= 0xD800 && c < 0xE000) c = 0xFFFD;
    return swf_strv(str_from_units(cx->vm, &c, 1));
}

swf_val swf_op_mb_ascii_to_char(swf_ctx *cx, swf_val a)
{
    uint16_t c = f64_to_u16(to_number(cx, a));
    if (c == 0) return swf_strv(cx->vm->s.empty);
    if (c >= 0xD800 && c < 0xE000) c = 0xFFFD;
    return swf_strv(str_from_units(cx->vm, &c, 1));
}

static uint16_t code_unit_char(uint16_t c)
{
    return c >= 0xD800 && c < 0xE000 ? 0xFFFD : c;
}

swf_val swf_op_char_to_ascii(swf_ctx *cx, swf_val a)
{
    swf_str *s = to_string(cx, a);
    return swf_num(s->len ? code_unit_char(s->ch[0]) : 0);
}

swf_val swf_op_mb_char_to_ascii(swf_ctx *cx, swf_val a)
{
    swf_str *s = to_string(cx, a);
    if (!s->len) return swf_num(0);
    return swf_num(cx->version < 6 ? (s->ch[0] & 0xFF) : code_unit_char(s->ch[0]));
}

swf_val swf_op_to_integer(swf_ctx *cx, swf_val a) { return swf_num((double)to_int32(cx, a)); }
swf_val swf_op_to_number(swf_ctx *cx, swf_val a) { return swf_num(to_number(cx, a)); }
swf_val swf_op_to_string(swf_ctx *cx, swf_val a) { return swf_strv(to_string(cx, a)); }
swf_val swf_op_typeof(swf_ctx *cx, swf_val a) { return swf_strv(type_of(cx, a)); }

swf_val swf_op_instanceof(swf_ctx *cx, swf_val obj, swf_val ctor)
{
    return swf_bool(instance_of(cx, obj, ctor));
}

swf_val swf_op_cast(swf_ctx *cx, swf_val ctor, swf_val obj)
{
    if (obj.t != SWF_T_OBJ) (void)to_object(cx, obj);
    return instance_of(cx, obj, ctor) ? obj : swf_null();
}

/* Flash's random(): a 32-bit generator modulo the bound. */
swf_val swf_op_random(swf_ctx *cx, swf_val max)
{
    double m = to_number(cx, max);
    int32_t n = isfinite(m) ? (int32_t)m : 0;
    if (n <= 0) return swf_num(0);
    uint32_t x = cx->vm->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    cx->vm->rng = x;
    return swf_num((double)((x & 0x7FFFFFFF) % (uint32_t)n));
}

void swf_op_trace(swf_ctx *cx, swf_val v)
{
    swf_vm *vm = cx->vm;
    swf_str *s = v.t == SWF_T_UNDEF ? vm->s.undefined : to_string(cx, v);
    char *u = str_to_utf8(s, NULL);
    if (!u) return;
    for (char *c = u; *c; c++)
        if (*c == 13) *c = 10; /* Flash prints CR as a line break */
    vm->trace(u, vm->trace_user);
    free(u);
}

/* --- display-related actions ---------------------------------------------------- */

static swf_dobj *target_or_root(swf_ctx *cx)
{
    return ctx_target(cx) ? ctx_target(cx) : clip_root(cx->vm);
}

static swf_dobj *target_sprite(swf_ctx *cx)
{
    swf_dobj *t = ctx_target(cx);
    return t && t->kind == SWF_CHAR_SPRITE ? t : NULL;
}

swf_val swf_get_property(swf_ctx *cx, swf_val target, swf_val index)
{
    double i = to_number(cx, index);
    swf_dobj *d = ctx_target(cx) ? resolve_target_clip(cx, ctx_target(cx), target, true)
                                 : resolve_target_clip(cx, ctx_base(cx), target, false);
    if (!d || !isfinite(i) || i <= -1.0) return swf_undef();
    return clip_get_property_index(cx, d, (int)i);
}

void swf_set_property(swf_ctx *cx, swf_val target, swf_val index, swf_val v)
{
    double i = to_number(cx, index);
    swf_dobj *d = ctx_target(cx) ? resolve_target_clip(cx, ctx_target(cx), target, true)
                                 : resolve_target_clip(cx, ctx_base(cx), target, false);
    if (!d || !isfinite(i) || i <= -1.0) return;
    clip_set_property_index(cx, d, (int)i, v);
}

void swf_act_goto_frame(swf_ctx *cx, uint32_t frame0)
{
    swf_dobj *d = target_sprite(cx);
    if (!d) return;
    swf_clip_stop(d);
    swf_clip_goto(cx->vm->movie, d, frame0 + 1);
}

void swf_act_goto_frame2(swf_ctx *cx, swf_val frame, bool play, uint32_t scene_bias)
{
    swf_dobj *d = target_or_root(cx);
    if (d->kind != SWF_CHAR_SPRITE) return;
    clip_goto(cx, d, frame, play, scene_bias);
}

void swf_act_goto_label(swf_ctx *cx, uint32_t a)
{
    swf_dobj *d = target_sprite(cx);
    if (!d) return;
    char *u = str_to_utf8(cx->vm->code->atoms[a], NULL);
    if (!u) return;
    int64_t f = swf_clip_find_label(cx->vm->movie, d, u);
    free(u);
    if (f < 1) return;
    swf_clip_stop(d);
    swf_clip_goto(cx->vm->movie, d, (uint32_t)f);
}

void swf_act_next_frame(swf_ctx *cx)
{
    swf_dobj *d = target_sprite(cx);
    if (!d) return;
    swf_clip_stop(d);
    if (d->frame < swf_clip_total_frames(cx->vm->movie, d)) swf_clip_goto(cx->vm->movie, d, d->frame + 1);
}

void swf_act_prev_frame(swf_ctx *cx)
{
    swf_dobj *d = target_sprite(cx);
    if (!d) return;
    swf_clip_stop(d);
    if (d->frame > 1) swf_clip_goto(cx->vm->movie, d, d->frame - 1);
}

void swf_act_play(swf_ctx *cx)
{
    swf_dobj *d = target_sprite(cx);
    if (d) swf_clip_play(d);
}

void swf_act_stop(swf_ctx *cx)
{
    swf_dobj *d = target_sprite(cx);
    if (d) swf_clip_stop(d);
}

void swf_act_toggle_quality(swf_ctx *cx)
{
    vm_warn(cx->vm, WARN_QUALITY, "avm1: toggleHighQuality ignored (the renderer has one quality)");
}

static void set_target_clip(swf_ctx *cx, swf_dobj *d, bool valid)
{
    cx->target_o = d ? clip_object(cx->vm, d) : NULL;
    cx->target_valid = valid;
    swf_dobj *scope_clip = d ? d : clip_root(cx->vm);
    cx->scope = scope_with_target(cx->vm, cx->scope, clip_object(cx->vm, scope_clip));
}

void swf_act_set_target(swf_ctx *cx, uint32_t a)
{
    swf_str *t = cx->vm->code->atoms[a];
    if (t->len == 0) {
        set_target_clip(cx, ctx_base(cx), true);
        return;
    }
    swf_obj *o = resolve_target_path(cx, clip_root(cx->vm), clip_object(cx->vm, ctx_base(cx)), t, 0, t->len, true, false);
    swf_dobj *d = o && o->cls == OC_CLIP ? o->u.clip : NULL;
    set_target_clip(cx, d, d != NULL);
}

void swf_act_set_target2(swf_ctx *cx, swf_val target)
{
    if (target.t == SWF_T_UNDEF) {
        set_target_clip(cx, cx->version > 6 ? NULL : ctx_base(cx), cx->version <= 6);
        return;
    }
    if (target.t == SWF_T_OBJ && target.u.o->cls == OC_CLIP) {
        set_target_clip(cx, target.u.o->u.clip, true);
        return;
    }
    swf_str *t = to_string(cx, target);
    if (t->len == 0) {
        set_target_clip(cx, ctx_base(cx), true);
        return;
    }
    swf_obj *o = resolve_target_path(cx, clip_root(cx->vm), clip_object(cx->vm, ctx_base(cx)), t, 0, t->len, true, false);
    swf_dobj *d = o && o->cls == OC_CLIP ? o->u.clip : NULL;
    set_target_clip(cx, d, d != NULL);
}

void swf_act_remove_sprite(swf_ctx *cx, swf_val target)
{
    swf_dobj *d = resolve_target_clip(cx, target_or_root(cx), target, true);
    /* Only clips created by script (depth >= 0 in AVM terms) can be removed. */
    if (!d || !d->parent || d->depth < 16384) return;
    swf_clip_remove_child(cx->vm->movie, d->parent, d->depth);
}

swf_val swf_act_target_path(swf_ctx *cx, swf_val v)
{
    swf_obj *o = to_object_or_bare(cx, v);
    if (o->cls == OC_CLIP && o->u.clip) return swf_strv(clip_path(cx->vm, o->u.clip));
    return swf_undef();
}

void swf_act_call(swf_ctx *cx, swf_val frame)
{
    (void)frame;
    vm_warn(cx->vm, WARN_CLONE, "avm1: call(frame) is not supported yet");
}

void swf_act_get_url(swf_ctx *cx, uint32_t url, uint32_t target)
{
    (void)url; (void)target;
    vm_warn(cx->vm, WARN_GETURL, "avm1: getURL/fscommand ignored (network features are out of scope)");
}

void swf_act_get_url2(swf_ctx *cx, swf_val url, swf_val target, uint8_t flags)
{
    (void)url; (void)target; (void)flags;
    vm_warn(cx->vm, WARN_GETURL, "avm1: getURL/loadMovie ignored (network features are out of scope)");
}

bool swf_act_frame_loaded(swf_ctx *cx, uint32_t frame)
{
    (void)cx;
    return frame <= 16000;
}

bool swf_act_frame_loaded2(swf_ctx *cx, swf_val frame)
{
    (void)cx; (void)frame;
    return true;
}

void swf_act_unknown(swf_ctx *cx, uint8_t opcode)
{
    vm_warn(cx->vm, WARN_UNKNOWN_OP, "avm1: unknown action 0x%02X skipped", opcode);
}

bool swf_base_removed(swf_ctx *cx)
{
    return ctx_base(cx) == NULL;
}
