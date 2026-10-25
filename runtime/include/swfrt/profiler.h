/* Frame profiler: CPU time per stage, draw and triangle counts, and memory (heap, script
 * heap, texture memory), shown as an on-screen overlay and logged once per second.
 * Renderers add their draw counts; the player brackets the stages. */
#ifndef SWFRT_PROFILER_H
#define SWFRT_PROFILER_H

#include "swfrt/assets.h"

#ifdef __cplusplus
extern "C" {
#endif

enum swf_prof_stage {
    SWF_PROF_SCRIPTS = 0, /* input events, timers, frame scripts */
    SWF_PROF_TIMELINE,    /* display list updates (swf_movie_advance) */
    SWF_PROF_RENDER,      /* display walk and draw submission */
    SWF_PROF_PRESENT,     /* waiting for the GPU and vsync */
    SWF_PROF_STAGES
};

typedef struct swf_profiler {
    bool overlay;         /* draw the overlay */
    bool log;             /* log a summary every second */
    /* counters for the frame being built (renderers add to them) */
    uint32_t draws, tris;
    /* memory, set by their owners each frame */
    size_t script_bytes, texture_used, texture_capacity;
    uint32_t texture_uploads, texture_evictions, texture_misses;
} swf_profiler;

extern swf_profiler swf_prof;

void swf_prof_begin(int stage);
void swf_prof_end(int stage);
/* Closes the frame: folds the counters into the one-second window. */
void swf_prof_frame_done(void);
/* Draws the overlay (device font of `a`) at the top left of the screen. */
void swf_prof_draw(const swf_assets *a);
/* Heap bytes in use, or 0 when the C library cannot tell. */
size_t swf_prof_heap_used(void);

#ifdef __cplusplus
}
#endif

#endif
