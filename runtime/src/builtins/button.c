/* Button objects (SWF DefineButton instances as seen by script) and the glue the
 * player calls once per display frame. The button state machine itself is in mouse.c. */
#include "../avm1/vm_internal.h"

#define DD (ATTR_DONT_ENUM | ATTR_DONT_DELETE)

static swf_val btn_get_depth(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)cx; (void)argc; (void)argv;
    swf_dobj *d = clip_of(this_);
    return d ? swf_num(d->depth - AVM_DEPTH_BIAS) : swf_undef();
}

static swf_val btn_class(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)cx; (void)argc; (void)argv;
    return this_;
}

void builtins_button(swf_vm *vm)
{
    swf_obj *p = obj_new(vm, vm->object_proto);
    vm->button_proto = p;
    swf_obj *c = func_new_native(vm, btn_class, NULL, p);
    c->u.fn.ctor = NULL;
    obj_define_ascii(vm, vm->global, "Button", swf_objv(c), ATTR_DONT_ENUM);
    obj_define_ascii(vm, p, "enabled", swf_bool(true), DD);
    obj_define_ascii(vm, p, "useHandCursor", swf_bool(true), DD);
    native_method(vm, p, "getDepth", btn_get_depth, DD | ATTR_READ_ONLY);
}

/* --- per-frame glue ---------------------------------------------------------------------- */

void swf_vm_set_config(swf_vm *vm, const swf_config *c)
{
    vm->cfg = *c;
    vm->movie->scale_mode = c->scale_mode;
}

void swf_vm_input(swf_vm *vm, const swf_input *in, double dt)
{
    key_update(vm, in);
    mouse_update(vm, in, dt);
    vm->prev_held = in->held;
    swf_vm_run_queue(vm);
}

void swf_vm_after_frame(swf_vm *vm)
{
    textfield_sync_variables(vm);
    sound_after_frame(vm);
    swf_vm_run_queue(vm);
}

void swf_vm_set_mixer(swf_vm *vm, swf_mixer *mixer)
{
    vm->mixer = mixer;
}

void swf_vm_flush_saves(swf_vm *vm)
{
    sharedobject_flush_all(vm);
}
