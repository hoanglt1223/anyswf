/* assets.bin loader (format: docs/dev/assets-bin-format.md, writer: swf2c/src/assets_bin.rs).
 * The loaded file stays in memory: vertex, index, texture and sound data are used in
 * place, so the buffer is 64-byte aligned and must outlive the swf_assets. */
#ifndef SWFRT_ASSETS_H
#define SWFRT_ASSETS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "platform.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SWF_ASSETS_VERSION 1
#define SWF_NONE 0xFFFFFFFFu

enum swf_char_kind {
    SWF_CHAR_NONE = 0,
    SWF_CHAR_SHAPE = 1,
    SWF_CHAR_MORPH = 2,
    SWF_CHAR_SPRITE = 3,
    SWF_CHAR_BUTTON = 4,
    SWF_CHAR_TEXT = 5,
    SWF_CHAR_EDIT_TEXT = 6,
    SWF_CHAR_FONT = 7,
    SWF_CHAR_BITMAP = 8,
    SWF_CHAR_SOUND = 9
};

enum swf_cmd {
    SWF_CMD_PLACE = 1,
    SWF_CMD_REMOVE = 2,
    SWF_CMD_ACTION = 3,
    SWF_CMD_START_SOUND = 4,
    SWF_CMD_STREAM_BLOCK = 5
};

enum swf_place_flag {
    SWF_PLACE_MOVE = 1 << 0,
    SWF_PLACE_CHARACTER = 1 << 1,
    SWF_PLACE_MATRIX = 1 << 2,
    SWF_PLACE_CXFORM = 1 << 3,
    SWF_PLACE_RATIO = 1 << 4,
    SWF_PLACE_NAME = 1 << 5,
    SWF_PLACE_CLIP_DEPTH = 1 << 6,
    SWF_PLACE_CLIP_ACTIONS = 1 << 7,
    SWF_PLACE_BLEND = 1 << 9,
    SWF_PLACE_CACHE = 1 << 10,
    SWF_PLACE_VISIBLE = 1 << 13
};

enum { SWF_DRAW_STROKE = 1 << 0, SWF_DRAW_SMOOTH = 1 << 1 };
enum { SWF_TEX_SWIZZLED = 1 << 0, SWF_TEX_REPEAT = 1 << 1 };
/* Values match GU_PSM_*. */
enum swf_tex_format {
    SWF_TEX_565 = 0,
    SWF_TEX_5551 = 1,
    SWF_TEX_4444 = 2,
    SWF_TEX_8888 = 3,
    SWF_TEX_CLUT8 = 5
};
enum { SWF_SOUND_PCM16 = 1, SWF_SOUND_IMA4 = 2, SWF_SOUND_MP3 = 3 };

#define SWF_SOLID_VERTEX_SIZE 12  /* u8 rgba[4]; s16 x, y, z, pad */
#define SWF_TEX_VERTEX_SIZE 16    /* f32 u, v; s16 x, y, z, pad */

typedef struct swf_char {
    uint8_t kind;
    uint32_t index;
} swf_char;

typedef struct swf_shape {
    int32_t bounds[4]; /* twips: xmin, ymin, xmax, ymax */
    uint32_t first_draw, draw_count;
} swf_shape;

typedef struct swf_draw {
    bool textured;
    uint8_t flags;
    uint8_t shift;      /* position unit is (1 << shift) twips */
    uint32_t texture;   /* SWF_NONE for solid draws */
    const void *vertices;
    uint32_t vertex_count;
    const uint16_t *indices;
    uint32_t index_count;
} swf_draw;

typedef struct swf_texture {
    uint8_t format, flags;
    uint16_t width, height;      /* power-of-two storage size */
    uint16_t used_w, used_h;     /* image size inside it */
    uint16_t clut_count;
    const void *data;
    uint32_t data_size;
    const uint8_t *clut;         /* RGBA8888 entries, NULL unless CLUT8 */
} swf_texture;

typedef struct swf_morph_frame {
    uint16_t ratio;
    uint32_t shape;
} swf_morph_frame;

typedef struct swf_morph {
    uint32_t first_frame, frame_count;
} swf_morph;

typedef struct swf_label {
    const char *name;
    uint32_t frame; /* 0-based */
} swf_label;

typedef struct swf_sprite {
    uint32_t frame_count;
    const uint32_t *frame_offsets; /* frame_count + 1 offsets into cmds */
    const swf_label *labels;
    uint32_t label_count;
    uint32_t stream;               /* sound index or SWF_NONE */
} swf_sprite;

typedef struct swf_button_record {
    uint8_t states, blend;
    uint16_t char_id, depth;
    float matrix[6];
    int16_t cxform[8];
} swf_button_record;

typedef struct swf_button_action {
    uint16_t conditions, key;
    uint32_t script;
} swf_button_action;

typedef struct swf_button {
    const swf_button_record *records;
    uint16_t record_count;
    uint16_t flags;
    const swf_button_action *actions;
    uint32_t action_count;
} swf_button;

typedef struct swf_clip_action {
    uint32_t events;
    uint8_t key_code;
    uint32_t script;
} swf_clip_action;

typedef struct swf_edit_text {
    int32_t bounds[4];
    uint16_t font_id, height;
    uint32_t color; /* RGBA bytes, R lowest */
    uint16_t max_length, flags;
    uint8_t align;
    uint16_t left_margin, right_margin, indent;
    int16_t leading;
    const char *var_name;
    const char *initial_text;
    /* Run-time state (TextField.background/border), initialised from the Border flag. */
    bool background, border;
    uint32_t background_color, border_color; /* RGBA bytes, R lowest */
    uint8_t auto_size;                       /* 0 none, 1 left, 2 center, 3 right */
} swf_edit_text;

typedef struct swf_glyph {
    uint16_t code;
    int16_t advance;
    uint32_t shape; /* SWF_NONE for blank glyphs */
} swf_glyph;

typedef struct swf_font {
    const char *name;
    uint16_t flags, glyph_count;
    const swf_glyph *glyphs;
    uint16_t ascent, descent;
    int16_t leading;
    uint16_t em;
} swf_font;

typedef struct swf_sound {
    uint8_t format, channels;
    uint32_t rate, frames;
    const uint8_t *data;
    uint32_t data_size;
} swf_sound;

typedef struct swf_export {
    uint16_t char_id;
    const char *name;
} swf_export;

typedef struct swf_init_action {
    uint16_t sprite_id;
    uint32_t script;
} swf_init_action;

typedef struct swf_device_font {
    uint32_t texture;
    uint16_t first, count, cell, glyph_h, cols;
    const uint8_t *advances;
} swf_device_font;

typedef struct swf_assets {
    int32_t stage[4];
    float frame_rate;
    uint16_t swf_version;
    uint32_t background; /* RGBA bytes, R lowest */
    float target_scale;  /* stage pixels to screen pixels */
    uint32_t swf_size;   /* uncompressed SWF length */
    uint16_t screen_w, screen_h; /* logical screen the movie is laid out on (480x272 for PSP builds) */

    swf_char *chars;          uint32_t char_count;
    swf_shape *shapes;        uint32_t shape_count;
    swf_draw *draws;          uint32_t draw_count;
    swf_texture *textures;    uint32_t texture_count;
    swf_morph *morphs;        uint32_t morph_count;
    swf_morph_frame *morph_frames; uint32_t morph_frame_count;
    swf_sprite *sprites;      uint32_t sprite_count;
    swf_label *labels;        uint32_t label_count;
    swf_button *buttons;      uint32_t button_count;
    swf_button_record *button_records; uint32_t button_record_count;
    swf_button_action *button_actions; uint32_t button_action_count;
    swf_clip_action *clip_actions;     uint32_t clip_action_count;
    swf_edit_text *edits;     uint32_t edit_count;
    swf_font *fonts;          uint32_t font_count;
    swf_glyph *glyphs;        uint32_t glyph_count;
    swf_sound *sounds;        uint32_t sound_count;
    swf_export *exports;      uint32_t export_count;
    swf_init_action *inits;   uint32_t init_count;
    uint32_t script_count;
    bool has_device_font;
    swf_device_font device_font;

    const uint8_t *cmds;      uint32_t cmds_size;
    const char *strs;         uint32_t strs_size;

    void *file;               /* owned, aligned; NULL when loaded from caller memory */
    size_t file_size;
} swf_assets;

/* Parses and validates an in-memory assets.bin. `data` must be 64-byte aligned and outlive
 * `out`. On failure `out` is left empty and an error is logged. */
plat_result swf_assets_parse(swf_assets *out, const void *data, size_t size);
/* Reads the file into an aligned buffer owned by `out`, then parses it. */
plat_result swf_assets_load(swf_assets *out, plat_root root, const char *rel);
void swf_assets_free(swf_assets *a);

/* Helpers bounds-checked against the tables. NULL / false when out of range. */
const swf_char *swf_assets_char(const swf_assets *a, uint16_t id);
/* String table entry; "" for an out-of-range offset. */
const char *swf_assets_string(const swf_assets *a, uint32_t off);
/* Shape to draw for a morph at `ratio`: the nearest baked ratio. */
uint32_t swf_assets_morph_shape(const swf_assets *a, uint32_t morph, uint16_t ratio);

/* Aligned allocation usable for GE data (64-byte alignment). */
void *swf_aligned_alloc(size_t size);
void swf_aligned_free(void *p);

#ifdef __cplusplus
}
#endif

#endif
