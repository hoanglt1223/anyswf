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
    uint32_t clip_action_first, clip_action_count;

    /* Sprites */
    uint32_t frame;       /* 1-based current frame; 0 before the first frame ran */
    bool playing;
    uint32_t born_tick;   /* clips placed during a tick do not also advance in it */
    swf_dobj *children;

    /* Text fields: current text (owned, UTF-8); NULL shows the initial text. */
    char *text;

    /* Buttons */
    uint8_t button_state; /* SWF button state bit (1 up, 2 over, 4 down) */

    void *script_object;  /* AVM1 object bound to this display object, if any */
};

struct swf_movie;
typedef void (*swf_action_fn)(struct swf_movie *m, swf_dobj *clip, uint32_t script, void *user);
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
} swf_movie;

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

swf_dobj *swf_clip_child_at(swf_dobj *clip, int32_t depth);
swf_dobj *swf_clip_child_named(swf_dobj *clip, const char *name, bool case_sensitive);
/* Creates a display object for a character at a depth (replacing what was there). */
swf_dobj *swf_clip_add_child(swf_movie *m, swf_dobj *clip, uint16_t char_id, int32_t depth);
void swf_clip_remove_child(swf_movie *m, swf_dobj *clip, int32_t depth);

/* Stage-to-screen transform: SWF "show all" scaling, centred. */
swf_matrix swf_movie_stage_matrix(const swf_assets *a);

/* Draws the display tree through the active renderer. */
void swf_movie_render(swf_movie *m);

#ifdef __cplusplus
}
#endif

#endif
