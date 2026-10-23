/* AVM1 value model: tagged values, GC-managed strings (UTF-16 code units, as in Flash)
 * and objects with ordered property maps. Collection only happens between frames
 * (swf_vm_collect), never while script code runs, so C locals need no rooting. */
#ifndef SWFRT_VALUE_H
#define SWFRT_VALUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_MSC_VER)
#define SWF_NORETURN __declspec(noreturn)
#elif defined(__GNUC__)
#define SWF_NORETURN __attribute__((noreturn))
#else
#define SWF_NORETURN
#endif

typedef struct swf_vm swf_vm;
typedef struct swf_str swf_str;
typedef struct swf_obj swf_obj;

typedef enum swf_type {
    SWF_T_UNDEF = 0,
    SWF_T_NULL,
    SWF_T_BOOL,
    SWF_T_NUM,
    SWF_T_STR,
    SWF_T_OBJ
} swf_type;

typedef struct swf_val {
    uint32_t t;
    union {
        double n;
        int b;
        swf_str *s;
        swf_obj *o;
    } u;
} swf_val;

typedef struct swf_gc_hdr {
    struct swf_gc_hdr *next;
    uint8_t kind;   /* SWF_GC_STR / SWF_GC_OBJ / SWF_GC_SCOPE */
    uint8_t mark;
    uint8_t flags;
    uint8_t pad;
} swf_gc_hdr;

enum { SWF_GC_STR = 1, SWF_GC_OBJ = 2, SWF_GC_SCOPE = 3 };

/* Immutable string. Atoms are interned (pointer-equal when equal); `fold` points to the
 * lower-cased atom used for case-insensitive (SWF 6 and older) property names. */
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4200) /* C99 flexible array member */
#endif
struct swf_str {
    swf_gc_hdr gc;
    uint32_t len;
    uint32_t hash;      /* of the case-folded text; valid for atoms */
    swf_str *fold;      /* atoms only */
    bool atom;
    uint16_t ch[];
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

static inline double swf_f64_bits(uint64_t bits)
{
    union {
        uint64_t u;
        double d;
    } c;
    c.u = bits;
    return c.d;
}

static inline swf_val swf_undef(void) { swf_val v; v.t = SWF_T_UNDEF; v.u.n = 0; return v; }
static inline swf_val swf_null(void) { swf_val v; v.t = SWF_T_NULL; v.u.n = 0; return v; }
static inline swf_val swf_bool(bool b) { swf_val v; v.t = SWF_T_BOOL; v.u.n = 0; v.u.b = b; return v; }
static inline swf_val swf_num(double n) { swf_val v; v.t = SWF_T_NUM; v.u.n = n; return v; }
static inline swf_val swf_strv(swf_str *s) { swf_val v; v.t = SWF_T_STR; v.u.s = s; return v; }
static inline swf_val swf_objv(swf_obj *o) { swf_val v; v.t = SWF_T_OBJ; v.u.o = o; return v; }

#ifdef __cplusplus
}
#endif

#endif
