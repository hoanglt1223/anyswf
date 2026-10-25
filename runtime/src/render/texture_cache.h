/* Texture residency for the GE: textures live in RAM (assets.bin) and are copied on demand
 * into a VRAM arena, evicting the least recently used ones. Textures drawn in the frame
 * being built are never evicted, because the GE reads them when the list runs. A texture
 * that cannot be made resident is drawn from RAM (slower, still correct).
 * The allocator is platform-neutral so it is unit-tested on PC. */
#ifndef SWFRT_TEXTURE_CACHE_H
#define SWFRT_TEXTURE_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef void (*swf_tex_upload_fn)(void *dst, const void *src, size_t size);

typedef struct swf_texcache_stats {
    size_t used, capacity;       /* bytes of the arena holding textures */
    uint32_t resident;           /* textures in the arena */
    uint32_t uploads, evictions; /* since the cache was created */
    uint32_t misses;             /* draws served from RAM because nothing fit */
} swf_texcache_stats;

typedef struct swf_texcache swf_texcache;

/* `arena` must be 16-byte aligned. NULL when out of memory. */
swf_texcache *swf_texcache_new(uint8_t *arena, size_t size, uint32_t texture_count, swf_tex_upload_fn upload);
void swf_texcache_free(swf_texcache *c);
/* Marks the start of a new frame (textures of earlier frames become evictable). */
void swf_texcache_frame(swf_texcache *c);
/* Address the GE should read texture `tex` from: its arena copy, or `src` itself. */
const void *swf_texcache_use(swf_texcache *c, uint32_t tex, const void *src, size_t size);
void swf_texcache_get_stats(const swf_texcache *c, swf_texcache_stats *out);

#endif
