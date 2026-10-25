/* Mouse: the analog nub moves an on-screen cursor (with acceleration) and a mapped
 * button (Cross by default) is the mouse button. Mouse listeners, onClipEvent mouse
 * events, the button state machine for SWF buttons and clips in button mode
 * (onPress/onRelease/onRollOver...), startDrag and _droptarget live here. Picking
 * follows Ruffle's mouse_pick_avm1: the topmost enabled button or button-mode clip whose
 * shape contains the point. */
#include "../avm1/vm_internal.h"
#include "../render/render.h"

#include <math.h>

#define DD (ATTR_DONT_ENUM | ATTR_DONT_DELETE)

enum {
    BC_IDLE_TO_OVER_UP = 1 << 0,
    BC_OVER_UP_TO_IDLE = 1 << 1,
    BC_OVER_UP_TO_OVER_DOWN = 1 << 2,
    BC_OVER_DOWN_TO_OVER_UP = 1 << 3,
    BC_OVER_DOWN_TO_OUT_DOWN = 1 << 4,
    BC_OUT_DOWN_TO_OVER_DOWN = 1 << 5,
    BC_OUT_DOWN_TO_IDLE = 1 << 6
};

enum { BS_UP = 1, BS_OVER = 2, BS_DOWN = 4 };

static swf_dobj *obj_clip(swf_obj *o)
{
    return o && o->cls == OC_CLIP && o->u.clip && !o->u.clip->removed ? o->u.clip : NULL;
}

void mouse_forget(swf_vm *vm, swf_obj *o)
{
    if (vm->hover == o) vm->hover = NULL;
    if (vm->pressed == o) vm->pressed = NULL;
    if (vm->drag == o) vm->drag = NULL;
}

/* --- picking ----------------------------------------------------------------------- */

static bool interactive(swf_vm *vm, swf_dobj *c)
{
    if (c->kind == SWF_CHAR_BUTTON) return vm_is_enabled(vm, c);
    return vm_button_mode(vm, c) && vm_is_enabled(vm, c);
}

static swf_dobj *pick(swf_vm *vm, swf_dobj *container, float x, float y)
{
    swf_dobj *kids[512];
    int n = 0;
    for (swf_dobj *c = container->children; c && n < 512; c = c->next) kids[n++] = c;
    for (int i = n - 1; i >= 0; i--) {
        swf_dobj *c = kids[i];
        if (c->removed || !c->visible || c->clip_depth > 0) continue;
        if (c->kind == SWF_CHAR_BUTTON || c->kind == SWF_CHAR_SPRITE) {
            if (interactive(vm, c)) {
                if (swf_dobj_hit(vm->movie, c, x, y, true)) return c;
                continue;
            }
            if (c->kind == SWF_CHAR_SPRITE) {
                swf_dobj *r = pick(vm, c, x, y);
                if (r) return r;
            }
        }
    }
    return NULL;
}

/* The sprite whose own content is under the point, ignoring `skip` (the dragged clip). */
static swf_dobj *drop_pick(swf_vm *vm, swf_dobj *container, swf_dobj *skip, float x, float y)
{
    swf_dobj *kids[512];
    int n = 0;
    for (swf_dobj *c = container->children; c && n < 512; c = c->next) kids[n++] = c;
    for (int i = n - 1; i >= 0; i--) {
        swf_dobj *c = kids[i];
        if (c == skip || c->removed || !c->visible || c->clip_depth > 0) continue;
        if (c->kind == SWF_CHAR_SPRITE) {
            swf_dobj *r = drop_pick(vm, c, skip, x, y);
            if (r) return r;
        } else if (swf_dobj_hit(vm->movie, c, x, y, true)) {
            return container;
        }
    }
    if (container->drawing && container != skip && swf_dobj_hit(vm->movie, container, x, y, true)) return container;
    return NULL;
}

swf_str *mouse_drop_target(swf_vm *vm, swf_dobj *dragged)
{
    swf_dobj *t = drop_pick(vm, clip_root(vm), dragged, vm->mouse_x, vm->mouse_y);
    if (!t || !t->parent) return vm->s.empty;
    swf_ctx c = vm_ctx(vm);
    swf_val v = clip_get_property_index(&c, t, 11 /* _target */);
    return v.t == SWF_T_STR ? v.u.s : vm->s.empty;
}

/* --- button events -------------------------------------------------------------------- */

static void button_actions(swf_vm *vm, swf_dobj *b, uint16_t cond)
{
    const swf_button *bt = &vm->movie->assets->buttons[b->index];
    if (!b->parent) return;
    for (uint32_t i = 0; i < bt->action_count; i++)
        if (bt->actions[i].conditions & cond) vm_queue_script(vm, b->parent, bt->actions[i].script, false);
}

static void button_method(swf_vm *vm, swf_dobj *b, const char *method)
{
    if (vm->version < 6) return;
    swf_obj *o = clip_object(vm, b);
    vm_queue_method(vm, b, o, atom_ascii(vm, method), 0, NULL, false);
}

static void clip_goto_label(swf_vm *vm, swf_dobj *d, const char *label)
{
    int64_t f = swf_clip_find_label(vm->movie, d, label);
    if (f > 0) {
        swf_clip_stop(d);
        swf_clip_goto(vm->movie, d, (uint32_t)f);
    }
}

typedef enum { EV_ROLL_OVER, EV_ROLL_OUT, EV_PRESS, EV_RELEASE, EV_RELEASE_OUTSIDE, EV_DRAG_OVER, EV_DRAG_OUT } btn_ev;

static void fire(swf_vm *vm, swf_obj *o, btn_ev ev)
{
    swf_dobj *d = obj_clip(o);
    if (!d) return;
    static const char *const METHOD[] = {"onRollOver", "onRollOut", "onPress", "onRelease", "onReleaseOutside",
                                         "onDragOver", "onDragOut"};
    if (d->kind == SWF_CHAR_BUTTON) {
        static const uint16_t COND[] = {BC_IDLE_TO_OVER_UP, BC_OVER_UP_TO_IDLE, BC_OVER_UP_TO_OVER_DOWN,
                                        BC_OVER_DOWN_TO_OVER_UP, BC_OUT_DOWN_TO_IDLE, BC_OUT_DOWN_TO_OVER_DOWN,
                                        BC_OVER_DOWN_TO_OUT_DOWN};
        static const uint8_t STATE[] = {BS_OVER, BS_UP, BS_DOWN, BS_OVER, BS_UP, BS_DOWN, BS_OVER};
        d->button_state = STATE[ev];
        button_actions(vm, d, COND[ev]);
        button_method(vm, d, METHOD[ev]);
        return;
    }
    static const uint32_t FLAG[] = {CE_ROLL_OVER, CE_ROLL_OUT, CE_PRESS, CE_RELEASE, CE_RELEASE_OUTSIDE,
                                    CE_DRAG_OVER, CE_DRAG_OUT};
    static const char *const LABEL[] = {"_over", "_up", "_down", "_over", "_up", "_down", "_over"};
    if (vm_button_mode(vm, d)) clip_goto_label(vm, d, LABEL[ev]);
    vm_clip_event(vm, d, FLAG[ev], METHOD[ev], 0);
}

void mouse_button_key_press(swf_vm *vm, uint8_t code)
{
    /* The first button or clip with a matching keyPress handler consumes the key. */
    const swf_assets *a = vm->movie->assets;
    for (swf_dobj *c = vm->movie->exec_head; c; c = c->exec_next) {
        if (c->removed || c->frame == 0) continue;
        for (uint32_t i = 0; i < c->clip_action_count; i++) {
            const swf_clip_action *ca = &a->clip_actions[c->clip_action_first + i];
            if ((ca->events & CE_KEY_PRESS) && ca->key_code == code) {
                vm_queue_script(vm, c, ca->script, false);
                return;
            }
        }
        for (swf_dobj *b = c->children; b; b = b->next) {
            if (b->kind != SWF_CHAR_BUTTON || b->removed) continue;
            const swf_button *bt = &a->buttons[b->index];
            for (uint32_t i = 0; i < bt->action_count; i++) {
                if (bt->actions[i].key == code) {
                    vm_queue_script(vm, c, bt->actions[i].script, false);
                    return;
                }
            }
        }
    }
}

/* --- drag ------------------------------------------------------------------------------- */

static void parent_point(swf_dobj *d, float x, float y, float *px, float *py)
{
    swf_matrix pw = d->parent ? swf_dobj_world_matrix(d->parent) : SWF_MATRIX_IDENTITY, inv;
    if (!swf_matrix_invert(&pw, &inv)) inv = SWF_MATRIX_IDENTITY;
    *px = inv.a * x + inv.c * y + inv.tx;
    *py = inv.b * x + inv.d * y + inv.ty;
}

void mouse_start_drag(swf_vm *vm, swf_dobj *d, bool lock, bool constrain, const float rect[4])
{
    vm->drag = clip_object(vm, d);
    vm->drag_lock = lock;
    vm->drag_constrain = constrain;
    if (constrain) {
        vm->drag_rect[0] = fminf(rect[0], rect[2]);
        vm->drag_rect[2] = fmaxf(rect[0], rect[2]);
        vm->drag_rect[1] = fminf(rect[1], rect[3]);
        vm->drag_rect[3] = fmaxf(rect[1], rect[3]);
    }
    float px, py;
    parent_point(d, vm->mouse_x, vm->mouse_y, &px, &py);
    vm->drag_dx = lock ? 0 : d->matrix.tx - px;
    vm->drag_dy = lock ? 0 : d->matrix.ty - py;
}

void mouse_stop_drag(swf_vm *vm)
{
    vm->drag = NULL;
}

static void drag_update(swf_vm *vm)
{
    swf_dobj *d = obj_clip(vm->drag);
    if (!d) {
        vm->drag = NULL;
        return;
    }
    float px, py;
    parent_point(d, vm->mouse_x, vm->mouse_y, &px, &py);
    float x = px + vm->drag_dx, y = py + vm->drag_dy;
    if (vm->drag_constrain) {
        x = fminf(fmaxf(x, vm->drag_rect[0]), vm->drag_rect[2]);
        y = fminf(fmaxf(y, vm->drag_rect[1]), vm->drag_rect[3]);
    }
    d->matrix.tx = roundf(x);
    d->matrix.ty = roundf(y);
}

/* --- per-frame update ------------------------------------------------------------------- */

static void clip_events_all(swf_vm *vm, uint32_t flag, const char *method)
{
    for (swf_dobj *c = vm->movie->exec_head; c; c = c->exec_next)
        if (!c->removed && c->frame > 0) vm_clip_event(vm, c, flag, method, 0);
}

void mouse_update(swf_vm *vm, const swf_input *in, double dt)
{
    float ox = vm->cursor_x, oy = vm->cursor_y;
    bool pointer_moved = in->has_pointer && (in->pointer_x != vm->pointer_x || in->pointer_y != vm->pointer_y ||
                                             in->pointer_down);
    if (in->has_pointer) {
        vm->pointer_x = in->pointer_x;
        vm->pointer_y = in->pointer_y;
    }
    if (in->has_cursor) {
        vm->cursor_x = in->cursor_x;
        vm->cursor_y = in->cursor_y;
    } else if (pointer_moved) {
        vm->pointer_mode = true;
        vm->cursor_x = in->pointer_x;
        vm->cursor_y = in->pointer_y;
    } else {
        float mag = sqrtf(in->stick_x * in->stick_x + in->stick_y * in->stick_y);
        if (mag > 0.0f) {
            vm->pointer_mode = false;
            vm->stick_hold += (float)dt;
            float boost = 1.0f + vm->cfg.cursor_accel * fminf(vm->stick_hold, 1.0f);
            /* Squared response: small tilts give fine control, full tilt the configured speed. */
            float v = vm->cfg.cursor_speed * mag * boost * (float)dt;
            vm->cursor_x += in->stick_x * v;
            vm->cursor_y += in->stick_y * v;
        } else {
            vm->stick_hold = 0;
        }
    }
    const swf_assets *as = vm->movie->assets;
    vm->cursor_x = fminf(fmaxf(vm->cursor_x, 0.0f), as->screen_w - 1.0f);
    vm->cursor_y = fminf(fmaxf(vm->cursor_y, 0.0f), as->screen_h - 1.0f);
    swf_matrix view = swf_movie_view_matrix(vm->movie), inv;
    if (!swf_matrix_invert(&view, &inv)) inv = SWF_MATRIX_IDENTITY;
    vm->mouse_x = inv.a * vm->cursor_x + inv.c * vm->cursor_y + inv.tx;
    vm->mouse_y = inv.b * vm->cursor_x + inv.d * vm->cursor_y + inv.ty;

    bool down = in->has_pointer && in->pointer_down;
    for (int i = 0; i < SWF_CONFIG_BUTTONS && !input_has_keys(in); i++)
        if (vm->cfg.keys[i] == SWF_KEY_MOUSE && (in->held & (1u << i))) down = true;

    bool moved = vm->cursor_x != ox || vm->cursor_y != oy;
    if (moved) {
        if (vm->drag) drag_update(vm);
        clip_events_all(vm, CE_MOUSE_MOVE, "onMouseMove");
        vm_queue_broadcast(vm, vm->mouse_obj, "onMouseMove", 0, NULL);
    }

    swf_dobj *t = pick(vm, clip_root(vm), vm->mouse_x, vm->mouse_y);
    swf_obj *to = t ? clip_object(vm, t) : NULL;
    if (to != vm->hover) {
        if (vm->pressed) {
            if (vm->hover == vm->pressed) fire(vm, vm->pressed, EV_DRAG_OUT);
            else if (to == vm->pressed) fire(vm, vm->pressed, EV_DRAG_OVER);
        } else {
            if (vm->hover) fire(vm, vm->hover, EV_ROLL_OUT);
            if (to) fire(vm, to, EV_ROLL_OVER);
        }
        vm->hover = to;
    }

    if (down && !vm->mouse_down) {
        vm->mouse_down = true;
        clip_events_all(vm, CE_MOUSE_DOWN, "onMouseDown");
        vm_queue_broadcast(vm, vm->mouse_obj, "onMouseDown", 0, NULL);
        if (to) {
            vm->pressed = to;
            fire(vm, to, EV_PRESS);
        }
    } else if (!down && vm->mouse_down) {
        vm->mouse_down = false;
        clip_events_all(vm, CE_MOUSE_UP, "onMouseUp");
        vm_queue_broadcast(vm, vm->mouse_obj, "onMouseUp", 0, NULL);
        if (vm->pressed) {
            swf_obj *p = vm->pressed;
            vm->pressed = NULL;
            if (p == to) {
                fire(vm, p, EV_RELEASE);
            } else {
                fire(vm, p, EV_RELEASE_OUTSIDE);
                if (to) fire(vm, to, EV_ROLL_OVER);
            }
        }
    }
}

/* --- cursor -------------------------------------------------------------------------- */

void swf_vm_draw_overlay(swf_vm *vm)
{
    if (!vm->mouse_visible || !vm->cfg.cursor_visible || vm->pointer_mode) return;
    /* An arrow 12 px tall: black outline under a white body, tip at the hot spot. */
    static const float OUTER[] = {-1, -2, -1, 14, 3, 11, 6, 17, 9, 16, 6, 10, 11, 10};
    static const float INNER[] = {0, 0, 0, 11, 3, 8.5f, 6, 15, 7.5f, 14.5f, 4.8f, 9, 8.5f, 9};
    static swf_mesh_vertex v[14]; /* read by the GE after this returns */
    for (int i = 0; i < 7; i++) {
        v[i] = (swf_mesh_vertex){.rgba = 0xFF000000u, .x = OUTER[i * 2], .y = OUTER[i * 2 + 1]};
        v[7 + i] = (swf_mesh_vertex){.rgba = 0xFFFFFFFFu, .x = INNER[i * 2], .y = INNER[i * 2 + 1]};
    }
    /* Each arrow: head (0,1,6) + tail quad (2,3,4,5) fanned from the notch. */
    static const uint16_t IDX[] = {0, 1, 6, 2, 3, 4, 2, 4, 5, 7, 8, 13, 9, 10, 11, 9, 11, 12};
    swf_matrix m = {1, 0, 0, 1, floorf(vm->cursor_x), floorf(vm->cursor_y)};
    swf_render_mesh(&m, v, 14, IDX, sizeof IDX / sizeof IDX[0], &SWF_CXFORM_IDENTITY, 0);
}

/* --- Mouse class ------------------------------------------------------------------------ */

static swf_val mouse_show(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)this_; (void)argc; (void)argv;
    bool was = cx->vm->mouse_visible;
    cx->vm->mouse_visible = true;
    return swf_num(was ? 0 : 1);
}

static swf_val mouse_hide(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)this_; (void)argc; (void)argv;
    bool was = cx->vm->mouse_visible;
    cx->vm->mouse_visible = false;
    return swf_num(was ? 0 : 1);
}

void builtins_mouse(swf_vm *vm)
{
    swf_obj *m = obj_new(vm, vm->object_proto);
    vm->mouse_obj = m;
    obj_define_ascii(vm, vm->global, "Mouse", swf_objv(m), ATTR_DONT_ENUM);
    native_method(vm, m, "show", mouse_show, DD | ATTR_READ_ONLY);
    native_method(vm, m, "hide", mouse_hide, DD | ATTR_READ_ONLY);
    broadcaster_init(vm, m);
}
