/* VM lifecycle, script dispatch from the timeline, and diagnostics. */
#include "vm_internal.h"

#include <stdio.h>

#define ARENA_VALUES (64 * 1024)
#define QUEUE_MAX 4096

typedef struct queued {
    swf_obj *clip;
    uint32_t script;
} queued;

/* Frame scripts queued by the timeline; run after each frame is fully built. */
static queued g_queue[QUEUE_MAX];
static uint32_t g_queue_head, g_queue_tail;
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
    char buf[512];
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

static void run_script(swf_vm *vm, swf_dobj *clip, uint32_t script)
{
    if (script >= vm->code->script_count || !vm->code->scripts[script]) return;
    swf_ctx c = ctx_for_clip(vm, clip);
    jmp_buf jb;
    jmp_buf *saved_abort = vm->abort_jb;
    swf_try *saved_handler = vm->handler;
    int saved_depth = vm->call_depth;
    size_t saved_top = vm->arena_top;
    swf_obj *saved_caller = vm->caller;
    vm->abort_jb = &jb;
    vm->handler = NULL;
    vm->uncaught_set = false;
    if (setjmp(jb) == 0) (void)vm->code->scripts[script](&c);
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
    vm->abort_jb = saved_abort;
    vm->handler = saved_handler;
    vm->call_depth = saved_depth;
    vm->arena_top = saved_top;
    vm->caller = saved_caller;
}

void swf_vm_on_action(swf_movie *m, swf_dobj *clip, uint32_t script, void *user)
{
    swf_vm *vm = user;
    (void)m;
    if (g_queue_tail - g_queue_head >= QUEUE_MAX) {
        vm_warn(vm, WARN_STACK, "avm1: frame script queue overflow; scripts dropped");
        return;
    }
    g_queue[g_queue_tail % QUEUE_MAX] = (queued){clip_object(vm, clip), script};
    g_queue_tail++;
}

void swf_vm_run_queue(swf_vm *vm)
{
    while (g_queue_head != g_queue_tail) {
        queued q = g_queue[g_queue_head % QUEUE_MAX];
        g_queue_head++;
        if (q.clip->cls == OC_CLIP && q.clip->u.clip && !q.clip->u.clip->removed)
            run_script(vm, q.clip->u.clip, q.script);
    }
    /* Flash clears the global registers between frames (Ruffle `Avm1::clear`). */
    for (int i = 0; i < SWF_GLOBAL_REGS; i++) vm->regs[i] = swf_undef();
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
                 code ? code->abi : 0, SWF_CODE_ABI);
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
    g_vm = vm;
    g_queue_head = g_queue_tail = 0;

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

    for (uint32_t i = 0; i < code->atom_count; i++)
        code->atoms[i] = atom(vm, str_from_utf8(vm, code->atom_utf8[i], code->atom_bytes[i]));
    vm->abort_jb = NULL;

    swf_dobj_free_hook = dobj_freed;
    movie->on_action = swf_vm_on_action;
    movie->on_warning = movie_warning;
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
        vm->movie->user = NULL;
    }
    gc_free_all(vm);
    free(vm->atoms);
    free(vm->arena);
    free(vm);
}
