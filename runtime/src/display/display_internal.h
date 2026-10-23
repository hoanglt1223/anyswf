#ifndef SWFRT_DISPLAY_INTERNAL_H
#define SWFRT_DISPLAY_INTERNAL_H

#include "swfrt/movie.h"

#include <stdarg.h>
#include <stdio.h>

enum {
    SWF_WARN_MISSING_CHAR = 1u << 0,
    SWF_WARN_BLEND = 1u << 1,
    SWF_WARN_EDIT_TEXT = 1u << 2,
    SWF_WARN_ACTIONS = 1u << 3,
    SWF_WARN_DEPTH_LIMIT = 1u << 4,
    SWF_WARN_BUTTON_SPRITE = 1u << 5
};

swf_dobj *swf_dobj_new(swf_movie *m, uint16_t char_id, int32_t depth);
void swf_dobj_free(swf_movie *m, swf_dobj *o);
void swf_clip_insert(swf_movie *m, swf_dobj *clip, swf_dobj *o);
void swf_exec_link(swf_movie *m, swf_dobj *o);
void swf_flush_graveyard(swf_movie *m);
void swf_clip_enter_first_frame(swf_movie *m, swf_dobj *clip);
void swf_clip_next_frame(swf_movie *m, swf_dobj *clip);
void swf_render_edit_text(swf_movie *m, const swf_dobj *o, const swf_matrix *wm, const swf_cxform *cx);
void swf_warn_once(swf_movie *m, uint32_t bit, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

#endif
