/* AVM1 runtime: the API that swf2c-generated C code calls, plus the VM entry points the
 * player uses. Semantics follow Ruffle's AVM1 (core/src/avm1), the project's reference. */
#ifndef SWFRT_AVM1_H
#define SWFRT_AVM1_H

#include <setjmp.h>

#include "swfrt/config.h"
#include "swfrt/movie.h"
#include "swfrt/value.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bumped whenever generated code and runtime stop being compatible. */
#define SWF_CODE_ABI 1

typedef struct swf_scope swf_scope;
typedef struct swf_ctx swf_ctx;
typedef struct swf_mixer swf_mixer;

typedef swf_val (*swf_code_fn)(swf_ctx *cx);

typedef struct swf_param {
    uint32_t atom;
    uint8_t reg; /* 0: named local variable */
} swf_param;

/* Static description of a DefineFunction/DefineFunction2 body. */
typedef struct swf_fn_desc {
    swf_code_fn code;
    int32_t name_atom;    /* -1: anonymous (the function value is pushed) */
    uint16_t flags;       /* DefineFunction2 preload/suppress flags */
    uint8_t nregs;        /* local registers; 0 for DefineFunction (global registers) */
    uint8_t nparams;
    uint8_t df2;
    const swf_param *params;
} swf_fn_desc;

typedef struct swf_pool {
    uint32_t n;
    const uint32_t *atoms;
} swf_pool;

/* Everything one compiled movie provides; generated as SWF_GAME_CODE in game.c. */
typedef struct swf_game_code {
    uint32_t abi;
    uint8_t swf_version;
    uint32_t atom_count;
    const char *const *atom_utf8;
    const uint32_t *atom_bytes;
    swf_str **atoms;
    uint32_t script_count;
    const swf_code_fn *scripts;
} swf_game_code;

extern const swf_game_code SWF_GAME_CODE;

/* Execution context of one running script or function call. */
struct swf_ctx {
    swf_vm *vm;
    swf_scope *scope;
    swf_val this_;
    swf_obj *callee;
    swf_obj *base_o;     /* script object of the clip that owns the code */
    swf_obj *target_o;   /* tellTarget target; NULL after a failed SetTarget */
    bool target_valid;
    const swf_pool *pool;
    swf_val *regs;       /* local registers (DefineFunction2) or NULL */
    uint32_t nregs;
    uint8_t version;
    struct swf_try *saved_handler;
    size_t arena_mark;
};

typedef struct swf_try {
    jmp_buf jb;
    struct swf_try *prev;
    swf_scope *scope;
    size_t arena_mark;
    int sp;
    int call_depth;
    swf_obj *caller;
    swf_val exc;
} swf_try;

/* --- VM lifecycle (player side) ---------------------------------------------- */

swf_vm *swf_vm_new(swf_movie *movie, const swf_game_code *code);
void swf_vm_free(swf_vm *vm);
/* Runs DoInitAction scripts; call once after the movie is initialised. */
void swf_vm_run_init_actions(swf_vm *vm);
/* Frame script dispatch (installed as the movie's on_action callback by swf_vm_new):
 * scripts are queued while a frame is built; run the queue after swf_movie_advance. */
void swf_vm_on_action(swf_movie *m, swf_dobj *clip, uint32_t script, void *user);
void swf_vm_run_queue(swf_vm *vm);
/* Mark-and-sweep collection; only call between frames. */
void swf_vm_collect(swf_vm *vm);
/* Trace output goes here (default: stdout). */
typedef void (*swf_trace_fn)(const char *utf8, void *user);
void swf_vm_set_trace(swf_vm *vm, swf_trace_fn fn, void *user);
size_t swf_vm_live_bytes(const swf_vm *vm);

/* Input for one display frame. */
typedef struct swf_input {
    uint32_t held;            /* PLAT_BTN_* bits, mapped to keys by anyswf.toml [keys] */
    float stick_x, stick_y;   /* analog nub, -1..1: moves the cursor */
    /* PC keyboard: Flash key codes held (plat_input.keys). While any is held, these are
     * the key state and buttons are not mapped, since the keyboard also drives them. */
    uint32_t keys[8];
    /* PC mouse or touch, in screen pixels; takes over the cursor when it moves. */
    bool has_pointer, pointer_down;
    float pointer_x, pointer_y;
    bool has_cursor;          /* scripted runs: put the cursor at cursor_x/y (screen pixels) */
    float cursor_x, cursor_y;
} swf_input;

void swf_vm_set_config(swf_vm *vm, const swf_config *c);
/* Clock for getTimer, intervals and Date-independent timing, in ms since the VM started.
 * Players pass real time, or frame-derived time for deterministic headless runs. */
void swf_vm_set_time(swf_vm *vm, uint64_t ms);
/* Maps buttons to keys and the nub to the cursor, then fires key, mouse and button events. */
void swf_vm_input(swf_vm *vm, const swf_input *in, double dt);
/* Runs due setInterval/setTimeout callbacks. */
void swf_vm_run_timers(swf_vm *vm);
/* Per-frame bookkeeping after scripts ran: text-field variables, sound completion. */
void swf_vm_after_frame(swf_vm *vm);
/* Draws the cursor (when shown) over the frame. */
void swf_vm_draw_overlay(swf_vm *vm);
/* Audio output for Sound and timeline sounds; NULL plays nothing. */
void swf_vm_set_mixer(swf_vm *vm, swf_mixer *mixer);
/* Writes every SharedObject that has data (also done by swf_vm_free). */
void swf_vm_flush_saves(swf_vm *vm);

/* --- support for generated code ------------------------------------------------ */

swf_val *swf_frame_alloc(swf_ctx *cx, size_t n);
swf_val swf_reg_get(swf_ctx *cx, unsigned r);
void swf_reg_set(swf_ctx *cx, unsigned r, swf_val v);
swf_val swf_pool_get(swf_ctx *cx, unsigned i);
static inline void swf_set_pool(swf_ctx *cx, const swf_pool *p) { cx->pool = p; }
swf_val swf_atomv(swf_ctx *cx, uint32_t atom);
bool swf_to_bool(swf_ctx *cx, swf_val v);
SWF_NORETURN void swf_bad_code(swf_ctx *cx, const char *what);

/* Arithmetic and comparison. */
swf_val swf_op_add(swf_ctx *cx, swf_val a, swf_val b);   /* AS1 numeric add */
swf_val swf_op_add2(swf_ctx *cx, swf_val a, swf_val b);
swf_val swf_op_sub(swf_ctx *cx, swf_val a, swf_val b);
swf_val swf_op_mul(swf_ctx *cx, swf_val a, swf_val b);
swf_val swf_op_div(swf_ctx *cx, swf_val a, swf_val b);
swf_val swf_op_mod(swf_ctx *cx, swf_val a, swf_val b);
swf_val swf_op_inc(swf_ctx *cx, swf_val a);
swf_val swf_op_dec(swf_ctx *cx, swf_val a);
swf_val swf_op_equals(swf_ctx *cx, swf_val a, swf_val b);
swf_val swf_op_equals2(swf_ctx *cx, swf_val a, swf_val b);
swf_val swf_op_strict_equals(swf_ctx *cx, swf_val a, swf_val b);
swf_val swf_op_less(swf_ctx *cx, swf_val a, swf_val b);
swf_val swf_op_less2(swf_ctx *cx, swf_val a, swf_val b);
swf_val swf_op_greater(swf_ctx *cx, swf_val a, swf_val b);
swf_val swf_op_and(swf_ctx *cx, swf_val a, swf_val b);
swf_val swf_op_or(swf_ctx *cx, swf_val a, swf_val b);
swf_val swf_op_not(swf_ctx *cx, swf_val a);
swf_val swf_op_bitand(swf_ctx *cx, swf_val a, swf_val b);
swf_val swf_op_bitor(swf_ctx *cx, swf_val a, swf_val b);
swf_val swf_op_bitxor(swf_ctx *cx, swf_val a, swf_val b);
swf_val swf_op_lshift(swf_ctx *cx, swf_val a, swf_val b);
swf_val swf_op_rshift(swf_ctx *cx, swf_val a, swf_val b);
swf_val swf_op_urshift(swf_ctx *cx, swf_val a, swf_val b);
swf_val swf_op_string_add(swf_ctx *cx, swf_val a, swf_val b);
swf_val swf_op_string_equals(swf_ctx *cx, swf_val a, swf_val b);
swf_val swf_op_string_less(swf_ctx *cx, swf_val a, swf_val b);
swf_val swf_op_string_greater(swf_ctx *cx, swf_val a, swf_val b);
swf_val swf_op_string_length(swf_ctx *cx, swf_val a);
swf_val swf_op_string_extract(swf_ctx *cx, swf_val s, swf_val index, swf_val count);
swf_val swf_op_mb_string_length(swf_ctx *cx, swf_val a);
swf_val swf_op_mb_string_extract(swf_ctx *cx, swf_val s, swf_val index, swf_val count);
swf_val swf_op_ascii_to_char(swf_ctx *cx, swf_val a);
swf_val swf_op_char_to_ascii(swf_ctx *cx, swf_val a);
swf_val swf_op_mb_ascii_to_char(swf_ctx *cx, swf_val a);
swf_val swf_op_mb_char_to_ascii(swf_ctx *cx, swf_val a);
swf_val swf_op_to_integer(swf_ctx *cx, swf_val a);
swf_val swf_op_to_number(swf_ctx *cx, swf_val a);
swf_val swf_op_to_string(swf_ctx *cx, swf_val a);
swf_val swf_op_typeof(swf_ctx *cx, swf_val a);
swf_val swf_op_instanceof(swf_ctx *cx, swf_val obj, swf_val ctor);
swf_val swf_op_cast(swf_ctx *cx, swf_val ctor, swf_val obj);
swf_val swf_op_random(swf_ctx *cx, swf_val max);
swf_val swf_op_get_time(swf_ctx *cx);
void swf_op_trace(swf_ctx *cx, swf_val v);

/* Variables, members and scope. Operands keep the AVM1 stack order of the action. */
swf_val swf_get_variable(swf_ctx *cx, swf_val name);
void swf_set_variable(swf_ctx *cx, swf_val name, swf_val v);
void swf_define_local(swf_ctx *cx, swf_val name, swf_val v);
void swf_define_local2(swf_ctx *cx, swf_val name);
swf_val swf_delete(swf_ctx *cx, swf_val obj, swf_val name);
swf_val swf_delete2(swf_ctx *cx, swf_val name);
swf_val swf_get_member(swf_ctx *cx, swf_val obj, swf_val name);
void swf_set_member(swf_ctx *cx, swf_val obj, swf_val name, swf_val v);
swf_val swf_get_property(swf_ctx *cx, swf_val target, swf_val index);
void swf_set_property(swf_ctx *cx, swf_val target, swf_val index, swf_val v);

/* Calls. `args` points at the deepest argument on the stack: argument i is
 * args[n - 1 - i], exactly as the values lie on the AVM1 stack. */
swf_val swf_call_function(swf_ctx *cx, swf_val name, uint32_t n, const swf_val *args);
swf_val swf_call_method(swf_ctx *cx, swf_val obj, swf_val name, uint32_t n, const swf_val *args);
swf_val swf_new_object(swf_ctx *cx, swf_val name, uint32_t n, const swf_val *args);
swf_val swf_new_method(swf_ctx *cx, swf_val obj, swf_val name, uint32_t n, const swf_val *args);
swf_val swf_init_array(swf_ctx *cx, uint32_t n, const swf_val *elems);
/* Pairs lie as value, name, value, name, ... from the top: pair i is
 * (name = pairs[2n - 2 - 2i], value = pairs[2n - 1 - 2i]). */
swf_val swf_init_object(swf_ctx *cx, uint32_t n, const swf_val *pairs);
void swf_implements(swf_ctx *cx, swf_val ctor, uint32_t n, const swf_val *ifaces);
void swf_extends(swf_ctx *cx, swf_val sub, swf_val super_);
swf_val swf_define_function(swf_ctx *cx, const swf_fn_desc *d);
/* Validates a runtime count operand; returns false (and the action pushes undefined)
 * when it is negative or larger than the values on the stack. */
bool swf_count_ok(swf_val count, int available, uint32_t *n);

/* Dynamic-stack variants for functions whose stack depth is not static. They pop
 * their operands from S[0..*sp) and push the result. */
void swf_dyn_call_function(swf_ctx *cx, swf_val *S, int *sp);
void swf_dyn_call_method(swf_ctx *cx, swf_val *S, int *sp);
void swf_dyn_new_object(swf_ctx *cx, swf_val *S, int *sp);
void swf_dyn_new_method(swf_ctx *cx, swf_val *S, int *sp);
void swf_dyn_init_array(swf_ctx *cx, swf_val *S, int *sp);
void swf_dyn_init_object(swf_ctx *cx, swf_val *S, int *sp);
void swf_dyn_implements(swf_ctx *cx, swf_val *S, int *sp);
void swf_dyn_enumerate(swf_ctx *cx, swf_val *S, int *sp, int cap, bool by_name);
void swf_dyn_start_drag(swf_ctx *cx, swf_val *S, int *sp);
SWF_NORETURN void swf_stack_overflow(swf_ctx *cx);

/* with / try */
bool swf_with_enter(swf_ctx *cx, swf_val obj);
void swf_try_enter(swf_ctx *cx, swf_try *t, int sp);
void swf_try_leave(swf_ctx *cx, swf_try *t);
SWF_NORETURN void swf_throw(swf_ctx *cx, swf_val v);
void swf_catch_var(swf_ctx *cx, uint32_t atom, swf_val v);

/* Timeline control. */
void swf_act_goto_frame(swf_ctx *cx, uint32_t frame0);
void swf_act_goto_frame2(swf_ctx *cx, swf_val frame, bool play, uint32_t scene_bias);
void swf_act_goto_label(swf_ctx *cx, uint32_t atom);
void swf_act_next_frame(swf_ctx *cx);
void swf_act_prev_frame(swf_ctx *cx);
void swf_act_play(swf_ctx *cx);
void swf_act_stop(swf_ctx *cx);
void swf_act_stop_sounds(swf_ctx *cx);
void swf_act_toggle_quality(swf_ctx *cx);
void swf_act_set_target(swf_ctx *cx, uint32_t atom);
void swf_act_set_target2(swf_ctx *cx, swf_val target);
void swf_act_clone_sprite(swf_ctx *cx, swf_val source, swf_val target, swf_val depth);
void swf_act_remove_sprite(swf_ctx *cx, swf_val target);
void swf_act_end_drag(swf_ctx *cx);
swf_val swf_act_target_path(swf_ctx *cx, swf_val v);
void swf_act_call(swf_ctx *cx, swf_val frame);
void swf_act_get_url(swf_ctx *cx, uint32_t url_atom, uint32_t target_atom);
void swf_act_get_url2(swf_ctx *cx, swf_val url, swf_val target, uint8_t flags);
bool swf_act_frame_loaded(swf_ctx *cx, uint32_t frame);
bool swf_act_frame_loaded2(swf_ctx *cx, swf_val frame);
void swf_act_unknown(swf_ctx *cx, uint8_t opcode);
/* True when the clip running this code was removed (scripts then stop). */
bool swf_base_removed(swf_ctx *cx);

#ifdef __cplusplus
}
#endif

#endif
