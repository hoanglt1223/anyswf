/* Function objects, the calling convention (Ruffle avm1/function.rs), the frame arena,
 * exceptions and the dynamic-stack call helpers used by generated code. */
#include "vm_internal.h"

#include <stdio.h>

#define ARENA_VALUES (64 * 1024)

/* --- frame arena ------------------------------------------------------------ */

swf_val *swf_frame_alloc(swf_ctx *cx, size_t n)
{
    swf_vm *vm = cx->vm;
    if (n > vm->arena_cap - vm->arena_top) vm_abort(vm, "script stack exhausted (too much recursion)");
    swf_val *p = vm->arena + vm->arena_top;
    vm->arena_top += n;
    for (size_t i = 0; i < n; i++) p[i] = swf_undef();
    return p;
}

SWF_NORETURN void swf_stack_overflow(swf_ctx *cx)
{
    vm_abort(cx->vm, "AVM1 operand stack overflow");
}

SWF_NORETURN void swf_bad_code(swf_ctx *cx, const char *what)
{
    vm_abort(cx->vm, what);
}

/* --- registers and constant pools --------------------------------------------- */

swf_val swf_reg_get(swf_ctx *cx, unsigned r)
{
    if (r < cx->nregs) return cx->regs[r];
    return r < SWF_GLOBAL_REGS ? cx->vm->regs[r] : swf_undef();
}

void swf_reg_set(swf_ctx *cx, unsigned r, swf_val v)
{
    if (r < cx->nregs) cx->regs[r] = v;
    else if (r < SWF_GLOBAL_REGS) cx->vm->regs[r] = v;
}

swf_val swf_pool_get(swf_ctx *cx, unsigned i)
{
    if (!cx->pool || i >= cx->pool->n) return swf_undef();
    return swf_strv(cx->vm->code->atoms[cx->pool->atoms[i]]);
}

swf_val swf_atomv(swf_ctx *cx, uint32_t a)
{
    return swf_strv(cx->vm->code->atoms[a]);
}

/* --- function objects --------------------------------------------------------- */

bool is_function(swf_val v)
{
    return v.t == SWF_T_OBJ && v.u.o->cls == OC_FUNCTION;
}

swf_obj *func_new_native(swf_vm *vm, swf_native_fn fn, swf_native_fn ctor, swf_obj *prototype)
{
    swf_obj *f = obj_new_class(vm, vm->function_proto, OC_FUNCTION);
    f->u.fn.native = fn;
    f->u.fn.ctor = ctor;
    if (prototype) {
        obj_define(vm, prototype, vm->s.constructor, swf_objv(f), ATTR_DONT_ENUM);
        obj_define(vm, f, vm->s.prototype, swf_objv(prototype), ATTR_DONT_ENUM | ATTR_DONT_DELETE);
    }
    return f;
}

void native_method(swf_vm *vm, swf_obj *o, const char *name, swf_native_fn fn, uint16_t attr)
{
    obj_define(vm, o, atom_ascii(vm, name), swf_objv(func_new_native(vm, fn, NULL, NULL)), attr);
}

swf_val swf_define_function(swf_ctx *cx, const swf_fn_desc *d)
{
    swf_vm *vm = cx->vm;
    swf_obj *proto = obj_new(vm, vm->object_proto);
    swf_obj *f = obj_new_class(vm, vm->function_proto, OC_FUNCTION);
    f->u.fn.desc = d;
    f->u.fn.scope = cx->scope;
    f->u.fn.pool = cx->pool;
    f->u.fn.base_o = cx->base_o;
    f->u.fn.version = cx->version;
    obj_define(vm, proto, vm->s.constructor, swf_objv(f), ATTR_DONT_ENUM);
    obj_define(vm, f, vm->s.prototype, swf_objv(proto), ATTR_DONT_ENUM | ATTR_DONT_DELETE);
    if (d->name_atom >= 0) {
        swf_val name = swf_strv(vm->code->atoms[d->name_atom]);
        swf_define_local(cx, name, swf_objv(f));
        return swf_undef();
    }
    return swf_objv(f);
}

/* --- calls ---------------------------------------------------------------------- */

enum {
    FF_PRELOAD_THIS = 1 << 0,
    FF_SUPPRESS_THIS = 1 << 1,
    FF_PRELOAD_ARGUMENTS = 1 << 2,
    FF_SUPPRESS_ARGUMENTS = 1 << 3,
    FF_PRELOAD_SUPER = 1 << 4,
    FF_SUPPRESS_SUPER = 1 << 5,
    FF_PRELOAD_ROOT = 1 << 6,
    FF_PRELOAD_PARENT = 1 << 7,
    FF_PRELOAD_GLOBAL = 1 << 8
};

static void set_local_reg(swf_ctx *c, unsigned r, swf_val v)
{
    if (r < c->nregs) c->regs[r] = v;
}

static swf_val super_call(swf_ctx *cx, swf_obj *zuper, uint32_t argc, const swf_val *argv)
{
    swf_obj *p = zuper->u.super_.this_;
    for (uint8_t i = 0; i < zuper->u.super_.depth && p; i++) {
        swf_val n = obj_proto(cx, p);
        p = n.t == SWF_T_OBJ ? n.u.o : NULL;
    }
    if (!p) return swf_undef();
    swf_val ctor;
    if (!obj_get_opt(cx, p, cx->vm->s.__constructor__, false, &ctor, NULL) || !is_function(ctor)) return swf_undef();
    return func_call(cx, ctor.u.o, swf_objv(zuper->u.super_.this_), argc, argv,
                     (uint8_t)(zuper->u.super_.depth + 1), true);
}

static swf_val call_bytecode(swf_ctx *cx, swf_obj *fn, swf_val this_, uint32_t argc, const swf_val *argv, uint8_t depth)
{
    swf_vm *vm = cx->vm;
    swf_func *f = &fn->u.fn;
    const swf_fn_desc *d = f->desc;
    if (vm->call_depth >= SWF_MAX_CALL_DEPTH) vm_abort(vm, "256 levels of recursion were exceeded");

    swf_ctx c;
    memset(&c, 0, sizeof c);
    c.vm = vm;
    swf_obj *this_obj = this_.t == SWF_T_OBJ ? this_.u.o : NULL;
    swf_dobj *this_do = this_obj && this_obj->cls == OC_CLIP && this_obj->u.clip ? this_obj->u.clip : NULL;
    swf_dobj *target = ctx_target(cx) ? ctx_target(cx) : clip_root(vm);
    if (!this_do) this_do = target;
    swf_scope *parent;
    if (f->version >= 6) {
        c.version = f->version;
        swf_dobj *fb = f->base_o && f->base_o->cls == OC_CLIP ? f->base_o->u.clip : NULL;
        c.base_o = clip_object(vm, fb ? fb : this_do);
        parent = f->scope;
    } else {
        /* SWF5 functions are not closures: they run in the scope of `this`. */
        c.version = vm->version > 5 ? vm->version : 5;
        c.base_o = clip_object(vm, this_do);
        parent = scope_new(vm, vm->global_scope, SC_TARGET, clip_object(vm, this_do));
    }
    c.scope = scope_new(vm, parent, SC_LOCAL, obj_new_bare(vm));
    c.this_ = (d->flags & (FF_PRELOAD_THIS | FF_SUPPRESS_THIS)) ? cx->this_ : this_;
    c.callee = fn;
    c.target_o = c.base_o;
    c.target_valid = true;
    c.pool = f->pool;
    c.saved_handler = vm->handler;
    c.arena_mark = vm->arena_top;
    if (d->df2 && d->nregs) {
        c.nregs = d->nregs;
        c.regs = swf_frame_alloc(&c, d->nregs);
    }

    swf_obj *caller = vm->caller;
    unsigned r = 1;
    if (d->flags & FF_PRELOAD_THIS) set_local_reg(&c, r++, (d->flags & FF_SUPPRESS_THIS) ? swf_undef() : this_);
    if (!((d->flags & FF_SUPPRESS_ARGUMENTS) && !(d->flags & FF_PRELOAD_ARGUMENTS))) {
        swf_obj *args = array_new(&c, argc, NULL);
        for (uint32_t i = 0; i < argc; i++) obj_set(&c, args, atom_int(vm, i), argv[i]);
        obj_define(vm, args, vm->s.callee, swf_objv(fn), ATTR_DONT_ENUM);
        obj_define(vm, args, vm->s.caller, caller ? swf_objv(caller) : swf_null(), ATTR_DONT_ENUM);
        if (d->flags & FF_PRELOAD_ARGUMENTS) set_local_reg(&c, r++, swf_objv(args));
        else obj_define(vm, c.scope->obj, vm->s.arguments, swf_objv(args), 0);
    }
    {
        swf_val zv = swf_undef();
        if (this_obj && !(d->flags & FF_SUPPRESS_SUPER)) {
            swf_obj *z = obj_new_class(vm, NULL, OC_SUPER);
            z->u.super_.this_ = this_obj;
            z->u.super_.depth = depth;
            zv = swf_objv(z);
        }
        if (d->flags & FF_PRELOAD_SUPER) set_local_reg(&c, r++, zv);
        else if (zv.t == SWF_T_OBJ) obj_define(vm, c.scope->obj, vm->s.super_, zv, 0);
    }
    if (d->flags & FF_PRELOAD_ROOT) set_local_reg(&c, r++, swf_objv(clip_object(vm, clip_root(vm))));
    if (d->flags & FF_PRELOAD_PARENT) {
        swf_dobj *p = clip_parent(vm, ctx_base(&c));
        if (p) set_local_reg(&c, r++, swf_objv(clip_object(vm, p)));
    }
    if (d->flags & FF_PRELOAD_GLOBAL) set_local_reg(&c, r++, swf_objv(vm->global));
    for (uint32_t i = 0; i < d->nparams; i++) {
        swf_val v = i < argc ? argv[i] : swf_undef();
        if (d->params[i].reg) set_local_reg(&c, d->params[i].reg, v);
        else obj_define(vm, c.scope->obj, vm->code->atoms[d->params[i].atom], v, 0);
    }

    vm->call_depth++;
    vm->caller = fn;
    swf_val ret = d->code(&c);
    vm->caller = caller;
    vm->call_depth--;
    vm->handler = c.saved_handler;
    vm->arena_top = c.arena_mark;
    return ret;
}

swf_val func_call(swf_ctx *cx, swf_obj *fn, swf_val this_, uint32_t argc, const swf_val *argv, uint8_t depth,
                  bool ctor_call)
{
    if (fn->cls == OC_SUPER) return super_call(cx, fn, argc, argv);
    if (fn->cls != OC_FUNCTION) return swf_undef();
    swf_func *f = &fn->u.fn;
    if (f->desc) return call_bytecode(cx, fn, this_, argc, argv, depth);
    swf_native_fn n = ctor_call && f->ctor ? f->ctor : f->native;
    if (!n) return swf_undef();
    return n(cx, this_, argc, argv);
}

static void define_constructor_props(swf_ctx *cx, swf_obj *this_, swf_obj *fn)
{
    obj_define(cx->vm, this_, cx->vm->s.__constructor__, swf_objv(fn), ATTR_DONT_ENUM);
    if (cx->version < 7) obj_define(cx->vm, this_, cx->vm->s.constructor, swf_objv(fn), ATTR_DONT_ENUM);
}

swf_val func_construct(swf_ctx *cx, swf_obj *fn, uint32_t argc, const swf_val *argv)
{
    if (fn->cls != OC_FUNCTION) return swf_undef();
    swf_val proto = obj_get_data(cx, fn, cx->vm->s.prototype);
    swf_obj *this_ = obj_new(cx->vm, proto.t == SWF_T_OBJ ? proto.u.o : NULL);
    if (proto.t != SWF_T_OBJ && proto.t != SWF_T_UNDEF)
        obj_define(cx->vm, this_, cx->vm->s.proto, proto, ATTR_DONT_ENUM | ATTR_DONT_DELETE);
    define_constructor_props(cx, this_, fn);
    bool propagate = !fn->u.fn.desc && fn->u.fn.ctor;
    swf_val r = func_call(cx, fn, swf_objv(this_), argc, argv, 1, true);
    return propagate ? r : swf_objv(this_);
}

void func_construct_on(swf_ctx *cx, swf_obj *fn, swf_obj *this_, uint32_t argc, const swf_val *argv)
{
    if (fn->cls != OC_FUNCTION) return;
    define_constructor_props(cx, this_, fn);
    (void)func_call(cx, fn, swf_objv(this_), argc, argv, 1, true);
}

/* --- exceptions ------------------------------------------------------------------ */

void swf_try_enter(swf_ctx *cx, swf_try *t, int sp)
{
    swf_vm *vm = cx->vm;
    t->prev = vm->handler;
    t->scope = cx->scope;
    t->arena_mark = vm->arena_top;
    t->sp = sp;
    t->call_depth = vm->call_depth;
    t->caller = vm->caller;
    t->exc = swf_undef();
    vm->handler = t;
}

void swf_try_leave(swf_ctx *cx, swf_try *t)
{
    cx->vm->handler = t->prev;
}

SWF_NORETURN void swf_throw(swf_ctx *cx, swf_val v)
{
    swf_vm *vm = cx->vm;
    swf_try *t = vm->handler;
    if (!t) {
        /* Uncaught: the script ends and Flash reports the value. */
        vm->uncaught = v;
        vm->uncaught_set = true;
        vm_abort(vm, NULL);
    }
    vm->handler = t->prev;
    t->exc = v;
    vm->arena_top = t->arena_mark;
    vm->call_depth = t->call_depth;
    vm->caller = t->caller;
    longjmp(t->jb, 1);
}

/* --- argument helpers ------------------------------------------------------------- */

/* Copies `n` stack values (argument i at base[n-1-i]) into argument order. */
static swf_val *args_from_stack(swf_ctx *cx, const swf_val *base, uint32_t n)
{
    swf_val *a = swf_frame_alloc(cx, n ? n : 1);
    for (uint32_t i = 0; i < n; i++) a[i] = base[n - 1 - i];
    return a;
}

static swf_val pop(swf_val *S, int *sp)
{
    return *sp > 0 ? S[--*sp] : swf_undef();
}

static void push(swf_ctx *cx, swf_val *S, int *sp, swf_val v)
{
    (void)cx;
    S[(*sp)++] = v;
}

/* Count operands are clamped to what the stack holds (Ruffle `pop_call_args`). */
static uint32_t take_count(swf_ctx *cx, swf_val *S, int *sp)
{
    uint32_t n = to_uint32(cx, pop(S, sp));
    if (n > (uint32_t)*sp) n = (uint32_t)*sp;
    return n;
}

swf_val swf_call_function(swf_ctx *cx, swf_val name, uint32_t n, const swf_val *args)
{
    swf_str *fname = to_string(cx, name);
    swf_obj *found_on = NULL;
    swf_val f = get_variable_str(cx, fname, &found_on);
    if (f.t != SWF_T_OBJ) return swf_undef();
    swf_val this_ = found_on ? swf_objv(found_on) : swf_objv(clip_object(cx->vm, ctx_target(cx) ? ctx_target(cx) : clip_root(cx->vm)));
    size_t mark = cx->vm->arena_top;
    swf_val *a = args_from_stack(cx, args, n);
    swf_val r = func_call(cx, f.u.o, this_, n, a, 0, false);
    cx->vm->arena_top = mark;
    return r;
}

swf_val swf_call_method(swf_ctx *cx, swf_val objv, swf_val name, uint32_t n, const swf_val *args)
{
    if (is_nullish(objv)) return swf_undef();
    swf_obj *o = to_object_or_bare(cx, objv);
    swf_str *mname = name.t == SWF_T_UNDEF ? cx->vm->s.empty : to_string(cx, name);
    size_t mark = cx->vm->arena_top;
    swf_val *a = args_from_stack(cx, args, n);
    swf_val r;
    if (mname->len == 0) r = func_call(cx, o, swf_undef(), n, a, 0, false);
    else r = obj_call_method(cx, o, mname, n, a, false);
    cx->vm->arena_top = mark;
    return r;
}

swf_val swf_new_object(swf_ctx *cx, swf_val name, uint32_t n, const swf_val *args)
{
    swf_str *cname = to_string(cx, name);
    swf_val c = scope_resolve(cx, atom(cx->vm, cname), NULL);
    swf_obj *ctor = to_object_or_bare(cx, c);
    size_t mark = cx->vm->arena_top;
    swf_val *a = args_from_stack(cx, args, n);
    swf_val r = func_construct(cx, ctor, n, a);
    cx->vm->arena_top = mark;
    return r;
}

swf_val swf_new_method(swf_ctx *cx, swf_val objv, swf_val name, uint32_t n, const swf_val *args)
{
    if (is_nullish(objv)) return swf_undef();
    swf_obj *o = to_object_or_bare(cx, objv);
    swf_str *mname = name.t == SWF_T_UNDEF ? cx->vm->s.empty : to_string(cx, name);
    size_t mark = cx->vm->arena_top;
    swf_val *a = args_from_stack(cx, args, n);
    swf_val r = swf_undef();
    if (mname->len == 0) {
        r = func_construct(cx, o, n, a);
    } else {
        swf_val c = obj_get(cx, o, mname);
        if (c.t == SWF_T_OBJ) r = func_construct(cx, c.u.o, n, a);
    }
    cx->vm->arena_top = mark;
    return r;
}

swf_val swf_init_array(swf_ctx *cx, uint32_t n, const swf_val *elems)
{
    swf_val *a = args_from_stack(cx, elems, n);
    swf_obj *arr = array_new(cx, n, a);
    return swf_objv(arr);
}

swf_val swf_init_object(swf_ctx *cx, uint32_t n, const swf_val *pairs)
{
    swf_obj *o = obj_new(cx->vm, cx->vm->object_proto);
    for (uint32_t i = 0; i < n; i++) {
        swf_val value = pairs[2 * n - 1 - 2 * i];
        swf_val name = pairs[2 * n - 2 - 2 * i];
        obj_set(cx, o, to_key(cx, name), value);
    }
    return swf_objv(o);
}

void swf_implements(swf_ctx *cx, swf_val ctor, uint32_t n, const swf_val *ifaces)
{
    swf_vm *vm = cx->vm;
    if (n == 0) return;
    swf_obj **list = malloc(n * sizeof *list);
    if (!list) vm_abort(vm, "out of memory in implements");
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++) {
        swf_val iv = ifaces[n - 1 - i];
        if (iv.t != SWF_T_OBJ) continue;
        swf_val p = obj_get_data(cx, iv.u.o, vm->s.prototype);
        if (p.t == SWF_T_OBJ) list[k++] = p.u.o;
    }
    if (ctor.t == SWF_T_OBJ && cx->version >= 7) {
        swf_val p = obj_get_data(cx, ctor.u.o, vm->s.prototype);
        if (p.t == SWF_T_OBJ) {
            free(p.u.o->ifaces);
            p.u.o->ifaces = list;
            p.u.o->nifaces = k;
            return;
        }
    }
    free(list);
}

bool swf_count_ok(swf_val count, int available, uint32_t *n)
{
    if (count.t != SWF_T_NUM || count.u.n < 0 || count.u.n > available) return false;
    *n = (uint32_t)count.u.n;
    return true;
}

void swf_dyn_call_function(swf_ctx *cx, swf_val *S, int *sp)
{
    swf_val name = pop(S, sp);
    uint32_t n = take_count(cx, S, sp);
    *sp -= (int)n;
    swf_val r = swf_call_function(cx, name, n, S + *sp);
    push(cx, S, sp, r);
}

void swf_dyn_call_method(swf_ctx *cx, swf_val *S, int *sp)
{
    swf_val name = pop(S, sp);
    swf_val obj = pop(S, sp);
    uint32_t n = take_count(cx, S, sp);
    *sp -= (int)n;
    swf_val r = swf_call_method(cx, obj, name, n, S + *sp);
    push(cx, S, sp, r);
}

void swf_dyn_new_object(swf_ctx *cx, swf_val *S, int *sp)
{
    swf_val name = pop(S, sp);
    uint32_t n = take_count(cx, S, sp);
    *sp -= (int)n;
    swf_val r = swf_new_object(cx, name, n, S + *sp);
    push(cx, S, sp, r);
}

void swf_dyn_new_method(swf_ctx *cx, swf_val *S, int *sp)
{
    swf_val name = pop(S, sp);
    swf_val obj = pop(S, sp);
    uint32_t n = take_count(cx, S, sp);
    *sp -= (int)n;
    swf_val r = swf_new_method(cx, obj, name, n, S + *sp);
    push(cx, S, sp, r);
}

void swf_dyn_init_array(swf_ctx *cx, swf_val *S, int *sp)
{
    double c = to_number(cx, pop(S, sp));
    if (!(c >= 0) || c > 2147483647.0) {
        push(cx, S, sp, swf_undef());
        return;
    }
    uint32_t n = (uint32_t)c;
    /* Popping past the bottom yields undefined elements. */
    swf_val *tmp = swf_frame_alloc(cx, n ? n : 1);
    for (uint32_t i = 0; i < n; i++) tmp[n - 1 - i] = pop(S, sp);
    push(cx, S, sp, swf_init_array(cx, n, tmp));
}

void swf_dyn_init_object(swf_ctx *cx, swf_val *S, int *sp)
{
    double c = to_number(cx, pop(S, sp));
    if (!(c >= 0) || c > 2147483647.0) {
        push(cx, S, sp, swf_undef());
        return;
    }
    uint32_t n = (uint32_t)c;
    swf_obj *o = obj_new(cx->vm, cx->vm->object_proto);
    for (uint32_t i = 0; i < n; i++) {
        swf_val value = pop(S, sp);
        swf_val name = pop(S, sp);
        obj_set(cx, o, to_key(cx, name), value);
    }
    push(cx, S, sp, swf_objv(o));
}

void swf_dyn_implements(swf_ctx *cx, swf_val *S, int *sp)
{
    swf_val ctor = pop(S, sp);
    swf_val count = pop(S, sp);
    uint32_t n = count.t == SWF_T_OBJ ? 0 : (uint32_t)to_int32(cx, count);
    if (n > (uint32_t)*sp) n = (uint32_t)*sp;
    *sp -= (int)n;
    swf_implements(cx, ctor.t == SWF_T_OBJ ? ctor : swf_undef(), n, S + *sp);
}

void swf_dyn_enumerate(swf_ctx *cx, swf_val *S, int *sp, int cap, bool by_name)
{
    swf_val v = pop(S, sp);
    if (by_name) v = get_variable_str(cx, to_string(cx, v), NULL);
    if (*sp >= cap) swf_stack_overflow(cx);
    S[(*sp)++] = swf_undef();
    swf_obj *o = v.t == SWF_T_OBJ ? v.u.o : NULL;
    if (!o) return;
    uint32_t n;
    swf_str **keys = obj_keys(cx, o, false, &n);
    for (int64_t i = (int64_t)n - 1; i >= 0; i--) {
        if (*sp >= cap) {
            free(keys);
            swf_stack_overflow(cx);
        }
        S[(*sp)++] = swf_strv(keys[i]);
    }
    free(keys);
}

void swf_extends(swf_ctx *cx, swf_val sub, swf_val super_)
{
    swf_vm *vm = cx->vm;
    swf_obj *sup = to_object_or_bare(cx, super_);
    swf_obj *sb = to_object_or_bare(cx, sub);
    swf_val sp = obj_get(cx, sup, vm->s.prototype);
    swf_obj *proto = obj_new(vm, sp.t == SWF_T_OBJ ? sp.u.o : NULL);
    if (sp.t != SWF_T_OBJ && sp.t != SWF_T_UNDEF) obj_define(vm, proto, vm->s.proto, sp, ATTR_DONT_ENUM | ATTR_DONT_DELETE);
    obj_define(vm, proto, vm->s.constructor, swf_objv(sup), ATTR_DONT_ENUM);
    obj_define(vm, proto, vm->s.__constructor__, swf_objv(sup), ATTR_DONT_ENUM);
    obj_set(cx, sb, vm->s.prototype, swf_objv(proto));
}
