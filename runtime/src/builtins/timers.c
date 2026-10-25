/* setInterval/clearInterval, setTimeout/clearTimeout and the clock behind getTimer.
 * Scheduling follows Ruffle's core/src/timer.rs (MIT/Apache-2.0): a timer fires when its
 * tick time is before the current time, intervals advance by their period (catching up
 * a bounded number of ticks), and the action queue runs after every callback. */
#include "../avm1/vm_internal.h"

#define MIN_INTERVAL_MS 10
#define MAX_TICKS 10

void swf_vm_set_time(swf_vm *vm, uint64_t ms)
{
    vm->now_ms = ms;
}

swf_val swf_op_get_time(swf_ctx *cx)
{
    return swf_num((double)cx->vm->now_ms);
}

static swf_val add_timer(swf_ctx *cx, uint32_t argc, const swf_val *argv, bool once)
{
    swf_vm *vm = cx->vm;
    if (argc < 1 || argv[0].t != SWF_T_OBJ) return swf_undef();
    swf_obj *rec = obj_new_bare(vm);
    uint32_t first_param;
    swf_val interval;
    if (argv[0].u.o->cls == OC_FUNCTION) {
        obj_define_ascii(vm, rec, "f", argv[0], 0);
        interval = arg(argc, argv, 1);
        first_param = 2;
    } else {
        obj_define_ascii(vm, rec, "o", argv[0], 0);
        obj_define_ascii(vm, rec, "m", swf_strv(to_string(cx, arg(argc, argv, 1))), 0);
        interval = arg(argc, argv, 2);
        first_param = 3;
    }
    if (interval.t == SWF_T_UNDEF) return swf_undef();
    int32_t ms = to_int32(cx, interval);
    if (ms < MIN_INTERVAL_MS) ms = MIN_INTERVAL_MS;
    uint32_t np = argc > first_param ? argc - first_param : 0;
    obj_define_ascii(vm, rec, "a", swf_objv(array_new(cx, 0, NULL)), 0);
    swf_obj *params = obj_get_data(cx, rec, atom_ascii(vm, "a")).u.o;
    for (uint32_t i = 0; i < np; i++) obj_set(cx, params, atom_int(vm, i), argv[first_param + i]);
    if (vm->ntimers == vm->timer_cap) {
        uint32_t cap = vm->timer_cap ? vm->timer_cap * 2 : 16;
        swf_timer *t = realloc(vm->timers, (size_t)cap * sizeof *t);
        if (!t) vm_abort(vm, "out of memory adding a timer");
        vm->timers = t;
        vm->timer_cap = cap;
    }
    uint32_t id = vm->next_timer_id++;
    if (vm->next_timer_id == 0) vm->next_timer_id = 1;
    vm->timers[vm->ntimers++] = (swf_timer){id, once, vm->now_ms + (uint64_t)ms, (uint64_t)ms};
    obj_define(vm, vm->timer_objs, atom_int(vm, id), swf_objv(rec), 0);
    return swf_num(id);
}

static bool remove_timer(swf_vm *vm, uint32_t id)
{
    for (uint32_t i = 0; i < vm->ntimers; i++) {
        if (vm->timers[i].id == id) {
            vm->timers[i] = vm->timers[--vm->ntimers];
            swf_ctx c = vm_ctx(vm);
            obj_delete(&c, vm->timer_objs, atom_int(vm, id));
            return true;
        }
    }
    return false;
}

static swf_val g_set_interval(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)this_;
    return add_timer(cx, argc, argv, false);
}

static swf_val g_set_timeout(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)this_;
    return add_timer(cx, argc, argv, true);
}

static swf_val g_clear(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)this_;
    if (argc < 1) return swf_undef();
    int32_t id = to_int32(cx, argv[0]);
    if (id > 0) remove_timer(cx->vm, (uint32_t)id);
    return swf_undef();
}

typedef struct fire_req {
    swf_obj *rec;
    bool cancel;
} fire_req;

static void fire_task(swf_ctx *cx, void *user)
{
    fire_req *r = user;
    swf_vm *vm = cx->vm;
    swf_val f = obj_get_data(cx, r->rec, atom_ascii(vm, "f"));
    swf_val o = obj_get_data(cx, r->rec, atom_ascii(vm, "o"));
    swf_val pa = obj_get_data(cx, r->rec, atom_ascii(vm, "a"));
    uint32_t n = pa.t == SWF_T_OBJ ? array_length(cx, pa.u.o) : 0;
    if (n > 256) n = 256;
    swf_val *args = swf_frame_alloc(cx, n ? n : 1);
    for (uint32_t i = 0; i < n; i++) args[i] = obj_get(cx, pa.u.o, atom_int(vm, i));
    if (f.t == SWF_T_OBJ) {
        (void)func_call(cx, f.u.o, swf_undef(), n, args, 1, false);
        return;
    }
    if (o.t != SWF_T_OBJ) return;
    /* A method timer on a removed clip cancels itself without firing. */
    if (o.u.o->cls == OC_CLIP && (!o.u.o->u.clip || o.u.o->u.clip->removed)) {
        r->cancel = true;
        return;
    }
    swf_val m = obj_get_data(cx, r->rec, atom_ascii(vm, "m"));
    (void)obj_call_method(cx, o.u.o, to_string(cx, m), n, args, true);
}

void swf_vm_run_timers(swf_vm *vm)
{
    for (int ticks = 0; ticks < MAX_TICKS && vm->ntimers; ticks++) {
        uint32_t best = 0;
        for (uint32_t i = 1; i < vm->ntimers; i++)
            if (vm->timers[i].due < vm->timers[best].due) best = i;
        swf_timer t = vm->timers[best];
        if (t.due >= vm->now_ms) break;
        swf_prop *p = obj_own(vm, vm->timer_objs, atom_int(vm, t.id));
        fire_req r = {p && p->val.t == SWF_T_OBJ ? p->val.u.o : NULL, false};
        if (r.rec) vm_run_protected(vm, clip_root(vm), fire_task, &r);
        swf_vm_run_queue(vm);
        /* The callback may have cleared or added timers; find this one again. */
        for (uint32_t i = 0; i < vm->ntimers; i++) {
            if (vm->timers[i].id != t.id) continue;
            if (t.once || r.cancel || !r.rec) remove_timer(vm, t.id);
            else vm->timers[i].due += vm->timers[i].interval;
            break;
        }
    }
}

void builtins_timers(swf_vm *vm)
{
    swf_obj *g = vm->global;
    native_method(vm, g, "setInterval", g_set_interval, ATTR_DONT_ENUM);
    native_method(vm, g, "clearInterval", g_clear, ATTR_DONT_ENUM);
    native_method(vm, g, "setTimeout", g_set_timeout, ATTR_DONT_ENUM);
    native_method(vm, g, "clearTimeout", g_clear, ATTR_DONT_ENUM);
}
