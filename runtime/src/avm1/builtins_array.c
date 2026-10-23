/* Array: elements are ordinary properties named "0", "1", ... with a `length` property,
 * exactly as in Flash; methods are ported from Ruffle's avm1/globals/array.rs
 * (MIT/Apache-2.0), including its Flash-compatible quicksort. */
#include "vm_internal.h"

#include <math.h>

#define DD (ATTR_DONT_ENUM | ATTR_DONT_DELETE)

enum { SORT_CASE_INSENSITIVE = 1, SORT_DESCENDING = 2, SORT_UNIQUE = 4, SORT_INDEXED = 8, SORT_NUMERIC = 16 };

static void array_init(swf_ctx *cx, swf_obj *o, uint32_t n, const swf_val *elems)
{
    swf_vm *vm = cx->vm;
    for (uint32_t i = 0; i < n; i++) obj_define(vm, o, atom_int(vm, i), elems ? elems[i] : swf_undef(), 0);
    obj_define(vm, o, vm->s.length, swf_num(n), DD);
    o->cls = OC_ARRAY;
}

swf_obj *array_new(swf_ctx *cx, uint32_t n, const swf_val *elems)
{
    swf_vm *vm = cx->vm;
    swf_obj *o = obj_new_bare(vm);
    swf_val ctor = obj_get(cx, vm->global, vm->s.Array);
    swf_val proto = ctor.t == SWF_T_OBJ ? obj_get_data(cx, ctor.u.o, vm->s.prototype) : swf_undef();
    if (proto.t != SWF_T_UNDEF) obj_define(vm, o, vm->s.proto, proto, DD);
    if (ctor.t == SWF_T_OBJ) obj_define(vm, o, vm->s.constructor, ctor, DD);
    array_init(cx, o, n, elems);
    return o;
}

static int32_t length_of(swf_ctx *cx, swf_obj *o)
{
    return to_int32(cx, obj_get(cx, o, cx->vm->s.length));
}

uint32_t array_length(swf_ctx *cx, swf_obj *a)
{
    swf_prop *p = obj_own(cx->vm, a, cx->vm->s.length);
    if (!p || p->val.t != SWF_T_NUM || !(p->val.u.n >= 0)) return 0;
    return (uint32_t)p->val.u.n;
}

static swf_val get_el(swf_ctx *cx, swf_obj *o, int32_t i)
{
    return obj_get(cx, o, atom_int(cx->vm, i));
}

static void set_el(swf_ctx *cx, swf_obj *o, int32_t i, swf_val v)
{
    obj_set(cx, o, atom_int(cx->vm, i), v);
}

static void del_el(swf_ctx *cx, swf_obj *o, int32_t i)
{
    obj_delete(cx, o, atom_int(cx->vm, i));
}

static bool has_el(swf_ctx *cx, swf_obj *o, int32_t i)
{
    return obj_has_property(cx, o, atom_int(cx->vm, i));
}

static void set_length(swf_ctx *cx, swf_obj *o, int32_t n)
{
    obj_set(cx, o, cx->vm->s.length, swf_num(n));
}

static swf_obj *self(swf_ctx *cx, swf_val this_)
{
    return to_object_or_bare(cx, this_);
}

static swf_val array_fn(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)this_;
    if (argc == 1 && argv[0].t == SWF_T_NUM) {
        swf_obj *a = array_new(cx, 0, NULL);
        double n = argv[0].u.n;
        set_length(cx, a, isnan(n) ? 0 : (int32_t)fmax(fmin(n, 2147483647.0), -2147483648.0));
        return swf_objv(a);
    }
    return swf_objv(array_new(cx, argc, argv));
}

static swf_val array_ctor(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    swf_obj *o = self(cx, this_);
    if (argc == 1 && argv[0].t == SWF_T_NUM) {
        array_init(cx, o, 0, NULL);
        double n = argv[0].u.n;
        set_length(cx, o, isnan(n) ? 0 : (int32_t)fmax(fmin(n, 2147483647.0), -2147483648.0));
    } else {
        array_init(cx, o, argc, argv);
    }
    return swf_objv(o);
}

static swf_val a_push(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    swf_obj *o = self(cx, this_);
    int32_t old = length_of(cx, o);
    for (uint32_t i = 0; i < argc; i++) set_el(cx, o, old + (int32_t)i, argv[i]);
    int32_t n = old + (int32_t)argc;
    set_length(cx, o, n);
    return swf_num(n);
}

static swf_val a_unshift(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    swf_obj *o = self(cx, this_);
    int32_t old = length_of(cx, o);
    int32_t n = old + (int32_t)argc;
    for (int32_t i = 0; i < old; i++) {
        int32_t from = old - i - 1, to = n - i - 1;
        swf_val e = get_el(cx, o, from);
        del_el(cx, o, to);
        set_el(cx, o, to, e);
    }
    for (uint32_t i = 0; i < argc; i++) {
        del_el(cx, o, (int32_t)i);
        set_el(cx, o, (int32_t)i, argv[i]);
    }
    if (o->cls == OC_ARRAY) set_length(cx, o, n);
    return swf_num(n);
}

static swf_val a_shift(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)argc; (void)argv;
    swf_obj *o = self(cx, this_);
    int32_t len = length_of(cx, o);
    if (len <= 0) return swf_undef();
    swf_val first = get_el(cx, o, 0);
    for (int32_t i = 1; i < len; i++) {
        swf_val e = get_el(cx, o, i);
        del_el(cx, o, i - 1);
        set_el(cx, o, i - 1, e);
    }
    if (o->cls == OC_ARRAY) set_length(cx, o, len - 1);
    return first;
}

static swf_val a_pop(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)argc; (void)argv;
    swf_obj *o = self(cx, this_);
    int32_t len = length_of(cx, o);
    if (len <= 0) return swf_undef();
    swf_val last = get_el(cx, o, len - 1);
    del_el(cx, o, len - 1);
    if (o->cls == OC_ARRAY) set_length(cx, o, len - 1);
    return last;
}

static swf_val a_reverse(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)argc; (void)argv;
    swf_obj *o = self(cx, this_);
    int32_t len = length_of(cx, o);
    for (int32_t lo = 0; lo < len / 2; lo++) {
        int32_t hi = len - lo - 1;
        swf_val lv = get_el(cx, o, lo), hv = get_el(cx, o, hi);
        del_el(cx, o, lo);
        del_el(cx, o, hi);
        set_el(cx, o, lo, hv);
        set_el(cx, o, hi, lv);
    }
    return swf_objv(o);
}

static swf_val a_join(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    swf_vm *vm = cx->vm;
    swf_obj *o = self(cx, this_);
    int32_t len = length_of(cx, o);
    swf_str *sep = argc && argv[0].t != SWF_T_UNDEF ? to_string(cx, argv[0]) : atom_ascii(vm, ",");
    if (argc && argv[0].t == SWF_T_UNDEF) sep = to_string(cx, argv[0]);
    if (len <= 0) return swf_strv(vm->s.empty);
    swf_str **parts = (swf_str **)swf_frame_alloc(cx, ((size_t)len * sizeof(swf_str *) + sizeof(swf_val) - 1) / sizeof(swf_val));
    uint64_t total = 0;
    for (int32_t i = 0; i < len; i++) {
        parts[i] = to_string(cx, get_el(cx, o, i));
        total += parts[i]->len;
    }
    total += (uint64_t)sep->len * (uint64_t)(len - 1);
    if (total > 0x7FFFFFFF) vm_abort(vm, "Array.join result too large");
    swf_str *r = str_new(vm, (uint32_t)total);
    uint32_t w = 0;
    for (int32_t i = 0; i < len; i++) {
        if (i) {
            memcpy(r->ch + w, sep->ch, sep->len * sizeof(uint16_t));
            w += sep->len;
        }
        memcpy(r->ch + w, parts[i]->ch, parts[i]->len * sizeof(uint16_t));
        w += parts[i]->len;
    }
    return swf_strv(r);
}

static swf_val a_to_string(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)argc; (void)argv;
    return a_join(cx, this_, 0, NULL);
}

static int32_t abs_index(int32_t i, int32_t len)
{
    if (i < 0) return i + len > 0 ? i + len : 0;
    return i < len ? i : len;
}

static swf_val a_slice(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    swf_obj *o = self(cx, this_);
    int32_t len = length_of(cx, o);
    int32_t start = abs_index(to_int32(cx, arg(argc, argv, 0)), len);
    int32_t end = argc > 1 && argv[1].t != SWF_T_UNDEF ? abs_index(to_int32(cx, argv[1]), len) : len;
    int32_t n = end > start ? end - start : 0;
    swf_val *e = swf_frame_alloc(cx, n ? (size_t)n : 1);
    for (int32_t i = 0; i < n; i++) e[i] = get_el(cx, o, start + i);
    return swf_objv(array_new(cx, (uint32_t)n, e));
}

static swf_val a_splice(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    swf_obj *o = self(cx, this_);
    if (argc < 1 || argv[0].t == SWF_T_UNDEF) return swf_undef();
    int32_t len = length_of(cx, o);
    int32_t start = abs_index(to_int32(cx, argv[0]), len);
    int32_t del;
    if (argc > 1) {
        if (argv[1].t == SWF_T_UNDEF) return swf_undef();
        del = to_int32(cx, argv[1]);
        if (del > len - start) del = len - start;
    } else {
        del = len - start;
    }
    if (del < 0) return swf_undef();
    swf_val *removed = swf_frame_alloc(cx, del ? (size_t)del : 1);
    for (int32_t i = 0; i < del; i++) removed[i] = get_el(cx, o, start + i);
    swf_obj *result = array_new(cx, (uint32_t)del, removed);
    int32_t nitems = argc > 2 ? (int32_t)argc - 2 : 0;
    const swf_val *items = argc > 2 ? argv + 2 : NULL;
    if (nitems > del) {
        for (int32_t i = len - 1; i >= start + del; i--) {
            if (has_el(cx, o, i)) set_el(cx, o, i - del + nitems, get_el(cx, o, i));
            else del_el(cx, o, i - del + nitems);
        }
    } else {
        for (int32_t i = start + del; i < len; i++) {
            if (has_el(cx, o, i)) set_el(cx, o, i - del + nitems, get_el(cx, o, i));
            else del_el(cx, o, i - del + nitems);
        }
    }
    for (int32_t i = 0; i < nitems; i++) set_el(cx, o, start + i, items[i]);
    set_length(cx, o, len - del + nitems);
    return swf_objv(result);
}

static bool concat_spreads(swf_val v, uint32_t k)
{
    return v.t == SWF_T_OBJ && (v.u.o->cls == OC_ARRAY || (k == 0 && v.u.o->cls == OC_PLAIN));
}

static swf_val a_concat(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    swf_obj *o = self(cx, this_);
    uint64_t n = 0;
    for (uint32_t k = 0; k <= argc; k++) {
        swf_val v = k == 0 ? swf_objv(o) : argv[k - 1];
        if (concat_spreads(v, k)) {
            int32_t len = length_of(cx, v.u.o);
            n += len > 0 ? (uint64_t)len : 0;
        } else {
            n++;
        }
    }
    if (n > 0x1000000) vm_abort(cx->vm, "Array.concat result too large");
    swf_val *e = swf_frame_alloc(cx, n ? (size_t)n : 1);
    uint32_t w = 0;
    for (uint32_t k = 0; k <= argc && w < n; k++) {
        swf_val v = k == 0 ? swf_objv(o) : argv[k - 1];
        if (concat_spreads(v, k)) {
            int32_t len = length_of(cx, v.u.o);
            for (int32_t i = 0; i < len && w < n; i++) e[w++] = get_el(cx, v.u.o, i);
        } else {
            e[w++] = v;
        }
    }
    return swf_objv(array_new(cx, w, e));
}

/* --- sorting --------------------------------------------------------------------- */

typedef struct sort_item {
    int32_t index;
    swf_val v;
} sort_item;

typedef struct sort_ctx {
    swf_ctx *cx;
    swf_obj *fn;          /* custom compare function */
    swf_str **fields;     /* sortOn fields */
    int32_t *field_opts;
    int32_t nfields;
} sort_ctx;

static int str_cmp_nocase(const swf_str *a, const swf_str *b)
{
    uint32_t n = a->len < b->len ? a->len : b->len;
    for (uint32_t i = 0; i < n; i++) {
        uint16_t x = swf_char_lower(a->ch[i]), y = swf_char_lower(b->ch[i]);
        if (x != y) return x < y ? -1 : 1;
    }
    return a->len < b->len ? -1 : a->len > b->len ? 1 : 0;
}

static int cmp_values(swf_ctx *cx, swf_val a, swf_val b, int32_t opts)
{
    int r;
    if (a.t == SWF_T_NUM && b.t == SWF_T_NUM && (opts & SORT_NUMERIC)) {
        r = a.u.n < b.u.n ? -1 : a.u.n > b.u.n ? 1 : 0;
    } else {
        swf_str *x = to_string(cx, a), *y = to_string(cx, b);
        r = (opts & SORT_CASE_INSENSITIVE) ? str_cmp_nocase(x, y) : str_cmp(x, y);
    }
    return (opts & SORT_DESCENDING) ? -r : r;
}

static int compare(sort_ctx *s, swf_val a, swf_val b, int32_t opts)
{
    swf_ctx *cx = s->cx;
    if (s->fn) {
        swf_val args[2] = {a, b};
        double r = to_number(cx, func_call(cx, s->fn, swf_undef(), 2, args, 0, false));
        return r < 0 ? -1 : r > 0 ? 1 : 0;
    }
    if (s->fields) {
        if (a.t == SWF_T_OBJ && b.t == SWF_T_OBJ) {
            for (int32_t i = 0; i < s->nfields; i++) {
                swf_prop *pa = obj_own(cx->vm, a.u.o, s->fields[i]);
                swf_prop *pb = obj_own(cx->vm, b.u.o, s->fields[i]);
                int r = cmp_values(cx, pa ? pa->val : swf_undef(), pb ? pb->val : swf_undef(), s->field_opts[i]);
                if (r) return r;
            }
            return 0;
        }
        return cmp_values(cx, a, b, opts);
    }
    return cmp_values(cx, a, b, opts);
}

static void qsort_flash(sort_ctx *s, sort_item *e, int32_t n, int32_t opts)
{
    if (n < 2) return;
    /* Frame memory, not malloc: a throwing compare function must not leak. */
    int32_t *stack = (int32_t *)swf_frame_alloc(s->cx, ((size_t)n * 4 * sizeof(int32_t)) / sizeof(swf_val) + 2);
    int32_t sp = 0;
    stack[sp++] = 0;
    stack[sp++] = n - 1;
    while (sp) {
        int32_t high = stack[--sp], low = stack[--sp];
        if (low >= high) continue;
        swf_val pivot = e[low].v;
        int32_t left = low + 1, right = high;
        for (;;) {
            while (left < right) {
                if (compare(s, pivot, e[left].v, opts) <= 0) break;
                left++;
            }
            while (right > low) {
                if (compare(s, pivot, e[right].v, opts) > 0) break;
                right--;
            }
            if (left >= right) break;
            sort_item t = e[left];
            e[left] = e[right];
            e[right] = t;
        }
        sort_item t = e[low];
        e[low] = e[right];
        e[right] = t;
        stack[sp++] = right + 1;
        stack[sp++] = high;
        if (right > 0) {
            stack[sp++] = low;
            stack[sp++] = right - 1;
        }
    }
}

static swf_val sort_internal(sort_ctx *s, swf_obj *o, int32_t opts, bool is_sort_on)
{
    swf_ctx *cx = s->cx;
    int32_t len = length_of(cx, o);
    if (len < 0) len = 0;
    sort_item *e = (sort_item *)swf_frame_alloc(cx, ((size_t)len * sizeof(sort_item) + sizeof(swf_val) - 1) / sizeof(swf_val) + 1);
    for (int32_t i = 0; i < len; i++) {
        e[i].index = i;
        e[i].v = get_el(cx, o, i);
    }
    bool descending = (opts & SORT_DESCENDING) != 0;
    if (!is_sort_on) opts &= ~SORT_DESCENDING;
    qsort_flash(s, e, len, opts);
    if (!is_sort_on && descending) {
        for (int32_t i = 0; i < len / 2; i++) {
            sort_item t = e[i];
            e[i] = e[len - 1 - i];
            e[len - 1 - i] = t;
        }
    }
    if (opts & SORT_UNIQUE) {
        sort_ctx plain = {cx, NULL, NULL, NULL, 0};
        for (int32_t i = 0; i + 1 < len; i++)
            if (compare(is_sort_on ? s : &plain, e[i].v, e[i + 1].v, opts) == 0) return swf_num(0);
    }
    if (opts & SORT_INDEXED) {
        swf_val *idx = swf_frame_alloc(cx, len ? (size_t)len : 1);
        for (int32_t i = 0; i < len; i++) idx[i] = swf_num(e[i].index);
        return swf_objv(array_new(cx, (uint32_t)len, idx));
    }
    for (int32_t i = 0; i < len; i++) set_el(cx, o, i, e[i].v);
    return swf_objv(o);
}

static swf_val a_sort(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    swf_obj *o = self(cx, this_);
    sort_ctx s = {cx, NULL, NULL, NULL, 0};
    int32_t opts = 0;
    if (argc >= 1 && argv[0].t == SWF_T_OBJ) {
        s.fn = argv[0].u.o;
        if (argc >= 2 && argv[1].t == SWF_T_NUM) opts = f64_to_i32(argv[1].u.n);
    } else if (argc >= 1 && argv[0].t == SWF_T_NUM) {
        opts = f64_to_i32(argc >= 2 && argv[1].t == SWF_T_NUM ? argv[1].u.n : argv[0].u.n);
    } else if (argc >= 1) {
        return swf_undef();
    }
    return sort_internal(&s, o, opts & 31, false);
}

static swf_val a_sort_on(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    swf_obj *o = self(cx, this_);
    if (argc < 1) return swf_undef();
    sort_ctx s = {cx, NULL, NULL, NULL, 0};
    if (argv[0].t == SWF_T_OBJ) {
        swf_obj *names = argv[0].u.o;
        if (names->cls != OC_ARRAY) return sort_internal(&s, o, 0, true);
        int32_t n = length_of(cx, names);
        if (n <= 0) return swf_objv(o);
        s.nfields = n;
        s.fields = (swf_str **)swf_frame_alloc(cx, ((size_t)n * sizeof(swf_str *) + sizeof(swf_val) - 1) / sizeof(swf_val));
        s.field_opts = (int32_t *)swf_frame_alloc(cx, ((size_t)n * sizeof(int32_t) + sizeof(swf_val) - 1) / sizeof(swf_val));
        for (int32_t i = 0; i < n; i++) {
            s.fields[i] = atom(cx->vm, to_string(cx, get_el(cx, names, i)));
            s.field_opts[i] = 0;
        }
        swf_val ov = arg(argc, argv, 1);
        if (ov.t == SWF_T_OBJ && ov.u.o->cls == OC_ARRAY && length_of(cx, ov.u.o) == n) {
            for (int32_t i = 0; i < n; i++) s.field_opts[i] = to_int32(cx, get_el(cx, ov.u.o, i)) & 31;
        } else if (argc > 1 && ov.t != SWF_T_OBJ) {
            int32_t opt = to_int32(cx, ov) & 31;
            for (int32_t i = 0; i < n; i++) s.field_opts[i] = opt;
        }
    } else {
        s.nfields = 1;
        s.fields = (swf_str **)swf_frame_alloc(cx, 1);
        s.field_opts = (int32_t *)swf_frame_alloc(cx, 1);
        s.fields[0] = atom(cx->vm, to_string(cx, argv[0]));
        s.field_opts[0] = argc > 1 && argv[1].t == SWF_T_NUM ? f64_to_i32(argv[1].u.n) & 31 : 0;
    }
    return sort_internal(&s, o, s.field_opts[0], true);
}

void builtins_array(swf_vm *vm)
{
    swf_obj *p = obj_new(vm, vm->object_proto);
    native_method(vm, p, "push", a_push, DD);
    native_method(vm, p, "unshift", a_unshift, DD);
    native_method(vm, p, "shift", a_shift, DD);
    native_method(vm, p, "pop", a_pop, DD);
    native_method(vm, p, "reverse", a_reverse, DD);
    native_method(vm, p, "join", a_join, DD);
    native_method(vm, p, "toString", a_to_string, DD);
    native_method(vm, p, "slice", a_slice, DD);
    native_method(vm, p, "splice", a_splice, DD);
    native_method(vm, p, "concat", a_concat, DD);
    native_method(vm, p, "sort", a_sort, DD);
    native_method(vm, p, "sortOn", a_sort_on, DD);
    vm->array_proto = p;
    swf_obj *c = make_class(vm, "Array", array_fn, array_ctor, p);
    vm->array_ctor = c;
    const uint16_t C = DD | ATTR_READ_ONLY;
    obj_define_ascii(vm, c, "CASEINSENSITIVE", swf_num(SORT_CASE_INSENSITIVE), C);
    obj_define_ascii(vm, c, "DESCENDING", swf_num(SORT_DESCENDING), C);
    obj_define_ascii(vm, c, "UNIQUESORT", swf_num(SORT_UNIQUE), C);
    obj_define_ascii(vm, c, "RETURNINDEXEDARRAY", swf_num(SORT_INDEXED), C);
    obj_define_ascii(vm, c, "NUMERIC", swf_num(SORT_NUMERIC), C);
}
