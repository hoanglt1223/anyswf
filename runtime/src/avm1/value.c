/* Strings (UTF-16 code units, like Flash) and the weak atom table. */
#include "vm_internal.h"

#include <stdio.h>

swf_str *str_new(swf_vm *vm, uint32_t len)
{
    swf_str *s = gc_alloc(vm, sizeof(swf_str) + (size_t)len * sizeof(uint16_t), SWF_GC_STR);
    s->len = len;
    s->hash = 0;
    s->fold = NULL;
    s->atom = false;
    return s;
}

swf_str *str_from_units(swf_vm *vm, const uint16_t *u, uint32_t n)
{
    swf_str *s = str_new(vm, n);
    if (n) memcpy(s->ch, u, n * sizeof(uint16_t));
    return s;
}

swf_str *str_from_utf8(swf_vm *vm, const char *src, size_t n)
{
    const unsigned char *p = (const unsigned char *)src, *end = p + n;
    uint32_t units = 0;
    for (const unsigned char *q = p; q < end;) {
        unsigned c = *q;
        int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        units += extra == 3 ? 2 : 1;
        q += 1 + extra;
    }
    swf_str *s = str_new(vm, units);
    uint32_t i = 0;
    while (p < end && i < units) {
        uint32_t c = *p++;
        int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (extra) c &= 0x3Fu >> extra;
        for (int k = 0; k < extra && p < end; k++) c = (c << 6) | (*p++ & 0x3F);
        if (c >= 0x10000) {
            c -= 0x10000;
            s->ch[i++] = (uint16_t)(0xD800 + (c >> 10));
            if (i < units) s->ch[i++] = (uint16_t)(0xDC00 + (c & 0x3FF));
        } else {
            s->ch[i++] = (uint16_t)c;
        }
    }
    s->len = i;
    return s;
}

swf_str *str_from_ascii(swf_vm *vm, const char *s)
{
    return str_from_utf8(vm, s, strlen(s));
}

char *str_to_utf8(const swf_str *s, size_t *len)
{
    char *out = malloc((size_t)s->len * 3 + 1);
    if (!out) {
        if (len) *len = 0;
        return NULL;
    }
    size_t o = 0;
    for (uint32_t i = 0; i < s->len; i++) {
        uint32_t c = s->ch[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < s->len && s->ch[i + 1] >= 0xDC00 && s->ch[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (s->ch[++i] - 0xDC00);
        } else if (c >= 0xD800 && c < 0xE000) {
            c = 0xFFFD; /* unpaired surrogate */
        }
        if (c < 0x80) {
            out[o++] = (char)c;
        } else if (c < 0x800) {
            out[o++] = (char)(0xC0 | (c >> 6));
            out[o++] = (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out[o++] = (char)(0xE0 | (c >> 12));
            out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (c & 0x3F));
        } else {
            out[o++] = (char)(0xF0 | (c >> 18));
            out[o++] = (char)(0x80 | ((c >> 12) & 0x3F));
            out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[o] = 0;
    if (len) *len = o;
    return out;
}

swf_str *str_concat(swf_vm *vm, swf_str *a, swf_str *b)
{
    if (a->len == 0) return b;
    if (b->len == 0) return a;
    swf_str *s = str_new(vm, a->len + b->len);
    memcpy(s->ch, a->ch, a->len * sizeof(uint16_t));
    memcpy(s->ch + a->len, b->ch, b->len * sizeof(uint16_t));
    return s;
}

swf_str *str_sub(swf_vm *vm, swf_str *s, uint32_t start, uint32_t end)
{
    if (end > s->len) end = s->len;
    if (start >= end) return vm->s.empty;
    if (start == 0 && end == s->len) return s;
    return str_from_units(vm, s->ch + start, end - start);
}

bool str_eq(const swf_str *a, const swf_str *b)
{
    if (a == b) return true;
    if (a->atom && b->atom) return false;
    return a->len == b->len && memcmp(a->ch, b->ch, a->len * sizeof(uint16_t)) == 0;
}

bool str_eq_ascii(const swf_str *a, const char *s)
{
    size_t n = strlen(s);
    if (a->len != n) return false;
    for (size_t i = 0; i < n; i++)
        if (a->ch[i] != (unsigned char)s[i]) return false;
    return true;
}

bool str_eq_nocase(const swf_str *a, const swf_str *b)
{
    if (a->len != b->len) return false;
    for (uint32_t i = 0; i < a->len; i++)
        if (swf_char_lower(a->ch[i]) != swf_char_lower(b->ch[i])) return false;
    return true;
}

int str_cmp(const swf_str *a, const swf_str *b)
{
    uint32_t n = a->len < b->len ? a->len : b->len;
    for (uint32_t i = 0; i < n; i++)
        if (a->ch[i] != b->ch[i]) return a->ch[i] < b->ch[i] ? -1 : 1;
    return a->len < b->len ? -1 : a->len > b->len ? 1 : 0;
}

int64_t str_find(const swf_str *h, const swf_str *n, uint32_t from)
{
    if (n->len == 0) return from <= h->len ? from : -1;
    for (uint32_t i = from; i + n->len <= h->len; i++)
        if (memcmp(h->ch + i, n->ch, n->len * sizeof(uint16_t)) == 0) return i;
    return -1;
}

int64_t str_rfind(const swf_str *h, const swf_str *n, int64_t from)
{
    if (n->len > h->len) return -1;
    int64_t start = (int64_t)(h->len - n->len);
    if (from < start) start = from;
    for (int64_t i = start; i >= 0; i--)
        if (memcmp(h->ch + i, n->ch, n->len * sizeof(uint16_t)) == 0) return i;
    return -1;
}

static uint32_t hash_units(const uint16_t *u, uint32_t n, bool fold)
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++) {
        h ^= fold ? swf_char_lower(u[i]) : u[i];
        h *= 16777619u;
    }
    return h;
}

/* Atom table: open addressing on the exact text; NULL is empty, the tombstone is a
 * sentinel pointer. */
static swf_str TOMB;

static void atoms_grow(swf_vm *vm)
{
    uint32_t cap = vm->atom_cap ? vm->atom_cap * 2 : 1024;
    swf_str **t = calloc(cap, sizeof *t);
    if (!t) vm_abort(vm, "out of memory growing the atom table");
    for (uint32_t i = 0; i < vm->atom_cap; i++) {
        swf_str *a = vm->atoms[i];
        if (!a || a == &TOMB) continue;
        uint32_t h = hash_units(a->ch, a->len, false) & (cap - 1);
        while (t[h]) h = (h + 1) & (cap - 1);
        t[h] = a;
    }
    free(vm->atoms);
    vm->atoms = t;
    vm->atom_cap = cap;
    vm->atom_tombs = 0;
}

static swf_str *atom_lookup_insert(swf_vm *vm, const uint16_t *u, uint32_t n, swf_str *src)
{
    if ((vm->atom_count + vm->atom_tombs + 1) * 2 > vm->atom_cap) atoms_grow(vm);
    uint32_t h = hash_units(u, n, false), mask = vm->atom_cap - 1;
    uint32_t i = h & mask, tomb = UINT32_MAX;
    for (;;) {
        swf_str *a = vm->atoms[i];
        if (!a) break;
        if (a == &TOMB) {
            if (tomb == UINT32_MAX) tomb = i;
        } else if (a->len == n && memcmp(a->ch, u, n * sizeof(uint16_t)) == 0) {
            return a;
        }
        i = (i + 1) & mask;
    }
    swf_str *s = src && !src->atom ? src : str_from_units(vm, u, n);
    s->atom = true;
    s->hash = hash_units(u, n, true);
    if (tomb != UINT32_MAX) {
        vm->atoms[tomb] = s;
        vm->atom_tombs--;
    } else {
        vm->atoms[i] = s;
    }
    vm->atom_count++;
    /* The fold atom is the lower-cased text (itself when already lower case). */
    bool lower = true;
    for (uint32_t k = 0; k < n; k++)
        if (swf_char_lower(u[k]) != u[k]) lower = false;
    if (lower) {
        s->fold = s;
    } else {
        uint16_t stackbuf[64];
        uint16_t *f = n <= 64 ? stackbuf : malloc(n * sizeof(uint16_t));
        if (!f) vm_abort(vm, "out of memory folding an identifier");
        for (uint32_t k = 0; k < n; k++) f[k] = swf_char_lower(u[k]);
        s->fold = atom_lookup_insert(vm, f, n, NULL);
        if (f != stackbuf) free(f);
    }
    return s;
}

swf_str *atom(swf_vm *vm, swf_str *s)
{
    if (s->atom) return s;
    return atom_lookup_insert(vm, s->ch, s->len, s);
}

swf_str *atom_ascii(swf_vm *vm, const char *s)
{
    uint16_t buf[128];
    size_t n = strlen(s);
    if (n > 128) return atom(vm, str_from_ascii(vm, s));
    for (size_t i = 0; i < n; i++) buf[i] = (unsigned char)s[i];
    return atom_lookup_insert(vm, buf, (uint32_t)n, NULL);
}

swf_str *atom_int(swf_vm *vm, int64_t v)
{
    if (v >= 0 && v < 1024 && vm->small_ints[v]) return vm->small_ints[v];
    char buf[32];
    snprintf(buf, sizeof buf, "%lld", (long long)v);
    swf_str *a = atom_ascii(vm, buf);
    if (v >= 0 && v < 1024) vm->small_ints[v] = a;
    return a;
}

/* Drops atoms the last mark phase did not reach (called before sweeping). */
void atoms_sweep(swf_vm *vm)
{
    for (uint32_t i = 0; i < vm->atom_cap; i++) {
        swf_str *a = vm->atoms[i];
        if (a && a != &TOMB && !a->gc.mark) {
            vm->atoms[i] = &TOMB;
            vm->atom_count--;
            vm->atom_tombs++;
        }
    }
    for (int i = 0; i < 1024; i++)
        if (vm->small_ints[i] && !vm->small_ints[i]->gc.mark) vm->small_ints[i] = NULL;
}
