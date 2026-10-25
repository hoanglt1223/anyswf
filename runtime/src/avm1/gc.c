/* Mark-and-sweep collector. It runs only between frames, when no script is on the C
 * stack, so the roots are the VM's own tables and the display tree. */
#include "vm_internal.h"

#define GC_MIN_THRESHOLD (1024 * 1024)

void *gc_alloc(swf_vm *vm, size_t size, uint8_t kind)
{
    swf_gc_hdr *h = calloc(1, size);
    if (!h) vm_abort(vm, "out of memory");
    h->kind = kind;
    h->next = vm->objects;
    vm->objects = h;
    vm->live_bytes += size;
    return h;
}

typedef struct gray {
    swf_gc_hdr **items;
    size_t n, cap;
} gray;

static void push(gray *g, swf_gc_hdr *h)
{
    if (!h || h->mark) return;
    h->mark = 1;
    if (g->n == g->cap) {
        size_t cap = g->cap ? g->cap * 2 : 4096;
        swf_gc_hdr **n = realloc(g->items, cap * sizeof *n);
        if (!n) {
            /* Without a gray stack we cannot finish marking safely; keep everything. */
            plat_log("avm1: out of memory while collecting");
            abort();
        }
        g->items = n;
        g->cap = cap;
    }
    g->items[g->n++] = h;
}

static void push_val(gray *g, swf_val v)
{
    if (v.t == SWF_T_STR) push(g, &v.u.s->gc);
    else if (v.t == SWF_T_OBJ) push(g, &v.u.o->gc);
}

static void push_obj(gray *g, swf_obj *o)
{
    if (o) push(g, &o->gc);
}

static void push_dobj_tree(gray *g, swf_dobj *d)
{
    for (; d; d = d->next) {
        if (d->script_object) push(g, d->script_object);
        push_dobj_tree(g, d->children);
    }
}

static void trace(gray *g, swf_gc_hdr *h)
{
    if (h->kind == SWF_GC_STR) {
        swf_str *s = (swf_str *)h;
        if (s->fold && s->fold != s) push(g, &s->fold->gc);
    } else if (h->kind == SWF_GC_SCOPE) {
        swf_scope *s = (swf_scope *)h;
        if (s->parent) push(g, &s->parent->gc);
        push_obj(g, s->obj);
    } else {
        swf_obj *o = (swf_obj *)h;
        for (uint32_t i = 0; i < o->nprops; i++) {
            swf_prop *p = &o->props[i];
            if (!p->key) continue;
            push(g, &p->key->gc);
            push_val(g, p->val);
            push_obj(g, p->getter);
            push_obj(g, p->setter);
        }
        for (uint32_t i = 0; i < o->nifaces; i++) push_obj(g, o->ifaces[i]);
        push_obj(g, o->watchers);
        switch (o->cls) {
        case OC_FUNCTION:
            if (o->u.fn.scope) push(g, &o->u.fn.scope->gc);
            push_obj(g, o->u.fn.base_o);
            break;
        case OC_STRING:
        case OC_NUMBER:
        case OC_BOOLEAN:
            push_val(g, o->u.prim);
            break;
        case OC_SUPER:
            push_obj(g, o->u.super_.this_);
            break;
        case OC_SOUND:
            push_obj(g, o->u.sound.target);
            break;
        default:
            break;
        }
    }
}

static void free_one(swf_gc_hdr *h)
{
    if (h->kind == SWF_GC_OBJ) {
        swf_obj *o = (swf_obj *)h;
        if (o->cls == OC_CLIP && o->u.clip && o->u.clip->script_object == o) o->u.clip->script_object = NULL;
        free(o->props);
        free(o->index);
        free(o->ifaces);
    }
    free(h);
}

static size_t size_of(swf_gc_hdr *h)
{
    switch (h->kind) {
    case SWF_GC_STR: return sizeof(swf_str) + ((swf_str *)h)->len * sizeof(uint16_t);
    case SWF_GC_SCOPE: return sizeof(swf_scope);
    default: return sizeof(swf_obj);
    }
}

static void collect(swf_vm *vm)
{
    gray g = {0};
    push_obj(&g, vm->global);
    if (vm->global_scope) push(&g, &vm->global_scope->gc);
    swf_obj *roots[] = {vm->object_proto, vm->function_proto, vm->array_proto, vm->string_proto,
                        vm->number_proto, vm->boolean_proto, vm->error_proto, vm->clip_proto,
                        vm->object_ctor, vm->function_ctor, vm->array_ctor, vm->string_ctor,
                        vm->number_ctor, vm->boolean_ctor, vm->key_obj, vm->mouse_obj, vm->stage_obj,
                        vm->button_proto, vm->textfield_proto, vm->date_proto, vm->sound_proto,
                        vm->textformat_proto, vm->color_proto, vm->classes, vm->timer_objs,
                        vm->shared_objects, vm->voices, vm->hover, vm->pressed, vm->drag};
    for (size_t i = 0; i < sizeof roots / sizeof roots[0]; i++) push_obj(&g, roots[i]);
    for (int i = 0; i < SWF_GLOBAL_REGS; i++) push_val(&g, vm->regs[i]);
    for (uint32_t i = 0; i < vm->nstreams; i++) push_obj(&g, vm->streams[i].clip);
    for (int p = 0; p < Q_PRIORITIES; p++)
        for (uint32_t i = vm->q_head[p]; i < vm->q_tail[p]; i++) {
            const queued *q = &vm->queue[p][i];
            push_obj(&g, q->clip);
            push_obj(&g, q->obj);
            if (q->name) push(&g, &q->name->gc);
            for (uint8_t k = 0; k < q->nargs; k++) push_val(&g, q->args[k]);
        }
    swf_str **names = (swf_str **)&vm->s;
    for (size_t i = 0; i < sizeof vm->s / sizeof(swf_str *); i++)
        if (names[i]) push(&g, &names[i]->gc);
    for (uint32_t i = 0; i < vm->code->atom_count; i++)
        if (vm->code->atoms[i]) push(&g, &vm->code->atoms[i]->gc);
    if (vm->movie && vm->movie->root) {
        if (vm->movie->root->script_object) push(&g, vm->movie->root->script_object);
        push_dobj_tree(&g, vm->movie->root->children);
    }
    while (g.n) trace(&g, g.items[--g.n]);
    free(g.items);

    atoms_sweep(vm);
    swf_gc_hdr **pp = &vm->objects;
    size_t live = 0;
    while (*pp) {
        swf_gc_hdr *h = *pp;
        if (h->mark) {
            h->mark = 0;
            live += size_of(h);
            pp = &h->next;
        } else {
            *pp = h->next;
            free_one(h);
        }
    }
    vm->live_bytes = live;
    vm->last_live = live;
    vm->threshold = live * 2 > GC_MIN_THRESHOLD ? live * 2 : GC_MIN_THRESHOLD;
}

void swf_vm_collect(swf_vm *vm)
{
    if (vm->live_bytes >= vm->threshold) collect(vm);
}

void gc_collect_now(swf_vm *vm)
{
    collect(vm);
}

void gc_free_all(swf_vm *vm)
{
    swf_gc_hdr *h = vm->objects;
    while (h) {
        swf_gc_hdr *n = h->next;
        free_one(h);
        h = n;
    }
    vm->objects = NULL;
    vm->live_bytes = 0;
}

size_t swf_vm_live_bytes(const swf_vm *vm)
{
    return vm->live_bytes;
}
