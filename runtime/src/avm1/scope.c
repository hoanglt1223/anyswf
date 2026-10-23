/* Scope chains, variable access by name or path (`_root.a.b`, `/a/b:c`, `..`), and the
 * variable actions. Ported from Ruffle's avm1/scope.rs and Activation::get_variable,
 * set_variable and resolve_target_path. */
#include "vm_internal.h"

swf_scope *scope_new(swf_vm *vm, swf_scope *parent, uint8_t cls, swf_obj *obj)
{
    swf_scope *s = gc_alloc(vm, sizeof(swf_scope), SWF_GC_SCOPE);
    s->cls = cls;
    s->parent = parent;
    s->obj = obj;
    return s;
}

/* tellTarget: replaces the object of the nearest Target scope. */
swf_scope *scope_with_target(swf_vm *vm, swf_scope *s, swf_obj *clip)
{
    if (!s) return NULL;
    if (s->cls == SC_TARGET) return scope_new(vm, s->parent, SC_TARGET, clip);
    return scope_new(vm, scope_with_target(vm, s->parent, clip), s->cls, s->obj);
}

static bool name_is(swf_ctx *cx, const swf_str *s, const char *lit, bool cs)
{
    size_t n = strlen(lit);
    if (s->len != n) return false;
    for (size_t i = 0; i < n; i++) {
        uint16_t c = s->ch[i], l = (unsigned char)lit[i];
        if (c == l) continue;
        if (!cs && c < 128 && (c | 0x20) == (l | 0x20) && ((c | 0x20) >= 'a' && (c | 0x20) <= 'z')) continue;
        return false;
    }
    (void)cx;
    return true;
}

/* `_root`, `_parent`, `_global`, `_levelN` (Ruffle stage_object::resolve_path_property). */
static bool path_property(swf_ctx *cx, swf_dobj *d, swf_str *name, swf_val *out)
{
    swf_vm *vm = cx->vm;
    if (cx->version < 5 || name->len < 5 || name->ch[0] != '_') return false;
    bool cs = vm->case_sensitive;
    if (name_is(cx, name, "_root", cs)) {
        *out = swf_objv(clip_object(vm, clip_root(vm)));
        return true;
    }
    if (name_is(cx, name, "_parent", cs)) {
        swf_dobj *p = d ? clip_parent(vm, d) : NULL;
        *out = p ? swf_objv(clip_object(vm, p)) : swf_undef();
        return true;
    }
    if (cx->version >= 6 && name_is(cx, name, "_global", cs)) {
        *out = swf_objv(vm->global);
        return true;
    }
    if (name->len >= 7) {
        swf_str prefix_check;
        (void)prefix_check;
        bool level = true;
        const char *pre = "_level", *pre2 = "_flash";
        for (int i = 0; i < 6; i++) {
            uint16_t c = name->ch[i];
            uint16_t lc = c < 128 ? (uint16_t)(c | 0x20) : c;
            if (!((cs ? c : lc) == (unsigned char)pre[i] || (cs ? c : lc) == (unsigned char)pre2[i])) level = false;
        }
        if (level) {
            int64_t id = 0;
            for (uint32_t i = 6; i < name->len; i++) {
                if (name->ch[i] < '0' || name->ch[i] > '9') return false;
                id = id * 10 + (name->ch[i] - '0');
                if (id > 100000) return false;
            }
            *out = id == 0 ? swf_objv(clip_object(vm, clip_root(vm))) : swf_undef();
            return true;
        }
    }
    return false;
}

swf_val scope_resolve(swf_ctx *cx, swf_str *name, swf_obj **found_on)
{
    swf_vm *vm = cx->vm;
    name = atom(vm, name);
    bool this_cs = cx->version <= 5 ? cx->scope->cls == SC_LOCAL : vm->case_sensitive;
    if (cx->version >= 5 && name_is(cx, name, "this", this_cs)) {
        if (found_on) *found_on = NULL;
        return cx->this_;
    }
    for (swf_scope *s = cx->scope; s; s = s->parent) {
        swf_obj *values = s->obj;
        if (values->cls == OC_CLIP && !values->u.clip) {
            /* A removed clip's scope is replaced by the current target. */
            values = clip_object(vm, ctx_target(cx) ? ctx_target(cx) : clip_root(vm));
        }
        if (obj_has_property(cx, values, name)) {
            if (found_on) *found_on = values;
            return obj_get(cx, values, name);
        }
    }
    if (found_on) *found_on = NULL;
    swf_val v;
    if (path_property(cx, ctx_target(cx) ? ctx_target(cx) : clip_root(vm), name, &v)) return v;
    return swf_undef();
}

void scope_set(swf_ctx *cx, swf_scope *s, swf_str *name, swf_val v)
{
    for (; s; s = s->parent) {
        bool removed = s->obj->cls == OC_CLIP && !s->obj->u.clip;
        if (!removed && (s->cls == SC_TARGET || obj_has_property(cx, s->obj, name))) {
            obj_set(cx, s->obj, name, v);
            return;
        }
        if (!s->parent) {
            obj_set(cx, s->obj, name, v);
            return;
        }
    }
}

void scope_define_local(swf_ctx *cx, swf_scope *s, swf_str *name, swf_val v)
{
    while (s->cls == SC_WITH && s->parent) {
        if (obj_has_own(cx, s->obj, name)) {
            obj_set(cx, s->obj, name, v);
            return;
        }
        s = s->parent;
    }
    obj_set(cx, s->obj, name, v);
}

bool in_local_scope(swf_ctx *cx)
{
    for (swf_scope *s = cx->scope; s; s = s->parent) {
        if (s->cls == SC_LOCAL) return true;
        if (s->cls == SC_TARGET) return false;
    }
    return false;
}

static bool is_delim(uint16_t c, bool swf5, bool slash_path)
{
    return c == '/' || (swf5 && c == ':') || (swf5 && c == '.' && !slash_path);
}

/* Resolves path[from..to) to an object, starting at `start` (Ruffle resolve_target_path). */
swf_obj *resolve_target_path(swf_ctx *cx, swf_dobj *root, swf_obj *start, const swf_str *path, uint32_t from,
                             uint32_t to, bool first, bool handle_this)
{
    swf_vm *vm = cx->vm;
    if (from >= to) return start;
    bool swf5 = cx->version >= 5;
    bool slash = false;
    swf_obj *obj = start;
    uint32_t i = from;
    if (path->ch[i] == '/') {
        i++;
        first = false;
        obj = clip_object(vm, root);
        slash = true;
    }
    while (i < to) {
        if (swf5)
            while (i < to && path->ch[i] == ':') i++;
        if (i >= to) break;
        swf_val val;
        bool dotdot = i + 1 < to && path->ch[i] == '.' && path->ch[i + 1] == '.' &&
                      (i + 2 >= to || path->ch[i + 2] == '/' || (swf5 && path->ch[i + 2] == ':'));
        if (dotdot) {
            if (i + 2 < to && path->ch[i + 2] == '/') slash = true;
            i += 3;
            swf_dobj *d = obj->cls == OC_CLIP ? obj->u.clip : NULL;
            swf_dobj *p = d ? clip_parent(vm, d) : NULL;
            if (!p) return NULL;
            val = swf_objv(clip_object(vm, p));
        } else {
            uint32_t s = i;
            while (i < to && !is_delim(path->ch[i], swf5, slash)) i++;
            uint32_t e = i;
            if (i < to && path->ch[i] == '/') slash = true;
            if (i < to) i++;
            swf_str *name = str_from_units(vm, path->ch + s, e - s);
            if (swf5 && handle_this && str_eq_ascii(name, "this")) {
                val = cx->this_;
            } else if (swf5 && first && str_eq_ascii(name, "_root")) {
                val = swf_objv(clip_object(vm, clip_root(vm)));
            } else {
                swf_val lv = swf_undef();
                bool is_level = false;
                if (first && name->len >= 6 && path_property(cx, root, name, &lv)) {
                    /* Only _levelN counts here; _root/_parent/_global are handled below. */
                    is_level = name->ch[1] == 'l' || name->ch[1] == 'L' || name->ch[1] == 'f' || name->ch[1] == 'F';
                }
                if (is_level) {
                    val = lv;
                } else {
                    swf_dobj *d = obj->cls == OC_CLIP ? obj->u.clip : NULL;
                    swf_dobj *child = d ? clip_child_by_name(vm, d, name, 0, name->len) : NULL;
                    if (child) val = swf_objv(clip_object(vm, child));
                    else val = obj_get(cx, obj, name);
                }
            }
        }
        first = false;
        if (val.t != SWF_T_OBJ) return NULL;
        obj = val.u.o;
    }
    return obj;
}

/* Index of the right-most variable separator (`:` always, `.` from SWF5), or -1. */
static int64_t var_separator(const swf_str *p, bool swf5)
{
    for (int64_t i = (int64_t)p->len - 1; i >= 0; i--)
        if (p->ch[i] == ':' || (swf5 && p->ch[i] == '.')) return i;
    return -1;
}

static bool contains_slash(const swf_str *p)
{
    for (uint32_t i = 0; i < p->len; i++)
        if (p->ch[i] == '/') return true;
    return false;
}

swf_val get_variable_str(swf_ctx *cx, swf_str *path, swf_obj **this_out)
{
    swf_vm *vm = cx->vm;
    if (this_out) *this_out = NULL;
    bool swf5 = cx->version >= 5;
    int64_t sep = var_separator(path, swf5);
    if (sep >= 0) {
        if (sep == 0) return swf_undef();
        swf_str *var = atom(vm, str_sub(vm, path, (uint32_t)sep + 1, path->len));
        for (swf_scope *s = cx->scope; s; s = s->parent) {
            swf_obj *o = resolve_target_path(cx, clip_root(vm), s->obj, path, 0, (uint32_t)sep, true, true);
            if (o && obj_has_property(cx, o, var)) {
                if (this_out) *this_out = o;
                return obj_get(cx, o, var);
            }
        }
        return swf_undef();
    }
    if (swf5 && contains_slash(path)) {
        for (swf_scope *s = cx->scope; s; s = s->parent) {
            swf_obj *o = resolve_target_path(cx, clip_root(vm), s->obj, path, 0, path->len, false, false);
            if (o) return swf_objv(o);
        }
    }
    return scope_resolve(cx, path, this_out);
}

void set_variable_str(swf_ctx *cx, swf_str *path, swf_val v)
{
    swf_vm *vm = cx->vm;
    if (path->len == 0) return;
    if (cx->version >= 5 && str_eq_ascii(path, "this")) {
        cx->this_ = v;
        return;
    }
    int64_t sep = var_separator(path, true);
    if (sep >= 0) {
        swf_str *var = atom(vm, str_sub(vm, path, (uint32_t)sep + 1, path->len));
        for (swf_scope *s = cx->scope; s; s = s->parent) {
            swf_obj *o = resolve_target_path(cx, clip_root(vm), s->obj, path, 0, (uint32_t)sep, true, true);
            if (o) {
                obj_set(cx, o, var, v);
                return;
            }
        }
        return;
    }
    scope_set(cx, cx->scope, atom(vm, path), v);
}

swf_val swf_get_variable(swf_ctx *cx, swf_val name)
{
    swf_val v = get_variable_str(cx, to_string(cx, name), NULL);
    if (cx->version <= 4 && v.t == SWF_T_OBJ && v.u.o->cls == OC_CLIP) return swf_undef();
    return v;
}

void swf_set_variable(swf_ctx *cx, swf_val name, swf_val v)
{
    set_variable_str(cx, to_string(cx, name), v);
}

static bool has_path_separator(const swf_str *s)
{
    for (uint32_t i = 0; i < s->len; i++)
        if (s->ch[i] == ':' || s->ch[i] == '.') return true;
    return false;
}

void swf_define_local(swf_ctx *cx, swf_val name, swf_val v)
{
    swf_str *n = to_string(cx, name);
    if (!in_local_scope(cx) && has_path_separator(n)) set_variable_str(cx, n, v);
    else scope_define_local(cx, cx->scope, atom(cx->vm, n), v);
}

void swf_define_local2(swf_ctx *cx, swf_val name)
{
    swf_str *n = to_string(cx, name);
    if (!in_local_scope(cx) && has_path_separator(n)) {
        swf_val cur = get_variable_str(cx, n, NULL);
        if (cur.t == SWF_T_UNDEF) set_variable_str(cx, n, swf_undef());
    } else {
        swf_str *a = atom(cx->vm, n);
        swf_obj *locals = cx->scope->obj;
        if (!obj_has_property(cx, locals, a)) scope_define_local(cx, cx->scope, a, swf_undef());
    }
}

swf_val swf_delete(swf_ctx *cx, swf_val obj, swf_val name)
{
    swf_str *n = to_string(cx, name);
    if (obj.t != SWF_T_OBJ) return swf_bool(false);
    return swf_bool(obj_delete(cx, obj.u.o, n));
}

swf_val swf_delete2(swf_ctx *cx, swf_val name)
{
    swf_vm *vm = cx->vm;
    swf_str *n = to_string(cx, name);
    int64_t sep = var_separator(n, true);
    if (sep >= 0) {
        swf_str *var = str_sub(vm, n, (uint32_t)sep + 1, n->len);
        swf_val o = get_variable_str(cx, str_sub(vm, n, 0, (uint32_t)sep), NULL);
        if (o.t != SWF_T_OBJ) return swf_bool(false);
        return swf_bool(obj_delete(cx, o.u.o, var));
    }
    for (swf_scope *s = cx->scope; s; s = s->parent) {
        if (obj_has_property(cx, s->obj, n)) return swf_bool(obj_delete(cx, s->obj, n));
    }
    return swf_bool(false);
}

swf_val swf_get_member(swf_ctx *cx, swf_val obj, swf_val name)
{
    swf_str *key = to_key(cx, name);
    swf_obj *o = to_object_or_bare(cx, obj);
    return obj_get(cx, o, key);
}

void swf_set_member(swf_ctx *cx, swf_val obj, swf_val name, swf_val v)
{
    swf_str *key = to_key(cx, name);
    swf_obj *o = to_object_or_bare(cx, obj);
    obj_set(cx, o, key, v);
}

bool swf_with_enter(swf_ctx *cx, swf_val v)
{
    if (is_nullish(v)) return false;
    swf_obj *o = to_object_or_bare(cx, v);
    cx->scope = scope_new(cx->vm, cx->scope, SC_WITH, o);
    return true;
}

void swf_catch_var(swf_ctx *cx, uint32_t a, swf_val v)
{
    set_variable_str(cx, cx->vm->code->atoms[a], v);
}

swf_dobj *resolve_target_clip(swf_ctx *cx, swf_dobj *start, swf_val target, bool allow_empty)
{
    if (target.t == SWF_T_OBJ && target.u.o->cls == OC_CLIP) return target.u.o->u.clip;
    swf_str *p = to_string(cx, target);
    if (!allow_empty && p->len == 0) return NULL;
    swf_obj *o = resolve_target_path(cx, clip_root(cx->vm), clip_object(cx->vm, start), p, 0, p->len, true, false);
    return o && o->cls == OC_CLIP ? o->u.clip : NULL;
}
