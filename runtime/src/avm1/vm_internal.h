/* Internal AVM1 runtime structures shared by the runtime/src/avm1 sources. */
#ifndef SWFRT_VM_INTERNAL_H
#define SWFRT_VM_INTERNAL_H

#include "swfrt/avm1.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* Property attributes (ASSetPropFlags bits). */
enum {
    ATTR_DONT_ENUM = 1 << 0,
    ATTR_DONT_DELETE = 1 << 1,
    ATTR_READ_ONLY = 1 << 2,
    ATTR_VERSION_6 = 0x80,
    ATTR_VERSION_7 = 0x500,
    ATTR_VERSION_8 = 0x1000,
    ATTR_VERSION_MASK = 0x1FFF << 3
};

typedef struct swf_prop {
    swf_str *key;            /* atom; NULL marks a deleted slot */
    swf_val val;
    swf_obj *getter, *setter;
    uint16_t attr;
} swf_prop;

typedef enum obj_class {
    OC_PLAIN = 0,
    OC_FUNCTION,
    OC_ARRAY,
    OC_STRING,
    OC_NUMBER,
    OC_BOOLEAN,
    OC_CLIP,
    OC_SUPER,
    OC_ERROR,
    OC_ARGUMENTS
} obj_class;

typedef swf_val (*swf_native_fn)(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv);

typedef struct swf_func {
    const swf_fn_desc *desc;   /* bytecode function, or NULL */
    swf_native_fn native;
    swf_native_fn ctor;        /* native constructor (propagates its result) */
    swf_scope *scope;          /* closed-over scope */
    const swf_pool *pool;
    swf_obj *base_o;           /* clip object the function was defined on */
    uint8_t version;
} swf_func;

struct swf_obj {
    swf_gc_hdr gc;
    uint8_t cls;
    uint32_t nprops, cap;      /* props[0..nprops) in insertion order, holes allowed */
    uint32_t live;
    swf_prop *props;
    int32_t *index;            /* open addressing into props; -1 empty */
    uint32_t index_cap;
    swf_obj **ifaces;
    uint32_t nifaces;
    swf_obj *watchers;         /* object mapping names to watcher records, or NULL */
    union {
        swf_func fn;
        swf_val prim;          /* String/Number/Boolean wrappers */
        swf_dobj *clip;
        struct {
            swf_obj *this_;
            uint8_t depth;
        } super_;
    } u;
};

enum scope_class { SC_GLOBAL, SC_TARGET, SC_LOCAL, SC_WITH };

struct swf_scope {
    swf_gc_hdr gc;
    uint8_t cls;
    swf_scope *parent;
    swf_obj *obj;
};

#define SWF_GLOBAL_REGS 4
#define SWF_MAX_CALL_DEPTH 256

typedef struct watcher_entry {
    swf_obj *callback;
    swf_val user;
} watcher_entry;

struct swf_vm {
    swf_movie *movie;
    const swf_game_code *code;
    uint8_t version;
    bool case_sensitive;

    swf_gc_hdr *objects;       /* all GC allocations */
    size_t live_bytes, last_live, threshold;

    /* atom table (weak) */
    swf_str **atoms;
    uint32_t atom_cap, atom_count, atom_tombs;
    swf_str *small_ints[1024];

    swf_obj *global;
    swf_scope *global_scope;
    swf_obj *object_proto, *function_proto, *array_proto, *string_proto, *number_proto,
        *boolean_proto, *error_proto, *clip_proto;
    swf_obj *object_ctor, *function_ctor, *array_ctor, *string_ctor, *number_ctor, *boolean_ctor;
    swf_val regs[SWF_GLOBAL_REGS];

    swf_try *handler;          /* innermost try handler */
    jmp_buf *abort_jb;         /* script-level abort target */
    int call_depth;
    swf_obj *caller;           /* callee of the running function, for arguments.caller */

    /* frame arena */
    swf_val *arena;
    size_t arena_cap, arena_top;

    swf_val uncaught;          /* exception that escaped a script, reported as a warning */
    bool uncaught_set;
    swf_trace_fn trace;
    void *trace_user;
    uint32_t rng;
    uint64_t start_us;
    uint32_t warned;

    /* names used by the runtime itself */
    struct {
        swf_str *proto, *prototype, *constructor, *__constructor__, *length, *valueOf, *toString,
            *callee, *caller, *arguments, *this_, *super_, *_global, *_root, *_parent, *_level0,
            *empty, *undefined, *null_, *true_, *false_, *NaN, *Infinity, *neg_Infinity,
            *number, *string, *boolean, *object, *function, *movieclip, *__resolve, *name,
            *message, *Error, *Object, *Array, *Function, *String, *Number, *Boolean, *type_Object,
            *type_Function, *__proto__check;
    } s;
};

/* --- strings (string.c) --- */
swf_str *str_new(swf_vm *vm, uint32_t len);
swf_str *str_from_utf8(swf_vm *vm, const char *s, size_t n);
swf_str *str_from_ascii(swf_vm *vm, const char *s);
swf_str *str_from_units(swf_vm *vm, const uint16_t *u, uint32_t n);
swf_str *str_concat(swf_vm *vm, swf_str *a, swf_str *b);
swf_str *str_sub(swf_vm *vm, swf_str *s, uint32_t start, uint32_t end);
bool str_eq(const swf_str *a, const swf_str *b);
bool str_eq_ascii(const swf_str *a, const char *s);
bool str_eq_nocase(const swf_str *a, const swf_str *b);
uint16_t swf_char_lower(uint16_t c);
uint16_t swf_char_upper(uint16_t c);
int str_cmp(const swf_str *a, const swf_str *b);
/* Encodes as UTF-8 into a malloc'd, NUL-terminated buffer. */
char *str_to_utf8(const swf_str *s, size_t *len);
swf_str *atom(swf_vm *vm, swf_str *s);
swf_str *atom_ascii(swf_vm *vm, const char *s);
swf_str *atom_int(swf_vm *vm, int64_t i);
void atoms_sweep(swf_vm *vm);
int64_t str_find(const swf_str *hay, const swf_str *needle, uint32_t from);
int64_t str_rfind(const swf_str *hay, const swf_str *needle, int64_t from);

/* --- GC (gc.c) --- */
void *gc_alloc(swf_vm *vm, size_t size, uint8_t kind);

/* --- objects (object.c) --- */
swf_obj *obj_new(swf_vm *vm, swf_obj *proto);
swf_obj *obj_new_bare(swf_vm *vm);
swf_obj *obj_new_class(swf_vm *vm, swf_obj *proto, uint8_t cls);
swf_prop *obj_own(swf_vm *vm, swf_obj *o, swf_str *key);
void obj_define(swf_vm *vm, swf_obj *o, swf_str *key, swf_val v, uint16_t attr);
void obj_define_ascii(swf_vm *vm, swf_obj *o, const char *key, swf_val v, uint16_t attr);
bool obj_delete(swf_ctx *cx, swf_obj *o, swf_str *key);
swf_val obj_proto(swf_ctx *cx, swf_obj *o);
bool obj_has_own(swf_ctx *cx, swf_obj *o, swf_str *key);
bool obj_has_property(swf_ctx *cx, swf_obj *o, swf_str *key);
/* Full lookup: getters, proto chain, display properties and __resolve. */
swf_val obj_get(swf_ctx *cx, swf_obj *o, swf_str *key);
bool obj_get_opt(swf_ctx *cx, swf_obj *o, swf_str *key, bool resolve, swf_val *out, int *depth);
void obj_set(swf_ctx *cx, swf_obj *o, swf_str *key, swf_val v);
swf_val obj_call_method(swf_ctx *cx, swf_obj *o, swf_str *name, uint32_t argc, const swf_val *argv,
                        bool special);
/* Keys in Flash enumeration order (prototype keys first, newest own keys first). */
swf_str **obj_keys(swf_ctx *cx, swf_obj *o, bool include_hidden, uint32_t *n);
void obj_add_property(swf_vm *vm, swf_obj *o, swf_str *key, swf_obj *getter, swf_obj *setter,
                      uint16_t attr);
void obj_set_attributes(swf_vm *vm, swf_obj *o, swf_str *key, uint16_t set, uint16_t clear);
bool obj_is_instance_of(swf_ctx *cx, swf_obj *o, swf_obj *proto);
bool prop_visible(const swf_prop *p, uint8_t version);
swf_val obj_get_data(swf_ctx *cx, swf_obj *o, swf_str *key);

/* --- functions and calls (call.c) --- */
swf_obj *func_new_native(swf_vm *vm, swf_native_fn fn, swf_native_fn ctor, swf_obj *prototype);
swf_val func_call(swf_ctx *cx, swf_obj *fn, swf_val this_, uint32_t argc, const swf_val *argv,
                  uint8_t depth, bool ctor_call);
swf_val func_construct(swf_ctx *cx, swf_obj *fn, uint32_t argc, const swf_val *argv);
void func_construct_on(swf_ctx *cx, swf_obj *fn, swf_obj *this_, uint32_t argc, const swf_val *argv);
bool is_function(swf_val v);
swf_ctx ctx_for_clip(swf_vm *vm, swf_dobj *clip);
void native_method(swf_vm *vm, swf_obj *o, const char *name, swf_native_fn fn, uint16_t attr);

/* --- coercion (coerce.c) --- */
double to_number(swf_ctx *cx, swf_val v);
double prim_to_number(swf_ctx *cx, swf_val v);
swf_str *to_string(swf_ctx *cx, swf_val v);
bool to_bool_v(uint8_t version, swf_val v);
swf_val to_primitive(swf_ctx *cx, swf_val v);
swf_val to_primitive_num(swf_ctx *cx, swf_val v);
int32_t to_int32(swf_ctx *cx, swf_val v);
uint32_t to_uint32(swf_ctx *cx, swf_val v);
int32_t f64_to_i32(double d);
uint32_t f64_to_u32(double d);
uint16_t f64_to_u16(double d);
swf_str *number_to_string(swf_vm *vm, double n);
double string_to_number(swf_str *s, uint8_t version);
double parse_float_impl(const uint16_t *s, uint32_t n, bool strict);
swf_obj *to_object(swf_ctx *cx, swf_val v);          /* NULL for null/undefined */
swf_obj *to_object_or_bare(swf_ctx *cx, swf_val v);
swf_str *type_of(swf_ctx *cx, swf_val v);
bool abstract_eq(swf_ctx *cx, swf_val a, swf_val b);
swf_val abstract_lt(swf_ctx *cx, swf_val a, swf_val b);
bool strict_eq(swf_val a, swf_val b);
bool instance_of(swf_ctx *cx, swf_val obj, swf_val ctor);
/* Property key for a value (atoms; integers use a small cache). */
swf_str *to_key(swf_ctx *cx, swf_val v);

/* --- scope and variables (scope.c) --- */
swf_scope *scope_new(swf_vm *vm, swf_scope *parent, uint8_t cls, swf_obj *obj);
swf_scope *scope_with_target(swf_vm *vm, swf_scope *s, swf_obj *clip);
swf_val scope_resolve(swf_ctx *cx, swf_str *name, swf_obj **found_on);
void scope_set(swf_ctx *cx, swf_scope *s, swf_str *name, swf_val v);
void scope_define_local(swf_ctx *cx, swf_scope *s, swf_str *name, swf_val v);
bool in_local_scope(swf_ctx *cx);
swf_val get_variable_str(swf_ctx *cx, swf_str *path, swf_obj **this_out);
void set_variable_str(swf_ctx *cx, swf_str *path, swf_val v);
swf_obj *resolve_target_path(swf_ctx *cx, swf_dobj *root, swf_obj *start, const swf_str *path,
                             uint32_t from, uint32_t to, bool first, bool handle_this);
swf_dobj *resolve_target_clip(swf_ctx *cx, swf_dobj *start, swf_val target, bool allow_empty);

/* --- display binding (clip.c) --- */
swf_obj *clip_object(swf_vm *vm, swf_dobj *d);
swf_dobj *clip_of(swf_val v);
swf_dobj *clip_root(swf_vm *vm);
swf_dobj *clip_parent(swf_vm *vm, swf_dobj *d);
bool clip_get_display_prop(swf_ctx *cx, swf_dobj *d, swf_str *name, swf_val *out);
bool clip_set_display_prop(swf_ctx *cx, swf_dobj *d, swf_str *name, swf_val v);
swf_dobj *clip_child_by_name(swf_vm *vm, swf_dobj *d, const swf_str *name, uint32_t from, uint32_t to);
swf_str *clip_path(swf_vm *vm, swf_dobj *d);
swf_val clip_get_property_index(swf_ctx *cx, swf_dobj *d, int index);
void clip_set_property_index(swf_ctx *cx, swf_dobj *d, int index, swf_val v);
void clip_goto(swf_ctx *cx, swf_dobj *d, swf_val frame, bool play, uint32_t scene_bias);
bool clip_removed(swf_dobj *d);
void clip_init_builtins(swf_vm *vm);

/* --- builtins (builtins_*.c) --- */
void builtins_init(swf_vm *vm);
void builtins_object(swf_vm *vm);
void builtins_function(swf_vm *vm);
void builtins_array(swf_vm *vm);
void builtins_string(swf_vm *vm);
void builtins_number(swf_vm *vm);
void builtins_math(swf_vm *vm);
void builtins_global(swf_vm *vm);
void builtins_error(swf_vm *vm);
swf_obj *array_new(swf_ctx *cx, uint32_t n, const swf_val *elems);
uint32_t array_length(swf_ctx *cx, swf_obj *a);
swf_obj *make_class(swf_vm *vm, const char *name, swf_native_fn fn, swf_native_fn ctor, swf_obj *proto);

/* Clip behind a context's base/target object; NULL once the clip was removed. */
static inline swf_dobj *ctx_base(swf_ctx *cx)
{
    swf_dobj *d = cx->base_o && cx->base_o->cls == OC_CLIP ? cx->base_o->u.clip : NULL;
    return d && !d->removed ? d : NULL;
}
static inline swf_dobj *ctx_target(swf_ctx *cx)
{
    swf_dobj *d = cx->target_o && cx->target_o->cls == OC_CLIP ? cx->target_o->u.clip : NULL;
    return d && !d->removed ? d : NULL;
}
/* Prints a Flash Player warning into the trace output ("Warning: ..."). */
void vm_trace_warning(swf_vm *vm, const char *msg);
void gc_collect_now(swf_vm *vm);
void gc_free_all(swf_vm *vm);

/* --- misc --- */
void vm_warn(swf_vm *vm, uint32_t once_bit, const char *fmt, ...);
SWF_NORETURN void vm_abort(swf_vm *vm, const char *why);
static inline swf_val arg(uint32_t argc, const swf_val *argv, uint32_t i)
{
    return i < argc ? argv[i] : swf_undef();
}
static inline bool is_nullish(swf_val v) { return v.t == SWF_T_UNDEF || v.t == SWF_T_NULL; }

enum {
    WARN_UNKNOWN_OP = 1u << 0,
    WARN_GETURL = 1u << 1,
    WARN_DRAG = 1u << 2,
    WARN_CLONE = 1u << 3,
    WARN_STACK = 1u << 4,
    WARN_QUALITY = 1u << 5,
    WARN_SOUND = 1u << 6
};

#endif
