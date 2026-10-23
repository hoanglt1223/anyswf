/* String, ported from Ruffle's avm1/globals/string.rs (MIT/Apache-2.0). Indices are
 * UTF-16 code units, as in Flash. */
#include "vm_internal.h"

#include <math.h>

#define DD (ATTR_DONT_ENUM | ATTR_DONT_DELETE)

static swf_str *this_string(swf_ctx *cx, swf_val this_)
{
    return to_string(cx, swf_objv(to_object_or_bare(cx, this_)));
}

static uint32_t clamp_index(int32_t i, uint32_t len)
{
    return i < 0 ? 0 : (uint32_t)i < len ? (uint32_t)i : len;
}

static uint32_t wrap_index(int32_t i, uint32_t len)
{
    if (i < 0) {
        int64_t r = (int64_t)len + i;
        return r < 0 ? 0 : (uint32_t)r;
    }
    return (uint32_t)i < len ? (uint32_t)i : len;
}

static swf_val string_fn(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)this_;
    if (!argc) return swf_strv(cx->vm->s.empty);
    return swf_strv(to_string(cx, argv[0]));
}

static swf_val string_ctor(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    swf_obj *o = to_object_or_bare(cx, this_);
    swf_str *s = argc ? to_string(cx, argv[0]) : cx->vm->s.empty;
    o->cls = OC_STRING;
    o->u.prim = swf_strv(s);
    obj_define(cx->vm, o, cx->vm->s.length, swf_num(s->len), DD);
    return swf_objv(o);
}

static swf_val s_to_string(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)cx; (void)argc; (void)argv;
    if (this_.t == SWF_T_STR) return this_;
    if (this_.t == SWF_T_OBJ && this_.u.o->cls == OC_STRING) return this_.u.o->u.prim;
    return swf_undef();
}

static swf_val s_char_at(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    swf_str *s = this_string(cx, this_);
    int32_t i = to_int32(cx, arg(argc, argv, 0));
    if (i < 0 || (uint32_t)i >= s->len) return swf_strv(cx->vm->s.empty);
    return swf_strv(str_from_units(cx->vm, s->ch + i, 1));
}

static swf_val s_char_code_at(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    swf_str *s = this_string(cx, this_);
    int32_t i = to_int32(cx, arg(argc, argv, 0));
    if (i < 0) return swf_num(NAN);
    if ((uint32_t)i >= s->len) return swf_num(cx->version == 5 ? 0 : NAN);
    return swf_num(s->ch[i]);
}

static swf_val s_concat(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    swf_str *r = this_string(cx, this_);
    for (uint32_t i = 0; i < argc; i++) r = str_concat(cx->vm, r, to_string(cx, argv[i]));
    return swf_strv(r);
}

static swf_val s_from_char_code(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)this_;
    uint16_t *buf = (uint16_t *)swf_frame_alloc(cx, argc / 4 + 1);
    uint32_t n = 0;
    for (uint32_t i = 0; i < argc; i++) {
        uint16_t c = f64_to_u16(to_number(cx, argv[i]));
        if (c == 0) break;
        buf[n++] = c;
    }
    return swf_strv(str_from_units(cx->vm, buf, n));
}

static swf_val s_index_of(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    swf_str *s = this_string(cx, this_);
    if (argc < 1) return swf_undef();
    swf_str *pat = to_string(cx, argv[0]);
    uint32_t start = 0;
    if (argc > 1 && argv[1].t != SWF_T_UNDEF) {
        int32_t v = to_int32(cx, argv[1]);
        start = v < 0 ? 0 : (uint32_t)v;
    }
    if (start > s->len) return swf_num(-1);
    return swf_num((double)str_find(s, pat, start));
}

static swf_val s_last_index_of(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    swf_str *s = this_string(cx, this_);
    if (argc < 1) return swf_undef();
    swf_str *pat = to_string(cx, argv[0]);
    uint64_t end = s->len;
    if (argc > 1 && argv[1].t != SWF_T_UNDEF) {
        int32_t v = to_int32(cx, argv[1]);
        if (v < 0) return swf_num(-1);
        end = (uint64_t)v + pat->len;
    }
    if (end > s->len) end = s->len;
    swf_str *hay = str_sub(cx->vm, s, 0, (uint32_t)end);
    return swf_num((double)str_rfind(hay, pat, (int64_t)hay->len));
}

static swf_val s_slice(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    if (argc < 1) return swf_undef();
    swf_str *s = this_string(cx, this_);
    uint32_t a = wrap_index(to_int32(cx, argv[0]), s->len);
    uint32_t b = argc > 1 && argv[1].t != SWF_T_UNDEF ? wrap_index(to_int32(cx, argv[1]), s->len) : s->len;
    return swf_strv(a < b ? str_sub(cx->vm, s, a, b) : cx->vm->s.empty);
}

static swf_val s_substr(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    if (argc < 1) return swf_undef();
    swf_str *s = this_string(cx, this_);
    uint32_t a = wrap_index(to_int32(cx, argv[0]), s->len);
    int32_t len = argc > 1 && argv[1].t != SWF_T_UNDEF ? to_int32(cx, argv[1]) : (int32_t)s->len;
    uint32_t b = wrap_index((int32_t)((uint32_t)a + (uint32_t)len), s->len);
    return swf_strv(a < b ? str_sub(cx->vm, s, a, b) : cx->vm->s.empty);
}

static swf_val s_substring(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    if (argc < 1) return swf_undef();
    swf_str *s = this_string(cx, this_);
    uint32_t a = clamp_index(to_int32(cx, argv[0]), s->len);
    uint32_t b = argc > 1 && argv[1].t != SWF_T_UNDEF ? clamp_index(to_int32(cx, argv[1]), s->len) : s->len;
    if (b < a) {
        uint32_t t = a;
        a = b;
        b = t;
    }
    return swf_strv(str_sub(cx->vm, s, a, b));
}

static swf_val s_split(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    swf_vm *vm = cx->vm;
    swf_str *s = this_string(cx, this_);
    uint64_t limit = UINT32_MAX;
    swf_val lv = arg(argc, argv, 1);
    if (lv.t != SWF_T_UNDEF) {
        int32_t l = to_int32(cx, lv);
        limit = l < 0 ? 0 : (uint64_t)l;
    }
    bool swf5 = cx->version == 5;
    swf_val dv = arg(argc, argv, 0);
    swf_str *delim = NULL;
    if (dv.t == SWF_T_UNDEF) {
        if (swf5) delim = atom_ascii(vm, ",");
    } else {
        delim = to_string(cx, dv);
        if (swf5 && delim->len == 0) delim = NULL;
    }
    if (!delim) {
        swf_val one = swf_strv(s);
        return swf_objv(array_new(cx, 1, &one));
    }
    uint32_t cap = s->len + 1;
    swf_val *parts = swf_frame_alloc(cx, cap);
    uint32_t n = 0;
    if (delim->len == 0) {
        for (uint32_t i = 0; i < s->len && n < limit; i++) parts[n++] = swf_strv(str_from_units(vm, s->ch + i, 1));
    } else {
        uint32_t start = 0;
        for (;;) {
            if (n >= limit) break;
            int64_t f = str_find(s, delim, start);
            if (f < 0) {
                parts[n++] = swf_strv(str_sub(vm, s, start, s->len));
                break;
            }
            parts[n++] = swf_strv(str_sub(vm, s, start, (uint32_t)f));
            start = (uint32_t)f + delim->len;
        }
    }
    return swf_objv(array_new(cx, n, parts));
}

/* Flash's case mapping covers Latin-1 and a few other ranges; ASCII and Latin-1 are
 * handled here, which covers the characters ActionScript sources use in practice. */
static uint16_t lower(uint16_t c)
{
    if ((c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7)) return (uint16_t)(c + 32);
    return c;
}

static uint16_t upper(uint16_t c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7)) return (uint16_t)(c - 32);
    return c;
}

static swf_val s_to_lower(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)argc; (void)argv;
    swf_str *s = this_string(cx, this_);
    swf_str *r = str_new(cx->vm, s->len);
    for (uint32_t i = 0; i < s->len; i++) r->ch[i] = lower(s->ch[i]);
    return swf_strv(r);
}

static swf_val s_to_upper(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)argc; (void)argv;
    swf_str *s = this_string(cx, this_);
    swf_str *r = str_new(cx->vm, s->len);
    for (uint32_t i = 0; i < s->len; i++) r->ch[i] = upper(s->ch[i]);
    return swf_strv(r);
}

void builtins_string(swf_vm *vm)
{
    swf_obj *p = obj_new(vm, vm->object_proto);
    native_method(vm, p, "toString", s_to_string, DD);
    native_method(vm, p, "valueOf", s_to_string, DD);
    native_method(vm, p, "charAt", s_char_at, DD);
    native_method(vm, p, "charCodeAt", s_char_code_at, DD);
    native_method(vm, p, "concat", s_concat, DD);
    native_method(vm, p, "indexOf", s_index_of, DD);
    native_method(vm, p, "lastIndexOf", s_last_index_of, DD);
    native_method(vm, p, "slice", s_slice, DD);
    native_method(vm, p, "substring", s_substring, DD);
    native_method(vm, p, "split", s_split, DD);
    native_method(vm, p, "substr", s_substr, DD);
    native_method(vm, p, "toUpperCase", s_to_upper, DD);
    native_method(vm, p, "toLowerCase", s_to_lower, DD);
    vm->string_proto = p;
    swf_obj *c = make_class(vm, "String", string_fn, string_ctor, p);
    native_method(vm, c, "fromCharCode", s_from_char_code, DD);
    vm->string_ctor = c;
}
