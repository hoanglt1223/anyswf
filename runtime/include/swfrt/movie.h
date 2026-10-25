/* Display list and timeline playback. A movie is a tree of display objects; sprites
 * (MovieClips) own child lists sorted by depth and play their timeline at the SWF frame
 * rate, independent of the 60 Hz display. */
#ifndef SWFRT_MOVIE_H
#define SWFRT_MOVIE_H

#include "swfrt/assets.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 2D affine transform in twips: x' = a*x + c*y + tx, y' = b*x + d*y + ty. */
typedef struct swf_matrix {
    float a, b, c, d, tx, ty;
} swf_matrix;

/* Flash colour transform: out = clamp(in * mul + add), add in 0..255 units. */
typedef struct swf_cxform {
    float mul[4];
    float add[4];
} swf_cxform;

extern const swf_matrix SWF_MATRIX_IDENTITY;
extern const swf_cxform SWF_CXFORM_IDENTITY;

swf_matrix swf_matrix_mul(const swf_matrix *parent, const swf_matrix *child);
swf_cxform swf_cxform_mul(const swf_cxform *parent, const swf_cxform *child);
bool swf_cxform_is_identity_mul(const swf_cxform *c);
bool swf_cxform_has_add(const swf_cxform *c);
/* Applies the transform to an RGBA colour (bytes, R first). */
void swf_cxform_apply(const swf_cxform *c, const uint8_t in[4], uint8_t out[4]);

enum swf_blend {
    SWF_BLEND_NORMAL = 0,
    SWF_BLEND_LAYER = 2,
    SWF_BLEND_MULTIPLY = 3,
    SWF_BLEND_SCREEN = 4,
    SWF_BLEND_ADD = 8
};

typedef struct swf_dobj swf_dobj;
typedef struct swf_drawing swf_drawing; /* MovieClip drawing API content (display/drawing.h) */

/* Sprite index of clips without a timeline (createEmptyMovieClip). */
#define SWF_EMPTY_SPRITE SWF_NONE

struct swf_dobj {
    swf_dobj *next;       /* sibling with the next higher depth */
    swf_dobj *parent;
    int32_t depth;
    uint16_t char_id;
    uint8_t kind;         /* swf_char_kind */
    uint32_t index;       /* into the kind's asset table */
    swf_matrix matrix;
    swf_cxform cxform;
    uint16_t ratio;
    uint16_t clip_depth;  /* non-zero: this object masks depths up to clip_depth */
    uint8_t blend;
    bool visible;
    bool from_timeline;
    uint32_t place_frame; /* parent frame whose PlaceObject created this instance */
    const char *name;
    char *name_owned;     /* set when script named or renamed the object; `name` points here */
    uint32_t clip_action_first, clip_action_count;

    /* Sprites */
    uint32_t frame;       /* 1-based current frame; 0 before the first frame ran */
    bool playing;
    uint32_t born_tick;   /* clips placed during a tick do not also advance in it */
    swf_dobj *children;
    /* AVM1 execution list: sprites newest first (Ruffle `clip_exec_list`). */
    swf_dobj *exec_next, *exec_prev;
    struct swf_movie *exec_owner;
    bool removed;

    /* Text fields: current text (owned, UTF-8); NULL shows the initial text. */
    char *text;
    /* Per-instance text record (owned): script-created fields and fields whose format
     * was changed by script. NULL uses the asset's record. */
    swf_edit_text *edit;
    bool var_bound;       /* the text field's variable was initialised from its text */

    /* Sprites: vector content drawn by script, below the children (owned). */
    swf_drawing *drawing;

    /* Buttons */
    uint8_t button_state; /* SWF button state bit (1 up, 2 over, 4 down) */

    void *script_object;  /* AVM1 object bound to this display object, if any */
};

/* Display events reported to the script VM. */
enum swf_display_event {
    SWF_EV_PLACED = 1,      /* a sprite was created; its first frame is about to be built */
    SWF_EV_ENTER_FRAME = 2, /* a sprite starts a new SWF frame (before its timeline steps) */
    SWF_EV_REMOVED = 3      /* a sprite left the display list (once per sprite, children too) */
};

/* Stage scale modes (Stage.scaleMode); "show all" is the Flash default. */
enum swf_scale_mode { SWF_SCALE_SHOW_ALL = 0, SWF_SCALE_EXACT_FIT, SWF_SCALE_NO_BORDER, SWF_SCALE_NO_SCALE };

struct swf_movie;
typedef void (*swf_event_fn)(struct swf_movie *m, swf_dobj *clip, int event, void *user);
typedef void (*swf_action_fn)(struct swf_movie *m, swf_dobj *clip, uint32_t script, void *user);
/* `cmd` points at a START_SOUND or STREAM_BLOCK command, opcode first. */
typedef void (*swf_sound_fn)(struct swf_movie *m, swf_dobj *clip, const uint8_t *cmd, void *user);

typedef struct swf_movie {
    const swf_assets *assets;
    swf_dobj *root;
    uint32_t tick;
    double accum;         /* seconds not yet turned into frames */
    swf_action_fn on_action;
    swf_sound_fn on_sound;
    void *user;
    uint32_t warned;      /* bit set of one-time warnings already logged */
    swf_dobj *exec_head;
    bool deferring;       /* frees are postponed while the execution list is walked */
    swf_dobj *graveyard;
    /* Flash Player warnings that appear in trace output ("Warning: ..."). */
    void (*on_warning)(struct swf_movie *m, const char *msg, void *user);
    swf_event_fn on_event;
    /* Removed objects stay allocated (flagged `removed`) until swf_movie_flush_removed,
     * so queued unload handlers can still run on them. */
    bool hold_removed;
    uint8_t scale_mode;   /* swf_scale_mode */
} swf_movie;

/* Called when a display object with a script object is destroyed (set by the VM). */
extern void (*swf_dobj_free_hook)(swf_dobj *o);

plat_result swf_movie_init(swf_movie *m, const swf_assets *a);
void swf_movie_free(swf_movie *m);

/* Runs one SWF frame: constructs frame 1 on the first call, then advances every playing
 * clip by one frame. */
void swf_movie_advance(swf_movie *m);
/* Accumulates real time and runs the frames that are due (at most `max_frames`). */
int swf_movie_update(swf_movie *m, double dt_seconds, int max_frames);

void swf_clip_goto(swf_movie *m, swf_dobj *clip, uint32_t frame);
int64_t swf_clip_find_label(const swf_movie *m, const swf_dobj *clip, const char *label);
void swf_clip_play(swf_dobj *clip);
void swf_clip_stop(swf_dobj *clip);
uint32_t swf_clip_total_frames(const swf_movie *m, const swf_dobj *clip);
/* Timeline of a sprite (an empty one-frame timeline for SWF_EMPTY_SPRITE). */
const swf_sprite *swf_clip_sprite(const swf_movie *m, const swf_dobj *clip);
/* Text record of an edit-text object (its own copy, else the asset's). */
const swf_edit_text *swf_dobj_edit(const swf_movie *m, const swf_dobj *o);

swf_dobj *swf_clip_child_at(swf_dobj *clip, int32_t depth);
swf_dobj *swf_clip_child_named(swf_dobj *clip, const char *name, bool case_sensitive);
/* Creates a display object for a character at a depth (replacing what was there). */
swf_dobj *swf_clip_add_child(swf_movie *m, swf_dobj *clip, uint16_t char_id, int32_t depth);
void swf_clip_remove_child(swf_movie *m, swf_dobj *clip, int32_t depth);
/* Creates an object without a character: an empty sprite (SWF_CHAR_SPRITE) or a text
 * field (SWF_CHAR_EDIT_TEXT, taking ownership of `edit`). Replaces what was at the depth. */
swf_dobj *swf_clip_add_object(swf_movie *m, swf_dobj *clip, uint8_t kind, int32_t depth, swf_edit_text *edit);
/* Two-step creation for script-made objects: create (char_id 0 makes an empty sprite or a
 * text field of `kind`), adjust fields, then place (inserts and builds the first frame). */
swf_dobj *swf_dobj_create(swf_movie *m, uint16_t char_id, uint8_t kind, int32_t depth);
void swf_clip_place(swf_movie *m, swf_dobj *clip, swf_dobj *o);
/* Unlinks a child without freeing it (to move it to another depth). */
bool swf_clip_detach(swf_dobj *clip, swf_dobj *child);
/* Gives an object a script-assigned instance name (copied). False when out of memory. */
bool swf_dobj_set_name(swf_dobj *o, const char *utf8);
/* Frees objects held back by hold_removed. */
void swf_movie_flush_removed(swf_movie *m);

/* Stage-to-screen transform: SWF "show all" scaling, centred. */
swf_matrix swf_movie_stage_matrix(const swf_assets *a);
/* Stage-to-screen transform for the movie's scale mode. */
swf_matrix swf_movie_view_matrix(const swf_movie *m);
/* World transform (root twips) of a display object, including its own matrix. */
swf_matrix swf_dobj_world_matrix(const swf_dobj *o);
bool swf_matrix_invert(const swf_matrix *m, swf_matrix *out);

/* Extent of a text field's laid-out text in twips (TextField.textWidth/textHeight). */
bool swf_edit_text_measure(swf_movie *m, const swf_dobj *o, float *w, float *h);

/* Bounds (twips) of an object's content transformed by `m` (pass the object's own matrix
 * for parent space). False when it has no content. */
bool swf_dobj_bounds(const swf_movie *m, const swf_dobj *o, const swf_matrix *mat, float out[4]);
/* Hit test of a point in root twips. `shape` tests the filled geometry, otherwise the
 * bounding box. Invisible objects still hit (callers filter). */
bool swf_dobj_hit(const swf_movie *m, const swf_dobj *o, float x, float y, bool shape);

/* Draws the display tree through the active renderer. */
void swf_movie_render(swf_movie *m);

#ifdef __cplusplus
}
#endif

#endif
