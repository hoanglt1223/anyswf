/* VM lifecycle, script dispatch from the timeline, and diagnostics. */
#include "vm_internal.h"

#include <stdio.h>

#define ARENA_VALUES (64 * 1024)

static swf_vm *g_vm;

static void default_trace(const char *utf8, void *user)
{
    (void)user;
    fputs(utf8, stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

void vm_warn(swf_vm *vm, uint32_t bit, const char *fmt, ...)
{
    if (vm->warned & bit) return;
    vm->warned |= bit;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    plat_log("%s", buf);
}

SWF_NORETURN void vm_abort(swf_vm *vm, const char *why)
{
    if (why) plat_log("avm1: script aborted: %s", why);
    if (vm->abort_jb) longjmp(*vm->abort_jb, 1);
    plat_log("avm1: fatal error outside script execution");
    abort();
}

void vm_trace_warning(swf_vm *vm, const char *msg)
{
    char buf[528]; /* "Warning: " plus a message of up to 511 characters */
    snprintf(buf, sizeof buf, "Warning: %s", msg);
    vm->trace(buf, vm->trace_user);
}

static void movie_warning(swf_movie *m, const char *msg, void *user)
{
    (void)m;
    vm_trace_warning(user, msg);
}

static void dobj_freed(swf_dobj *d)
{
    swf_obj *o = d->script_object;
    if (o && o->cls == OC_CLIP && o->u.clip == d) o->u.clip = NULL;
    d->script_object = NULL;
}

swf_ctx ctx_for_clip(swf_vm *vm, swf_dobj *clip)
{
    swf_ctx c;
    memset(&c, 0, sizeof c);
    c.vm = vm;
    swf_obj *o = clip_object(vm, clip);
    c.scope = scope_new(vm, vm->global_scope, SC_TARGET, o);
    c.this_ = swf_objv(o);
    c.base_o = o;
    c.target_o = o;
    c.target_valid = true;
    c.version = vm->version;
    c.saved_handler = vm->handler;
    c.arena_mark = vm->arena_top;
    return c;
}

void vm_run_protected(swf_vm *vm, swf_dobj *clip, vm_task_fn fn, void *user)
{
    swf_ctx c = ctx_for_clip(vm, clip ? clip : clip_root(vm));
    jmp_buf jb;
    jmp_buf *saved_abort = vm->abort_jb;
    swf_try *saved_handler = vm->handler;
    int saved_depth = vm->call_depth;
    size_t saved_top = vm->arena_top;
    swf_obj *saved_caller = vm->caller;
    bool saved_uncaught = vm->uncaught_set;
    vm->abort_jb = &jb;
    vm->handler = NULL;
    vm->uncaught_set = false;
    if (setjmp(jb) == 0) fn(&c, user);
    if (vm->uncaught_set) {
        vm->uncaught_set = false;
        swf_str *s = to_string(&c, vm->uncaught);
        char *u = str_to_utf8(s, NULL);
        if (u) {
            char msg[512];
            snprintf(msg, sizeof msg, "Uncaught exception, %s", u);
            vm_trace_warning(vm, msg);
            free(u);
        }
    }
    vm->uncaught_set = saved_uncaught;
    vm->abort_jb = saved_abort;
    vm->handler = saved_handler;
    vm->call_depth = saved_depth;
    vm->arena_top = saved_top;
    vm->caller = saved_caller;
}

static void script_task(swf_ctx *cx, void *user)
{
    uint32_t script = *(const uint32_t *)user;
    (void)cx->vm->code->scripts[script](cx);
}

static void run_script(swf_vm *vm, swf_dobj *clip, uint32_t script)
{
    if (script >= vm->code->script_count || !vm->code->scripts[script]) return;
    vm_run_protected(vm, clip, script_task, &script);
}

void swf_vm_on_action(swf_movie *m, swf_dobj *clip, uint32_t script, void *user)
{
    (void)m;
    vm_queue_script(user, clip, script, false);
}

static void method_task(swf_ctx *cx, void *user)
{
    const queued *q = user;
    swf_val fn;
    if (!obj_get_opt(cx, q->obj, q->name, true, &fn, NULL) || !is_function(fn)) return;
    (void)func_call(cx, fn.u.o, swf_objv(q->obj), q->nargs, q->args, 1, false);
}

static void broadcast_task(swf_ctx *cx, void *user)
{
    const queued *q = user;
    swf_val args[3] = {swf_strv(q->name), q->args[0], q->args[1]};
    (void)obj_call_method(cx, q->obj, atom_ascii(cx->vm, "broadcastMessage"), 1u + q->nargs, args, false);
}

static void call_task(swf_ctx *cx, void *user)
{
    const queued *q = user;
    (void)func_call(cx, q->obj, swf_objv(cx->vm->global), q->nargs, q->args, 1, false);
}

static void construct_task(swf_ctx *cx, void *user)
{
    const queued *q = user;
    swf_vm *vm = cx->vm;
    swf_dobj *d = q->clip->u.clip;
    if (q->obj) {
        swf_val proto = obj_get(cx, q->obj, vm->s.prototype);
        if (proto.t == SWF_T_OBJ) obj_define(vm, q->clip, vm->s.proto, proto, ATTR_DONT_ENUM | ATTR_DONT_DELETE);
    }
    /* onClipEvent(construct) handlers run before the class constructor. */
    const swf_assets *a = vm->movie->assets;
    for (uint32_t i = 0; i < d->clip_action_count; i++) {
        const swf_clip_action *ca = &a->clip_actions[d->clip_action_first + i];
        if ((ca->events & (1u << 18)) && ca->script < vm->code->script_count && vm->code->scripts[ca->script])
            run_script(vm, d, ca->script);
    }
    if (q->obj) func_construct_on(cx, q->obj, q->clip, 0, NULL);
}

static bool queue_pop(swf_vm *vm, queued *out)
{
    for (int p = 0; p < Q_PRIORITIES; p++) {
        if (vm->q_head[p] != vm->q_tail[p]) {
            *out = vm->queue[p][vm->q_head[p]++];
            if (vm->q_head[p] == vm->q_tail[p]) vm->q_head[p] = vm->q_tail[p] = 0;
            return true;
        }
    }
    return false;
}

void swf_vm_run_queue(swf_vm *vm)
{
    queued q;
    while (queue_pop(vm, &q)) {
        if (q.kind == QK_BROADCAST || q.kind == QK_CALL) {
            vm_run_protected(vm, NULL, q.kind == QK_CALL ? call_task : broadcast_task, &q);
            continue;
        }
        if (q.clip->cls != OC_CLIP || !q.clip->u.clip) continue;
        swf_dobj *d = q.clip->u.clip;
        if (d->removed && !q.unload) continue;
        /* Unload handlers run on the removed clip, which stays allocated until the queue
         * is drained (movie->hold_removed). */
        bool was_removed = d->removed;
        d->removed = false;
        switch (q.kind) {
        case QK_SCRIPT: run_script(vm, d, q.script); break;
        case QK_METHOD: vm_run_protected(vm, d, method_task, &q); break;
        case QK_CONSTRUCT: vm_run_protected(vm, d, construct_task, &q); break;
        default: break;
        }
        if (q.clip->u.clip == d) d->removed = was_removed || d->removed;
    }
    /* Flash clears the global registers between frames (Ruffle `Avm1::clear`). */
    for (int i = 0; i < SWF_GLOBAL_REGS; i++) vm->regs[i] = swf_undef();
    swf_movie_flush_removed(vm->movie);
}

void swf_vm_run_init_actions(swf_vm *vm)
{
    const swf_assets *a = vm->movie->assets;
    for (uint32_t i = 0; i < a->init_count; i++)
        if (a->inits[i].script != SWF_NONE) run_script(vm, clip_root(vm), a->inits[i].script);
}

void swf_vm_set_trace(swf_vm *vm, swf_trace_fn fn, void *user)
{
    vm->trace = fn ? fn : default_trace;
    vm->trace_user = user;
}

static swf_str *A(swf_vm *vm, const char *s)
{
    return atom_ascii(vm, s);
}

swf_vm *swf_vm_new(swf_movie *movie, const swf_game_code *code)
{
    if (!code || code->abi != SWF_CODE_ABI) {
        plat_log("avm1: compiled code ABI %u does not match the runtime (%u); recompile with this swf2c",
                 code ? (unsigned)code->abi : 0u, (unsigned)SWF_CODE_ABI);
        return NULL;
    }
    swf_vm *vm = calloc(1, sizeof *vm);
    if (!vm) return NULL;
    vm->arena = malloc(ARENA_VALUES * sizeof(swf_val));
    if (!vm->arena) {
        free(vm);
        return NULL;
    }
    vm->arena_cap = ARENA_VALUES;
    vm->movie = movie;
    vm->code = code;
    vm->version = movie->assets->swf_version ? (uint8_t)movie->assets->swf_version : code->swf_version;
    vm->case_sensitive = vm->version >= 7;
    vm->trace = default_trace;
    vm->rng = 0x2545F491u;
    vm->start_us = plat_time_us();
    vm->threshold = 1024 * 1024;
    swf_config_defaults(&vm->cfg);
    vm->mouse_visible = true;
    vm->cursor_x = movie->assets->screen_w / 2.0f;
    vm->cursor_y = movie->assets->screen_h / 2.0f;
    vm->next_timer_id = 1;
    g_vm = vm;

    jmp_buf jb;
    vm->abort_jb = &jb;
    if (setjmp(jb)) {
        plat_log("avm1: out of memory initialising the VM");
        vm->abort_jb = NULL;
        swf_vm_free(vm);
        return NULL;
    }

    vm->s.empty = A(vm, "");
    vm->s.proto = A(vm, "__proto__");
    vm->s.prototype = A(vm, "prototype");
    vm->s.constructor = A(vm, "constructor");
    vm->s.__constructor__ = A(vm, "__constructor__");
    vm->s.length = A(vm, "length");
    vm->s.valueOf = A(vm, "valueOf");
    vm->s.toString = A(vm, "toString");
    vm->s.callee = A(vm, "callee");
    vm->s.caller = A(vm, "caller");
    vm->s.arguments = A(vm, "arguments");
    vm->s.this_ = A(vm, "this");
    vm->s.super_ = A(vm, "super");
    vm->s._global = A(vm, "_global");
    vm->s._root = A(vm, "_root");
    vm->s._parent = A(vm, "_parent");
    vm->s._level0 = A(vm, "_level0");
    vm->s.undefined = A(vm, "undefined");
    vm->s.null_ = A(vm, "null");
    vm->s.true_ = A(vm, "true");
    vm->s.false_ = A(vm, "false");
    vm->s.NaN = A(vm, "NaN");
    vm->s.Infinity = A(vm, "Infinity");
    vm->s.neg_Infinity = A(vm, "-Infinity");
    vm->s.number = A(vm, "number");
    vm->s.string = A(vm, "string");
    vm->s.boolean = A(vm, "boolean");
    vm->s.object = A(vm, "object");
    vm->s.function = A(vm, "function");
    vm->s.movieclip = A(vm, "movieclip");
    vm->s.__resolve = A(vm, "__resolve");
    vm->s.name = A(vm, "name");
    vm->s.message = A(vm, "message");
    vm->s.Error = A(vm, "Error");
    vm->s.Object = A(vm, "Object");
    vm->s.Array = A(vm, "Array");
    vm->s.Function = A(vm, "Function");
    vm->s.String = A(vm, "String");
    vm->s.Number = A(vm, "Number");
    vm->s.Boolean = A(vm, "Boolean");
    vm->s.type_Object = A(vm, "[type Object]");
    vm->s.type_Function = A(vm, "[type Function]");

    vm->object_proto = obj_new_bare(vm);
    vm->function_proto = obj_new(vm, vm->object_proto);
    vm->global = obj_new_bare(vm); /* Flash's _global has no prototype */
    vm->global_scope = scope_new(vm, NULL, SC_GLOBAL, vm->global);
    builtins_object(vm);
    builtins_function(vm);
    builtins_number(vm);
    builtins_string(vm);
    builtins_array(vm);
    builtins_math(vm);
    builtins_error(vm);
    builtins_global(vm);
    clip_init_builtins(vm);
    vm->classes = obj_new_bare(vm);
    vm->timer_objs = obj_new_bare(vm);
    vm->shared_objects = obj_new_bare(vm);
    vm->voices = obj_new_bare(vm);
    builtins_broadcaster(vm);
    builtins_movieclip(vm);
    builtins_button(vm);
    builtins_key(vm);
    builtins_mouse(vm);
    builtins_timers(vm);
    builtins_textfield(vm);
    builtins_date(vm);
    builtins_sound(vm);
    builtins_sharedobject(vm);
    builtins_stage(vm);

    for (uint32_t i = 0; i < code->atom_count; i++)
        code->atoms[i] = atom(vm, str_from_utf8(vm, code->atom_utf8[i], code->atom_bytes[i]));
    vm->abort_jb = NULL;

    swf_dobj_free_hook = dobj_freed;
    movie->on_action = swf_vm_on_action;
    movie->on_warning = movie_warning;
    movie->on_event = vm_display_event;
    movie->on_sound = sound_timeline;
    movie->hold_removed = true;
    movie->user = vm;
    return vm;
}

static void clear_script_objects(swf_dobj *d)
{
    for (; d; d = d->next) {
        d->script_object = NULL;
        clear_script_objects(d->children);
    }
}

void swf_vm_free(swf_vm *vm)
{
    if (!vm) return;
    if (vm->global) swf_vm_flush_saves(vm);
    if (vm->mixer) sound_stop_all(vm);
    if (g_vm == vm) {
        swf_dobj_free_hook = NULL;
        g_vm = NULL;
    }
    if (vm->movie) {
        if (vm->movie->root) {
            vm->movie->root->script_object = NULL;
            clear_script_objects(vm->movie->root->children);
        }
        vm->movie->on_action = NULL;
        vm->movie->on_event = NULL;
        vm->movie->on_sound = NULL;
        vm->movie->hold_removed = false;
        swf_movie_flush_removed(vm->movie);
        vm->movie->user = NULL;
    }
    gc_free_all(vm);
    for (int p = 0; p < Q_PRIORITIES; p++) free(vm->queue[p]);
    free(vm->timers);
    free(vm->atoms);
    free(vm->arena);
    free(vm);
}
