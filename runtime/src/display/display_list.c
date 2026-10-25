#include "display_internal.h"
#include "drawing.h"

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
    if (o->kind == SWF_CHAR_SPRITE) swf_exec_link(m, o);
    return o;
}

void (*swf_dobj_free_hook)(swf_dobj *o) = NULL;

void swf_exec_link(swf_movie *m, swf_dobj *o)
{
    o->exec_owner = m;
    o->exec_prev = NULL;
    o->exec_next = m->exec_head;
    if (m->exec_head) m->exec_head->exec_prev = o;
    m->exec_head = o;
}

static void exec_unlink(swf_dobj *o)
{
    swf_movie *m = o->exec_owner;
    if (!m) return;
    if (o->exec_prev) o->exec_prev->exec_next = o->exec_next;
    else if (m->exec_head == o) m->exec_head = o->exec_next;
    if (o->exec_next) o->exec_next->exec_prev = o->exec_prev;
    o->exec_owner = NULL;
    o->exec_prev = o->exec_next = NULL;
}

static void mark_removed(swf_dobj *o)
{
    o->removed = true;
    for (swf_dobj *c = o->children; c; c = c->next) mark_removed(c);
}

/* Parents report removal before their children, as Flash runs unload handlers. */
static void report_removed(swf_movie *m, swf_dobj *o)
{
    if (o->removed) return;
    if (o->kind == SWF_CHAR_SPRITE) m->on_event(m, o, SWF_EV_REMOVED, m->user);
    for (swf_dobj *c = o->children; c; c = c->next) report_removed(m, c);
}

static void free_now(swf_dobj *o)
{
    swf_dobj *c = o->children;
    while (c) {
        swf_dobj *n = c->next;
        free_now(c);
        c = n;
    }
    exec_unlink(o);
    if (o->script_object && swf_dobj_free_hook) swf_dobj_free_hook(o);
    free(o->text);
    free(o->name_owned);
    if (o->edit) {
        free((char *)o->edit->var_name);
        free((char *)o->edit->initial_text);
        free(o->edit);
    }
    swf_drawing_free(o->drawing);
    free(o);
}

/* While the execution list is being walked, removed objects stay allocated (flagged
 * `removed`) until the walk ends, so the walk never touches freed memory. */
void swf_dobj_free(swf_movie *m, swf_dobj *o)
{
    if (!o) return;
    if (m && m->on_event) report_removed(m, o);
    if (m && (m->deferring || m->hold_removed)) {
        mark_removed(o);
        o->next = m->graveyard;
        m->graveyard = o;
        return;
    }
    free_now(o);
}

void swf_flush_graveyard(swf_movie *m)
{
    swf_dobj *g = m->graveyard;
    m->graveyard = NULL;
    while (g) {
        swf_dobj *n = g->next;
        free_now(g);
        g = n;
    }
}

void swf_movie_flush_removed(swf_movie *m)
{
    if (!m->deferring) swf_flush_graveyard(m);
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
void swf_clip_insert(swf_movie *m, swf_dobj *clip, swf_dobj *o)
{
    swf_dobj **pp = &clip->children;
    while (*pp && (*pp)->depth < o->depth) pp = &(*pp)->next;
    if (*pp && (*pp)->depth == o->depth) {
        swf_dobj *old = *pp;
        o->next = old->next;
        swf_dobj_free(m, old);
    } else {
        o->next = *pp;
    }
    *pp = o;
    o->parent = clip;
}

void swf_clip_remove_child(swf_movie *m, swf_dobj *clip, int32_t depth)
{
    swf_dobj **pp = &clip->children;
    while (*pp && (*pp)->depth < depth) pp = &(*pp)->next;
    if (*pp && (*pp)->depth == depth) {
        swf_dobj *old = *pp;
        *pp = old->next;
        swf_dobj_free(m, old);
    }
}

bool swf_dobj_set_name(swf_dobj *o, const char *utf8)
{
    size_t n = strlen(utf8);
    char *c = malloc(n + 1);
    if (!c) return false;
    memcpy(c, utf8, n + 1);
    free(o->name_owned);
    o->name_owned = c;
    o->name = c;
    return true;
}

bool swf_clip_detach(swf_dobj *clip, swf_dobj *child)
{
    for (swf_dobj **pp = &clip->children; *pp; pp = &(*pp)->next) {
        if (*pp == child) {
            *pp = child->next;
            child->next = NULL;
            return true;
        }
    }
    return false;
}

swf_dobj *swf_dobj_create(swf_movie *m, uint16_t char_id, uint8_t kind, int32_t depth)
{
    if (char_id) return swf_dobj_new(m, char_id, depth);
    if (kind != SWF_CHAR_SPRITE && kind != SWF_CHAR_EDIT_TEXT) return NULL;
    swf_dobj *o = calloc(1, sizeof *o);
    if (!o) {
        plat_log("display: out of memory creating an object");
        return NULL;
    }
    o->depth = depth;
    o->kind = kind;
    o->index = kind == SWF_CHAR_SPRITE ? SWF_EMPTY_SPRITE : 0;
    o->matrix = SWF_MATRIX_IDENTITY;
    o->cxform = SWF_CXFORM_IDENTITY;
    o->visible = true;
    o->name = "";
    o->playing = true;
    o->born_tick = m->tick;
    o->button_state = 1;
    if (kind == SWF_CHAR_SPRITE) swf_exec_link(m, o);
    return o;
}

void swf_clip_place(swf_movie *m, swf_dobj *clip, swf_dobj *o)
{
    swf_clip_insert(m, clip, o);
    if (o->kind == SWF_CHAR_SPRITE) swf_clip_enter_first_frame(m, o);
}

swf_dobj *swf_clip_add_object(swf_movie *m, swf_dobj *clip, uint8_t kind, int32_t depth, swf_edit_text *edit)
{
    swf_dobj *o = swf_dobj_create(m, 0, kind, depth);
    if (!o) return NULL;
    o->edit = edit;
    swf_clip_place(m, clip, o);
    return o;
}

swf_dobj *swf_clip_add_child(swf_movie *m, swf_dobj *clip, uint16_t char_id, int32_t depth)
{
    swf_dobj *o = swf_dobj_new(m, char_id, depth);
    if (!o) return NULL;
    swf_clip_insert(m, clip, o);
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
