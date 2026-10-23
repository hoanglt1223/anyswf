/* Objects: ordered property maps keyed by atoms, `__proto__` chains, virtual
 * properties (addProperty), watchers, and the display-object property fallbacks.
 * Behaviour follows Ruffle's avm1/object.rs and object/script_object.rs. */
#include "vm_internal.h"

static const uint16_t VERSION_MASKS[10] = {0x7FF8, 0x7FF8, 0x7FF8, 0x7FF8, 0x7FF8,
                                           0x7480, 0x7500, 0x7000, 0x6000, 0x4000};

bool prop_visible(const swf_prop *p, uint8_t version)
{
    uint16_t mask = version < 10 ? VERSION_MASKS[version] : 0;
    return (p->attr & mask) == 0;
}

swf_obj *obj_new_class(swf_vm *vm, swf_obj *proto, uint8_t cls)
{
    swf_obj *o = gc_alloc(vm, sizeof(swf_obj), SWF_GC_OBJ);
    o->cls = cls;
    if (proto) obj_define(vm, o, vm->s.proto, swf_objv(proto), ATTR_DONT_ENUM | ATTR_DONT_DELETE);
    return o;
}

swf_obj *obj_new(swf_vm *vm, swf_obj *proto)
{
    return obj_new_class(vm, proto, OC_PLAIN);
}

swf_obj *obj_new_bare(swf_vm *vm)
{
    return obj_new_class(vm, NULL, OC_PLAIN);
}

static bool key_match(const swf_str *a, const swf_str *b, bool cs)
{
    return cs ? a == b : a->fold == b->fold;
}

static void index_rebuild(swf_vm *vm, swf_obj *o, uint32_t cap)
{
    int32_t *idx = malloc(cap * sizeof *idx);
    if (!idx) vm_abort(vm, "out of memory growing an object");
    memset(idx, 0xFF, cap * sizeof *idx);
    for (uint32_t i = 0; i < o->nprops; i++) {
        swf_str *k = o->props[i].key;
        if (!k) continue;
        uint32_t h = k->hash & (cap - 1);
        while (idx[h] != -1) h = (h + 1) & (cap - 1);
        idx[h] = (int32_t)i;
    }
    free(o->index);
    o->index = idx;
    o->index_cap = cap;
}

static void compact(swf_vm *vm, swf_obj *o)
{
    uint32_t w = 0;
    for (uint32_t r = 0; r < o->nprops; r++)
        if (o->props[r].key) o->props[w++] = o->props[r];
    o->nprops = w;
    index_rebuild(vm, o, o->index_cap ? o->index_cap : 8);
}

static int32_t find(swf_obj *o, const swf_str *key, bool cs)
{
    if (!o->index_cap) return -1;
    uint32_t mask = o->index_cap - 1, h = key->hash & mask;
    for (;;) {
        int32_t i = o->index[h];
        if (i == -1) return -1;
        swf_str *k = o->props[i].key;
        if (k && key_match(k, key, cs)) return i;
        h = (h + 1) & mask;
    }
}

static swf_prop *insert(swf_vm *vm, swf_obj *o, swf_str *key)
{
    if (o->nprops == o->cap) {
        if (o->live < o->nprops / 2 && o->nprops > 8) {
            compact(vm, o);
        } else {
            uint32_t cap = o->cap ? o->cap * 2 : 4;
            swf_prop *p = realloc(o->props, cap * sizeof *p);
            if (!p) vm_abort(vm, "out of memory growing an object");
            o->props = p;
            o->cap = cap;
        }
    }
    if ((o->nprops + 1) * 2 > o->index_cap) index_rebuild(vm, o, o->index_cap ? o->index_cap * 2 : 8);
    uint32_t i = o->nprops++;
    swf_prop *p = &o->props[i];
    memset(p, 0, sizeof *p);
    p->key = key;
    uint32_t mask = o->index_cap - 1, h = key->hash & mask;
    while (o->index[h] != -1) h = (h + 1) & mask;
    o->index[h] = (int32_t)i;
    o->live++;
    return p;
}

swf_prop *obj_own(swf_vm *vm, swf_obj *o, swf_str *key)
{
    int32_t i = find(o, key, vm->case_sensitive);
    return i < 0 ? NULL : &o->props[i];
}

void obj_define(swf_vm *vm, swf_obj *o, swf_str *key, swf_val v, uint16_t attr)
{
    if (o->cls == OC_SUPER) return;
    key = atom(vm, key);
    int32_t i = find(o, key, true);
    swf_prop *p = i >= 0 ? &o->props[i] : insert(vm, o, key);
    p->val = v;
    p->getter = p->setter = NULL;
    p->attr = attr;
}

void obj_define_ascii(swf_vm *vm, swf_obj *o, const char *key, swf_val v, uint16_t attr)
{
    obj_define(vm, o, atom_ascii(vm, key), v, attr);
}

static void remove_at(swf_obj *o, int32_t i)
{
    swf_str *k = o->props[i].key;
    uint32_t mask = o->index_cap - 1, h = k->hash & mask;
    while (o->index[h] != i) h = (h + 1) & mask;
    /* Backward-shift deletion keeps probe chains intact. */
    uint32_t j = h;
    for (;;) {
        j = (j + 1) & mask;
        if (o->index[j] == -1) break;
        uint32_t home = o->props[o->index[j]].key->hash & mask;
        if ((j > h && (home <= h || home > j)) || (j < h && (home <= h && home > j))) {
            o->index[h] = o->index[j];
            h = j;
        }
    }
    o->index[h] = -1;
    o->props[i].key = NULL;
    o->live--;
}

bool obj_delete(swf_ctx *cx, swf_obj *o, swf_str *key)
{
    if (o->cls == OC_SUPER) return false;
    int32_t i = find(o, atom(cx->vm, key), cx->vm->case_sensitive);
    if (i < 0 || (o->props[i].attr & ATTR_DONT_DELETE)) return false;
    remove_at(o, i);
    return true;
}

swf_val obj_get_data(swf_ctx *cx, swf_obj *o, swf_str *key)
{
    swf_prop *p = obj_own(cx->vm, o, key);
    return p ? p->val : swf_undef();
}

swf_val obj_proto(swf_ctx *cx, swf_obj *o)
{
    if (o->cls == OC_SUPER) {
        swf_obj *p = o->u.super_.this_;
        for (uint8_t i = 0; i < o->u.super_.depth && p; i++) {
            swf_val n = obj_proto(cx, p);
            p = n.t == SWF_T_OBJ ? n.u.o : NULL;
        }
        return p ? obj_proto(cx, p) : swf_undef();
    }
    swf_prop *p = obj_own(cx->vm, o, cx->vm->s.proto);
    return p ? p->val : swf_undef();
}

bool obj_has_own(swf_ctx *cx, swf_obj *o, swf_str *key)
{
    if (o->cls == OC_SUPER) return obj_has_own(cx, o->u.super_.this_, key);
    return find(o, atom(cx->vm, key), cx->vm->case_sensitive) >= 0;
}

bool obj_has_property(swf_ctx *cx, swf_obj *o, swf_str *key)
{
    if (o->cls == OC_SUPER) return obj_has_property(cx, o->u.super_.this_, key);
    key = atom(cx->vm, key);
    int depth = 0;
    swf_obj *p = o;
    bool removed_clip = o->cls == OC_CLIP && !o->u.clip;
    while (p && depth < 256 && !removed_clip) {
        if (find(p, key, cx->vm->case_sensitive) >= 0) return true;
        swf_val n = obj_proto(cx, p);
        p = n.t == SWF_T_OBJ ? n.u.o : NULL;
        depth++;
    }
    if (o->cls == OC_CLIP && o->u.clip) {
        swf_val v;
        return clip_get_display_prop(cx, o->u.clip, key, &v);
    }
    return false;
}

static swf_val call_accessor(swf_ctx *cx, swf_obj *fn, swf_val this_, uint32_t argc, const swf_val *argv)
{
    return func_call(cx, fn, this_, argc, argv, 1, false);
}

/* Ruffle's search_prototype: getters and stored values along the chain, then the
 * display-object fallbacks of each clip in the chain. */
static bool search(swf_ctx *cx, swf_val start, swf_str *key, swf_obj *this_, swf_val *out, int *depth_out)
{
    int depth = 0;
    swf_val pv = start;
    while (pv.t == SWF_T_OBJ) {
        swf_obj *p = pv.u.o;
        if (depth == 255) vm_abort(cx->vm, "prototype chain too deep (255 levels)");
        if (p->cls == OC_SUPER) {
            pv = obj_proto(cx, p);
            continue;
        }
        int32_t i = find(p, key, cx->vm->case_sensitive);
        if (i >= 0 && prop_visible(&p->props[i], cx->version)) {
            swf_prop *pr = &p->props[i];
            if (pr->getter) {
                *out = call_accessor(cx, pr->getter, swf_objv(this_), 0, NULL);
            } else {
                *out = pr->val;
            }
            if (depth_out) *depth_out = depth;
            return true;
        }
        if (p->cls == OC_CLIP && p->u.clip && clip_get_display_prop(cx, p->u.clip, key, out)) {
            if (depth_out) *depth_out = depth;
            return true;
        }
        pv = obj_proto(cx, p);
        depth++;
    }
    return false;
}

static swf_obj *find_resolve(swf_ctx *cx, swf_val start)
{
    swf_val r;
    if (start.t != SWF_T_OBJ) return NULL;
    swf_val pv = start;
    int depth = 0;
    while (pv.t == SWF_T_OBJ && depth < 255) {
        swf_obj *p = pv.u.o;
        int32_t i = p->cls == OC_SUPER ? -1 : find(p, cx->vm->s.__resolve, cx->vm->case_sensitive);
        if (i >= 0 && prop_visible(&p->props[i], cx->version)) {
            r = p->props[i].getter ? call_accessor(cx, p->props[i].getter, start, 0, NULL) : p->props[i].val;
            return r.t == SWF_T_OBJ ? r.u.o : NULL;
        }
        pv = obj_proto(cx, p);
        depth++;
    }
    return NULL;
}

bool obj_get_opt(swf_ctx *cx, swf_obj *o, swf_str *key, bool resolve, swf_val *out, int *depth)
{
    key = atom(cx->vm, key);
    swf_obj *this_ = o;
    swf_val start = swf_objv(o);
    if (o->cls == OC_SUPER) {
        this_ = o->u.super_.this_;
        start = obj_proto(cx, o);
    }
    if (search(cx, start, key, this_, out, depth)) return true;
    if (resolve) {
        swf_obj *r = find_resolve(cx, start);
        if (r) {
            swf_val name = swf_strv(key);
            *out = func_call(cx, r, swf_objv(this_), 1, &name, 0, false);
            if (depth) *depth = 0;
            return true;
        }
    }
    return false;
}

swf_val obj_get(swf_ctx *cx, swf_obj *o, swf_str *key)
{
    swf_val v;
    return obj_get_opt(cx, o, key, true, &v, NULL) ? v : swf_undef();
}

static void call_watcher(swf_ctx *cx, swf_obj *o, swf_str *key, swf_val *value)
{
    if (!o->watchers) return;
    swf_prop *w = obj_own(cx->vm, o->watchers, key);
    if (!w || w->val.t != SWF_T_OBJ) return;
    swf_obj *rec = w->val.u.o;
    swf_val cb = obj_get_data(cx, rec, cx->vm->s.callee);
    swf_val user = obj_get_data(cx, rec, cx->vm->s.arguments);
    if (cb.t != SWF_T_OBJ) return;
    swf_val old;
    if (!obj_get_opt(cx, o, key, false, &old, NULL)) old = swf_undef();
    swf_val args[4] = {swf_strv(key), old, *value, user};
    *value = func_call(cx, cb.u.o, swf_objv(o), 4, args, 1, false);
}

void obj_set(swf_ctx *cx, swf_obj *o, swf_str *key, swf_val v)
{
    swf_vm *vm = cx->vm;
    if (key->len == 0) return;
    key = atom(vm, key);
    swf_obj *this_ = o;
    swf_val proto = swf_objv(o);
    if (o->cls == OC_SUPER) {
        this_ = o->u.super_.this_;
        proto = obj_proto(cx, o);
    }
    call_watcher(cx, this_, key, &v);
    if (o->cls == OC_SUPER) return;

    int32_t own = find(o, key, vm->case_sensitive);
    if (own < 0) {
        /* A virtual setter anywhere on the prototype chain takes the assignment. */
        int depth = 0;
        while (proto.t == SWF_T_OBJ && depth < 256) {
            swf_obj *p = proto.u.o;
            int32_t i = find(p, key, vm->case_sensitive);
            if (i >= 0 && p->props[i].getter && prop_visible(&p->props[i], cx->version)) {
                if (p->props[i].setter) call_accessor(cx, p->props[i].setter, swf_objv(this_), 1, &v);
                return;
            }
            proto = obj_proto(cx, p);
            depth++;
        }
    }
    if (o->cls == OC_ARRAY) {
        if (key == vm->s.length || (!vm->case_sensitive && key->fold == vm->s.length->fold)) {
            int32_t new_len = to_int32(cx, v);
            uint32_t old = array_length(cx, o);
            for (int64_t i = new_len < 0 ? 0 : new_len; i < (int64_t)old; i++) obj_delete(cx, o, atom_int(vm, i));
        } else {
            /* Writing an index at or past the end grows length. */
            uint32_t idx = 0;
            bool numeric = key->len > 0 && key->len <= 10;
            for (uint32_t k = 0; numeric && k < key->len; k++) {
                uint16_t c = key->ch[k];
                if (c < '0' || c > '9' || (k == 0 && c == '0' && key->len > 1)) numeric = false;
                else idx = idx * 10 + (c - '0');
            }
            if (numeric && idx >= array_length(cx, o) && idx < 0xFFFFFFFFu) {
                swf_prop *lp = obj_own(vm, o, vm->s.length);
                if (lp) lp->val = swf_num((double)idx + 1);
            }
        }
    } else if (o->cls == OC_CLIP && o->u.clip) {
        if (own < 0 && clip_set_display_prop(cx, o->u.clip, key, v)) return;
    }
    if (own >= 0) {
        swf_prop *p = &o->props[own];
        if (p->getter) {
            if (p->setter) call_accessor(cx, p->setter, swf_objv(this_), 1, &v);
            return;
        }
        if (p->attr & ATTR_READ_ONLY) return;
        p->val = v;
        return;
    }
    swf_prop *p = insert(vm, o, key);
    p->val = v;
}

swf_val obj_call_method(swf_ctx *cx, swf_obj *o, swf_str *name, uint32_t argc, const swf_val *argv, bool special)
{
    if (o->cls == OC_CLIP && !o->u.clip) return swf_undef();
    swf_val m;
    int depth = 0;
    swf_obj *this_ = o;
    swf_val start = swf_objv(o);
    uint8_t base_depth = 0;
    if (o->cls == OC_SUPER) {
        this_ = o->u.super_.this_;
        start = obj_proto(cx, o);
        base_depth = (uint8_t)(o->u.super_.depth + 1);
    }
    name = atom(cx->vm, name);
    bool found = search(cx, start, name, this_, &m, &depth);
    if (!found && !special) {
        swf_obj *r = find_resolve(cx, start);
        if (r) {
            swf_val n = swf_strv(name);
            m = func_call(cx, r, swf_objv(this_), 1, &n, 0, false);
            found = true;
            depth = 0;
        }
    }
    if (!found || m.t != SWF_T_OBJ) return swf_undef();
    uint8_t d = (uint8_t)(o->cls == OC_SUPER ? base_depth + depth : (depth < 1 ? 1 : depth));
    return func_call(cx, m.u.o, swf_objv(this_), argc, argv, d, false);
}

typedef struct keybuf {
    swf_str **k;
    uint32_t n, cap;
} keybuf;

static void key_push(swf_vm *vm, keybuf *b, swf_str *k)
{
    if (b->n == b->cap) {
        uint32_t cap = b->cap ? b->cap * 2 : 16;
        swf_str **n = realloc(b->k, cap * sizeof *n);
        if (!n) vm_abort(vm, "out of memory enumerating an object");
        b->k = n;
        b->cap = cap;
    }
    b->k[b->n++] = k;
}

static void keys_rec(swf_ctx *cx, swf_obj *o, bool hidden, keybuf *b, int depth)
{
    if (o->cls == OC_SUPER || depth > 255) return;
    swf_val pv = obj_proto(cx, o);
    if (pv.t == SWF_T_OBJ) {
        keybuf pk = {0};
        keys_rec(cx, pv.u.o, hidden, &pk, depth + 1);
        for (uint32_t i = 0; i < pk.n; i++)
            if (find(o, pk.k[i], cx->vm->case_sensitive) < 0) key_push(cx->vm, b, pk.k[i]);
        free(pk.k);
    }
    for (int64_t i = (int64_t)o->nprops - 1; i >= 0; i--) {
        swf_prop *p = &o->props[i];
        if (!p->key) continue;
        if (hidden || !(p->attr & ATTR_DONT_ENUM)) key_push(cx->vm, b, p->key);
    }
}

swf_str **obj_keys(swf_ctx *cx, swf_obj *o, bool include_hidden, uint32_t *n)
{
    keybuf b = {0};
    keys_rec(cx, o, include_hidden, &b, 0);
    if (o->cls == OC_CLIP && o->u.clip) {
        /* Child clips with instance names enumerate too (most recent depth first). */
        swf_dobj *kids[256];
        int nk = 0;
        for (swf_dobj *c = o->u.clip->children; c && nk < 256; c = c->next)
            if (c->name[0] && (c->kind == SWF_CHAR_SPRITE || c->kind == SWF_CHAR_EDIT_TEXT || c->kind == SWF_CHAR_BUTTON))
                kids[nk++] = c;
        for (int i = nk - 1; i >= 0; i--) {
            swf_str *k = atom(cx->vm, str_from_utf8(cx->vm, kids[i]->name, strlen(kids[i]->name)));
            if (find(o, k, cx->vm->case_sensitive) < 0) key_push(cx->vm, &b, k);
        }
    }
    *n = b.n;
    return b.k;
}

void obj_add_property(swf_vm *vm, swf_obj *o, swf_str *key, swf_obj *getter, swf_obj *setter, uint16_t attr)
{
    if (o->cls == OC_SUPER) return;
    key = atom(vm, key);
    int32_t i = find(o, key, false);
    swf_prop *p;
    if (i >= 0) {
        p = &o->props[i];
    } else {
        p = insert(vm, o, key);
        p->attr = attr;
    }
    p->getter = getter;
    p->setter = setter;
    p->val = swf_undef();
}

void obj_set_attributes(swf_vm *vm, swf_obj *o, swf_str *key, uint16_t set, uint16_t clear)
{
    if (o->cls == OC_SUPER) return;
    if (!key) {
        for (uint32_t i = 0; i < o->nprops; i++)
            if (o->props[i].key) o->props[i].attr = (uint16_t)((o->props[i].attr & ~clear) | set);
        return;
    }
    int32_t i = find(o, atom(vm, key), false);
    if (i >= 0) o->props[i].attr = (uint16_t)((o->props[i].attr & ~clear) | set);
}

bool obj_is_instance_of(swf_ctx *cx, swf_obj *o, swf_obj *proto)
{
    swf_obj *stack[64];
    swf_obj *this_ = o;
    for (int guard = 0; guard < 256; guard++) {
        swf_val pv = obj_proto(cx, this_);
        if (pv.t != SWF_T_OBJ) break;
        int n = 0;
        stack[n++] = pv.u.o;
        while (n) {
            swf_obj *i = stack[--n];
            if (i == proto) return true;
            for (uint32_t k = 0; k < i->nifaces && n < 64; k++) stack[n++] = i->ifaces[k];
        }
        this_ = pv.u.o;
    }
    return false;
}
