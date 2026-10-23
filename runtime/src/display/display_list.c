#include "display_internal.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

swf_dobj *swf_dobj_new(swf_movie *m, uint16_t char_id, int32_t depth)
{
    const swf_char *ch = swf_assets_char(m->assets, char_id);
    if (!ch) {
        swf_warn_once(m, SWF_WARN_MISSING_CHAR, "display: character %u is not defined; placement ignored", char_id);
        return NULL;
    }
    swf_dobj *o = calloc(1, sizeof *o);
    if (!o) {
        plat_log("display: out of memory placing character %u", char_id);
        return NULL;
    }
    o->depth = depth;
    o->char_id = char_id;
    o->kind = ch->kind;
    o->index = ch->index;
    o->matrix = SWF_MATRIX_IDENTITY;
    o->cxform = SWF_CXFORM_IDENTITY;
    o->visible = true;
    o->name = "";
    o->playing = true;
    o->born_tick = m->tick;
    o->button_state = 1;
    return o;
}

void swf_dobj_free(swf_dobj *o)
{
    if (!o) return;
    swf_dobj *c = o->children;
    while (c) {
        swf_dobj *n = c->next;
        swf_dobj_free(c);
        c = n;
    }
    free(o->text);
    free(o);
}

swf_dobj *swf_clip_child_at(swf_dobj *clip, int32_t depth)
{
    for (swf_dobj *c = clip->children; c && c->depth <= depth; c = c->next)
        if (c->depth == depth) return c;
    return NULL;
}

static bool name_eq(const char *a, const char *b, bool cs)
{
    if (cs) return strcmp(a, b) == 0;
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    return *a == *b;
}

swf_dobj *swf_clip_child_named(swf_dobj *clip, const char *name, bool case_sensitive)
{
    for (swf_dobj *c = clip->children; c; c = c->next)
        if (c->name[0] && name_eq(c->name, name, case_sensitive)) return c;
    return NULL;
}

/* Inserts `o` into the depth-sorted child list, replacing any object at that depth. */
void swf_clip_insert(swf_dobj *clip, swf_dobj *o)
{
    swf_dobj **pp = &clip->children;
    while (*pp && (*pp)->depth < o->depth) pp = &(*pp)->next;
    if (*pp && (*pp)->depth == o->depth) {
        swf_dobj *old = *pp;
        o->next = old->next;
        swf_dobj_free(old);
    } else {
        o->next = *pp;
    }
    *pp = o;
    o->parent = clip;
}

void swf_clip_remove_child(swf_movie *m, swf_dobj *clip, int32_t depth)
{
    (void)m;
    swf_dobj **pp = &clip->children;
    while (*pp && (*pp)->depth < depth) pp = &(*pp)->next;
    if (*pp && (*pp)->depth == depth) {
        swf_dobj *old = *pp;
        *pp = old->next;
        swf_dobj_free(old);
    }
}

swf_dobj *swf_clip_add_child(swf_movie *m, swf_dobj *clip, uint16_t char_id, int32_t depth)
{
    swf_dobj *o = swf_dobj_new(m, char_id, depth);
    if (!o) return NULL;
    swf_clip_insert(clip, o);
    if (o->kind == SWF_CHAR_SPRITE) swf_clip_enter_first_frame(m, o);
    return o;
}

void swf_warn_once(swf_movie *m, uint32_t bit, const char *fmt, ...)
{
    if (m->warned & bit) return;
    m->warned |= bit;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    plat_log("%s", buf);
}
