/* Dynamic text fields: simple layout of plain (or tag-stripped HTML) text with embedded
 * font outlines, or the bundled bitmap font for device text. */
#include "display_internal.h"

#include "../render/render.h"

#include <string.h>

#define GUTTER 40 /* Flash keeps 2 px between the field border and its text */
#define MAX_TEXT 4096

enum { ET_WORD_WRAP = 1 << 6, ET_MULTILINE = 1 << 5, ET_HTML = 1 << 9, ET_BORDER = 1 << 11, ET_USE_OUTLINES = 1 << 8 };

static uint32_t next_cp(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    uint32_t c = *p++;
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (extra) c &= 0x3F >> extra;
    while (extra-- > 0 && (*p & 0xC0) == 0x80) c = (c << 6) | (*p++ & 0x3F);
    *s = (const char *)p;
    return c;
}

/* Strips tags, turns <br> and </p> into line breaks and decodes the basic entities. */
void swf_html_to_text(const char *in, char *out, size_t cap)
{
    size_t n = 0;
    while (*in && n + 4 < cap) {
        if (*in == '<') {
            const char *e = strchr(in, '>');
            if (!e) break;
            if ((e - in >= 3 && (in[1] | 0x20) == 'b' && (in[2] | 0x20) == 'r') ||
                (e - in >= 3 && in[1] == '/' && (in[2] | 0x20) == 'p'))
                out[n++] = '\n';
            in = e + 1;
        } else if (*in == '&') {
            static const struct { const char *name; char ch; } ents[] = {
                {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}, {"&nbsp;", ' '}};
            size_t k;
            for (k = 0; k < sizeof ents / sizeof ents[0]; k++) {
                size_t l = strlen(ents[k].name);
                if (strncmp(in, ents[k].name, l) == 0) {
                    out[n++] = ents[k].ch;
                    in += l;
                    break;
                }
            }
            if (k == sizeof ents / sizeof ents[0]) out[n++] = *in++;
        } else {
            out[n++] = *in++;
        }
    }
    out[n] = 0;
}

typedef struct face {
    const swf_assets *a;
    const swf_font *font;     /* NULL: device font */
    float scale;              /* twips per font unit, or per device-font pixel */
    float ascent, line_h;     /* twips */
} face;

static const swf_glyph *find_glyph(const swf_font *f, uint32_t cp)
{
    for (uint16_t i = 0; i < f->glyph_count; i++)
        if (f->glyphs[i].code == cp) return &f->glyphs[i];
    return NULL;
}

static float advance(const face *fc, uint32_t cp)
{
    if (fc->font) {
        const swf_glyph *g = find_glyph(fc->font, cp);
        return g ? g->advance * fc->scale : 0.0f;
    }
    const swf_device_font *d = &fc->a->device_font;
    if (cp < d->first || cp >= (uint32_t)d->first + d->count) cp = '?';
    return d->advances[cp - d->first] * fc->scale;
}

static void draw_char(const face *fc, const swf_matrix *wm, float x, float y, uint32_t cp, uint32_t rgba,
                      const swf_cxform *cx)
{
    if (fc->font) {
        const swf_glyph *g = find_glyph(fc->font, cp);
        if (!g || g->shape == SWF_NONE) return;
        swf_matrix gm = {fc->scale, 0, 0, fc->scale, x, y};
        swf_matrix m = swf_matrix_mul(wm, &gm);
        swf_cxform col = SWF_CXFORM_IDENTITY;
        for (int k = 0; k < 4; k++) col.mul[k] = ((rgba >> (8 * k)) & 0xFF) / 255.0f;
        swf_cxform c = swf_cxform_mul(cx, &col);
        swf_render_shape(fc->a, g->shape, &m, &c, 0, true);
        return;
    }
    const swf_device_font *d = &fc->a->device_font;
    if (cp == ' ') return;
    if (cp < d->first || cp >= (uint32_t)d->first + d->count) cp = '?';
    uint32_t i = cp - d->first;
    float src[4] = {(float)(i % d->cols * d->cell), (float)(i / d->cols * d->cell), (float)d->cell, (float)d->cell};
    float dst[4] = {x, y - fc->ascent, d->cell * fc->scale, d->cell * fc->scale};
    swf_render_glyph_quad(fc->a, wm, src, dst, rgba, cx);
}

/* Lays the text out; draws it when `wm` is set, and reports the extent of the text in
 * twips (widest line, total line height). */
static bool layout(swf_movie *m, const swf_dobj *o, const swf_matrix *wm, const swf_cxform *cx, float *out_w,
                   float *out_h)
{
    const swf_assets *a = m->assets;
    const swf_edit_text *e = swf_dobj_edit(m, o);
    *out_w = *out_h = 0;
    const char *src = o->text ? o->text : e->initial_text;
    static char buf[MAX_TEXT];
    if (e->flags & ET_HTML) {
        swf_html_to_text(src, buf, sizeof buf);
        src = buf;
    }
    if (!*src) return true;

    face fc = {a, NULL, 1, 0, 0};
    const swf_char *fch = swf_assets_char(a, e->font_id);
    if (fch && fch->kind == SWF_CHAR_FONT && (e->flags & ET_USE_OUTLINES) && a->fonts[fch->index].glyph_count) {
        fc.font = &a->fonts[fch->index];
        fc.scale = (float)e->height / fc.font->em;
        float asc = fc.font->ascent ? fc.font->ascent * fc.scale : e->height * 0.8f;
        float desc = fc.font->descent ? fc.font->descent * fc.scale : e->height * 0.2f;
        fc.ascent = asc;
        fc.line_h = asc + desc + e->leading;
    } else if (a->has_device_font) {
        /* The bitmap font's 8-pixel cell stands in for the requested em size. */
        fc.scale = (float)e->height / a->device_font.cell;
        fc.ascent = a->device_font.glyph_h * fc.scale;
        fc.line_h = e->height * 1.15f + e->leading;
    } else {
        swf_warn_once(m, SWF_WARN_EDIT_TEXT, "text: no font available for device text");
        return false;
    }

    float left = (float)(e->bounds[0] + GUTTER + e->left_margin);
    float right = (float)(e->bounds[2] - GUTTER - e->right_margin);
    float width = right - left;
    bool wrap = (e->flags & ET_WORD_WRAP) != 0;
    bool multiline = (e->flags & ET_MULTILINE) != 0;
    float y = e->bounds[1] + GUTTER + fc.ascent;
    const char *p = src;
    bool first_line = true;
    float max_w = 0;
    int lines = 0;
    while (*p) {
        /* Find the end of the line: hard break, or the last space that fits when wrapping. */
        const char *line = p, *q = p, *brk = NULL, *end = NULL;
        float w = first_line ? (float)e->indent : 0.0f, w_at_brk = 0;
        while (*q && *q != '\n' && *q != '\r') {
            const char *before = q;
            uint32_t cp = next_cp(&q);
            float adv = advance(&fc, cp);
            if (wrap && w + adv > width && before > line) {
                end = brk ? brk : before;
                break;
            }
            if (cp == ' ') {
                brk = before;
                w_at_brk = w;
            }
            w += adv;
        }
        if (!end) end = q;
        float lw = end == q ? w : (brk && end == brk ? w_at_brk : w);
        if (lw > max_w) max_w = lw;
        lines++;
        if (wm) {
            float x = left + (first_line ? e->indent : 0);
            if (e->align == 1) x = right - lw;
            else if (e->align == 2) x = left + (width - lw) * 0.5f;
            for (const char *s = line; s < end;) {
                uint32_t cp = next_cp(&s);
                draw_char(&fc, wm, x, y, cp, e->color, cx);
                x += advance(&fc, cp);
            }
        }
        p = end;
        if (*p == ' ' && end != q) p++;
        if (*p == '\r' && p[1] == '\n') p += 2;
        else if (*p == '\n' || *p == '\r') p++;
        if (!multiline && !wrap) break;
        y += fc.line_h;
        first_line = false;
        if (wm && y - fc.ascent > e->bounds[3]) break;
    }
    *out_w = max_w;
    *out_h = lines ? fc.line_h * (float)(lines - 1) + fc.ascent + (fc.line_h - fc.ascent - e->leading) : 0;
    return true;
}

void swf_render_edit_text(swf_movie *m, const swf_dobj *o, const swf_matrix *wm, const swf_cxform *cx)
{
    const swf_edit_text *e = swf_dobj_edit(m, o);
    if (e->background) swf_render_rect(wm, e->bounds, e->background_color, cx);
    if (e->border) {
        int32_t b = 20;
        int32_t r[4][4] = {{e->bounds[0], e->bounds[1], e->bounds[2], e->bounds[1] + b},
                           {e->bounds[0], e->bounds[3] - b, e->bounds[2], e->bounds[3]},
                           {e->bounds[0], e->bounds[1], e->bounds[0] + b, e->bounds[3]},
                           {e->bounds[2] - b, e->bounds[1], e->bounds[2], e->bounds[3]}};
        for (int k = 0; k < 4; k++) swf_render_rect(wm, r[k], e->border_color, cx);
    }
    float w, h;
    (void)layout(m, o, wm, cx, &w, &h);
}

bool swf_edit_text_measure(swf_movie *m, const swf_dobj *o, float *w, float *h)
{
    return layout(m, o, NULL, NULL, w, h);
}

const swf_edit_text *swf_dobj_edit(const swf_movie *m, const swf_dobj *o)
{
    if (o->edit) return o->edit;
    static const swf_edit_text EMPTY = {{0, 0, 0, 0}, 0, 240, 0xFF000000u, 0, 0, 0, 0, 0, 0, 0, "", "",
                                        false, false, 0xFFFFFFFFu, 0xFF000000u, 0, 0xFF000000u};
    return o->index < m->assets->edit_count ? &m->assets->edits[o->index] : &EMPTY;
}
