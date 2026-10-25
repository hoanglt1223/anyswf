/* Key: PSP buttons mapped to Flash key codes (anyswf.toml [keys]), Key.isDown/getCode/
 * getAscii, key listeners, onClipEvent(keyDown/keyUp) and button key presses. Event
 * order follows Ruffle's Player::handle_event: clip events, then Key listeners. */
#include "../avm1/vm_internal.h"

#define DD (ATTR_DONT_ENUM | ATTR_DONT_DELETE)

uint8_t key_ascii(swf_vm *vm, uint8_t code)
{
    bool shift = vm->key_down[16];
    if (code >= 'A' && code <= 'Z') return shift ? code : (uint8_t)(code + 32);
    if (code >= '0' && code <= '9') return code;
    switch (code) {
    case 8: case 9: case 13: case 27: case 32: return code;
    case 46: return 127;
    default: return 0;
    }
}

/* Key codes for button "keyPress" conditions (swf::ButtonKeyCode). */
static uint8_t button_key_code(swf_vm *vm, uint8_t code)
{
    switch (code) {
    case 37: return 1;
    case 39: return 2;
    case 36: return 3;
    case 35: return 4;
    case 45: return 5;
    case 46: return 6;
    case 8: return 8;
    case 13: return 13;
    case 38: return 14;
    case 40: return 15;
    case 33: return 16;
    case 34: return 17;
    case 9: return 18;
    case 27: return 19;
    default: {
        uint8_t a = key_ascii(vm, code);
        return a >= 32 ? a : 0;
    }
    }
}

static void clip_events_all(swf_vm *vm, uint32_t flag)
{
    for (swf_dobj *c = vm->movie->exec_head; c; c = c->exec_next)
        if (!c->removed && c->frame > 0) vm_clip_event(vm, c, flag, NULL, 0);
}

bool input_has_keys(const swf_input *in)
{
    for (int i = 0; i < 8; i++)
        if (in->keys[i]) return true;
    return false;
}

void key_update(swf_vm *vm, const swf_input *in)
{
    bool want[SWF_KEYS] = {0};
    if (input_has_keys(in)) {
        for (int k = 1; k < SWF_KEYS; k++) want[k] = (in->keys[k >> 5] >> (k & 31)) & 1u;
    } else {
        for (int i = 0; i < SWF_CONFIG_BUTTONS; i++) {
            uint8_t k = vm->cfg.keys[i];
            if (k != SWF_KEY_NONE && k != SWF_KEY_MOUSE && (in->held & (1u << i))) want[k] = true;
        }
    }
    /* Releases first, so a remapped button pair never reports two keys down at once. */
    for (int k = 1; k < SWF_KEYS; k++) {
        if (vm->key_down[k] && !want[k]) {
            vm->key_down[k] = false;
            vm->last_key = (uint8_t)k;
            clip_events_all(vm, CE_KEY_UP);
            vm_queue_broadcast(vm, vm->key_obj, "onKeyUp", 0, NULL);
            /* Each key event runs its actions before the next (Key.getCode stays right). */
            swf_vm_run_queue(vm);
        }
    }
    for (int k = 1; k < SWF_KEYS; k++) {
        if (!vm->key_down[k] && want[k]) {
            vm->key_down[k] = true;
            vm->last_key = (uint8_t)k;
            vm->last_ascii = key_ascii(vm, (uint8_t)k);
            clip_events_all(vm, CE_KEY_DOWN);
            vm_queue_broadcast(vm, vm->key_obj, "onKeyDown", 0, NULL);
            uint8_t bk = button_key_code(vm, (uint8_t)k);
            if (bk) mouse_button_key_press(vm, bk);
            swf_vm_run_queue(vm);
        }
    }
}

static swf_val key_is_down(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)this_;
    if (argc < 1) return swf_bool(false);
    int32_t k = to_int32(cx, argv[0]);
    return swf_bool(k > 0 && k < SWF_KEYS && cx->vm->key_down[k]);
}

static swf_val key_get_code(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)this_; (void)argc; (void)argv;
    return swf_num(cx->vm->last_key);
}

static swf_val key_get_ascii(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)this_; (void)argc; (void)argv;
    return swf_num(cx->vm->last_ascii);
}

static swf_val key_false(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)cx; (void)this_; (void)argc; (void)argv;
    return swf_bool(false);
}

void builtins_key(swf_vm *vm)
{
    swf_obj *k = obj_new(vm, vm->object_proto);
    vm->key_obj = k;
    obj_define_ascii(vm, vm->global, "Key", swf_objv(k), ATTR_DONT_ENUM);
    static const struct { const char *name; int code; } CODES[] = {
        {"ALT", 18}, {"BACKSPACE", 8}, {"CAPSLOCK", 20}, {"CONTROL", 17}, {"DELETEKEY", 46}, {"DOWN", 40},
        {"END", 35}, {"ENTER", 13}, {"ESCAPE", 27}, {"HOME", 36}, {"INSERT", 45}, {"LEFT", 37}, {"PGDN", 34},
        {"PGUP", 33}, {"RIGHT", 39}, {"SHIFT", 16}, {"SPACE", 32}, {"TAB", 9}, {"UP", 38}};
    const uint16_t C = DD | ATTR_READ_ONLY;
    for (size_t i = 0; i < sizeof CODES / sizeof CODES[0]; i++)
        obj_define_ascii(vm, k, CODES[i].name, swf_num(CODES[i].code), C);
    native_method(vm, k, "isDown", key_is_down, C);
    native_method(vm, k, "getCode", key_get_code, C);
    native_method(vm, k, "getAscii", key_get_ascii, C);
    native_method(vm, k, "isToggled", key_false, C);
    native_method(vm, k, "isAccessible", key_false, C);
    broadcaster_init(vm, k);
}
