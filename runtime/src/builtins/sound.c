/* Sound objects (attachSound, start, stop, volume and pan, onSoundComplete), timeline
 * event sounds, stream sounds kept in step with their timeline, and stopAllSounds. Audio
 * goes through the mixer (swfrt/mixer.h). A Sound's volume belongs to its target clip and
 * multiplies down the display tree, with `new Sound()` as the global transform. */
#include "../avm1/vm_internal.h"
#include "swfrt/mixer.h"

#include <math.h>

#define DD (ATTR_DONT_ENUM | ATTR_DONT_DELETE)
#define STREAM_RESYNC_SECONDS 0.25

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* Hidden per-clip transform: volume and pan stored on the clip's script object. */
static swf_str *k_vol(swf_vm *vm) { return atom_ascii(vm, "$soundVolume"); }
static swf_str *k_pan(swf_vm *vm) { return atom_ascii(vm, "$soundPan"); }

static void transform_of(swf_vm *vm, swf_obj *target, double *vol, double *pan)
{
    *vol = 100;
    *pan = 0;
    if (!target) {
        *vol = vm->snd_volume;
        *pan = vm->snd_pan;
        return;
    }
    swf_prop *v = obj_own(vm, target, k_vol(vm)), *p = obj_own(vm, target, k_pan(vm));
    if (v && v->val.t == SWF_T_NUM) *vol = v->val.u.n;
    if (p && p->val.t == SWF_T_NUM) *pan = p->val.u.n;
}

/* Left/right gains for a sound owned by `owner` (a clip object, or NULL for global). */
static void gains(swf_vm *vm, swf_obj *owner, float *l, float *r)
{
    double vol, pan, g = vm->snd_volume / 100.0, p = vm->snd_pan;
    swf_dobj *d = owner && owner->cls == OC_CLIP ? owner->u.clip : NULL;
    for (; d; d = d->parent) {
        if (!d->script_object) continue;
        transform_of(vm, d->script_object, &vol, &pan);
        g *= vol / 100.0;
        if (pan != 0) p = pan;
    }
    if (p > 100) p = 100;
    if (p < -100) p = -100;
    *l = (float)(g * (p > 0 ? (100 - p) / 100.0 : 1.0));
    *r = (float)(g * (p < 0 ? (100 + p) / 100.0 : 1.0));
}

static void record_voice(swf_vm *vm, uint32_t id, swf_obj *sound_obj, swf_obj *owner, uint32_t sound)
{
    swf_obj *rec = obj_new_bare(vm);
    obj_define_ascii(vm, rec, "s", sound_obj ? swf_objv(sound_obj) : swf_undef(), 0);
    obj_define_ascii(vm, rec, "o", owner ? swf_objv(owner) : swf_undef(), 0);
    obj_define_ascii(vm, rec, "n", swf_num(sound), 0);
    obj_define(vm, vm->voices, atom_int(vm, id), swf_objv(rec), 0);
}

static swf_obj *rec_field(swf_vm *vm, swf_obj *rec, const char *k)
{
    swf_prop *p = obj_own(vm, rec, atom_ascii(vm, k));
    return p && p->val.t == SWF_T_OBJ ? p->val.u.o : NULL;
}

static uint32_t rec_sound(swf_vm *vm, swf_obj *rec)
{
    swf_prop *p = obj_own(vm, rec, atom_ascii(vm, "n"));
    return p && p->val.t == SWF_T_NUM ? (uint32_t)p->val.u.n : SWF_NONE;
}

static void forget_voice(swf_vm *vm, uint32_t id)
{
    swf_ctx c = vm_ctx(vm);
    obj_delete(&c, vm->voices, atom_int(vm, id));
}

/* Calls fn(id, record) for every playing voice; the callback may stop it. */
typedef void (*voice_fn)(swf_vm *vm, uint32_t id, swf_obj *rec, void *user);

static void each_voice(swf_vm *vm, voice_fn fn, void *user)
{
    /* Snapshot first: callbacks delete records, which may compact the property table. */
    uint32_t ids[SWF_MIXER_VOICES * 2], n = 0;
    swf_obj *recs[SWF_MIXER_VOICES * 2];
    swf_obj *v = vm->voices;
    for (uint32_t i = 0; i < v->nprops && n < SWF_MIXER_VOICES * 2; i++) {
        swf_prop *p = &v->props[i];
        if (!p->key || p->val.t != SWF_T_OBJ) continue;
        double id = string_to_number(p->key, 7);
        if (id > 0) {
            ids[n] = (uint32_t)id;
            recs[n++] = p->val.u.o;
        }
    }
    for (uint32_t i = 0; i < n; i++) fn(vm, ids[i], recs[i], user);
}

static void restore_volume(swf_vm *vm, uint32_t id, swf_obj *rec, void *user)
{
    (void)user;
    float l, r;
    gains(vm, rec_field(vm, rec, "o"), &l, &r);
    swf_mixer_set_volume(vm->mixer, id, l, r);
}

/* --- timeline sounds --------------------------------------------------------------------- */

static bool sound_playing(swf_vm *vm, uint32_t sound)
{
    swf_obj *v = vm->voices;
    for (uint32_t i = 0; i < v->nprops; i++) {
        swf_prop *p = &v->props[i];
        if (p->key && p->val.t == SWF_T_OBJ && rec_sound(vm, p->val.u.o) == sound) return true;
    }
    return false;
}

typedef struct stop_req {
    uint32_t sound;   /* SWF_NONE = any */
    swf_obj *owner;   /* NULL = any */
} stop_req;

static void stop_matching(swf_vm *vm, uint32_t id, swf_obj *rec, void *user)
{
    stop_req *r = user;
    if (r->sound != SWF_NONE && rec_sound(vm, rec) != r->sound) return;
    if (r->owner && rec_field(vm, rec, "o") != r->owner) return;
    swf_mixer_stop(vm->mixer, id);
    forget_voice(vm, id);
}

static void stop_stream(swf_vm *vm, uint32_t i)
{
    swf_mixer_stop(vm->mixer, vm->streams[i].voice);
    vm->streams[i] = vm->streams[--vm->nstreams];
}

static void stream_block(swf_vm *vm, swf_dobj *clip, uint32_t pos)
{
    const swf_sprite *sp = swf_clip_sprite(vm->movie, clip);
    if (sp->stream == SWF_NONE || sp->stream >= vm->movie->assets->sound_count) return;
    const swf_sound *s = &vm->movie->assets->sounds[sp->stream];
    swf_obj *o = clip_object(vm, clip);
    uint32_t i = 0;
    while (i < vm->nstreams && vm->streams[i].clip != o) i++;
    if (i < vm->nstreams) {
        uint32_t at = swf_mixer_position(vm->mixer, vm->streams[i].voice);
        double drift = fabs((double)at - (double)pos) / (s->rate ? s->rate : 1);
        if (swf_mixer_active(vm->mixer, vm->streams[i].voice) && drift <= STREAM_RESYNC_SECONDS) {
            vm->streams[i].tick = vm->movie->tick;
            return;
        }
        stop_stream(vm, i);
    }
    if (vm->nstreams == sizeof vm->streams / sizeof vm->streams[0]) return;
    float l, r;
    gains(vm, o, &l, &r);
    uint32_t id = swf_mixer_play(vm->mixer, sp->stream, pos, 0, 1, l, r);
    if (!id) return;
    vm->streams[vm->nstreams].clip = o;
    vm->streams[vm->nstreams].voice = id;
    vm->streams[vm->nstreams].tick = vm->movie->tick;
    vm->nstreams++;
}

void sound_timeline(swf_movie *m, swf_dobj *clip, const uint8_t *cmd, void *user)
{
    swf_vm *vm = user;
    if (!vm->mixer) return;
    if (cmd[0] == SWF_CMD_STREAM_BLOCK) {
        stream_block(vm, clip, rd32(cmd + 1));
        return;
    }
    const uint8_t *p = cmd + 1;
    const swf_char *ch = swf_assets_char(m->assets, rd16(p));
    if (!ch || ch->kind != SWF_CHAR_SOUND) return;
    uint8_t event = p[2];
    uint16_t loops = rd16(p + 4);
    uint32_t in = rd32(p + 8), out = rd32(p + 12);
    if (event == 2) {
        stop_req r = {ch->index, NULL};
        each_voice(vm, stop_matching, &r);
        return;
    }
    if (event == 1 && sound_playing(vm, ch->index)) return;
    swf_obj *owner = clip_object(vm, clip);
    float l, r;
    gains(vm, owner, &l, &r);
    uint32_t id = swf_mixer_play(vm->mixer, ch->index, in == SWF_NONE ? 0 : in, out == SWF_NONE ? 0 : out,
                                 loops ? loops : 1, l, r);
    if (id) record_voice(vm, id, NULL, owner, ch->index);
}

void sound_stop_all(swf_vm *vm)
{
    swf_mixer_stop_all(vm->mixer);
    swf_obj *fresh = obj_new_bare(vm);
    vm->voices = fresh;
    vm->nstreams = 0;
}

void swf_act_stop_sounds(swf_ctx *cx)
{
    if (cx->vm->mixer) sound_stop_all(cx->vm);
}

void sound_clip_removed(swf_vm *vm, swf_dobj *clip)
{
    if (!clip->script_object) return;
    for (uint32_t i = 0; i < vm->nstreams; i++) {
        if (vm->streams[i].clip == clip->script_object) {
            stop_stream(vm, i);
            break;
        }
    }
}

void sound_after_frame(swf_vm *vm)
{
    if (!vm->mixer) return;
    uint32_t ids[32], n;
    while ((n = swf_mixer_poll_finished(vm->mixer, ids, 32)) > 0) {
        for (uint32_t i = 0; i < n; i++) {
            swf_prop *p = obj_own(vm, vm->voices, atom_int(vm, ids[i]));
            swf_obj *rec = p && p->val.t == SWF_T_OBJ ? p->val.u.o : NULL;
            forget_voice(vm, ids[i]);
            swf_obj *so = rec ? rec_field(vm, rec, "s") : NULL;
            if (!so || vm->version < 6) continue;
            swf_obj *owner = rec_field(vm, rec, "o");
            swf_dobj *clip = owner && owner->cls == OC_CLIP && owner->u.clip && !owner->u.clip->removed
                                 ? owner->u.clip
                                 : clip_root(vm);
            vm_queue_method(vm, clip, so, atom_ascii(vm, "onSoundComplete"), 0, NULL, false);
        }
    }
    /* A stream stops when its timeline stops delivering blocks. */
    if (vm->sound_tick != vm->movie->tick) {
        vm->sound_tick = vm->movie->tick;
        for (uint32_t i = 0; i < vm->nstreams;) {
            swf_obj *c = vm->streams[i].clip;
            bool gone = !c->u.clip || c->u.clip->removed || vm->movie->tick - vm->streams[i].tick > 1 ||
                        !swf_mixer_active(vm->mixer, vm->streams[i].voice);
            if (gone) stop_stream(vm, i);
            else i++;
        }
    }
}

/* --- Sound class -------------------------------------------------------------------------- */

static swf_sound_state *state_of(swf_val this_)
{
    return this_.t == SWF_T_OBJ && this_.u.o->cls == OC_SOUND ? &this_.u.o->u.sound : NULL;
}

static swf_val sound_ctor(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    swf_obj *o = to_object(cx, this_);
    if (!o) return swf_undef();
    o->cls = OC_SOUND;
    memset(&o->u.sound, 0, sizeof o->u.sound);
    o->u.sound.sound = SWF_NONE;
    if (argc && !is_nullish(argv[0])) {
        swf_dobj *base = ctx_base(cx) ? ctx_base(cx) : clip_root(cx->vm);
        swf_dobj *d = resolve_target_clip(cx, base, argv[0], false);
        if (d) o->u.sound.target = clip_object(cx->vm, d);
    }
    return swf_undef();
}

static swf_val snd_attach(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    swf_sound_state *s = state_of(this_);
    if (!s || argc < 1) return swf_undef();
    const swf_assets *a = cx->vm->movie->assets;
    char *name = str_to_utf8(to_string(cx, argv[0]), NULL);
    if (!name) return swf_undef();
    for (uint32_t i = 0; i < a->export_count; i++) {
        if (strcmp(a->exports[i].name, name) != 0) continue;
        const swf_char *ch = swf_assets_char(a, a->exports[i].char_id);
        if (ch && ch->kind == SWF_CHAR_SOUND) s->sound = ch->index;
        break;
    }
    free(name);
    return swf_undef();
}

static swf_val snd_start(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    swf_vm *vm = cx->vm;
    swf_sound_state *s = state_of(this_);
    if (!s || s->sound == SWF_NONE || !vm->mixer) return swf_undef();
    const swf_sound *snd = &vm->movie->assets->sounds[s->sound];
    double offset = argc > 0 ? to_number(cx, argv[0]) : 0;
    double loops = argc > 1 ? to_number(cx, argv[1]) : 1;
    if (!(offset > 0)) offset = 0;
    uint32_t start = (uint32_t)fmin(offset * snd->rate, (double)snd->frames);
    int plays = loops >= 1 && loops < 1e6 ? (int)loops : 1;
    swf_obj *owner = s->target;
    float l, r;
    gains(vm, owner, &l, &r);
    uint32_t id = swf_mixer_play(vm->mixer, s->sound, start, 0, plays, l, r);
    if (!id) return swf_undef();
    s->voice = id;
    record_voice(vm, id, this_.u.o, owner, s->sound);
    return swf_undef();
}

static swf_val snd_stop(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    swf_vm *vm = cx->vm;
    swf_sound_state *s = state_of(this_);
    if (!s || !vm->mixer) return swf_undef();
    stop_req r = {SWF_NONE, s->target};
    if (argc > 0) {
        r.owner = NULL;
        char *name = str_to_utf8(to_string(cx, argv[0]), NULL);
        const swf_assets *a = vm->movie->assets;
        r.sound = SWF_NONE - 1; /* matches nothing unless the name resolves */
        for (uint32_t i = 0; name && i < a->export_count; i++) {
            const swf_char *ch = swf_assets_char(a, a->exports[i].char_id);
            if (ch && ch->kind == SWF_CHAR_SOUND && strcmp(a->exports[i].name, name) == 0) r.sound = ch->index;
        }
        free(name);
    }
    each_voice(vm, stop_matching, &r);
    if (!s->target && argc == 0) vm->nstreams = 0, swf_mixer_stop_all(vm->mixer);
    return swf_undef();
}

static void set_transform_value(swf_vm *vm, swf_sound_state *s, bool volume, double v)
{
    if (!s->target) {
        if (volume) vm->snd_volume = (int16_t)v;
        else vm->snd_pan = (int16_t)v;
    } else {
        obj_define(vm, s->target, volume ? k_vol(vm) : k_pan(vm), swf_num(v), ATTR_DONT_ENUM);
    }
    if (vm->mixer) each_voice(vm, restore_volume, NULL);
    for (uint32_t i = 0; vm->mixer && i < vm->nstreams; i++) {
        float l, r;
        gains(vm, vm->streams[i].clip, &l, &r);
        swf_mixer_set_volume(vm->mixer, vm->streams[i].voice, l, r);
    }
}

static swf_val snd_set_volume(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    swf_sound_state *s = state_of(this_);
    if (!s || argc < 1) return swf_undef();
    double v = to_number(cx, argv[0]);
    if (isnan(v)) v = 0;
    if (v < -32768) v = -32768;
    if (v > 32767) v = 32767;
    set_transform_value(cx->vm, s, true, trunc(v));
    return swf_undef();
}

static swf_val snd_get_volume(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)argc; (void)argv;
    swf_sound_state *s = state_of(this_);
    if (!s) return swf_undef();
    double vol, pan;
    transform_of(cx->vm, s->target, &vol, &pan);
    return swf_num(vol);
}

static swf_val snd_set_pan(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    swf_sound_state *s = state_of(this_);
    if (!s || argc < 1) return swf_undef();
    double v = to_number(cx, argv[0]);
    if (isnan(v)) v = 0;
    set_transform_value(cx->vm, s, false, trunc(fmax(-100, fmin(100, v))));
    return swf_undef();
}

static swf_val snd_get_pan(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)argc; (void)argv;
    swf_sound_state *s = state_of(this_);
    if (!s) return swf_undef();
    double vol, pan;
    transform_of(cx->vm, s->target, &vol, &pan);
    return swf_num(pan);
}

static swf_val snd_duration(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)argc; (void)argv;
    swf_sound_state *s = state_of(this_);
    if (!s || s->sound == SWF_NONE) return swf_undef();
    const swf_sound *snd = &cx->vm->movie->assets->sounds[s->sound];
    return swf_num(snd->rate ? floor((double)snd->frames * 1000.0 / snd->rate) : 0);
}

static swf_val snd_position(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)argc; (void)argv;
    swf_sound_state *s = state_of(this_);
    if (!s || s->sound == SWF_NONE) return swf_undef();
    const swf_sound *snd = &cx->vm->movie->assets->sounds[s->sound];
    uint32_t at = swf_mixer_position(cx->vm->mixer, s->voice);
    return swf_num(snd->rate ? floor((double)at * 1000.0 / snd->rate) : 0);
}

static swf_val snd_bytes(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)argc; (void)argv;
    swf_sound_state *s = state_of(this_);
    if (!s || s->sound == SWF_NONE) return swf_undef();
    return swf_num(cx->vm->movie->assets->sounds[s->sound].data_size);
}

static swf_val snd_load(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)this_; (void)argc; (void)argv;
    vm_warn(cx->vm, WARN_LOADSOUND, "avm1: Sound.loadSound is not supported (external files are out of scope)");
    return swf_undef();
}

void builtins_sound(swf_vm *vm)
{
    vm->snd_volume = 100;
    vm->snd_pan = 0;
    swf_obj *p = obj_new(vm, vm->object_proto);
    vm->sound_proto = p;
    swf_obj *c = func_new_native(vm, sound_ctor, NULL, p);
    c->u.fn.ctor = NULL;
    obj_define_ascii(vm, vm->global, "Sound", swf_objv(c), ATTR_DONT_ENUM);
    const uint16_t A = DD | ATTR_READ_ONLY;
    native_method(vm, p, "attachSound", snd_attach, A);
    native_method(vm, p, "start", snd_start, A);
    native_method(vm, p, "stop", snd_stop, A);
    native_method(vm, p, "setVolume", snd_set_volume, A);
    native_method(vm, p, "getVolume", snd_get_volume, A);
    native_method(vm, p, "setPan", snd_set_pan, A);
    native_method(vm, p, "getPan", snd_get_pan, A);
    native_method(vm, p, "getBytesLoaded", snd_bytes, A | ATTR_VERSION_6);
    native_method(vm, p, "getBytesTotal", snd_bytes, A | ATTR_VERSION_6);
    native_method(vm, p, "loadSound", snd_load, A | ATTR_VERSION_6);
    obj_add_property(vm, p, atom_ascii(vm, "duration"), func_new_native(vm, snd_duration, NULL, NULL), NULL,
                     DD | ATTR_VERSION_6);
    obj_add_property(vm, p, atom_ascii(vm, "position"), func_new_native(vm, snd_position, NULL, NULL), NULL,
                     DD | ATTR_VERSION_6);
}
