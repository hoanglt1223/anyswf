/* TextField (dynamic text: text, htmlText, colours, border/background, autoSize,
 * variable binding, setTextFormat) and TextFormat. Input fields are shown but not
 * editable yet (an on-screen keyboard through sceUtilityOsk comes later). Property
 * names follow Ruffle's avm1/globals/text_field.rs (MIT/Apache-2.0). */
#include "../avm1/vm_internal.h"
#include "../display/display_internal.h"

#include <math.h>

#define DD (ATTR_DONT_ENUM | ATTR_DONT_DELETE)
#define GUTTER 40
#define MAX_TEXT 4096

enum { ET_READ_ONLY = 1 << 3, ET_WORD_WRAP = 1 << 6, ET_MULTILINE = 1 << 5, ET_HTML = 1 << 9,
       ET_USE_OUTLINES = 1 << 8, ET_NO_SELECT = 1 << 12 };

static char *dup_str(const char *s)
{
    size_t n = strlen(s);
    char *c = malloc(n + 1);
    if (c) memcpy(c, s, n + 1);
    return c;
}

static swf_dobj *this_text(swf_val this_)
{
    swf_dobj *d = clip_of(this_);
    return d && d->kind == SWF_CHAR_EDIT_TEXT && !d->removed ? d : NULL;
}

/* The field's own text record, copied from the asset on first change. */
static swf_edit_text *edit_mut(swf_vm *vm, swf_dobj *d)
{
    if (d->edit) return d->edit;
    const swf_edit_text *src = swf_dobj_edit(vm->movie, d);
    swf_edit_text *e = malloc(sizeof *e);
    char *vn = dup_str(src->var_name), *it = dup_str(src->initial_text);
    if (!e || !vn || !it) {
        free(e);
        free(vn);
        free(it);
        vm_abort(vm, "out of memory changing a text field");
    }
    *e = *src;
    e->var_name = vn;
    e->initial_text = it;
    d->edit = e;
    return e;
}

static const char *raw_text(swf_vm *vm, swf_dobj *d)
{
    return d->text ? d->text : swf_dobj_edit(vm->movie, d)->initial_text;
}

static void apply_auto_size(swf_vm *vm, swf_dobj *d)
{
    const swf_edit_text *ce = swf_dobj_edit(vm->movie, d);
    if (!ce->auto_size) return;
    float w, h;
    if (!swf_edit_text_measure(vm->movie, d, &w, &h)) return;
    swf_edit_text *e = edit_mut(vm, d);
    int32_t tw = (int32_t)ceilf(w) + 2 * GUTTER + e->left_margin + e->right_margin;
    int32_t th = (int32_t)ceilf(h) + 2 * GUTTER;
    if (!(e->flags & ET_WORD_WRAP)) {
        if (e->auto_size == 1) {
            e->bounds[2] = e->bounds[0] + tw;
        } else if (e->auto_size == 3) {
            e->bounds[0] = e->bounds[2] - tw;
        } else {
            int32_t mid = (e->bounds[0] + e->bounds[2]) / 2;
            e->bounds[0] = mid - tw / 2;
            e->bounds[2] = e->bounds[0] + tw;
        }
    }
    e->bounds[3] = e->bounds[1] + th;
}

static void set_raw_text(swf_vm *vm, swf_dobj *d, const char *utf8)
{
    const char *cur = raw_text(vm, d);
    if (strcmp(cur, utf8) == 0) return;
    char *c = dup_str(utf8);
    if (!c) vm_abort(vm, "out of memory setting text");
    free(d->text);
    d->text = c;
    apply_auto_size(vm, d);
}

static swf_str *plain_text(swf_vm *vm, swf_dobj *d)
{
    const char *raw = raw_text(vm, d);
    if (!(swf_dobj_edit(vm->movie, d)->flags & ET_HTML)) return str_from_utf8(vm, raw, strlen(raw));
    static char buf[MAX_TEXT];
    swf_html_to_text(raw, buf, sizeof buf);
    return str_from_utf8(vm, buf, strlen(buf));
}

/* Writes the text to the bound variable (TextField.text = ... updates it too). */
static void propagate_variable(swf_ctx *cx, swf_dobj *d)
{
    const swf_edit_text *e = swf_dobj_edit(cx->vm->movie, d);
    if (!e->var_name[0] || !d->parent) return;
    swf_ctx c = ctx_for_clip(cx->vm, d->parent);
    set_variable_str(&c, str_from_utf8(cx->vm, e->var_name, strlen(e->var_name)), swf_strv(plain_text(cx->vm, d)));
}

static void set_text_value(swf_ctx *cx, swf_dobj *d, swf_val v, bool propagate)
{
    char *u = str_to_utf8(to_string(cx, v), NULL);
    if (!u) vm_abort(cx->vm, "out of memory setting text");
    set_raw_text(cx->vm, d, u);
    free(u);
    if (propagate) propagate_variable(cx, d);
}

/* --- variable binding ---------------------------------------------------------------- */

typedef struct bind_req {
    swf_dobj *d;
} bind_req;

static void bind_task(swf_ctx *cx, void *user)
{
    swf_dobj *d = ((bind_req *)user)->d;
    swf_vm *vm = cx->vm;
    const swf_edit_text *e = swf_dobj_edit(vm->movie, d);
    swf_str *path = str_from_utf8(vm, e->var_name, strlen(e->var_name));
    swf_val v = get_variable_str(cx, path, NULL);
    if (v.t == SWF_T_UNDEF) {
        /* An unset variable takes the field's initial text, once. */
        if (!d->var_bound) set_variable_str(cx, path, swf_strv(plain_text(vm, d)));
    } else {
        set_text_value(cx, d, v, false);
    }
    d->var_bound = true;
}

static void sync_tree(swf_vm *vm, swf_dobj *d)
{
    for (swf_dobj *c = d->children; c; c = c->next) {
        if (c->removed) continue;
        if (c->kind == SWF_CHAR_EDIT_TEXT) {
            if (swf_dobj_edit(vm->movie, c)->var_name[0]) {
                bind_req r = {c};
                vm_run_protected(vm, d, bind_task, &r);
            }
        } else if (c->kind == SWF_CHAR_SPRITE) {
            sync_tree(vm, c);
        }
    }
}

void textfield_sync_variables(swf_vm *vm)
{
    if (vm->movie->root) sync_tree(vm, vm->movie->root);
}

/* --- creation ---------------------------------------------------------------------- */

swf_dobj *textfield_create(swf_ctx *cx, swf_dobj *parent, swf_val name, int32_t depth, double x, double y,
                           double w, double h)
{
    swf_vm *vm = cx->vm;
    swf_edit_text *e = calloc(1, sizeof *e);
    char *vn = dup_str(""), *it = dup_str("");
    char *nm = str_to_utf8(to_string(cx, name), NULL);
    swf_dobj *o = e && vn && it && nm ? swf_dobj_create(vm->movie, 0, SWF_CHAR_EDIT_TEXT, depth) : NULL;
    if (!o || !swf_dobj_set_name(o, nm)) {
        free(e);
        free(vn);
        free(it);
        free(nm);
        if (o) swf_dobj_free(NULL, o);
        return NULL;
    }
    free(nm);
    e->bounds[2] = (int32_t)(w * 20);
    e->bounds[3] = (int32_t)(h * 20);
    e->height = 240; /* 12 pt Times in Flash; the device font here */
    e->flags = ET_READ_ONLY;
    e->color = 0xFF000000u;
    e->var_name = vn;
    e->initial_text = it;
    e->background_color = 0xFFFFFFFFu;
    e->border_color = 0xFF000000u;
    o->edit = e;
    o->matrix.tx = (float)(x * 20);
    o->matrix.ty = (float)(y * 20);
    swf_clip_place(vm->movie, parent, o);
    return o;
}

/* --- properties ---------------------------------------------------------------------- */

#define TF_GET(fname) static swf_val fname(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
#define TF_PRELUDE                                                                                         \
    (void)argc; (void)argv;                                                                                \
    swf_dobj *d = this_text(this_);                                                                        \
    if (!d) return swf_undef();                                                                            \
    swf_vm *vm = cx->vm;                                                                                   \
    (void)vm

static uint32_t rgb_of(uint32_t rgba)
{
    return ((rgba & 0xFF) << 16) | (rgba & 0xFF00) | ((rgba >> 16) & 0xFF);
}

static uint32_t rgba_from(uint32_t rgb, uint32_t keep_alpha)
{
    return ((rgb >> 16) & 0xFF) | (rgb & 0xFF00) | ((rgb & 0xFF) << 16) | (keep_alpha & 0xFF000000u);
}

TF_GET(tf_get_text) { TF_PRELUDE; return swf_strv(plain_text(vm, d)); }
TF_GET(tf_set_text)
{
    TF_PRELUDE;
    if (argc) set_text_value(cx, d, argv[0], true);
    return swf_undef();
}
TF_GET(tf_get_html_text)
{
    TF_PRELUDE;
    if (!(swf_dobj_edit(vm->movie, d)->flags & ET_HTML)) return swf_strv(plain_text(vm, d));
    const char *raw = raw_text(vm, d);
    return swf_strv(str_from_utf8(vm, raw, strlen(raw)));
}
TF_GET(tf_set_html_text)
{
    TF_PRELUDE;
    if (argc) set_text_value(cx, d, argv[0], true);
    return swf_undef();
}

static swf_val flag_get(swf_ctx *cx, swf_val this_, uint16_t flag, bool invert)
{
    swf_dobj *d = this_text(this_);
    if (!d) return swf_undef();
    bool on = (swf_dobj_edit(cx->vm->movie, d)->flags & flag) != 0;
    return swf_bool(invert ? !on : on);
}

static void flag_set(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv, uint16_t flag, bool invert)
{
    swf_dobj *d = this_text(this_);
    if (!d || argc < 1) return;
    bool on = to_bool_v(cx->version, argv[0]) != invert;
    swf_edit_text *e = edit_mut(cx->vm, d);
    e->flags = (uint16_t)(on ? e->flags | flag : e->flags & ~flag);
    apply_auto_size(cx->vm, d);
}

#define FLAG_PROP(get, set, flag, invert)                                                            \
    TF_GET(get) { (void)argc; (void)argv; return flag_get(cx, this_, flag, invert); }                 \
    TF_GET(set) { flag_set(cx, this_, argc, argv, flag, invert); return swf_undef(); }

FLAG_PROP(tf_get_html, tf_set_html, ET_HTML, false)
FLAG_PROP(tf_get_word_wrap, tf_set_word_wrap, ET_WORD_WRAP, false)
FLAG_PROP(tf_get_multiline, tf_set_multiline, ET_MULTILINE, false)
FLAG_PROP(tf_get_embed_fonts, tf_set_embed_fonts, ET_USE_OUTLINES, false)
FLAG_PROP(tf_get_selectable, tf_set_selectable, ET_NO_SELECT, true)

TF_GET(tf_get_text_color) { TF_PRELUDE; return swf_num(rgb_of(swf_dobj_edit(vm->movie, d)->color)); }
TF_GET(tf_set_text_color)
{
    TF_PRELUDE;
    if (argc) {
        swf_edit_text *e = edit_mut(vm, d);
        e->color = rgba_from(to_uint32(cx, argv[0]), e->color);
    }
    return swf_undef();
}
TF_GET(tf_get_background) { TF_PRELUDE; return swf_bool(swf_dobj_edit(vm->movie, d)->background); }
TF_GET(tf_set_background)
{
    TF_PRELUDE;
    if (argc) edit_mut(vm, d)->background = to_bool_v(cx->version, argv[0]);
    return swf_undef();
}
TF_GET(tf_get_border) { TF_PRELUDE; return swf_bool(swf_dobj_edit(vm->movie, d)->border); }
TF_GET(tf_set_border)
{
    TF_PRELUDE;
    if (argc) edit_mut(vm, d)->border = to_bool_v(cx->version, argv[0]);
    return swf_undef();
}
TF_GET(tf_get_background_color) { TF_PRELUDE; return swf_num(rgb_of(swf_dobj_edit(vm->movie, d)->background_color)); }
TF_GET(tf_set_background_color)
{
    TF_PRELUDE;
    if (argc) edit_mut(vm, d)->background_color = rgba_from(to_uint32(cx, argv[0]), 0xFF000000u);
    return swf_undef();
}
TF_GET(tf_get_border_color) { TF_PRELUDE; return swf_num(rgb_of(swf_dobj_edit(vm->movie, d)->border_color)); }
TF_GET(tf_set_border_color)
{
    TF_PRELUDE;
    if (argc) edit_mut(vm, d)->border_color = rgba_from(to_uint32(cx, argv[0]), 0xFF000000u);
    return swf_undef();
}
TF_GET(tf_get_auto_size)
{
    TF_PRELUDE;
    static const char *const NAMES[] = {"none", "left", "center", "right"};
    uint8_t a = swf_dobj_edit(vm->movie, d)->auto_size;
    return swf_strv(atom_ascii(vm, NAMES[a < 4 ? a : 0]));
}
TF_GET(tf_set_auto_size)
{
    TF_PRELUDE;
    if (!argc) return swf_undef();
    uint8_t mode = 0;
    swf_val v = argv[0];
    if (v.t == SWF_T_BOOL) {
        mode = v.u.b ? 1 : 0;
    } else {
        swf_str *s = to_string(cx, v);
        if (str_eq_ascii(s, "left")) mode = 1;
        else if (str_eq_ascii(s, "center")) mode = 2;
        else if (str_eq_ascii(s, "right")) mode = 3;
    }
    edit_mut(vm, d)->auto_size = mode;
    apply_auto_size(vm, d);
    return swf_undef();
}
TF_GET(tf_get_type)
{
    TF_PRELUDE;
    return swf_strv(atom_ascii(vm, swf_dobj_edit(vm->movie, d)->flags & ET_READ_ONLY ? "dynamic" : "input"));
}
TF_GET(tf_set_type)
{
    TF_PRELUDE;
    if (!argc) return swf_undef();
    swf_str *s = to_string(cx, argv[0]);
    swf_edit_text *e = edit_mut(vm, d);
    if (str_eq_ascii(s, "input")) {
        e->flags = (uint16_t)(e->flags & ~ET_READ_ONLY);
        vm_warn(vm, WARN_TEXT_INPUT, "avm1: input text fields are shown but cannot be edited yet");
    } else if (str_eq_ascii(s, "dynamic")) {
        e->flags = (uint16_t)(e->flags | ET_READ_ONLY);
    }
    return swf_undef();
}
TF_GET(tf_get_variable)
{
    TF_PRELUDE;
    const char *v = swf_dobj_edit(vm->movie, d)->var_name;
    return v[0] ? swf_strv(str_from_utf8(vm, v, strlen(v))) : swf_null();
}
TF_GET(tf_set_variable)
{
    TF_PRELUDE;
    if (!argc) return swf_undef();
    char *u = is_nullish(argv[0]) ? dup_str("") : str_to_utf8(to_string(cx, argv[0]), NULL);
    if (!u) vm_abort(vm, "out of memory");
    swf_edit_text *e = edit_mut(vm, d);
    free((char *)e->var_name);
    e->var_name = u;
    d->var_bound = false;
    return swf_undef();
}
TF_GET(tf_get_max_chars)
{
    TF_PRELUDE;
    uint16_t m = swf_dobj_edit(vm->movie, d)->max_length;
    return m ? swf_num(m) : swf_null();
}
TF_GET(tf_set_max_chars)
{
    TF_PRELUDE;
    if (argc) {
        int32_t m = to_int32(cx, argv[0]);
        edit_mut(vm, d)->max_length = (uint16_t)(m > 0 && m < 65536 ? m : 0);
    }
    return swf_undef();
}
TF_GET(tf_get_length) { TF_PRELUDE; return swf_num(plain_text(vm, d)->len); }
TF_GET(tf_get_text_width)
{
    TF_PRELUDE;
    float w, h;
    return swf_num(swf_edit_text_measure(vm->movie, d, &w, &h) ? floor(w / 20.0) : 0);
}
TF_GET(tf_get_text_height)
{
    TF_PRELUDE;
    float w, h;
    return swf_num(swf_edit_text_measure(vm->movie, d, &w, &h) ? floor(h / 20.0) : 0);
}
TF_GET(tf_get_scroll) { TF_PRELUDE; return swf_num(1); }
TF_GET(tf_ignore_set) { (void)cx; (void)this_; (void)argc; (void)argv; return swf_undef(); }

/* --- methods ----------------------------------------------------------------------- */

static swf_obj *textformat_new(swf_ctx *cx);

static void format_set(swf_ctx *cx, swf_obj *f, const char *k, swf_val v)
{
    obj_set(cx, f, atom_ascii(cx->vm, k), v);
}

static swf_val tf_get_text_format(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    TF_PRELUDE;
    const swf_edit_text *e = swf_dobj_edit(vm->movie, d);
    swf_obj *f = textformat_new(cx);
    const swf_char *fch = swf_assets_char(vm->movie->assets, e->font_id);
    const char *font = fch && fch->kind == SWF_CHAR_FONT ? vm->movie->assets->fonts[fch->index].name : "_sans";
    format_set(cx, f, "font", swf_strv(str_from_utf8(vm, font, strlen(font))));
    format_set(cx, f, "size", swf_num(e->height / 20.0));
    format_set(cx, f, "color", swf_num(rgb_of(e->color)));
    static const char *const ALIGN[] = {"left", "right", "center", "justify"};
    format_set(cx, f, "align", swf_strv(atom_ascii(vm, ALIGN[e->align < 4 ? e->align : 0])));
    format_set(cx, f, "leftMargin", swf_num(e->left_margin / 20.0));
    format_set(cx, f, "rightMargin", swf_num(e->right_margin / 20.0));
    format_set(cx, f, "indent", swf_num(e->indent / 20.0));
    format_set(cx, f, "leading", swf_num(e->leading / 20.0));
    format_set(cx, f, "bold", swf_bool(false));
    format_set(cx, f, "italic", swf_bool(false));
    format_set(cx, f, "underline", swf_bool(false));
    return swf_objv(f);
}

/* Applies a TextFormat to the whole field (runs of formatting are not kept). */
static swf_val tf_set_text_format(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    TF_PRELUDE;
    if (argc < 1) return swf_undef();
    swf_val fv = argv[argc - 1];
    if (fv.t != SWF_T_OBJ) return swf_undef();
    swf_obj *f = fv.u.o;
    swf_edit_text *e = edit_mut(vm, d);
    swf_val v = obj_get(cx, f, atom_ascii(vm, "color"));
    if (!is_nullish(v)) e->color = rgba_from(to_uint32(cx, v), e->color);
    v = obj_get(cx, f, atom_ascii(vm, "size"));
    if (!is_nullish(v)) {
        double s = to_number(cx, v);
        if (s > 0 && s < 1000) e->height = (uint16_t)(s * 20.0);
    }
    v = obj_get(cx, f, atom_ascii(vm, "align"));
    if (!is_nullish(v)) {
        swf_str *a = to_string(cx, v);
        if (str_eq_ascii(a, "left")) e->align = 0;
        else if (str_eq_ascii(a, "right")) e->align = 1;
        else if (str_eq_ascii(a, "center")) e->align = 2;
        else if (str_eq_ascii(a, "justify")) e->align = 3;
    }
    static const char *const MARGINS[] = {"leftMargin", "rightMargin", "indent"};
    uint16_t *dst[] = {&e->left_margin, &e->right_margin, &e->indent};
    for (int i = 0; i < 3; i++) {
        v = obj_get(cx, f, atom_ascii(vm, MARGINS[i]));
        if (!is_nullish(v)) {
            double n = to_number(cx, v);
            if (n >= 0 && n < 3000) *dst[i] = (uint16_t)(n * 20.0);
        }
    }
    v = obj_get(cx, f, atom_ascii(vm, "leading"));
    if (!is_nullish(v)) {
        double n = to_number(cx, v);
        if (n > -1600 && n < 1600) e->leading = (int16_t)(n * 20.0);
    }
    v = obj_get(cx, f, atom_ascii(vm, "font"));
    if (!is_nullish(v)) {
        /* Embedded fonts are looked up by name; anything else falls back to the device font. */
        char *name = str_to_utf8(to_string(cx, v), NULL);
        const swf_assets *a = vm->movie->assets;
        for (uint32_t i = 0; name && i < a->char_count; i++) {
            if (a->chars[i].kind == SWF_CHAR_FONT && a->fonts[a->chars[i].index].glyph_count &&
                strcmp(a->fonts[a->chars[i].index].name, name) == 0) {
                e->font_id = (uint16_t)i;
                break;
            }
        }
        free(name);
    }
    apply_auto_size(vm, d);
    return swf_undef();
}

static swf_val tf_remove(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    TF_PRELUDE;
    if (d->depth >= AVM_DEPTH_BIAS && d->parent) swf_clip_remove_child(vm->movie, d->parent, d->depth);
    return swf_undef();
}

static swf_val tf_get_depth(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    TF_PRELUDE;
    return swf_num(d->depth - AVM_DEPTH_BIAS);
}

static swf_val tf_replace_sel(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)cx; (void)this_; (void)argc; (void)argv;
    return swf_undef();
}

/* --- TextFormat ------------------------------------------------------------------- */

static const char *const TF_KEYS[] = {"font", "size", "color", "bold", "italic", "underline", "url",
                                      "target", "align", "leftMargin", "rightMargin", "indent", "leading"};

static swf_val textformat_ctor(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    swf_obj *o = to_object(cx, this_);
    if (!o) return swf_undef();
    for (uint32_t i = 0; i < sizeof TF_KEYS / sizeof TF_KEYS[0]; i++)
        obj_set(cx, o, atom_ascii(cx->vm, TF_KEYS[i]), i < argc ? argv[i] : swf_null());
    obj_set(cx, o, atom_ascii(cx->vm, "blockIndent"), swf_null());
    obj_set(cx, o, atom_ascii(cx->vm, "bullet"), swf_null());
    obj_set(cx, o, atom_ascii(cx->vm, "tabStops"), swf_null());
    return swf_undef();
}

static swf_obj *textformat_new(swf_ctx *cx)
{
    swf_obj *o = obj_new(cx->vm, cx->vm->textformat_proto);
    textformat_ctor(cx, swf_objv(o), 0, NULL);
    return o;
}

static swf_val tf_class(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)cx; (void)argc; (void)argv;
    return this_;
}

static void prop(swf_vm *vm, swf_obj *o, const char *name, swf_native_fn get, swf_native_fn set, uint16_t attr)
{
    obj_add_property(vm, o, atom_ascii(vm, name), func_new_native(vm, get, NULL, NULL),
                     set ? func_new_native(vm, set, NULL, NULL) : NULL, attr);
}

void builtins_textfield(swf_vm *vm)
{
    swf_obj *p = obj_new(vm, vm->object_proto);
    vm->textfield_proto = p;
    swf_obj *c = func_new_native(vm, tf_class, NULL, p);
    c->u.fn.ctor = NULL;
    obj_define_ascii(vm, vm->global, "TextField", swf_objv(c), ATTR_DONT_ENUM | ATTR_VERSION_6);
    const uint16_t M = DD | ATTR_VERSION_6;
    native_method(vm, p, "getTextFormat", tf_get_text_format, M);
    native_method(vm, p, "setTextFormat", tf_set_text_format, M);
    native_method(vm, p, "getNewTextFormat", tf_get_text_format, M);
    native_method(vm, p, "setNewTextFormat", tf_set_text_format, M);
    native_method(vm, p, "removeTextField", tf_remove, M);
    native_method(vm, p, "getDepth", tf_get_depth, M | ATTR_READ_ONLY);
    native_method(vm, p, "replaceSel", tf_replace_sel, M);
    prop(vm, p, "text", tf_get_text, tf_set_text, 0);
    prop(vm, p, "htmlText", tf_get_html_text, tf_set_html_text, 0);
    prop(vm, p, "html", tf_get_html, tf_set_html, 0);
    prop(vm, p, "textColor", tf_get_text_color, tf_set_text_color, 0);
    prop(vm, p, "background", tf_get_background, tf_set_background, 0);
    prop(vm, p, "backgroundColor", tf_get_background_color, tf_set_background_color, 0);
    prop(vm, p, "border", tf_get_border, tf_set_border, 0);
    prop(vm, p, "borderColor", tf_get_border_color, tf_set_border_color, 0);
    prop(vm, p, "autoSize", tf_get_auto_size, tf_set_auto_size, 0);
    prop(vm, p, "type", tf_get_type, tf_set_type, 0);
    prop(vm, p, "variable", tf_get_variable, tf_set_variable, 0);
    prop(vm, p, "maxChars", tf_get_max_chars, tf_set_max_chars, 0);
    prop(vm, p, "wordWrap", tf_get_word_wrap, tf_set_word_wrap, 0);
    prop(vm, p, "multiline", tf_get_multiline, tf_set_multiline, 0);
    prop(vm, p, "embedFonts", tf_get_embed_fonts, tf_set_embed_fonts, 0);
    prop(vm, p, "selectable", tf_get_selectable, tf_set_selectable, 0);
    prop(vm, p, "length", tf_get_length, NULL, 0);
    prop(vm, p, "textWidth", tf_get_text_width, NULL, 0);
    prop(vm, p, "textHeight", tf_get_text_height, NULL, 0);
    prop(vm, p, "scroll", tf_get_scroll, tf_ignore_set, 0);
    prop(vm, p, "maxscroll", tf_get_scroll, NULL, 0);
    obj_set_attributes(vm, p, NULL, DD, 0);

    swf_obj *fp = obj_new(vm, vm->object_proto);
    vm->textformat_proto = fp;
    swf_obj *fc = func_new_native(vm, textformat_ctor, NULL, fp);
    fc->u.fn.ctor = NULL;
    obj_define_ascii(vm, vm->global, "TextFormat", swf_objv(fc), ATTR_DONT_ENUM | ATTR_VERSION_6);
}
