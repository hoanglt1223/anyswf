/* Runtime unit tests: assets.bin validation and timeline playback, on hand-built files. */
#include "swfrt/assets.h"
#include "swfrt/movie.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
            failures++;                                                  \
        }                                                                \
    } while (0)

/* --- tiny assets.bin builder ------------------------------------------------ */

typedef struct buf {
    uint8_t d[4096];
    size_t n;
} buf;

static void put8(buf *b, uint8_t v) { b->d[b->n++] = v; }
static void put16(buf *b, uint16_t v) { put8(b, (uint8_t)v); put8(b, (uint8_t)(v >> 8)); }
static void put32(buf *b, uint32_t v) { put16(b, (uint16_t)v); put16(b, (uint16_t)(v >> 16)); }
static void putf(buf *b, float f) { uint32_t u; memcpy(&u, &f, 4); put32(b, u); }

typedef struct block {
    char tag[5];
    buf data;
    uint32_t count;
} block;

#define MAX_BLOCKS 24
static block blocks[MAX_BLOCKS];
static int nblocks;

static buf *add_block(const char *tag, uint32_t count)
{
    block *b = &blocks[nblocks++];
    memcpy(b->tag, tag, 5);
    b->data.n = 0;
    b->count = count;
    return &b->data;
}

/* Root timeline with two frames: frame 1 places shape char 1 at depth 1 and sprite
 * char 2 at depth 2; frame 2 removes depth 1. Sprite char 2 has 3 empty frames. */
static size_t build(uint8_t *out, size_t cap)
{
    nblocks = 0;
    buf *strs = add_block("STRS", 0);
    put8(strs, 0);
    memcpy(strs->d + strs->n, "clip\0", 5);
    strs->n += 5;
    blocks[0].count = (uint32_t)strs->n;

    buf *data = add_block("DATA", 0);
    /* three solid vertices: rgba + s16 x,y,z,pad */
    int16_t xy[3][2] = {{0, 0}, {200, 0}, {0, 200}};
    for (int i = 0; i < 3; i++) {
        put32(data, 0xFF0000FFu);
        put16(data, (uint16_t)xy[i][0]);
        put16(data, (uint16_t)xy[i][1]);
        put32(data, 0);
    }
    while (data->n % 16) put8(data, 0);
    size_t idx_off = data->n;
    put16(data, 0); put16(data, 1); put16(data, 2);
    blocks[1].count = (uint32_t)data->n;

    add_block("TEXR", 0);
    buf *draw = add_block("DRAW", 1);
    put8(draw, 0); put8(draw, 0); put8(draw, 0); put8(draw, 0);
    put32(draw, SWF_NONE);
    put32(draw, 0); put32(draw, 3);
    put32(draw, (uint32_t)idx_off); put32(draw, 3);
    put32(draw, 0); put32(draw, 0);
    buf *shap = add_block("SHAP", 1);
    put32(shap, 0); put32(shap, 0); put32(shap, 200); put32(shap, 200);
    put32(shap, 0); put32(shap, 1);
    add_block("MRPH", 0);
    add_block("MRPF", 0);

    buf *cmds = add_block("CMDS", 0);
    /* root frame 1 */
    put8(cmds, SWF_CMD_PLACE); put16(cmds, SWF_PLACE_CHARACTER | SWF_PLACE_MATRIX); put16(cmds, 1); put16(cmds, 1);
    putf(cmds, 1); putf(cmds, 0); putf(cmds, 0); putf(cmds, 1); putf(cmds, 100); putf(cmds, 40);
    put8(cmds, SWF_CMD_PLACE); put16(cmds, SWF_PLACE_CHARACTER | SWF_PLACE_NAME); put16(cmds, 2); put16(cmds, 2);
    put32(cmds, 1);
    uint32_t f1_end = (uint32_t)cmds->n;
    /* root frame 2 */
    put8(cmds, SWF_CMD_REMOVE); put16(cmds, 1);
    uint32_t f2_end = (uint32_t)cmds->n;
    blocks[nblocks - 1].count = (uint32_t)cmds->n;

    buf *frms = add_block("FRMS", 7);
    put32(frms, 0); put32(frms, f1_end); put32(frms, f2_end);
    put32(frms, f2_end); put32(frms, f2_end); put32(frms, f2_end); put32(frms, f2_end);
    buf *sprt = add_block("SPRT", 2);
    put32(sprt, 2); put32(sprt, 0); put32(sprt, 0); put32(sprt, 0); put32(sprt, SWF_NONE); put32(sprt, 0);
    put32(sprt, 3); put32(sprt, 3); put32(sprt, 0); put32(sprt, 0); put32(sprt, SWF_NONE); put32(sprt, 0);
    add_block("LABL", 0);
    buf *chr = add_block("CHAR", 3);
    put8(chr, 0); put8(chr, 0); put16(chr, 0); put32(chr, 0);
    put8(chr, SWF_CHAR_SHAPE); put8(chr, 0); put16(chr, 0); put32(chr, 0);
    put8(chr, SWF_CHAR_SPRITE); put8(chr, 0); put16(chr, 0); put32(chr, 1);
    add_block("SCRP", 0);

    size_t off = 64 + (size_t)nblocks * 16;
    off = (off + 63) / 64 * 64;
    size_t offsets[MAX_BLOCKS];
    for (int i = 0; i < nblocks; i++) {
        offsets[i] = off;
        off = (off + blocks[i].data.n + 63) / 64 * 64;
    }
    if (off > cap) return 0;
    memset(out, 0, off);
    memcpy(out, "ASWF", 4);
    buf h = {{0}, 0};
    put16(&h, SWF_ASSETS_VERSION); put16(&h, 64); put32(&h, (uint32_t)off); put32(&h, (uint32_t)nblocks);
    put32(&h, 0); put32(&h, 0); put32(&h, 9600); put32(&h, 5440);
    put16(&h, 30 << 8); put16(&h, 8); put32(&h, 0xFFFFFFFFu); putf(&h, 1.0f);
    memcpy(out + 4, h.d, h.n);
    for (int i = 0; i < nblocks; i++) {
        uint8_t *e = out + 64 + i * 16;
        memcpy(e, blocks[i].tag, 4);
        buf t = {{0}, 0};
        put32(&t, (uint32_t)offsets[i]); put32(&t, (uint32_t)blocks[i].data.n); put32(&t, blocks[i].count);
        memcpy(e + 4, t.d, 12);
        memcpy(out + offsets[i], blocks[i].data.d, blocks[i].data.n);
    }
    return off;
}

static uint8_t *file_copy(size_t *size)
{
    static uint8_t tmp[8192];
    *size = build(tmp, sizeof tmp);
    uint8_t *p = swf_aligned_alloc(*size);
    memcpy(p, tmp, *size);
    return p;
}

static size_t block_offset(const uint8_t *f, const char *tag)
{
    uint32_t n = f[12] | (f[13] << 8);
    for (uint32_t i = 0; i < n; i++)
        if (memcmp(f + 64 + i * 16, tag, 4) == 0) {
            const uint8_t *e = f + 64 + i * 16 + 4;
            return e[0] | (e[1] << 8) | (e[2] << 16) | ((size_t)e[3] << 24);
        }
    return 0;
}

static void test_parse_valid(void)
{
    size_t size;
    uint8_t *f = file_copy(&size);
    swf_assets a;
    CHECK(size > 0);
    CHECK(swf_assets_parse(&a, f, size) == PLAT_OK);
    CHECK(a.shape_count == 1 && a.draw_count == 1 && a.sprite_count == 2);
    CHECK(a.draws[0].vertex_count == 3 && a.draws[0].indices[2] == 2);
    CHECK(a.frame_rate == 30.0f);
    CHECK(swf_assets_char(&a, 1) && swf_assets_char(&a, 1)->kind == SWF_CHAR_SHAPE);
    CHECK(swf_assets_char(&a, 0) == NULL && swf_assets_char(&a, 99) == NULL);
    swf_assets_free(&a);
    swf_aligned_free(f);
}

static void expect_reject(const char *what, void (*corrupt)(uint8_t *f, size_t *size))
{
    size_t size;
    uint8_t *f = file_copy(&size);
    corrupt(f, &size);
    swf_assets a;
    plat_result r = swf_assets_parse(&a, f, size);
    if (r == PLAT_OK) {
        fprintf(stderr, "corruption not detected: %s\n", what);
        failures++;
        swf_assets_free(&a);
    }
    swf_aligned_free(f);
}

static void bad_magic(uint8_t *f, size_t *s) { (void)s; f[0] = 'X'; }
static void bad_version(uint8_t *f, size_t *s) { (void)s; f[4] = 99; }
static void truncated(uint8_t *f, size_t *s) { (void)f; *s -= 64; }
static void index_oob(uint8_t *f, size_t *s)
{
    (void)s;
    size_t d = block_offset(f, "DATA");
    f[d + 48 + 4] = 7; /* third index -> 7 >= 3 vertices */
}
static void char_oob(uint8_t *f, size_t *s)
{
    (void)s;
    f[block_offset(f, "CHAR") + 8 + 4] = 5; /* shape index 5 of 1 */
}
static void cmd_truncated(uint8_t *f, size_t *s)
{
    (void)s;
    size_t fr = block_offset(f, "FRMS");
    f[fr + 4] += 2; /* frame 1 ends inside the next command */
}
static void bad_string(uint8_t *f, size_t *s)
{
    (void)s;
    size_t c = block_offset(f, "CMDS");
    f[c + 1 + 4 + 2 + 24 + 1 + 4 + 2] = 200; /* name offset past the string table */
}
static void block_outside(uint8_t *f, size_t *s)
{
    (void)s;
    f[64 + 1 * 16 + 9] = 0x7F; /* DATA size huge */
}

static void test_parse_rejects_corruption(void)
{
    expect_reject("magic", bad_magic);
    expect_reject("version", bad_version);
    expect_reject("truncated file", truncated);
    expect_reject("vertex index out of range", index_oob);
    expect_reject("character index out of range", char_oob);
    expect_reject("truncated command", cmd_truncated);
    expect_reject("string offset out of range", bad_string);
    expect_reject("block outside file", block_outside);
    swf_assets a;
    CHECK(swf_assets_parse(&a, (const void *)(uintptr_t)8, 100) != PLAT_OK); /* unaligned */
}

static int count_children(const swf_dobj *clip)
{
    int n = 0;
    for (const swf_dobj *c = clip->children; c; c = c->next) n++;
    return n;
}

static void test_timeline(void)
{
    size_t size;
    uint8_t *f = file_copy(&size);
    swf_assets a;
    CHECK(swf_assets_parse(&a, f, size) == PLAT_OK);
    swf_movie m;
    CHECK(swf_movie_init(&m, &a) == PLAT_OK);
    swf_movie_advance(&m); /* frame 1 */
    CHECK(m.root->frame == 1);
    CHECK(count_children(m.root) == 2);
    swf_dobj *shape = swf_clip_child_at(m.root, 1);
    CHECK(shape && shape->kind == SWF_CHAR_SHAPE && shape->matrix.tx == 100.0f);
    swf_dobj *clip = swf_clip_child_named(m.root, "CLIP", false);
    CHECK(clip && clip->kind == SWF_CHAR_SPRITE && clip->frame == 1);
    CHECK(swf_clip_child_named(m.root, "CLIP", true) == NULL);

    swf_movie_advance(&m); /* frame 2 removes the shape; the sprite advances */
    CHECK(m.root->frame == 2 && count_children(m.root) == 1);
    clip = swf_clip_child_at(m.root, 2);
    CHECK(clip && clip->frame == 2);

    swf_movie_advance(&m); /* loops to frame 1: the shape comes back, the sprite persists */
    CHECK(m.root->frame == 1 && count_children(m.root) == 2);
    CHECK(swf_clip_child_at(m.root, 2) == clip && clip->frame == 3);

    swf_clip_stop(m.root);
    swf_movie_advance(&m);
    CHECK(m.root->frame == 1);
    swf_clip_goto(&m, m.root, 2);
    CHECK(m.root->frame == 2 && swf_clip_child_at(m.root, 1) == NULL);
    swf_clip_goto(&m, m.root, 99); /* clamps */
    CHECK(m.root->frame == 2);
    CHECK(swf_clip_total_frames(&m, m.root) == 2);
    swf_movie_free(&m);
    swf_assets_free(&a);
    swf_aligned_free(f);
}

static void test_transforms(void)
{
    swf_matrix p = {2, 0, 0, 2, 10, 20}, c = {1, 0, 0, 1, 5, 5};
    swf_matrix r = swf_matrix_mul(&p, &c);
    CHECK(r.a == 2 && r.tx == 20 && r.ty == 30);
    swf_cxform half = {{0.5f, 0.5f, 0.5f, 1}, {10, 0, 0, 0}};
    swf_cxform both = swf_cxform_mul(&half, &half);
    CHECK(both.mul[0] == 0.25f && both.add[0] == 15.0f);
    uint8_t in[4] = {200, 100, 0, 255}, out[4];
    swf_cxform_apply(&half, in, out);
    CHECK(out[0] == 110 && out[1] == 50 && out[3] == 255);
    swf_cxform sat = {{4, 1, 1, 1}, {0, -300, 0, 0}};
    swf_cxform_apply(&sat, in, out);
    CHECK(out[0] == 255 && out[1] == 0);
}

int main(void)
{
    test_parse_valid();
    test_parse_rejects_corruption();
    test_timeline();
    test_transforms();
    if (failures) {
        fprintf(stderr, "test_runtime: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_runtime: all passed\n");
    return 0;
}
