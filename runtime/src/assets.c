#include "swfrt/assets.h"

#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER)
#include <malloc.h>
#endif

#define HEADER_SIZE 64u

void *swf_aligned_alloc(size_t size)
{
    if (size == 0) size = 1;
#if defined(_MSC_VER)
    return _aligned_malloc(size, 64);
#elif defined(__PSP__)
    return memalign(64, size);
#else
    void *p = NULL;
    return posix_memalign(&p, 64, size) == 0 ? p : NULL;
#endif
}

void swf_aligned_free(void *p)
{
#if defined(_MSC_VER)
    _aligned_free(p);
#else
    free(p);
#endif
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static float rdf(const uint8_t *p)
{
    uint32_t u = rd32(p);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

typedef struct block {
    const uint8_t *p;
    uint32_t size, count;
} block;

typedef struct ctx {
    const uint8_t *file;
    size_t size;
    block strs, data;
    const char *what;
} ctx;

#define FAIL(...)                    \
    do {                             \
        plat_log("assets: " __VA_ARGS__); \
        return false;                \
    } while (0)

static bool find_block(const ctx *c, const char tag[4], block *out)
{
    uint32_t n = rd32(c->file + 12);
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *e = c->file + HEADER_SIZE + i * 16;
        if (memcmp(e, tag, 4) == 0) {
            uint32_t off = rd32(e + 4), size = rd32(e + 8);
            if (off % 64 != 0 || off > c->size || size > c->size - off)
                FAIL("block %.4s lies outside the file", tag);
            out->p = c->file + off;
            out->size = size;
            out->count = rd32(e + 12);
            return true;
        }
    }
    out->p = NULL;
    out->size = 0;
    out->count = 0;
    return true;
}

/* Fixed-size record table: the block size must match count * rec exactly. */
static bool table(const ctx *c, const char tag[4], uint32_t rec, block *out)
{
    if (!find_block(c, tag, out)) return false;
    if (out->p && (uint64_t)out->count * rec != out->size)
        FAIL("block %.4s has %u bytes for %u records of %u", tag, out->size, out->count, rec);
    return true;
}

static bool str_ref(const ctx *c, uint32_t off, const char **out)
{
    if (off >= c->strs.size) FAIL("string offset %u out of range in %s", off, c->what);
    *out = (const char *)c->strs.p + off;
    return true;
}

static bool data_ref(const ctx *c, uint32_t off, uint64_t size, uint32_t align, const uint8_t **out)
{
    if (off % align != 0 || off > c->data.size || size > c->data.size - off)
        FAIL("data range %u+%llu invalid in %s", off, (unsigned long long)size, c->what);
    *out = c->data.p + off;
    return true;
}

static void *alloc_array(uint32_t count, size_t elem)
{
    return calloc(count ? count : 1, elem);
}

static bool tex_size_ok(uint16_t v) { return v >= 1 && v <= 512 && (v & (v - 1)) == 0; }

static bool parse_textures(swf_assets *a, ctx *c)
{
    block b;
    if (!table(c, "TEXR", 32, &b)) return false;
    a->texture_count = b.count;
    a->textures = alloc_array(b.count, sizeof *a->textures);
    if (!a->textures) FAIL("out of memory");
    c->what = "TEXR";
    for (uint32_t i = 0; i < b.count; i++) {
        const uint8_t *r = b.p + i * 32;
        swf_texture *t = &a->textures[i];
        t->format = r[0];
        t->flags = r[1];
        t->width = rd16(r + 2);
        t->height = rd16(r + 4);
        t->used_w = rd16(r + 6);
        t->used_h = rd16(r + 8);
        t->clut_count = rd16(r + 10);
        uint32_t bpp;
        switch (t->format) {
        case SWF_TEX_565: case SWF_TEX_5551: case SWF_TEX_4444: bpp = 16; break;
        case SWF_TEX_8888: bpp = 32; break;
        case SWF_TEX_CLUT8: bpp = 8; break;
        default: FAIL("texture %u has unknown format %u", i, t->format);
        }
        if (!tex_size_ok(t->width) || !tex_size_ok(t->height) || t->used_w == 0 || t->used_h == 0 ||
            t->used_w > t->width || t->used_h > t->height)
            FAIL("texture %u has bad size %ux%u (%ux%u used)", i, t->width, t->height, t->used_w, t->used_h);
        uint64_t need = (uint64_t)t->width * t->height * bpp / 8;
        t->data_size = rd32(r + 16);
        if (t->data_size != need) FAIL("texture %u data is %u bytes, expected %llu", i, t->data_size, (unsigned long long)need);
        if ((t->flags & SWF_TEX_SWIZZLED) && ((t->width * bpp / 8) % 16 != 0 || t->height % 8 != 0))
            FAIL("texture %u cannot be swizzled at %ux%u", i, t->width, t->height);
        const uint8_t *p;
        if (!data_ref(c, rd32(r + 12), t->data_size, 16, &p)) return false;
        t->data = p;
        t->clut = NULL;
        if (t->format == SWF_TEX_CLUT8) {
            if (t->clut_count == 0 || t->clut_count > 256 || t->clut_count % 16 != 0)
                FAIL("texture %u has %u palette entries", i, t->clut_count);
            if (!data_ref(c, rd32(r + 20), (uint64_t)t->clut_count * 4, 16, &p)) return false;
            t->clut = p;
        }
    }
    return true;
}

static bool parse_shapes(swf_assets *a, ctx *c)
{
    block b;
    if (!table(c, "DRAW", 32, &b)) return false;
    a->draw_count = b.count;
    a->draws = alloc_array(b.count, sizeof *a->draws);
    if (!a->draws) FAIL("out of memory");
    c->what = "DRAW";
    for (uint32_t i = 0; i < b.count; i++) {
        const uint8_t *r = b.p + i * 32;
        swf_draw *d = &a->draws[i];
        d->textured = r[0] != 0;
        d->flags = r[1];
        d->shift = r[2];
        d->texture = rd32(r + 4);
        d->vertex_count = rd32(r + 12);
        d->index_count = rd32(r + 20);
        if (r[0] > 1 || d->shift > 16) FAIL("draw %u has bad kind or shift", i);
        if (d->textured ? d->texture >= a->texture_count : d->texture != SWF_NONE)
            FAIL("draw %u has bad texture %u", i, d->texture);
        if (d->vertex_count == 0 || d->vertex_count > 65535 || d->index_count == 0 ||
            d->index_count > 65535 || d->index_count % 3 != 0)
            FAIL("draw %u has %u vertices / %u indices", i, d->vertex_count, d->index_count);
        const uint8_t *p;
        uint32_t vsize = d->textured ? SWF_TEX_VERTEX_SIZE : SWF_SOLID_VERTEX_SIZE;
        if (!data_ref(c, rd32(r + 8), (uint64_t)d->vertex_count * vsize, 16, &p)) return false;
        d->vertices = p;
        if (!data_ref(c, rd32(r + 16), (uint64_t)d->index_count * 2, 16, &p)) return false;
        d->indices = (const uint16_t *)p;
        for (uint32_t k = 0; k < d->index_count; k++)
            if (rd16(p + k * 2) >= d->vertex_count) FAIL("draw %u index %u out of range", i, k);
    }
    if (!table(c, "SHAP", 24, &b)) return false;
    a->shape_count = b.count;
    a->shapes = alloc_array(b.count, sizeof *a->shapes);
    if (!a->shapes) FAIL("out of memory");
    for (uint32_t i = 0; i < b.count; i++) {
        const uint8_t *r = b.p + i * 24;
        swf_shape *s = &a->shapes[i];
        for (int k = 0; k < 4; k++) s->bounds[k] = (int32_t)rd32(r + k * 4);
        s->first_draw = rd32(r + 16);
        s->draw_count = rd32(r + 20);
        if (s->first_draw > a->draw_count || s->draw_count > a->draw_count - s->first_draw)
            FAIL("shape %u draw range out of bounds", i);
    }
    if (!table(c, "MRPF", 8, &b)) return false;
    a->morph_frame_count = b.count;
    a->morph_frames = alloc_array(b.count, sizeof *a->morph_frames);
    if (!a->morph_frames) FAIL("out of memory");
    for (uint32_t i = 0; i < b.count; i++) {
        const uint8_t *r = b.p + i * 8;
        a->morph_frames[i].ratio = rd16(r);
        a->morph_frames[i].shape = rd32(r + 4);
        if (a->morph_frames[i].shape >= a->shape_count) FAIL("morph frame %u has bad shape", i);
    }
    if (!table(c, "MRPH", 8, &b)) return false;
    a->morph_count = b.count;
    a->morphs = alloc_array(b.count, sizeof *a->morphs);
    if (!a->morphs) FAIL("out of memory");
    for (uint32_t i = 0; i < b.count; i++) {
        swf_morph *m = &a->morphs[i];
        m->first_frame = rd32(b.p + i * 8);
        m->frame_count = rd32(b.p + i * 8 + 4);
        if (m->frame_count == 0 || m->first_frame > a->morph_frame_count ||
            m->frame_count > a->morph_frame_count - m->first_frame)
            FAIL("morph %u frame range out of bounds", i);
    }
    return true;
}

static bool script_ok(const swf_assets *a, uint32_t s) { return s == SWF_NONE || s < a->script_count; }

/* Walks one frame's commands, checking every field fits and every reference resolves. */
static bool check_commands(const swf_assets *a, ctx *c, uint32_t from, uint32_t to)
{
    const uint8_t *p = a->cmds;
    uint32_t i = from;
#define NEED(n) do { if (to - i < (uint32_t)(n)) FAIL("truncated command at %u", i); } while (0)
    while (i < to) {
        uint8_t op = p[i++];
        switch (op) {
        case SWF_CMD_PLACE: {
            NEED(4);
            uint16_t f = rd16(p + i);
            i += 4;
            if (f & SWF_PLACE_CHARACTER) { NEED(2); i += 2; }
            if (f & SWF_PLACE_MATRIX) { NEED(24); i += 24; }
            if (f & SWF_PLACE_CXFORM) { NEED(16); i += 16; }
            if (f & SWF_PLACE_RATIO) { NEED(2); i += 2; }
            if (f & SWF_PLACE_NAME) {
                NEED(4);
                const char *s;
                if (!str_ref(c, rd32(p + i), &s)) return false;
                i += 4;
            }
            if (f & SWF_PLACE_CLIP_DEPTH) { NEED(2); i += 2; }
            if (f & SWF_PLACE_CLIP_ACTIONS) {
                NEED(6);
                uint32_t first = rd32(p + i), n = rd16(p + i + 4);
                if (first > a->clip_action_count || n > a->clip_action_count - first)
                    FAIL("clip action range out of bounds at %u", i);
                i += 6;
            }
            if (f & SWF_PLACE_BLEND) { NEED(1); i += 1; }
            if (f & SWF_PLACE_CACHE) { NEED(1); i += 1; }
            if (f & SWF_PLACE_VISIBLE) { NEED(1); i += 1; }
            break;
        }
        case SWF_CMD_REMOVE: NEED(2); i += 2; break;
        case SWF_CMD_ACTION:
            NEED(4);
            if (rd32(p + i) >= a->script_count) FAIL("action script out of range at %u", i);
            i += 4;
            break;
        case SWF_CMD_START_SOUND: NEED(16); i += 16; break;
        case SWF_CMD_STREAM_BLOCK: NEED(4); i += 4; break;
        default: FAIL("unknown command %u at %u", op, i - 1);
        }
    }
#undef NEED
    return true;
}

static bool parse_timelines(swf_assets *a, ctx *c)
{
    block cmds, frames, b;
    if (!find_block(c, "CMDS", &cmds) || !table(c, "FRMS", 4, &frames)) return false;
    a->cmds = cmds.p;
    a->cmds_size = cmds.size;
    if (!table(c, "LABL", 8, &b)) return false;
    a->label_count = b.count;
    a->labels = alloc_array(b.count, sizeof *a->labels);
    if (!a->labels) FAIL("out of memory");
    c->what = "LABL";
    for (uint32_t i = 0; i < b.count; i++) {
        if (!str_ref(c, rd32(b.p + i * 8), &a->labels[i].name)) return false;
        a->labels[i].frame = rd32(b.p + i * 8 + 4);
    }
    if (!table(c, "SPRT", 24, &b)) return false;
    if (b.count == 0) FAIL("no root timeline");
    a->sprite_count = b.count;
    a->sprites = alloc_array(b.count, sizeof *a->sprites);
    if (!a->sprites) FAIL("out of memory");
    c->what = "CMDS";
    for (uint32_t i = 0; i < b.count; i++) {
        const uint8_t *r = b.p + i * 24;
        swf_sprite *s = &a->sprites[i];
        s->frame_count = rd32(r);
        uint32_t ft = rd32(r + 4), lf = rd32(r + 8);
        s->label_count = rd32(r + 12);
        s->stream = rd32(r + 16);
        if (s->frame_count == 0 || ft > frames.count || s->frame_count >= frames.count - ft)
            FAIL("sprite %u frame table out of bounds", i);
        if (lf > a->label_count || s->label_count > a->label_count - lf)
            FAIL("sprite %u labels out of bounds", i);
        if (s->stream != SWF_NONE && s->stream >= a->sound_count) FAIL("sprite %u stream out of range", i);
        /* FRMS is a u32 table in a 64-byte aligned block, so in-place use is aligned. */
        s->frame_offsets = (const uint32_t *)(frames.p + (size_t)ft * 4);
        s->labels = a->labels + lf;
        for (uint32_t f = 0; f < s->frame_count; f++) {
            uint32_t from = rd32(frames.p + (ft + f) * 4), to = rd32(frames.p + (ft + f + 1) * 4);
            if (from > to || to > a->cmds_size) FAIL("sprite %u frame %u offsets invalid", i, f);
            if (!check_commands(a, c, from, to)) return false;
        }
    }
    return true;
}

static bool parse_misc(swf_assets *a, ctx *c)
{
    block b;
    if (!table(c, "SCRP", 8, &b)) return false;
    a->script_count = b.count;

    if (!table(c, "CLAC", 12, &b)) return false;
    a->clip_action_count = b.count;
    a->clip_actions = alloc_array(b.count, sizeof *a->clip_actions);
    if (!a->clip_actions) FAIL("out of memory");
    for (uint32_t i = 0; i < b.count; i++) {
        const uint8_t *r = b.p + i * 12;
        a->clip_actions[i].events = rd32(r);
        a->clip_actions[i].key_code = r[4];
        a->clip_actions[i].script = rd32(r + 8);
        if (!script_ok(a, a->clip_actions[i].script)) FAIL("clip action %u script out of range", i);
    }

    if (!table(c, "SOND", 24, &b)) return false;
    a->sound_count = b.count;
    a->sounds = alloc_array(b.count, sizeof *a->sounds);
    if (!a->sounds) FAIL("out of memory");
    c->what = "SOND";
    for (uint32_t i = 0; i < b.count; i++) {
        const uint8_t *r = b.p + i * 24;
        swf_sound *s = &a->sounds[i];
        s->format = r[0];
        s->channels = r[1];
        s->rate = rd32(r + 4);
        s->frames = rd32(r + 8);
        s->data_size = rd32(r + 16);
        if (s->format < SWF_SOUND_PCM16 || s->format > SWF_SOUND_MP3 || s->channels < 1 ||
            s->channels > 2 || s->rate == 0 || s->rate > 48000)
            FAIL("sound %u has bad format", i);
        if (!data_ref(c, rd32(r + 12), s->data_size, 16, &s->data)) return false;
    }

    if (!table(c, "BTRC", 48, &b)) return false;
    a->button_record_count = b.count;
    a->button_records = alloc_array(b.count, sizeof *a->button_records);
    if (!a->button_records) FAIL("out of memory");
    for (uint32_t i = 0; i < b.count; i++) {
        const uint8_t *r = b.p + i * 48;
        swf_button_record *br = &a->button_records[i];
        br->states = r[0];
        br->blend = r[1];
        br->char_id = rd16(r + 2);
        br->depth = rd16(r + 4);
        for (int k = 0; k < 6; k++) br->matrix[k] = rdf(r + 8 + k * 4);
        for (int k = 0; k < 8; k++) br->cxform[k] = (int16_t)rd16(r + 32 + k * 2);
    }
    if (!table(c, "BTAC", 8, &b)) return false;
    a->button_action_count = b.count;
    a->button_actions = alloc_array(b.count, sizeof *a->button_actions);
    if (!a->button_actions) FAIL("out of memory");
    for (uint32_t i = 0; i < b.count; i++) {
        const uint8_t *r = b.p + i * 8;
        a->button_actions[i].conditions = rd16(r);
        a->button_actions[i].key = rd16(r + 2);
        a->button_actions[i].script = rd32(r + 4);
        if (!script_ok(a, a->button_actions[i].script)) FAIL("button action %u script out of range", i);
    }
    if (!table(c, "BTTN", 16, &b)) return false;
    a->button_count = b.count;
    a->buttons = alloc_array(b.count, sizeof *a->buttons);
    if (!a->buttons) FAIL("out of memory");
    for (uint32_t i = 0; i < b.count; i++) {
        const uint8_t *r = b.p + i * 16;
        uint32_t fr = rd32(r), fa = rd32(r + 8), na = rd32(r + 12);
        uint16_t nr = rd16(r + 4);
        if (fr > a->button_record_count || nr > a->button_record_count - fr ||
            fa > a->button_action_count || na > a->button_action_count - fa)
            FAIL("button %u ranges out of bounds", i);
        a->buttons[i] = (swf_button){a->button_records + fr, nr, rd16(r + 6), a->button_actions + fa, na};
    }

    if (!table(c, "EDIT", 48, &b)) return false;
    a->edit_count = b.count;
    a->edits = alloc_array(b.count, sizeof *a->edits);
    if (!a->edits) FAIL("out of memory");
    c->what = "EDIT";
    for (uint32_t i = 0; i < b.count; i++) {
        const uint8_t *r = b.p + i * 48;
        swf_edit_text *e = &a->edits[i];
        for (int k = 0; k < 4; k++) e->bounds[k] = (int32_t)rd32(r + k * 4);
        e->font_id = rd16(r + 16);
        e->height = rd16(r + 18);
        e->color = rd32(r + 20);
        e->max_length = rd16(r + 24);
        e->flags = rd16(r + 26);
        e->align = r[28];
        e->left_margin = rd16(r + 30);
        e->right_margin = rd16(r + 32);
        e->indent = rd16(r + 34);
        e->leading = (int16_t)rd16(r + 36);
        if (!str_ref(c, rd32(r + 40), &e->var_name) || !str_ref(c, rd32(r + 44), &e->initial_text))
            return false;
    }

    if (!table(c, "GLYF", 12, &b)) return false;
    a->glyph_count = b.count;
    a->glyphs = alloc_array(b.count, sizeof *a->glyphs);
    if (!a->glyphs) FAIL("out of memory");
    for (uint32_t i = 0; i < b.count; i++) {
        const uint8_t *r = b.p + i * 12;
        a->glyphs[i].code = rd16(r);
        a->glyphs[i].advance = (int16_t)rd16(r + 2);
        a->glyphs[i].shape = rd32(r + 4);
        if (a->glyphs[i].shape != SWF_NONE && a->glyphs[i].shape >= a->shape_count)
            FAIL("glyph %u shape out of range", i);
    }
    if (!table(c, "FONT", 24, &b)) return false;
    a->font_count = b.count;
    a->fonts = alloc_array(b.count, sizeof *a->fonts);
    if (!a->fonts) FAIL("out of memory");
    c->what = "FONT";
    for (uint32_t i = 0; i < b.count; i++) {
        const uint8_t *r = b.p + i * 24;
        swf_font *f = &a->fonts[i];
        if (!str_ref(c, rd32(r), &f->name)) return false;
        f->flags = rd16(r + 4);
        f->glyph_count = rd16(r + 6);
        uint32_t fg = rd32(r + 8);
        if (fg > a->glyph_count || f->glyph_count > a->glyph_count - fg) FAIL("font %u glyphs out of range", i);
        f->glyphs = a->glyphs + fg;
        f->ascent = rd16(r + 12);
        f->descent = rd16(r + 14);
        f->leading = (int16_t)rd16(r + 16);
        f->em = rd16(r + 18);
        if (f->em == 0) FAIL("font %u has zero em size", i);
    }

    if (!table(c, "EXPT", 8, &b)) return false;
    a->export_count = b.count;
    a->exports = alloc_array(b.count, sizeof *a->exports);
    if (!a->exports) FAIL("out of memory");
    c->what = "EXPT";
    for (uint32_t i = 0; i < b.count; i++) {
        a->exports[i].char_id = rd16(b.p + i * 8);
        if (!str_ref(c, rd32(b.p + i * 8 + 4), &a->exports[i].name)) return false;
    }
    if (!table(c, "INIT", 8, &b)) return false;
    a->init_count = b.count;
    a->inits = alloc_array(b.count, sizeof *a->inits);
    if (!a->inits) FAIL("out of memory");
    for (uint32_t i = 0; i < b.count; i++) {
        a->inits[i].sprite_id = rd16(b.p + i * 8);
        a->inits[i].script = rd32(b.p + i * 8 + 4);
        if (!script_ok(a, a->inits[i].script)) FAIL("init action %u script out of range", i);
    }

    if (!find_block(c, "DFNT", &b)) return false;
    a->has_device_font = b.p != NULL;
    if (b.p) {
        if (b.size < 16) FAIL("device font block too small");
        swf_device_font *d = &a->device_font;
        d->texture = rd32(b.p);
        d->first = rd16(b.p + 4);
        d->count = rd16(b.p + 6);
        d->cell = rd16(b.p + 8);
        d->glyph_h = rd16(b.p + 10);
        d->cols = rd16(b.p + 12);
        d->advances = b.p + 16;
        if (d->texture >= a->texture_count || d->cols == 0 || d->cell == 0 || b.size - 16 < d->count)
            FAIL("device font record invalid");
    }
    return true;
}

static bool parse_chars(swf_assets *a, ctx *c)
{
    block b;
    if (!table(c, "CHAR", 8, &b)) return false;
    a->char_count = b.count;
    a->chars = alloc_array(b.count, sizeof *a->chars);
    if (!a->chars) FAIL("out of memory");
    for (uint32_t i = 0; i < b.count; i++) {
        swf_char *ch = &a->chars[i];
        ch->kind = b.p[i * 8];
        ch->index = rd32(b.p + i * 8 + 4);
        uint32_t limit;
        switch (ch->kind) {
        case SWF_CHAR_NONE: continue;
        case SWF_CHAR_SHAPE: case SWF_CHAR_TEXT: limit = a->shape_count; break;
        case SWF_CHAR_MORPH: limit = a->morph_count; break;
        case SWF_CHAR_SPRITE: limit = a->sprite_count; break;
        case SWF_CHAR_BUTTON: limit = a->button_count; break;
        case SWF_CHAR_EDIT_TEXT: limit = a->edit_count; break;
        case SWF_CHAR_FONT: limit = a->font_count; break;
        case SWF_CHAR_BITMAP: limit = a->texture_count; break;
        case SWF_CHAR_SOUND: limit = a->sound_count; break;
        default: FAIL("character %u has unknown kind %u", i, ch->kind);
        }
        if (ch->index >= limit) FAIL("character %u index %u out of range", i, ch->index);
    }
    return true;
}

plat_result swf_assets_parse(swf_assets *out, const void *data, size_t size)
{
    memset(out, 0, sizeof *out);
    const uint8_t *f = data;
    if (!f || ((uintptr_t)f % 64) != 0) {
        plat_log("assets: buffer must be 64-byte aligned");
        return PLAT_ERR_INVALID;
    }
    if (size < HEADER_SIZE || memcmp(f, "ASWF", 4) != 0) {
        plat_log("assets: not an assets.bin file");
        return PLAT_ERR_INVALID;
    }
    if (rd16(f + 4) != SWF_ASSETS_VERSION || rd16(f + 6) != HEADER_SIZE) {
        plat_log("assets: version %u not supported (runtime expects %u); rebuild with this swf2c",
                 rd16(f + 4), SWF_ASSETS_VERSION);
        return PLAT_ERR_INVALID;
    }
    uint32_t blocks = rd32(f + 12);
    if (rd32(f + 8) != size || blocks > 256 || HEADER_SIZE + (uint64_t)blocks * 16 > size) {
        plat_log("assets: header sizes do not match the file (truncated?)");
        return PLAT_ERR_INVALID;
    }
    ctx c = {f, size, {0}, {0}, ""};
    for (int k = 0; k < 4; k++) out->stage[k] = (int32_t)rd32(f + 16 + k * 4);
    out->frame_rate = rd16(f + 32) / 256.0f;
    out->swf_version = rd16(f + 34);
    out->background = rd32(f + 36);
    out->target_scale = rdf(f + 40);
    if (!(out->target_scale > 0.0f && out->target_scale < 1000.0f) || out->stage[2] <= out->stage[0] ||
        out->stage[3] <= out->stage[1]) {
        plat_log("assets: bad stage geometry");
        return PLAT_ERR_INVALID;
    }
    if (out->frame_rate <= 0.0f) out->frame_rate = 12.0f;

    bool ok = find_block(&c, "STRS", &c.strs) && find_block(&c, "DATA", &c.data);
    if (ok && (c.strs.size == 0 || c.strs.p[c.strs.size - 1] != 0 || c.strs.p[0] != 0)) {
        plat_log("assets: string table is not NUL-terminated");
        ok = false;
    }
    out->strs = (const char *)c.strs.p;
    out->strs_size = c.strs.size;
    ok = ok && parse_textures(out, &c) && parse_shapes(out, &c) && parse_misc(out, &c) &&
         parse_timelines(out, &c) && parse_chars(out, &c);
    if (!ok) {
        swf_assets_free(out);
        return PLAT_ERR_INVALID;
    }
    return PLAT_OK;
}

plat_result swf_assets_load(swf_assets *out, plat_root root, const char *rel)
{
    memset(out, 0, sizeof *out);
    plat_file *fp;
    plat_result r = plat_file_open(root, rel, PLAT_OPEN_READ, &fp);
    if (r != PLAT_OK) {
        plat_log("assets: cannot open %s (%d)", rel, (int)r);
        return r;
    }
    int64_t size = plat_file_size(fp);
    if (size < (int64_t)HEADER_SIZE || size > 256 * 1024 * 1024) {
        plat_file_close(fp);
        plat_log("assets: %s has unusable size %lld", rel, (long long)size);
        return PLAT_ERR_INVALID;
    }
    void *buf = swf_aligned_alloc((size_t)size);
    if (!buf) {
        plat_file_close(fp);
        return PLAT_ERR_NO_MEMORY;
    }
    int64_t got = plat_file_read(fp, buf, (size_t)size);
    plat_file_close(fp);
    if (got != size) {
        swf_aligned_free(buf);
        plat_log("assets: short read on %s", rel);
        return PLAT_ERR_IO;
    }
    r = swf_assets_parse(out, buf, (size_t)size);
    if (r != PLAT_OK) {
        swf_aligned_free(buf);
        return r;
    }
    out->file = buf;
    out->file_size = (size_t)size;
    return PLAT_OK;
}

void swf_assets_free(swf_assets *a)
{
    free(a->chars);
    free(a->shapes);
    free(a->draws);
    free(a->textures);
    free(a->morphs);
    free(a->morph_frames);
    free(a->sprites);
    free(a->labels);
    free(a->buttons);
    free(a->button_records);
    free(a->button_actions);
    free(a->clip_actions);
    free(a->edits);
    free(a->fonts);
    free(a->glyphs);
    free(a->sounds);
    free(a->exports);
    free(a->inits);
    if (a->file) swf_aligned_free(a->file);
    memset(a, 0, sizeof *a);
}

const char *swf_assets_string(const swf_assets *a, uint32_t off)
{
    return off < a->strs_size ? a->strs + off : "";
}

const swf_char *swf_assets_char(const swf_assets *a, uint16_t id)
{
    if (id >= a->char_count || a->chars[id].kind == SWF_CHAR_NONE) return NULL;
    return &a->chars[id];
}

uint32_t swf_assets_morph_shape(const swf_assets *a, uint32_t morph, uint16_t ratio)
{
    const swf_morph *m = &a->morphs[morph];
    const swf_morph_frame *f = a->morph_frames + m->first_frame;
    uint32_t best = 0;
    int best_d = 1 << 30;
    for (uint32_t i = 0; i < m->frame_count; i++) {
        int d = abs((int)f[i].ratio - (int)ratio);
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return f[best].shape;
}
