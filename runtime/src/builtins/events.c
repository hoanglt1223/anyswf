/* Action queue, clip events (onClipEvent and the AS2 handler methods), display events
 * from the timeline, and AsBroadcaster. Ordering follows Ruffle's ActionQueue and
 * MovieClip::event_dispatch (MIT/Apache-2.0): Initialize and Construct actions of new
 * clips run before ordinary ones; SWF-defined handlers are queued before methods. */
#include "../avm1/vm_internal.h"

#define DD (ATTR_DONT_ENUM | ATTR_DONT_DELETE)

/* --- queue ------------------------------------------------------------------------- */

void vm_queue(swf_vm *vm, int prio, const queued *q)
{
    if (vm->q_tail[prio] == vm->q_cap) {
        /* All queues share one capacity; grow every ring together. */
        uint32_t cap = vm->q_cap ? vm->q_cap * 2 : 256;
        if (cap > (1u << 20)) {
            vm_warn(vm, WARN_QUEUE, "avm1: action queue overflow; actions dropped");
            return;
        }
        for (int p = 0; p < Q_PRIORITIES; p++) {
            queued *n = realloc(vm->queue[p], (size_t)cap * sizeof *n);
            if (!n) {
                vm_warn(vm, WARN_QUEUE, "avm1: out of memory queueing actions; actions dropped");
                return;
            }
            vm->queue[p] = n;
        }
        vm->q_cap = cap;
    }
    vm->queue[prio][vm->q_tail[prio]++] = *q;
}

void vm_queue_script(swf_vm *vm, swf_dobj *clip, uint32_t script, bool unload)
{
    queued q = {0};
    q.clip = clip_object(vm, clip);
    q.kind = QK_SCRIPT;
    q.unload = unload;
    q.script = script;
    /* Unload handlers run before frame scripts already queued (the removal happened
     * while the frame was built). */
    vm_queue(vm, unload ? Q_CONSTRUCT : Q_NORMAL, &q);
}

void vm_queue_method(swf_vm *vm, swf_dobj *clip, swf_obj *obj, swf_str *name, uint32_t nargs, const swf_val *args,
                     bool unload)
{
    queued q = {0};
    q.clip = clip_object(vm, clip);
    q.kind = QK_METHOD;
    q.unload = unload;
    q.name = name;
    q.obj = obj;
    q.nargs = (uint8_t)(nargs > 2 ? 2 : nargs);
    for (uint8_t i = 0; i < q.nargs; i++) q.args[i] = args[i];
    vm_queue(vm, unload ? Q_CONSTRUCT : Q_NORMAL, &q);
}

void vm_queue_broadcast(swf_vm *vm, swf_obj *obj, const char *name, uint32_t nargs, const swf_val *args)
{
    if (!obj) return;
    queued q = {0};
    q.clip = clip_object(vm, clip_root(vm));
    q.kind = QK_BROADCAST;
    q.name = atom_ascii(vm, name);
    q.obj = obj;
    q.nargs = (uint8_t)(nargs > 2 ? 2 : nargs);
    for (uint8_t i = 0; i < q.nargs; i++) q.args[i] = args[i];
    vm_queue(vm, Q_NORMAL, &q);
}

swf_val vm_call_value(swf_ctx *cx, swf_val fn, swf_val this_, uint32_t argc, const swf_val *argv)
{
    if (!is_function(fn)) return swf_undef();
    return func_call(cx, fn.u.o, this_, argc, argv, 1, false);
}

/* --- clip events -------------------------------------------------------------------- */

static bool has_property_ascii(swf_vm *vm, swf_obj *o, const char *name)
{
    swf_ctx c = vm_ctx(vm);
    return obj_has_property(&c, o, atom_ascii(vm, name));
}

typedef struct get_req {
    swf_obj *o;
    swf_str *name;
    swf_val out;
    bool found;
} get_req;

static void get_task(swf_ctx *cx, void *user)
{
    get_req *r = user;
    r->found = obj_get_opt(cx, r->o, r->name, false, &r->out, NULL);
}

bool vm_get_safe(swf_vm *vm, swf_obj *o, const char *name, swf_val *out)
{
    get_req r = {o, atom_ascii(vm, name), swf_undef(), false};
    swf_ctx c = vm_ctx(vm);
    vm_protect(vm, &c, get_task, &r);
    *out = r.out;
    return r.found;
}

void vm_clip_event(swf_vm *vm, swf_dobj *clip, uint32_t flag, const char *method, uint8_t key_code)
{
    if (vm->version < 5 || clip->kind != SWF_CHAR_SPRITE) return;
    const swf_assets *a = vm->movie->assets;
    bool unload = flag == CE_UNLOAD;
    for (uint32_t i = 0; i < clip->clip_action_count; i++) {
        const swf_clip_action *ca = &a->clip_actions[clip->clip_action_first + i];
        if (!(ca->events & flag)) continue;
        if (flag == CE_KEY_PRESS && ca->key_code != key_code) continue;
        vm_queue_script(vm, clip, ca->script, unload);
    }
    if (!method || vm->version < 6) return;
    swf_obj *o = clip_object(vm, clip);
    /* Methods are looked up when the action runs; skip clips that cannot have one. */
    if (has_property_ascii(vm, o, method)) vm_queue_method(vm, clip, o, atom_ascii(vm, method), 0, NULL, unload);
}

swf_obj *vm_registered_class(swf_vm *vm, swf_dobj *d)
{
    if (!vm->classes || d->kind != SWF_CHAR_SPRITE || d->index == SWF_EMPTY_SPRITE) return NULL;
    swf_prop *p = obj_own(vm, vm->classes, atom_int(vm, d->char_id));
    return p && p->val.t == SWF_T_OBJ && p->val.u.o->cls == OC_FUNCTION ? p->val.u.o : NULL;
}

static void placed(swf_vm *vm, swf_dobj *clip)
{
    const swf_assets *a = vm->movie->assets;
    (void)clip_object(vm, clip);
    /* attachMovie and duplicateMovieClip call a registered constructor themselves. */
    if (!(vm->instantiating && vm_registered_class(vm, clip))) {
        bool construct_events = false;
        for (uint32_t i = 0; i < clip->clip_action_count; i++) {
            const swf_clip_action *ca = &a->clip_actions[clip->clip_action_first + i];
            if (ca->events & CE_INITIALIZE) {
                queued q = {0};
                q.clip = clip_object(vm, clip);
                q.kind = QK_SCRIPT;
                q.script = ca->script;
                vm_queue(vm, Q_INIT, &q);
            }
            if (ca->events & CE_CONSTRUCT) construct_events = true;
        }
        swf_obj *ctor = vm_registered_class(vm, clip);
        if (ctor || construct_events) {
            queued q = {0};
            q.clip = clip_object(vm, clip);
            q.kind = QK_CONSTRUCT;
            q.obj = ctor;
            vm_queue(vm, Q_CONSTRUCT, &q);
        }
    }
    vm_clip_event(vm, clip, CE_LOAD, "onLoad", 0);
}

void vm_display_event(swf_movie *m, swf_dobj *clip, int event, void *user)
{
    swf_vm *vm = user;
    (void)m;
    switch (event) {
    case SWF_EV_PLACED:
        if (clip->kind == SWF_CHAR_EDIT_TEXT) textfield_placed(vm, clip);
        else placed(vm, clip);
        break;
    case SWF_EV_ENTER_FRAME: vm_clip_event(vm, clip, CE_ENTER_FRAME, "onEnterFrame", 0); break;
    case SWF_EV_REMOVED:
        if (clip->script_object) {
            mouse_forget(vm, clip->script_object);
            vm_clip_event(vm, clip, CE_UNLOAD, "onUnload", 0);
        }
        sound_clip_removed(vm, clip);
        break;
    default: break;
    }
}

bool vm_is_enabled(swf_vm *vm, swf_dobj *d)
{
    if (!d->script_object) return true;
    swf_val v;
    if (!vm_get_safe(vm, d->script_object, "enabled", &v)) return true;
    return to_bool_v(vm->version, v);
}

bool vm_button_mode(swf_vm *vm, swf_dobj *clip)
{
    if (clip->kind != SWF_CHAR_SPRITE || !clip->parent) return false;
    const swf_assets *a = vm->movie->assets;
    for (uint32_t i = 0; i < clip->clip_action_count; i++)
        if (a->clip_actions[clip->clip_action_first + i].events & CE_BUTTON_EVENTS) return true;
    if (!clip->script_object || vm->version < 6) return false;
    static const char *const METHODS[] = {"onDragOver", "onDragOut", "onPress", "onRelease",
                                          "onReleaseOutside", "onRollOut", "onRollOver"};
    for (size_t i = 0; i < sizeof METHODS / sizeof METHODS[0]; i++)
        if (has_property_ascii(vm, clip->script_object, METHODS[i])) return true;
    return false;
}

/* --- AsBroadcaster (Ruffle avm1/globals/AsBroadcaster.as) ------------------------------- */

static swf_val bc_broadcast(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    swf_vm *vm = cx->vm;
    if (argc < 1 || argv[0].t == SWF_T_UNDEF) return swf_undef();
    swf_obj *self = to_object(cx, this_);
    if (!self) return swf_undef();
    swf_str *name = to_string(cx, argv[0]);
    swf_val lv = obj_get(cx, self, atom_ascii(vm, "_listeners"));
    if (lv.t != SWF_T_OBJ) return swf_undef();
    uint32_t n = to_uint32(cx, obj_get(cx, lv.u.o, vm->s.length));
    if (n > 4096) n = 4096;
    swf_val *list = swf_frame_alloc(cx, n ? n : 1);
    for (uint32_t i = 0; i < n; i++) list[i] = obj_get(cx, lv.u.o, atom_int(vm, i));
    bool any = false;
    for (uint32_t i = 0; i < n; i++) {
        swf_obj *l = to_object(cx, list[i]);
        if (!l) continue;
        any = true;
        if (name->len == 0) {
            (void)func_call(cx, l, list[i], argc - 1, argv + 1, 1, false);
        } else {
            (void)obj_call_method(cx, l, name, argc - 1, argv + 1, true);
        }
    }
    return any ? swf_bool(true) : swf_undef();
}

static swf_val bc_remove(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    swf_vm *vm = cx->vm;
    swf_obj *self = to_object(cx, this_);
    if (!self) return swf_bool(false);
    swf_val lv = obj_get(cx, self, atom_ascii(vm, "_listeners"));
    if (lv.t != SWF_T_OBJ) return swf_bool(false);
    swf_val l = arg(argc, argv, 0);
    for (uint32_t i = 0; i < to_uint32(cx, obj_get(cx, lv.u.o, vm->s.length)); i++) {
        if (abstract_eq(cx, obj_get(cx, lv.u.o, atom_int(vm, i)), l)) {
            swf_val sargs[2] = {swf_num(i), swf_num(1)};
            (void)obj_call_method(cx, lv.u.o, atom_ascii(vm, "splice"), 2, sargs, false);
            return swf_bool(true);
        }
    }
    return swf_bool(false);
}

static swf_val bc_add(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    swf_vm *vm = cx->vm;
    swf_obj *self = to_object(cx, this_);
    if (!self) return swf_bool(true);
    swf_val l = arg(argc, argv, 0);
    (void)obj_call_method(cx, self, atom_ascii(vm, "removeListener"), 1, &l, false);
    swf_val lv = obj_get(cx, self, atom_ascii(vm, "_listeners"));
    if (lv.t == SWF_T_OBJ) (void)obj_call_method(cx, lv.u.o, atom_ascii(vm, "push"), 1, &l, false);
    return swf_bool(true);
}

void broadcaster_init(swf_vm *vm, swf_obj *o)
{
    swf_ctx c = vm_ctx(vm);
    obj_define(vm, o, atom_ascii(vm, "broadcastMessage"), swf_objv(func_new_native(vm, bc_broadcast, NULL, NULL)), DD);
    swf_obj *bc = vm->global;
    swf_val asb = obj_get(&c, bc, atom_ascii(vm, "AsBroadcaster"));
    swf_val add = asb.t == SWF_T_OBJ ? obj_get(&c, asb.u.o, atom_ascii(vm, "addListener")) : swf_undef();
    swf_val rem = asb.t == SWF_T_OBJ ? obj_get(&c, asb.u.o, atom_ascii(vm, "removeListener")) : swf_undef();
    if (add.t != SWF_T_OBJ) add = swf_objv(func_new_native(vm, bc_add, NULL, NULL));
    if (rem.t != SWF_T_OBJ) rem = swf_objv(func_new_native(vm, bc_remove, NULL, NULL));
    obj_define(vm, o, atom_ascii(vm, "addListener"), add, DD);
    obj_define(vm, o, atom_ascii(vm, "removeListener"), rem, DD);
    obj_define(vm, o, atom_ascii(vm, "_listeners"), swf_objv(array_new(&c, 0, NULL)), DD);
}

static swf_val bc_initialize(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)this_;
    swf_obj *o = argc ? to_object(cx, argv[0]) : NULL;
    if (o) broadcaster_init(cx->vm, o);
    return swf_undef();
}

static swf_val bc_ctor(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)cx; (void)argc; (void)argv;
    return this_;
}

void builtins_broadcaster(swf_vm *vm)
{
    swf_obj *p = obj_new(vm, vm->object_proto);
    swf_obj *c = func_new_native(vm, bc_ctor, NULL, p);
    c->u.fn.ctor = NULL; /* `new AsBroadcaster()` returns the new object */
    obj_define(vm, vm->global, atom_ascii(vm, "AsBroadcaster"), swf_objv(c), ATTR_DONT_ENUM);
    const uint16_t A = DD | ATTR_VERSION_6;
    native_method(vm, c, "broadcastMessage", bc_broadcast, A);
    native_method(vm, c, "addListener", bc_add, A);
    native_method(vm, c, "removeListener", bc_remove, A);
    native_method(vm, c, "initialize", bc_initialize, A);
}
