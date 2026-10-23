/* Type coercions and comparisons. Number formatting and parsing are ported from
 * Ruffle's avm1/value.rs (MIT/Apache-2.0), which reproduces Flash Player's quirks. */
#include "vm_internal.h"

#include <math.h>
#include <stdio.h>

int32_t f64_to_i32(double d)
{
    if (!isfinite(d)) return 0;
    double t = trunc(d);
    double m = fmod(t, 4294967296.0);
    if (m < 0) m += 4294967296.0;
    return (int32_t)(uint32_t)m;
}

uint32_t f64_to_u32(double d)
{
    return (uint32_t)f64_to_i32(d);
}

uint16_t f64_to_u16(double d)
{
    return (uint16_t)f64_to_i32(d);
}

static double decimal_shift(double value, int exp)
{
    double base = 10.0;
    if (exp > 0) {
        while (exp > 0) {
            if (exp & 1) value *= base;
            exp >>= 1;
            base *= base;
        }
    } else {
        unsigned e = exp == INT32_MIN ? 0x80000000u : (unsigned)(-exp);
        while (e > 0) {
            if (e & 1) value /= base;
            e >>= 1;
            base *= base;
        }
    }
    return value;
}

swf_str *number_to_string(swf_vm *vm, double n)
{
    if (isnan(n)) return vm->s.NaN;
    if (n == INFINITY) return vm->s.Infinity;
    if (n == -INFINITY) return vm->s.neg_Infinity;
    if (n == 0.0) return atom_int(vm, 0);
    if (n >= -2147483648.0 && n <= 2147483647.0 && n == trunc(n)) {
        int32_t i = (int32_t)n;
        if (i >= 0 && i < 1024) return atom_int(vm, i);
        char b[16];
        snprintf(b, sizeof b, "%d", (int)i);
        return str_from_ascii(vm, b);
    }
    char buf[40];
    int len = 0;
    bool neg = n < 0;
    if (neg) {
        n = -n;
        buf[len++] = '-';
    }
    uint64_t bits;
    memcpy(&bits, &n, sizeof bits);
    int exp2 = (int)((bits >> 52) & 0x7FF) - 1023;
    if (exp2 == -1023) {
        double m = n * 1.801439850948198e16;
        memcpy(&bits, &m, sizeof bits);
        exp2 = (int)((bits >> 52) & 0x7FF) - 1023 - 54;
    }
    int exp = (int)round((double)exp2 * 0.301029995663981);
    double mantissa = decimal_shift(n, -exp);
    if ((int)mantissa == 0) {
        exp -= 1;
        mantissa = decimal_shift(n, -exp);
    }
    if ((int)mantissa >= 10) {
        exp += 1;
        mantissa = decimal_shift(n, -exp);
    }
    char digits[64];
    int nd = 0;
    /* Digits are generated lazily in Ruffle; generating a fixed run up front gives the
     * same sequence because each step only depends on the previous mantissa. */
    for (int i = 0; i < 40; i++) {
        int d = (int)mantissa;
        mantissa -= d;
        mantissa *= 10.0;
        digits[nd++] = (char)('0' + d);
    }
    int di = 0;
#define TAKE() (digits[di++])
    const int MAX = 15;
    if (exp >= 15) {
        buf[len++] = TAKE();
        buf[len++] = '.';
        for (int i = 0; i < MAX - 1; i++) buf[len++] = TAKE();
    } else if (exp >= 0) {
        buf[len++] = '0';
        for (int i = 0; i <= exp; i++) buf[len++] = TAKE();
        buf[len++] = '.';
        for (int i = 0; i < MAX - exp - 1; i++) buf[len++] = TAKE();
        exp = 0;
    } else if (exp >= -5) {
        buf[len++] = '0';
        buf[len++] = '0';
        buf[len++] = '.';
        for (int i = 0; i < -exp - 1; i++) buf[len++] = '0';
        for (int i = 0; i < MAX; i++) buf[len++] = TAKE();
        exp = 0;
    } else {
        buf[len++] = '0';
        char d = TAKE();
        if (d != '0') buf[len++] = d;
        buf[len++] = '.';
        for (int i = 0; i < MAX - 1; i++) buf[len++] = TAKE();
    }
    if (TAKE() >= '5') {
        for (int i = len - 1; i >= 0; i--) {
            if (buf[i] == '9') {
                buf[i] = '0';
            } else if (buf[i] >= '0') {
                buf[i]++;
                break;
            }
        }
    }
#undef TAKE
    while (len > 0 && buf[len - 1] == '0') len--;
    if (len > 0 && buf[len - 1] == '.') len--;
    int start = 0;
    if (exp != 0) {
        int pos = 0;
        while (pos < len && buf[pos] == '0') pos++;
        if (pos) {
            memmove(buf, buf + pos, (size_t)(len - pos));
            len -= pos;
        }
        if (len == 0) {
            buf[len++] = '1';
            exp += 1;
        } else {
            int last = -1;
            for (int i = len - 1; i >= 0; i--)
                if (buf[i] != '0') {
                    last = i;
                    break;
                }
            if (last <= 0) {
                exp += len - 1;
                len = 1;
            }
        }
        len += snprintf(buf + len, sizeof buf - (size_t)len, "e%+d", exp);
    }
    int i = neg ? 1 : 0;
    if (len > i + 1 && buf[i] == '0' && buf[i + 1] != '.') {
        if (i > 0) buf[i] = buf[i - 1];
        start = 1;
    }
    return str_from_utf8(vm, buf + start, (size_t)(len - start));
}

static bool is_ws(uint16_t c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0x0B || c == 0x0C;
}

static bool is_digit(uint16_t c)
{
    return c >= '0' && c <= '9';
}

double parse_float_impl(const uint16_t *s, uint32_t n, bool strict)
{
    uint32_t i = 0;
    while (i < n && is_ws(s[i])) i++;
    bool negative = false;
    if (i < n && (s[i] == '-' || s[i] == '+')) {
        negative = s[i] == '-';
        i++;
    }
    uint32_t after_sign = i;
    while (i < n && is_digit(s[i])) i++;
    int exp = (int)(i - after_sign) - 1;
    if (i < n && s[i] == '.') {
        i++;
        while (i < n && is_digit(s[i])) i++;
    }
    if (i == after_sign) return NAN;
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        bool eneg = false;
        if (i < n && (s[i] == '-' || s[i] == '+')) {
            eneg = s[i] == '-';
            i++;
        }
        int32_t e = 0;
        while (i < n && is_digit(s[i])) {
            e = (int32_t)((uint32_t)e * 10u + (uint32_t)(s[i] - '0'));
            i++;
        }
        if (eneg) e = (int32_t)(0u - (uint32_t)e);
        exp = (int32_t)((uint32_t)exp + (uint32_t)e);
    }
    if (strict && i != n) return NAN;
    double result = 0.0;
    for (uint32_t k = after_sign; k < n; k++) {
        uint16_t c = s[k];
        if (is_digit(c)) {
            result += decimal_shift(c - '0', exp);
            exp = (int32_t)((uint32_t)exp - 1u);
        } else if (c == '.') {
            continue;
        } else {
            break;
        }
    }
    return negative ? -result : result;
}

static int guess_radix(const uint16_t *s, uint32_t n)
{
    uint32_t i = 0;
    if (i < n && (s[i] == '+' || s[i] == '-')) i++;
    if (i < n && s[i] == '0') {
        if (i + 1 < n && (s[i + 1] == 'x' || s[i + 1] == 'X')) return 16;
        bool oct = true;
        for (uint32_t k = i + 1; k < n; k++)
            if (s[k] < '0' || s[k] > '7') oct = false;
        if (oct) return 8;
    }
    return 10;
}

/* Wrapping integer parse as in Ruffle's `Wrapping<i32>::from_wstr_radix`. */
static bool parse_wrapping_i32(const uint16_t *s, uint32_t n, int radix, int32_t *out)
{
    uint32_t i = 0;
    bool neg = false;
    if (i < n && (s[i] == '+' || s[i] == '-')) {
        neg = s[i] == '-';
        i++;
    }
    if (i == n) return false;
    uint32_t v = 0;
    for (; i < n; i++) {
        int d;
        uint16_t c = s[i];
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10;
        else return false;
        if (d >= radix) return false;
        v = v * (uint32_t)radix + (uint32_t)d;
    }
    *out = (int32_t)(neg ? 0u - v : v);
    return true;
}

double string_to_number(swf_str *str, uint8_t version)
{
    const uint16_t *s = str->ch;
    uint32_t n = str->len;
    if (version >= 6) {
        int radix = guess_radix(s, n);
        if (radix != 10) {
            if (radix == 16) {
                /* Flash fails to skip a hex prefix after a sign, giving NaN. */
                s += 2;
                n -= 2;
            }
            int32_t v;
            return parse_wrapping_i32(s, n, radix, &v) ? (double)v : NAN;
        }
    }
    bool strict = version >= 5;
    double r = parse_float_impl(s, n, strict);
    if (!strict && isnan(r)) return 0.0;
    return r;
}

double prim_to_number(swf_ctx *cx, swf_val v)
{
    switch (v.t) {
    case SWF_T_UNDEF:
    case SWF_T_NULL: return cx->version < 7 ? 0.0 : NAN;
    case SWF_T_BOOL: return v.u.b ? 1.0 : 0.0;
    case SWF_T_NUM: return v.u.n;
    case SWF_T_STR: return string_to_number(v.u.s, cx->version);
    default: return cx->version < 5 ? 0.0 : NAN;
    }
}

swf_val to_primitive_num(swf_ctx *cx, swf_val v)
{
    if (v.t == SWF_T_OBJ && v.u.o->cls != OC_CLIP)
        return obj_call_method(cx, v.u.o, cx->vm->s.valueOf, 0, NULL, true);
    return v;
}

double to_number(swf_ctx *cx, swf_val v)
{
    if (v.t == SWF_T_OBJ) return prim_to_number(cx, to_primitive_num(cx, v));
    return prim_to_number(cx, v);
}

int32_t to_int32(swf_ctx *cx, swf_val v)
{
    return f64_to_i32(to_number(cx, v));
}

uint32_t to_uint32(swf_ctx *cx, swf_val v)
{
    return f64_to_u32(to_number(cx, v));
}

swf_val to_primitive(swf_ctx *cx, swf_val v)
{
    if (v.t != SWF_T_OBJ) return v;
    swf_obj *o = v.u.o;
    if (o->cls == OC_CLIP) {
        swf_val r = obj_call_method(cx, o, cx->vm->s.valueOf, 0, NULL, true);
        if (r.t == SWF_T_UNDEF) return swf_strv(to_string(cx, v));
        return v;
    }
    swf_val r = obj_call_method(cx, o, cx->vm->s.valueOf, 0, NULL, true);
    return r.t == SWF_T_OBJ ? v : r;
}

swf_str *to_string(swf_ctx *cx, swf_val v)
{
    swf_vm *vm = cx->vm;
    switch (v.t) {
    case SWF_T_UNDEF: return cx->version < 7 ? vm->s.empty : vm->s.undefined;
    case SWF_T_NULL: return vm->s.null_;
    case SWF_T_BOOL:
        if (cx->version < 5) return atom_int(vm, v.u.b ? 1 : 0);
        return v.u.b ? vm->s.true_ : vm->s.false_;
    case SWF_T_NUM: return number_to_string(vm, v.u.n);
    case SWF_T_STR: return v.u.s;
    default: {
        swf_obj *o = v.u.o;
        if (o->cls == OC_STRING && o->u.prim.t == SWF_T_STR) return o->u.prim.u.s;
        if (o->cls == OC_CLIP) {
            if (o->u.clip) return clip_path(vm, o->u.clip);
        }
        swf_val r = obj_call_method(cx, o, vm->s.toString, 0, NULL, true);
        if (r.t == SWF_T_STR) return r.u.s;
        return o->cls == OC_FUNCTION ? vm->s.type_Function : vm->s.type_Object;
    }
    }
}

bool to_bool_v(uint8_t version, swf_val v)
{
    switch (v.t) {
    case SWF_T_BOOL: return v.u.b != 0;
    case SWF_T_NUM: return !isnan(v.u.n) && v.u.n != 0.0;
    case SWF_T_STR:
        if (version >= 7) return v.u.s->len != 0;
        else {
            double d = string_to_number(v.u.s, version);
            return !isnan(d) && d != 0.0;
        }
    case SWF_T_OBJ: return true;
    default: return false;
    }
}

swf_obj *to_object(swf_ctx *cx, swf_val v)
{
    swf_vm *vm = cx->vm;
    swf_obj *ctor;
    switch (v.t) {
    case SWF_T_OBJ: return v.u.o;
    case SWF_T_BOOL: ctor = NULL; {
            swf_val c = obj_get(cx, vm->global, atom_ascii(vm, "Boolean"));
            ctor = c.t == SWF_T_OBJ ? c.u.o : NULL;
        }
        break;
    case SWF_T_NUM: {
        swf_val c = obj_get(cx, vm->global, atom_ascii(vm, "Number"));
        ctor = c.t == SWF_T_OBJ ? c.u.o : NULL;
        break;
    }
    case SWF_T_STR: {
        swf_val c = obj_get(cx, vm->global, atom_ascii(vm, "String"));
        ctor = c.t == SWF_T_OBJ ? c.u.o : NULL;
        break;
    }
    default: return NULL;
    }
    if (!ctor) return NULL;
    swf_val r = func_construct(cx, ctor, 1, &v);
    return r.t == SWF_T_OBJ ? r.u.o : NULL;
}

swf_obj *to_object_or_bare(swf_ctx *cx, swf_val v)
{
    swf_obj *o = to_object(cx, v);
    return o ? o : obj_new_bare(cx->vm);
}

swf_str *type_of(swf_ctx *cx, swf_val v)
{
    swf_vm *vm = cx->vm;
    switch (v.t) {
    case SWF_T_UNDEF: return vm->s.undefined;
    case SWF_T_NULL: return vm->s.null_;
    case SWF_T_NUM: return vm->s.number;
    case SWF_T_BOOL: return vm->s.boolean;
    case SWF_T_STR: return vm->s.string;
    default:
        if (v.u.o->cls == OC_FUNCTION) return vm->s.function;
        if (v.u.o->cls == OC_CLIP && v.u.o->u.clip && v.u.o->u.clip->kind == SWF_CHAR_SPRITE) return vm->s.movieclip;
        return vm->s.object;
    }
}

bool strict_eq(swf_val a, swf_val b)
{
    if (a.t != b.t) return false;
    switch (a.t) {
    case SWF_T_UNDEF:
    case SWF_T_NULL: return true;
    case SWF_T_BOOL: return (a.u.b != 0) == (b.u.b != 0);
    case SWF_T_NUM: return a.u.n == b.u.n || (isnan(a.u.n) && isnan(b.u.n));
    case SWF_T_STR: return str_eq(a.u.s, b.u.s);
    default:
        if (a.u.o == b.u.o) return true;
        /* Clip references compare by the clip they denote. */
        return a.u.o->cls == OC_CLIP && b.u.o->cls == OC_CLIP && a.u.o->u.clip && a.u.o->u.clip == b.u.o->u.clip;
    }
}

static bool is_prim(swf_val v)
{
    return v.t != SWF_T_OBJ;
}

/* ECMA-262 2nd ed. 11.9.3 with Flash's divergences (Ruffle `abstract_eq`). */
static bool eq_impl(swf_ctx *cx, swf_val a, swf_val b, int depth)
{
    if (depth > 16) return false;
    if (is_nullish(a) && is_nullish(b)) return true;
    if (a.t == b.t) {
        switch (a.t) {
        case SWF_T_BOOL: return (a.u.b != 0) == (b.u.b != 0);
        case SWF_T_STR: return str_eq(a.u.s, b.u.s);
        case SWF_T_OBJ: return strict_eq(a, b);
        case SWF_T_NUM: return a.u.n == b.u.n || (isnan(a.u.n) && isnan(b.u.n));
        default: break;
        }
    }
    if (a.t == SWF_T_BOOL) return eq_impl(cx, b, swf_num(a.u.b ? 1 : 0), depth + 1);
    if (b.t == SWF_T_BOOL) return eq_impl(cx, a, swf_num(b.u.b ? 1 : 0), depth + 1);
    if (a.t == SWF_T_NUM && b.t == SWF_T_STR) return a.u.n == prim_to_number(cx, b);
    if (a.t == SWF_T_STR && b.t == SWF_T_NUM) return b.u.n == prim_to_number(cx, a);
    if (a.t == SWF_T_OBJ || b.t == SWF_T_OBJ) {
        swf_val obj = a.t == SWF_T_OBJ ? a : b;
        swf_val val = a.t == SWF_T_OBJ ? b : a;
        swf_val p = to_primitive_num(cx, obj);
        return is_prim(p) && eq_impl(cx, val, p, depth + 1);
    }
    return false;
}

bool abstract_eq(swf_ctx *cx, swf_val left, swf_val right)
{
    if (cx->version > 5) return eq_impl(cx, right, left, 0);
    swf_val a = to_primitive_num(cx, right), b = to_primitive_num(cx, left);
    return eq_impl(cx, a, b, 0);
}

/* left < right, `undefined` when NaN is involved (Ruffle `abstract_lt`). */
swf_val abstract_lt(swf_ctx *cx, swf_val left, swf_val right)
{
    swf_val a = to_primitive_num(cx, left);
    if (a.t == SWF_T_OBJ && a.u.o->cls != OC_CLIP) return swf_bool(false);
    swf_val b = to_primitive_num(cx, right);
    if (b.t == SWF_T_OBJ && b.u.o->cls != OC_CLIP) return swf_bool(false);
    if (a.t == SWF_T_STR && b.t == SWF_T_STR) return swf_bool(str_cmp(a.u.s, b.u.s) < 0);
    double x = prim_to_number(cx, a), y = prim_to_number(cx, b);
    if (isnan(x) || isnan(y)) return swf_undef();
    return swf_bool(x < y);
}

bool instance_of(swf_ctx *cx, swf_val obj, swf_val ctor)
{
    if (obj.t != SWF_T_OBJ) return false;
    swf_obj *c = to_object(cx, ctor);
    if (!c) return false;
    swf_val p = obj_get_data(cx, c, cx->vm->s.prototype);
    swf_obj *po = to_object(cx, p);
    if (!po) return false;
    return obj_is_instance_of(cx, obj.u.o, po);
}

swf_str *to_key(swf_ctx *cx, swf_val v)
{
    if (v.t == SWF_T_NUM && v.u.n >= 0 && v.u.n < 1024 && v.u.n == trunc(v.u.n))
        return atom_int(cx->vm, (int64_t)v.u.n);
    return atom(cx->vm, to_string(cx, v));
}
