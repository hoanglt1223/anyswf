/* Texture residency (see texture_cache.h): first-fit allocation in an arena kept sorted
 * by offset, least-recently-used eviction among textures not used this frame. */
#include "texture_cache.h"

#include <stdlib.h>
#include <string.h>

#define ALIGN 16u

typedef struct entry {
    size_t offset, size;
    uint32_t last_frame;
    bool resident;
} entry;

struct swf_texcache {
    uint8_t *arena;
    size_t capacity, used;
    swf_tex_upload_fn upload;
    entry *e;
    uint32_t count;
    uint32_t *order;   /* resident texture ids sorted by offset */
    uint32_t nres;
    uint32_t frame;
    uint32_t uploads, evictions, misses;
};

swf_texcache *swf_texcache_new(uint8_t *arena, size_t size, uint32_t texture_count, swf_tex_upload_fn upload)
{
    swf_texcache *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->e = calloc(texture_count ? texture_count : 1, sizeof *c->e);
    c->order = calloc(texture_count ? texture_count : 1, sizeof *c->order);
    if (!c->e || !c->order) {
        swf_texcache_free(c);
        return NULL;
    }
    c->arena = arena;
    c->capacity = size & ~(size_t)(ALIGN - 1);
    c->count = texture_count;
    c->upload = upload;
    c->frame = 1;
    return c;
}

void swf_texcache_free(swf_texcache *c)
{
    if (!c) return;
    free(c->e);
    free(c->order);
    free(c);
}

void swf_texcache_frame(swf_texcache *c)
{
    c->frame++;
}

static void remove_at(swf_texcache *c, uint32_t i)
{
    entry *e = &c->e[c->order[i]];
    e->resident = false;
    c->used -= e->size;
    memmove(c->order + i, c->order + i + 1, (c->nres - i - 1) * sizeof *c->order);
    c->nres--;
    c->evictions++;
}

/* First gap of `size` bytes; returns the insertion index in `order`, or -1. */
static int64_t find_gap(const swf_texcache *c, size_t size, size_t *offset)
{
    size_t at = 0;
    for (uint32_t i = 0; i <= c->nres; i++) {
        size_t next = i < c->nres ? c->e[c->order[i]].offset : c->capacity;
        if (next - at >= size) {
            *offset = at;
            return i;
        }
        if (i < c->nres) at = c->e[c->order[i]].offset + c->e[c->order[i]].size;
    }
    return -1;
}

static bool evict_one(swf_texcache *c)
{
    int64_t victim = -1;
    for (uint32_t i = 0; i < c->nres; i++) {
        const entry *e = &c->e[c->order[i]];
        if (e->last_frame >= c->frame) continue; /* the GE may still read it this frame */
        if (victim < 0 || e->last_frame < c->e[c->order[victim]].last_frame) victim = i;
    }
    if (victim < 0) return false;
    remove_at(c, (uint32_t)victim);
    return true;
}

const void *swf_texcache_use(swf_texcache *c, uint32_t tex, const void *src, size_t size)
{
    if (!c || tex >= c->count) return src;
    entry *e = &c->e[tex];
    if (e->resident) {
        e->last_frame = c->frame;
        return c->arena + e->offset;
    }
    size_t need = (size + ALIGN - 1) & ~(size_t)(ALIGN - 1);
    if (need == 0 || need > c->capacity) {
        c->misses++;
        return src;
    }
    size_t offset = 0;
    int64_t at;
    while ((at = find_gap(c, need, &offset)) < 0) {
        if (!evict_one(c)) {
            c->misses++;
            return src;
        }
    }
    memmove(c->order + at + 1, c->order + at, (c->nres - (uint32_t)at) * sizeof *c->order);
    c->order[at] = tex;
    c->nres++;
    e->offset = offset;
    e->size = need;
    e->resident = true;
    e->last_frame = c->frame;
    c->used += need;
    c->uploads++;
    c->upload(c->arena + offset, src, size);
    return c->arena + offset;
}

void swf_texcache_get_stats(const swf_texcache *c, swf_texcache_stats *out)
{
    memset(out, 0, sizeof *out);
    if (!c) return;
    out->used = c->used;
    out->capacity = c->capacity;
    out->resident = c->nres;
    out->uploads = c->uploads;
    out->evictions = c->evictions;
    out->misses = c->misses;
}
