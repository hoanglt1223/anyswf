/* SharedObject.getLocal: persistent `data` objects saved as <name>.sol in the game's
 * user directory (ms0:/PSP/SAVEDATA/<app id>/ on PSP, named by the build's game id). The file
 * is an 8-byte magic, a u32 version and the data object as AMF0 (Flash's own encoding,
 * with object references for shared and cyclic values). Files are written by flush(),
 * and for every open object when the VM shuts down, as Flash does on exit. Values are
 * read straight from the properties: getters, functions and clips are not saved. */
#include "../avm1/vm_internal.h"

#include <math.h>
#include <stdio.h>

#define DD (ATTR_DONT_ENUM | ATTR_DONT_DELETE)
#define MAGIC "ANYSWFSO"
#define FORMAT_VERSION 1
#define MAX_DEPTH 64
#define MAX_FILE (4u * 1024 * 1024)
#define MAX_REFS 65535

enum {
    AMF_NUMBER = 0, AMF_BOOL = 1, AMF_STRING = 2, AMF_OBJECT = 3, AMF_NULL = 5, AMF_UNDEFINED = 6,
    AMF_REFERENCE = 7, AMF_ECMA_ARRAY = 8, AMF_OBJECT_END = 9, AMF_STRICT_ARRAY = 10, AMF_DATE = 11,
    AMF_LONG_STRING = 12
};

/* --- writer ----------------------------------------------------------------------------- */

typedef struct wbuf {
    uint8_t *p;
    size_t n, cap;
    bool oom;
    swf_obj **seen;
    uint32_t nseen, cap_seen;
} wbuf;

static void put(wbuf *b, const void *data, size_t n)
{
    if (b->oom) return;
    if (b->n + n > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 256;
        while (cap < b->n + n) cap *= 2;
        uint8_t *q = realloc(b->p, cap);
        if (!q) {
            b->oom = true;
            return;
        }
        b->p = q;
        b->cap = cap;
    }
    memcpy(b->p + b->n, data, n);
    b->n += n;
}

static void put_u8(wbuf *b, uint8_t v) { put(b, &v, 1); }
static void put_u16(wbuf *b, uint16_t v)
{
    uint8_t q[2] = {(uint8_t)(v >> 8), (uint8_t)v};
    put(b, q, 2);
}
static void put_u32(wbuf *b, uint32_t v)
{
    uint8_t q[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    put(b, q, 4);
}
static void put_f64(wbuf *b, double d)
{
    uint64_t u;
    memcpy(&u, &d, 8);
    put_u32(b, (uint32_t)(u >> 32));
    put_u32(b, (uint32_t)u);
}

static void put_utf8(wbuf *b, const swf_str *s, bool with_marker)
{
    size_t n;
    char *u = str_to_utf8(s, &n);
    if (!u) {
        b->oom = true;
        return;
    }
    if (n > 0xFFFF) {
        if (with_marker) put_u8(b, AMF_LONG_STRING);
        put_u32(b, (uint32_t)n);
    } else {
        if (with_marker) put_u8(b, AMF_STRING);
        put_u16(b, (uint16_t)n);
    }
    put(b, u, n);
    free(u);
}

static bool saveable(const swf_val *v)
{
    if (v->t != SWF_T_OBJ) return true;
    uint8_t c = v->u.o->cls;
    return c != OC_FUNCTION && c != OC_CLIP && c != OC_SUPER && c != OC_SOUND;
}

static void write_value(swf_vm *vm, wbuf *b, swf_val v, int depth);

static void write_props(swf_vm *vm, wbuf *b, swf_obj *o, int depth, bool skip_length)
{
    for (uint32_t i = 0; i < o->nprops; i++) {
        swf_prop *p = &o->props[i];
        if (!p->key || p->getter || (p->attr & ATTR_DONT_ENUM) || !saveable(&p->val)) continue;
        if (skip_length && p->key == vm->s.length) continue;
        put_utf8(b, p->key, false);
        write_value(vm, b, p->val, depth + 1);
    }
    put_u16(b, 0);
    put_u8(b, AMF_OBJECT_END);
}

static void write_value(swf_vm *vm, wbuf *b, swf_val v, int depth)
{
    switch (v.t) {
    case SWF_T_NUM: put_u8(b, AMF_NUMBER); put_f64(b, v.u.n); return;
    case SWF_T_BOOL: put_u8(b, AMF_BOOL); put_u8(b, v.u.b ? 1 : 0); return;
    case SWF_T_STR: put_utf8(b, v.u.s, true); return;
    case SWF_T_NULL: put_u8(b, AMF_NULL); return;
    case SWF_T_OBJ: break;
    default: put_u8(b, AMF_UNDEFINED); return;
    }
    swf_obj *o = v.u.o;
    if (depth > MAX_DEPTH) {
        put_u8(b, AMF_NULL);
        return;
    }
    for (uint32_t i = 0; i < b->nseen; i++) {
        if (b->seen[i] == o) {
            put_u8(b, AMF_REFERENCE);
            put_u16(b, (uint16_t)i);
            return;
        }
    }
    if (o->cls == OC_DATE) {
        put_u8(b, AMF_DATE);
        put_f64(b, o->u.date);
        put_u16(b, 0);
        return;
    }
    if (o->cls == OC_STRING || o->cls == OC_NUMBER || o->cls == OC_BOOLEAN) {
        write_value(vm, b, o->u.prim, depth);
        return;
    }
    if (b->nseen < MAX_REFS) {
        if (b->nseen == b->cap_seen) {
            uint32_t cap = b->cap_seen ? b->cap_seen * 2 : 32;
            swf_obj **s = realloc(b->seen, cap * sizeof *s);
            if (!s) {
                b->oom = true;
                return;
            }
            b->seen = s;
            b->cap_seen = cap;
        }
        b->seen[b->nseen++] = o;
    }
    if (o->cls == OC_ARRAY) {
        swf_ctx c = vm_ctx(vm);
        put_u8(b, AMF_ECMA_ARRAY);
        put_u32(b, array_length(&c, o));
        write_props(vm, b, o, depth, true);
        return;
    }
    put_u8(b, AMF_OBJECT);
    write_props(vm, b, o, depth, false);
}

/* --- reader --------------------------------------------------------------------------- */

typedef struct rbuf {
    const uint8_t *p;
    size_t n, at;
    bool bad;
    swf_obj **refs;
    uint32_t nrefs, cap_refs;
} rbuf;

static bool need(rbuf *r, size_t n)
{
    if (r->bad || r->n - r->at < n) {
        r->bad = true;
        return false;
    }
    return true;
}

static uint8_t get_u8(rbuf *r) { return need(r, 1) ? r->p[r->at++] : 0; }
static uint16_t get_u16(rbuf *r)
{
    if (!need(r, 2)) return 0;
    uint16_t v = (uint16_t)((r->p[r->at] << 8) | r->p[r->at + 1]);
    r->at += 2;
    return v;
}
static uint32_t get_u32(rbuf *r)
{
    if (!need(r, 4)) return 0;
    const uint8_t *q = r->p + r->at;
    r->at += 4;
    return ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) | ((uint32_t)q[2] << 8) | q[3];
}
static double get_f64(rbuf *r)
{
    uint64_t hi = get_u32(r), lo = get_u32(r), u = (hi << 32) | lo;
    double d;
    memcpy(&d, &u, 8);
    return d;
}

static swf_str *get_str(swf_vm *vm, rbuf *r, uint32_t len)
{
    if (!need(r, len)) return vm->s.empty;
    swf_str *s = str_from_utf8(vm, (const char *)r->p + r->at, len);
    r->at += len;
    return s;
}

static void add_ref(rbuf *r, swf_obj *o)
{
    if (r->nrefs == r->cap_refs) {
        uint32_t cap = r->cap_refs ? r->cap_refs * 2 : 32;
        swf_obj **n = realloc(r->refs, cap * sizeof *n);
        if (!n) {
            r->bad = true;
            return;
        }
        r->refs = n;
        r->cap_refs = cap;
    }
    r->refs[r->nrefs++] = o;
}

static swf_val read_value(swf_ctx *cx, rbuf *r, int depth);

static void read_props(swf_ctx *cx, rbuf *r, swf_obj *o, int depth)
{
    while (!r->bad) {
        uint16_t len = get_u16(r);
        if (len == 0) {
            if (get_u8(r) != AMF_OBJECT_END) r->bad = true;
            return;
        }
        swf_str *k = get_str(cx->vm, r, len);
        swf_val v = read_value(cx, r, depth + 1);
        if (!r->bad) obj_set(cx, o, k, v);
    }
}

static swf_val read_value(swf_ctx *cx, rbuf *r, int depth)
{
    swf_vm *vm = cx->vm;
    if (depth > MAX_DEPTH) {
        r->bad = true;
        return swf_undef();
    }
    switch (get_u8(r)) {
    case AMF_NUMBER: return swf_num(get_f64(r));
    case AMF_BOOL: return swf_bool(get_u8(r) != 0);
    case AMF_STRING: return swf_strv(get_str(vm, r, get_u16(r)));
    case AMF_LONG_STRING: return swf_strv(get_str(vm, r, get_u32(r)));
    case AMF_NULL: return swf_null();
    case AMF_UNDEFINED: return swf_undef();
    case AMF_REFERENCE: {
        uint16_t i = get_u16(r);
        if (i >= r->nrefs) {
            r->bad = true;
            return swf_undef();
        }
        return swf_objv(r->refs[i]);
    }
    case AMF_DATE: {
        double t = get_f64(r);
        (void)get_u16(r);
        swf_obj *d = obj_new(vm, vm->date_proto);
        d->cls = OC_DATE;
        d->u.date = t;
        return swf_objv(d);
    }
    case AMF_OBJECT: {
        swf_obj *o = obj_new(vm, vm->object_proto);
        add_ref(r, o);
        read_props(cx, r, o, depth);
        return swf_objv(o);
    }
    case AMF_ECMA_ARRAY: {
        uint32_t len = get_u32(r);
        swf_obj *a = array_new(cx, 0, NULL);
        add_ref(r, a);
        read_props(cx, r, a, depth);
        obj_set(cx, a, vm->s.length, swf_num(len));
        return swf_objv(a);
    }
    case AMF_STRICT_ARRAY: {
        uint32_t len = get_u32(r);
        swf_obj *a = array_new(cx, 0, NULL);
        add_ref(r, a);
        for (uint32_t i = 0; i < len && !r->bad; i++) obj_set(cx, a, atom_int(vm, i), read_value(cx, r, depth + 1));
        return swf_objv(a);
    }
    default:
        r->bad = true;
        return swf_undef();
    }
}

/* --- files ------------------------------------------------------------------------------ */

static bool file_name(swf_str *name, char *out, size_t cap)
{
    char *u = str_to_utf8(name, NULL);
    if (!u) return false;
    int n = 0;
    for (const char *p = u; *p && n + 5 < (int)cap; p++) {
        unsigned char c = (unsigned char)*p;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        out[n++] = ok ? (char)c : '_';
    }
    free(u);
    if (n + 5 >= (int)cap) return false;
    memcpy(out + n, ".sol", 5);
    return true;
}

static swf_obj *data_of(swf_vm *vm, swf_obj *so)
{
    swf_prop *p = obj_own(vm, so, atom_ascii(vm, "data"));
    return p && p->val.t == SWF_T_OBJ ? p->val.u.o : NULL;
}

static swf_str *name_of(swf_vm *vm, swf_obj *so)
{
    swf_prop *p = obj_own(vm, so, atom_ascii(vm, "$name"));
    return p && p->val.t == SWF_T_STR ? p->val.u.s : NULL;
}

static bool has_data(const swf_obj *o)
{
    for (uint32_t i = 0; i < o->nprops; i++)
        if (o->props[i].key && !(o->props[i].attr & ATTR_DONT_ENUM)) return true;
    return false;
}

static bool encode(swf_vm *vm, swf_obj *data, wbuf *b)
{
    put(b, MAGIC, 8);
    put_u32(b, FORMAT_VERSION);
    write_value(vm, b, swf_objv(data), 0);
    free(b->seen);
    b->seen = NULL;
    return !b->oom;
}

static bool save(swf_vm *vm, swf_obj *so)
{
    swf_obj *data = data_of(vm, so);
    swf_str *name = name_of(vm, so);
    char path[PLAT_PATH_MAX];
    if (!data || !name || !file_name(name, path, sizeof path)) return false;
    wbuf b = {0};
    bool ok = encode(vm, data, &b);
    if (ok) {
        plat_result r = plat_file_write_all(PLAT_ROOT_USER, path, b.p, b.n);
        if (r != PLAT_OK) {
            vm_warn(vm, WARN_SO_IO, "SharedObject: cannot write %s (error %d)", path, (int)r);
            ok = false;
        }
    }
    free(b.p);
    return ok;
}

static void load(swf_ctx *cx, swf_obj *data, swf_str *name)
{
    swf_vm *vm = cx->vm;
    char path[PLAT_PATH_MAX];
    if (!file_name(name, path, sizeof path)) return;
    void *buf;
    size_t size;
    plat_result res = plat_file_read_all(PLAT_ROOT_USER, path, &buf, &size);
    if (res != PLAT_OK) return;
    if (size < 13 || size > MAX_FILE || memcmp(buf, MAGIC, 8) != 0) {
        vm_warn(vm, WARN_SO_IO, "SharedObject: %s is not a save file of this format; ignored", path);
        free(buf);
        return;
    }
    rbuf r = {buf, size, 8, false, NULL, 0, 0};
    if (get_u32(&r) != FORMAT_VERSION) {
        vm_warn(vm, WARN_SO_IO, "SharedObject: %s has an unknown version; ignored", path);
        free(buf);
        return;
    }
    /* The top-level object reads into the existing `data` object. */
    if (get_u8(&r) == AMF_OBJECT) {
        add_ref(&r, data);
        read_props(cx, &r, data, 0);
    } else {
        r.bad = true;
    }
    if (r.bad) vm_warn(vm, WARN_SO_IO, "SharedObject: %s is damaged; data read up to the damage", path);
    free(r.refs);
    free(buf);
}

/* --- class ------------------------------------------------------------------------------- */

static bool valid_name(const swf_str *s)
{
    if (s->len == 0) return false;
    for (uint32_t i = 0; i < s->len; i++) {
        uint16_t c = s->ch[i];
        if (c == '~' || c == '%' || c == '&' || c == '\\' || c == ';' || c == ':' || c == '"' || c == '\'' ||
            c == ',' || c == '<' || c == '>' || c == '?' || c == '#' || c == ' ')
            return false;
    }
    return true;
}

static swf_val so_get_local(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)this_;
    swf_vm *vm = cx->vm;
    if (argc < 1) return swf_null();
    swf_str *name = to_string(cx, argv[0]);
    if (!valid_name(name)) return swf_null();
    swf_str *key = atom(vm, name);
    swf_prop *cached = obj_own(vm, vm->shared_objects, key);
    if (cached && cached->val.t == SWF_T_OBJ) return cached->val;
    swf_val ctor = obj_get(cx, vm->global, atom_ascii(vm, "SharedObject"));
    swf_val proto = ctor.t == SWF_T_OBJ ? obj_get(cx, ctor.u.o, vm->s.prototype) : swf_undef();
    swf_obj *so = obj_new(vm, proto.t == SWF_T_OBJ ? proto.u.o : vm->object_proto);
    swf_obj *data = obj_new(vm, vm->object_proto);
    obj_define_ascii(vm, so, "data", swf_objv(data), DD);
    obj_define_ascii(vm, so, "$name", swf_strv(key), DD | ATTR_READ_ONLY);
    load(cx, data, key);
    obj_define(vm, vm->shared_objects, key, swf_objv(so), 0);
    return swf_objv(so);
}

static swf_val so_flush(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)argc; (void)argv;
    if (this_.t != SWF_T_OBJ || !name_of(cx->vm, this_.u.o)) return swf_undef();
    return swf_bool(save(cx->vm, this_.u.o));
}

static swf_val so_get_size(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)argc; (void)argv;
    if (this_.t != SWF_T_OBJ) return swf_undef();
    swf_obj *data = data_of(cx->vm, this_.u.o);
    if (!data) return swf_num(0);
    wbuf b = {0};
    bool ok = encode(cx->vm, data, &b);
    free(b.p);
    /* An empty data object counts as zero bytes, as in Flash. */
    return swf_num(ok && has_data(data) ? (double)b.n : 0);
}

static swf_val so_clear(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)argc; (void)argv;
    swf_vm *vm = cx->vm;
    if (this_.t != SWF_T_OBJ) return swf_undef();
    swf_str *name = name_of(vm, this_.u.o);
    if (!name) return swf_undef();
    obj_define_ascii(vm, this_.u.o, "data", swf_objv(obj_new(vm, vm->object_proto)), DD);
    char path[PLAT_PATH_MAX];
    if (file_name(name, path, sizeof path)) (void)plat_file_remove(PLAT_ROOT_USER, path);
    return swf_undef();
}

static swf_val so_ctor(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)cx; (void)argc; (void)argv;
    return this_;
}

void sharedobject_flush_all(swf_vm *vm)
{
    swf_obj *c = vm->shared_objects;
    if (!c) return;
    for (uint32_t i = 0; i < c->nprops; i++) {
        swf_prop *p = &c->props[i];
        if (!p->key || p->val.t != SWF_T_OBJ) continue;
        swf_obj *data = data_of(vm, p->val.u.o);
        if (data && has_data(data)) (void)save(vm, p->val.u.o);
    }
}

void builtins_sharedobject(swf_vm *vm)
{
    swf_obj *p = obj_new(vm, vm->object_proto);
    swf_obj *c = func_new_native(vm, so_ctor, NULL, p);
    c->u.fn.ctor = NULL;
    obj_define_ascii(vm, vm->global, "SharedObject", swf_objv(c), ATTR_DONT_ENUM | ATTR_VERSION_6);
    native_method(vm, c, "getLocal", so_get_local, DD);
    native_method(vm, p, "flush", so_flush, DD);
    native_method(vm, p, "getSize", so_get_size, DD);
    native_method(vm, p, "clear", so_clear, DD);
}
