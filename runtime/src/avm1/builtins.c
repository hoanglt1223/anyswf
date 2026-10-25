/* Core built-ins: Object, Function, Boolean, Number, Math, Error and the global
 * functions. Behaviour follows Ruffle's avm1/globals (MIT/Apache-2.0). */
#include "vm_internal.h"

#include <math.h>
#include <stdio.h>

#define DD (ATTR_DONT_ENUM | ATTR_DONT_DELETE)

swf_obj *make_class(swf_vm *vm, const char *name, swf_native_fn fn, swf_native_fn ctor, swf_obj *proto)
{
    swf_obj *c = func_new_native(vm, fn, ctor, proto);
    obj_define(vm, vm->global, atom_ascii(vm, name), swf_objv(c), ATTR_DONT_ENUM);
    return c;
}

static swf_obj *this_obj(swf_ctx *cx, swf_val this_)
{
    return to_object_or_bare(cx, this_);
}

/* --- Object ---------------------------------------------------------------------- */

static swf_val object_ctor(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    swf_val v = arg(argc, argv, 0);
    if (is_nullish(v)) return swf_objv(this_obj(cx, this_));
    return swf_objv(to_object_or_bare(cx, v));
}

static swf_val object_fn(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)this_;
    swf_val v = arg(argc, argv, 0);
    if (is_nullish(v)) return swf_objv(obj_new_bare(cx->vm));
    return swf_objv(to_object_or_bare(cx, v));
}

static swf_val object_value_of(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)argc; (void)argv;
    return swf_objv(this_obj(cx, this_));
}

static swf_val object_to_string(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)argc; (void)argv;
    swf_obj *o = this_obj(cx, this_);
    return swf_strv(o->cls == OC_FUNCTION ? cx->vm->s.type_Function : atom_ascii(cx->vm, "[object Object]"));
}

static swf_val object_to_locale_string(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)argc; (void)argv;
    return swf_strv(to_string(cx, swf_objv(this_obj(cx, this_))));
}

static swf_val object_has_own(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    if (argc < 1) return swf_bool(false);
    return swf_bool(obj_has_own(cx, this_obj(cx, this_), to_string(cx, argv[0])));
}

static swf_val object_is_enumerable(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    if (argc < 1) return swf_bool(false);
    swf_obj *o = this_obj(cx, this_);
    swf_prop *p = obj_own(cx->vm, o, atom(cx->vm, to_string(cx, argv[0])));
    return swf_bool(p && !(p->attr & ATTR_DONT_ENUM));
}

static swf_val object_is_prototype_of(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    if (argc < 1) return swf_bool(false);
    swf_obj *self = this_obj(cx, this_);
    swf_obj *ob = to_object_or_bare(cx, argv[0]);
    swf_val p = obj_proto(cx, ob);
    for (int i = 0; p.t == SWF_T_OBJ && i < 256; i++) {
        if (p.u.o == self) return swf_bool(true);
        p = obj_proto(cx, p.u.o);
    }
    return swf_bool(false);
}

static swf_val object_add_property(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    if (argc < 1) return swf_bool(false);
    swf_obj *o = this_obj(cx, this_);
    swf_str *name = to_string(cx, argv[0]);
    swf_val get = arg(argc, argv, 1), set = arg(argc, argv, 2);
    if (get.t != SWF_T_OBJ || name->len == 0) return swf_bool(false);
    if (set.t == SWF_T_OBJ) obj_add_property(cx->vm, o, name, get.u.o, set.u.o, 0);
    else if (set.t == SWF_T_NULL) obj_add_property(cx->vm, o, name, get.u.o, NULL, ATTR_READ_ONLY);
    else return swf_bool(false);
    return swf_bool(true);
}

static swf_val object_watch(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    swf_vm *vm = cx->vm;
    if (argc < 1) return swf_bool(false);
    swf_obj *o = this_obj(cx, this_);
    swf_str *name = to_string(cx, argv[0]);
    swf_obj *cb = to_object_or_bare(cx, arg(argc, argv, 1));
    if (cb->cls != OC_FUNCTION) return swf_bool(false);
    if (!o->watchers) o->watchers = obj_new_bare(vm);
    swf_obj *rec = obj_new_bare(vm);
    obj_define(vm, rec, vm->s.callee, swf_objv(cb), 0);
    obj_define(vm, rec, vm->s.arguments, arg(argc, argv, 2), 0);
    obj_define(vm, o->watchers, name, swf_objv(rec), 0);
    return swf_bool(true);
}

static swf_val object_unwatch(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    if (argc < 1) return swf_bool(false);
    swf_obj *o = this_obj(cx, this_);
    if (!o->watchers) return swf_bool(false);
    return swf_bool(obj_delete(cx, o->watchers, to_string(cx, argv[0])));
}

static swf_val object_register_class(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)this_;
    if (argc < 2) return swf_bool(false);
    if (!is_nullish(argv[1]) && !is_function(argv[1])) return swf_bool(false);
    /* Links an exported symbol to a class: its instances get the class prototype and
     * constructor (placed by the timeline or by attachMovie). */
    swf_vm *vm = cx->vm;
    char *name = str_to_utf8(to_string(cx, argv[0]), NULL);
    if (!name) return swf_bool(false);
    const swf_assets *a = vm->movie->assets;
    for (uint32_t i = 0; i < a->export_count; i++) {
        if (strcmp(a->exports[i].name, name) != 0) continue;
        swf_str *key = atom_int(vm, a->exports[i].char_id);
        if (is_nullish(argv[1])) obj_delete(cx, vm->classes, key);
        else obj_define(vm, vm->classes, key, argv[1], 0);
    }
    free(name);
    return swf_bool(true);
}

void builtins_object(swf_vm *vm)
{
    swf_obj *p = vm->object_proto;
    native_method(vm, p, "watch", object_watch, DD | ATTR_VERSION_6);
    native_method(vm, p, "unwatch", object_unwatch, DD | ATTR_VERSION_6);
    native_method(vm, p, "addProperty", object_add_property, DD | ATTR_VERSION_6);
    native_method(vm, p, "valueOf", object_value_of, DD);
    native_method(vm, p, "toString", object_to_string, DD);
    native_method(vm, p, "hasOwnProperty", object_has_own, DD | ATTR_VERSION_6);
    native_method(vm, p, "isPrototypeOf", object_is_prototype_of, DD | ATTR_VERSION_6);
    native_method(vm, p, "isPropertyEnumerable", object_is_enumerable, DD | ATTR_VERSION_6);
    native_method(vm, p, "toLocaleString", object_to_locale_string, DD);
    swf_obj *c = make_class(vm, "Object", object_fn, object_ctor, p);
    native_method(vm, c, "registerClass", object_register_class, DD | ATTR_READ_ONLY);
    vm->object_ctor = c;
}

/* --- Function ------------------------------------------------------------------- */

static swf_val function_fn(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)this_;
    return argc ? argv[0] : swf_objv(obj_new_bare(cx->vm));
}

static swf_val function_ctor(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)cx;
    return argc ? argv[0] : this_;
}

static swf_obj *call_this(swf_ctx *cx, uint32_t argc, const swf_val *argv)
{
    swf_obj *t = argc ? to_object(cx, argv[0]) : NULL;
    return t ? t : cx->vm->global;
}

static swf_val function_call(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    if (this_.t != SWF_T_OBJ || this_.u.o->cls != OC_FUNCTION) return swf_undef();
    swf_obj *t = call_this(cx, argc, argv);
    return func_call(cx, this_.u.o, swf_objv(t), argc > 1 ? argc - 1 : 0, argc > 1 ? argv + 1 : NULL, 1, false);
}

static swf_val function_apply(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    if (this_.t != SWF_T_OBJ || this_.u.o->cls != OC_FUNCTION) return swf_undef();
    swf_obj *t = call_this(cx, argc, argv);
    swf_val a = arg(argc, argv, 1);
    uint32_t n = 0;
    swf_val *list = NULL;
    if (a.t == SWF_T_OBJ) {
        n = to_uint32(cx, obj_get(cx, a.u.o, cx->vm->s.length));
        if (n > 0x10000) n = 0x10000;
        list = swf_frame_alloc(cx, n ? n : 1);
        for (uint32_t i = 0; i < n; i++) list[i] = obj_get(cx, a.u.o, atom_int(cx->vm, i));
    }
    return func_call(cx, this_.u.o, swf_objv(t), n, list, 1, false);
}

void builtins_function(swf_vm *vm)
{
    swf_obj *p = vm->function_proto;
    native_method(vm, p, "call", function_call, DD);
    native_method(vm, p, "apply", function_apply, DD);
    vm->function_ctor = make_class(vm, "Function", function_fn, function_ctor, p);
}

/* --- Boolean ---------------------------------------------------------------------- */

static swf_val boolean_fn(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)this_;
    return argc ? swf_bool(to_bool_v(cx->version, argv[0])) : swf_undef();
}

static swf_val boolean_ctor(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    swf_obj *o = this_obj(cx, this_);
    o->cls = OC_BOOLEAN;
    o->u.prim = swf_bool(argc ? to_bool_v(cx->version, argv[0]) : false);
    return swf_objv(o);
}

static swf_val boolean_value(swf_ctx *cx, swf_val this_)
{
    (void)cx;
    if (this_.t == SWF_T_BOOL) return this_;
    if (this_.t == SWF_T_OBJ && this_.u.o->cls == OC_BOOLEAN) return this_.u.o->u.prim;
    return swf_undef();
}

static swf_val boolean_to_string(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)argc; (void)argv;
    swf_val v = boolean_value(cx, this_);
    if (v.t != SWF_T_BOOL) return swf_undef();
    return swf_strv(v.u.b ? cx->vm->s.true_ : cx->vm->s.false_);
}

static swf_val boolean_value_of(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)argc; (void)argv;
    return boolean_value(cx, this_);
}

/* --- Number ---------------------------------------------------------------------- */

static swf_val number_fn(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)this_;
    return swf_num(argc ? to_number(cx, argv[0]) : 0.0);
}

static swf_val number_ctor(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    swf_obj *o = this_obj(cx, this_);
    o->cls = OC_NUMBER;
    o->u.prim = swf_num(argc ? to_number(cx, argv[0]) : 0.0);
    return swf_objv(o);
}

static bool number_value(swf_val this_, double *out)
{
    if (this_.t == SWF_T_NUM) {
        *out = this_.u.n;
        return true;
    }
    if (this_.t == SWF_T_OBJ && this_.u.o->cls == OC_NUMBER) {
        *out = this_.u.o->u.prim.u.n;
        return true;
    }
    return false;
}

static swf_val number_to_string_m(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    double n;
    if (!number_value(this_, &n)) return swf_undef();
    int32_t radix = argc && argv[0].t != SWF_T_UNDEF ? to_int32(cx, argv[0]) : 10;
    if (radix < 2 || radix > 36 || radix == 10 || !isfinite(n)) return swf_strv(number_to_string(cx->vm, n));
    /* Other radixes only print the integer part (as a wrapped 32-bit value). */
    int32_t v = f64_to_i32(n);
    bool neg = v < 0;
    uint32_t u = neg ? (uint32_t)(-(int64_t)v) : (uint32_t)v;
    char buf[40];
    int len = 0;
    do {
        uint32_t d = u % (uint32_t)radix;
        buf[len++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        u /= (uint32_t)radix;
    } while (u);
    if (neg) buf[len++] = '-';
    for (int i = 0; i < len / 2; i++) {
        char t = buf[i];
        buf[i] = buf[len - 1 - i];
        buf[len - 1 - i] = t;
    }
    return swf_strv(str_from_utf8(cx->vm, buf, (size_t)len));
}

static swf_val number_value_of(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)cx; (void)argc; (void)argv;
    double n;
    return number_value(this_, &n) ? swf_num(n) : swf_undef();
}

void builtins_number(swf_vm *vm)
{
    swf_obj *bp = obj_new(vm, vm->object_proto);
    native_method(vm, bp, "toString", boolean_to_string, DD);
    native_method(vm, bp, "valueOf", boolean_value_of, DD);
    vm->boolean_proto = bp;
    vm->boolean_ctor = make_class(vm, "Boolean", boolean_fn, boolean_ctor, bp);

    swf_obj *np = obj_new(vm, vm->object_proto);
    native_method(vm, np, "toString", number_to_string_m, DD);
    native_method(vm, np, "valueOf", number_value_of, DD);
    vm->number_proto = np;
    swf_obj *nc = make_class(vm, "Number", number_fn, number_ctor, np);
    vm->number_ctor = nc;
    const uint16_t C = DD | ATTR_READ_ONLY;
    obj_define_ascii(vm, nc, "MAX_VALUE", swf_num(1.7976931348623157e308), C);
    obj_define_ascii(vm, nc, "MIN_VALUE", swf_num(5e-324), C);
    obj_define_ascii(vm, nc, "NaN", swf_num(NAN), C);
    obj_define_ascii(vm, nc, "NEGATIVE_INFINITY", swf_num(-INFINITY), C);
    obj_define_ascii(vm, nc, "POSITIVE_INFINITY", swf_num(INFINITY), C);
}

/* --- Math (Ruffle avm1/globals/math.rs) ----------------------------------------------- */

enum {
    M_ABS, M_MIN, M_MAX, M_SIN, M_COS, M_ATAN2, M_TAN, M_EXP, M_LOG, M_SQRT, M_ROUND, M_RANDOM, M_FLOOR,
    M_CEIL, M_ATAN, M_ASIN, M_ACOS, M_POW
};

static swf_val math_op(swf_ctx *cx, uint32_t argc, const swf_val *argv, int op)
{
    /* Every Math method coerces its first two arguments, used or not. */
    double x = to_number(cx, arg(argc, argv, 0));
    double y = to_number(cx, arg(argc, argv, 1));
    if (cx->version <= 6) {
        bool ok;
        if (op == M_RANDOM) ok = true;
        else if ((op == M_MIN || op == M_MAX) && argc == 0) ok = true;
        else if (op == M_POW && argc == 1 && x == 1.0) ok = true;
        else if ((op == M_MIN || op == M_MAX || op == M_POW || op == M_ATAN2) && argc < 2) ok = false;
        else ok = argc > 0;
        if (!ok) return swf_num(NAN);
    }
    double r;
    switch (op) {
    case M_ABS: r = fabs(x); break;
    case M_MIN:
        r = argc == 0 ? INFINITY : (isnan(x) || isnan(y)) ? NAN : (x < y ? x : y);
        break;
    case M_MAX:
        r = argc == 0 ? -INFINITY : (isnan(x) || isnan(y)) ? NAN : (x > y ? x : y);
        break;
    case M_SIN: r = sin(x); break;
    case M_COS: r = cos(x); break;
    case M_ATAN2: r = atan2(x, y); break;
    case M_TAN: r = tan(x); break;
    case M_EXP: r = exp(x); break;
    case M_LOG: r = log(x); break;
    case M_SQRT: r = sqrt(x); break;
    case M_ROUND: r = floor(x + 0.5); break;
    case M_RANDOM: {
        uint32_t s = cx->vm->rng;
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        cx->vm->rng = s;
        r = (double)(s & 0x7FFFFFFF) / 2147483648.0;
        break;
    }
    case M_FLOOR: r = floor(x); break;
    case M_CEIL: r = ceil(x); break;
    case M_ATAN: r = atan(x); break;
    case M_ASIN: r = asin(x); break;
    case M_ACOS: r = acos(x); break;
    case M_POW: r = pow(x, y); break;
    default: r = NAN; break;
    }
    return swf_num(r);
}

#define MATH_FN(name, op)                                                                   \
    static swf_val math_##name(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv) \
    {                                                                                       \
        (void)this_;                                                                        \
        return math_op(cx, argc, argv, op);                                                 \
    }

MATH_FN(abs, M_ABS)
MATH_FN(min, M_MIN)
MATH_FN(max, M_MAX)
MATH_FN(sin, M_SIN)
MATH_FN(cos, M_COS)
MATH_FN(atan2, M_ATAN2)
MATH_FN(tan, M_TAN)
MATH_FN(exp, M_EXP)
MATH_FN(log, M_LOG)
MATH_FN(sqrt, M_SQRT)
MATH_FN(round, M_ROUND)
MATH_FN(random, M_RANDOM)
MATH_FN(floor, M_FLOOR)
MATH_FN(ceil, M_CEIL)
MATH_FN(atan, M_ATAN)
MATH_FN(asin, M_ASIN)
MATH_FN(acos, M_ACOS)
MATH_FN(pow, M_POW)

void builtins_math(swf_vm *vm)
{
    swf_obj *m = obj_new(vm, vm->object_proto);
    obj_define_ascii(vm, vm->global, "Math", swf_objv(m), ATTR_DONT_ENUM);
    const uint16_t C = DD | ATTR_READ_ONLY;
    obj_define_ascii(vm, m, "E", swf_num(2.718281828459045), C);
    obj_define_ascii(vm, m, "LN10", swf_num(2.302585092994046), C);
    obj_define_ascii(vm, m, "LN2", swf_num(0.6931471805599453), C);
    obj_define_ascii(vm, m, "LOG10E", swf_num(0.4342944819032518), C);
    obj_define_ascii(vm, m, "LOG2E", swf_num(1.4426950408889634), C);
    obj_define_ascii(vm, m, "PI", swf_num(3.141592653589793), C);
    obj_define_ascii(vm, m, "SQRT1_2", swf_num(0.7071067811865476), C);
    obj_define_ascii(vm, m, "SQRT2", swf_num(1.4142135623730951), C);
    native_method(vm, m, "abs", math_abs, C);
    native_method(vm, m, "min", math_min, C);
    native_method(vm, m, "max", math_max, C);
    native_method(vm, m, "sin", math_sin, C);
    native_method(vm, m, "cos", math_cos, C);
    native_method(vm, m, "atan2", math_atan2, C);
    native_method(vm, m, "tan", math_tan, C);
    native_method(vm, m, "exp", math_exp, C);
    native_method(vm, m, "log", math_log, C);
    native_method(vm, m, "sqrt", math_sqrt, C);
    native_method(vm, m, "round", math_round, C);
    native_method(vm, m, "random", math_random, C);
    native_method(vm, m, "floor", math_floor, C);
    native_method(vm, m, "ceil", math_ceil, C);
    native_method(vm, m, "atan", math_atan, C);
    native_method(vm, m, "asin", math_asin, C);
    native_method(vm, m, "acos", math_acos, C);
    native_method(vm, m, "pow", math_pow, C);
}

/* --- Error (Ruffle defines it in AS: Error.as) -------------------------------------- */

static swf_val error_ctor(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    swf_obj *o = this_obj(cx, this_);
    if (argc && argv[0].t != SWF_T_UNDEF) obj_set(cx, o, cx->vm->s.message, argv[0]);
    return swf_objv(o);
}

static swf_val error_to_string(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)argc; (void)argv;
    swf_obj *o = this_obj(cx, this_);
    return obj_get(cx, o, cx->vm->s.message);
}

void builtins_error(swf_vm *vm)
{
    swf_obj *p = obj_new(vm, vm->object_proto);
    obj_define(vm, p, vm->s.message, swf_strv(vm->s.Error), 0);
    obj_define(vm, p, vm->s.name, swf_strv(vm->s.Error), 0);
    native_method(vm, p, "toString", error_to_string, 0);
    vm->error_proto = p;
    swf_obj *c = func_new_native(vm, error_ctor, NULL, p);
    /* AS-defined classes behave like bytecode constructors: `new` returns `this`. */
    c->u.fn.native = error_ctor;
    c->u.fn.ctor = NULL;
    obj_define(vm, vm->global, vm->s.Error, swf_objv(c), ATTR_DONT_ENUM);
    obj_set_attributes(vm, p, NULL, ATTR_DONT_ENUM, 0);
}

/* --- global functions ----------------------------------------------------------------- */

static swf_val g_is_nan(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)this_;
    return swf_bool(argc ? isnan(to_number(cx, argv[0])) : true);
}

static swf_val g_is_finite(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)this_;
    return swf_bool(argc ? isfinite(to_number(cx, argv[0])) : false);
}

static int digit_val(uint16_t c, uint32_t radix)
{
    int d;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10;
    else return -1;
    return d < (int)radix ? d : -1;
}

static swf_val g_parse_int(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)this_;
    if (argc < 1) return swf_undef();
    int32_t radix = 0;
    bool have_radix = argc > 1;
    if (have_radix) {
        radix = to_int32(cx, argv[1]);
        if (radix < 2 || radix > 36) return swf_num(NAN);
    }
    swf_str *s = to_string(cx, argv[0]);
    const uint16_t *c = s->ch;
    uint32_t n = s->len, i = 0;
    bool has_sign = n > 0 && (c[0] == '+' || c[0] == '-');
    uint32_t off = has_sign ? 1 : 0;
    bool zero = off < n && c[off] == '0';
    bool hex = zero && off + 1 < n && (c[off + 1] == 'x' || c[off + 1] == 'X');
    bool ignore_sign = false;
    uint32_t r;
    if (hex) {
        if (has_sign) {
            if (!have_radix || radix <= 33) return swf_num(NAN);
            r = (uint32_t)radix;
            ignore_sign = true;
        } else {
            r = have_radix ? (uint32_t)radix : 16;
            i = 2;
        }
    } else {
        bool octal = zero && !have_radix;
        for (uint32_t k = 1; octal && k < n; k++)
            if (c[k] < '0' || c[k] > '7') octal = false;
        r = octal ? 8 : (have_radix ? (uint32_t)radix : 10);
    }
    while (i < n && (c[i] == '\t' || c[i] == '\n' || c[i] == '\r' || c[i] == ' ')) i++;
    double sign = 1.0;
    if (i < n && (c[i] == '+' || c[i] == '-')) {
        if (!ignore_sign && c[i] == '-') sign = -1.0;
        i++;
    }
    bool empty = true;
    double result = 0.0;
    for (; i < n; i++) {
        int d = digit_val(c[i], r);
        if (d < 0) break;
        result = result * r + d;
        empty = false;
    }
    return swf_num(empty ? NAN : copysign(result, sign));
}

static swf_val g_parse_float(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)this_;
    if (argc < 1) return swf_undef();
    swf_str *s = to_string(cx, argv[0]);
    return swf_num(parse_float_impl(s->ch, s->len, false));
}

static swf_val g_escape(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)this_;
    if (argc < 1) return swf_undef();
    size_t n;
    char *u = str_to_utf8(to_string(cx, argv[0]), &n);
    if (!u) return swf_undef();
    char *out = malloc(n * 3 + 1);
    if (!out) {
        free(u);
        vm_abort(cx->vm, "out of memory in escape");
    }
    size_t o = 0;
    static const char HEX[] = "0123456789ABCDEF";
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)u[i];
        if ((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) {
            out[o++] = (char)ch;
        } else {
            out[o++] = '%';
            out[o++] = HEX[ch >> 4];
            out[o++] = HEX[ch & 15];
        }
    }
    swf_str *r = str_from_utf8(cx->vm, out, o);
    free(out);
    free(u);
    return swf_strv(r);
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static swf_val g_unescape(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)this_;
    if (argc < 1) return swf_undef();
    size_t n;
    char *u = str_to_utf8(to_string(cx, argv[0]), &n);
    if (!u) return swf_undef();
    char *out = malloc(n + 1);
    if (!out) {
        free(u);
        vm_abort(cx->vm, "out of memory in unescape");
    }
    size_t o = 0;
    int remain = 0, acc = 0;
    for (size_t i = 0; i < n; i++) {
        char ch = u[i];
        if (ch == '%') {
            remain = 2;
            acc = 0;
        } else if (remain > 0 && hexval(ch) >= 0) {
            acc = acc * 16 + hexval(ch);
            if (--remain == 0) out[o++] = (char)acc;
        } else if (remain > 0) {
            remain = 0;
        } else if (ch == '+') {
            out[o++] = ' ';
        } else {
            out[o++] = ch;
        }
    }
    swf_str *r = str_from_utf8(cx->vm, out, o);
    free(out);
    free(u);
    return swf_strv(r);
}

static swf_val g_trace(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)this_;
    swf_str *s = to_string(cx, arg(argc, argv, 0));
    char *u = str_to_utf8(s, NULL);
    if (u) {
        cx->vm->trace(u, cx->vm->trace_user);
        free(u);
    }
    return swf_undef();
}

static swf_val g_set_prop_flags(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)this_;
    if (argc < 1) return swf_undef();
    swf_obj *o = to_object_or_bare(cx, argv[0]);
    uint16_t set = (uint16_t)to_number(cx, argc > 2 ? argv[2] : swf_num(0));
    uint16_t clear = (uint16_t)to_number(cx, argc > 3 ? argv[3] : swf_num(0));
    swf_val names = arg(argc, argv, 1);
    if (names.t == SWF_T_NULL) {
        obj_set_attributes(cx->vm, o, NULL, set, clear);
        return swf_undef();
    }
    swf_str *s = to_string(cx, names);
    uint32_t start = 0;
    for (uint32_t i = 0; i <= s->len; i++) {
        if (i == s->len || s->ch[i] == ',') {
            obj_set_attributes(cx->vm, o, str_sub(cx->vm, s, start, i), set, clear);
            start = i + 1;
        }
    }
    return swf_undef();
}

static swf_val g_noop(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)cx; (void)this_; (void)argc; (void)argv;
    return swf_undef();
}

void builtins_global(swf_vm *vm)
{
    swf_obj *g = vm->global;
    native_method(vm, g, "isNaN", g_is_nan, ATTR_DONT_ENUM);
    native_method(vm, g, "isFinite", g_is_finite, ATTR_DONT_ENUM);
    native_method(vm, g, "parseInt", g_parse_int, ATTR_DONT_ENUM);
    native_method(vm, g, "parseFloat", g_parse_float, ATTR_DONT_ENUM);
    native_method(vm, g, "escape", g_escape, ATTR_DONT_ENUM);
    native_method(vm, g, "unescape", g_unescape, ATTR_DONT_ENUM);
    native_method(vm, g, "trace", g_trace, ATTR_DONT_ENUM);
    native_method(vm, g, "ASSetPropFlags", g_set_prop_flags, ATTR_DONT_ENUM);
    native_method(vm, g, "updateAfterEvent", g_noop, ATTR_DONT_ENUM);
    /* Flash's playerglobals leave a scratch variable "o" set to null. */
    obj_define_ascii(vm, g, "o", swf_null(), 0);
    /* SWF 4 movies read these globals as undefined. */
    if (vm->version > 4) {
        obj_define_ascii(vm, g, "NaN", swf_num(NAN), ATTR_DONT_ENUM);
        obj_define_ascii(vm, g, "Infinity", swf_num(INFINITY), ATTR_DONT_ENUM);
    }
}
