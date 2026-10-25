/* Software mixer (see swfrt/mixer.h). The control thread owns voice slot allocation;
 * commands reach the render thread through a single-producer ring, and the render
 * thread publishes positions per slot and reports finished voices through a second
 * ring. Formats follow docs/dev/assets-bin-format.md (Sounds). */
#include "swfrt/mixer.h"

#include "mp3.h"

#include <stdlib.h>
#include <string.h>

#ifdef __PSP__
/* One CPU runs both threads and Allegrex has no LL/SC: aligned word accesses are atomic,
 * and a compiler barrier orders the payload before the index that publishes it. */
typedef volatile uint32_t shared_u32;
#define LOAD(p) (*(p))
#define STORE(p, v) (*(p) = (v))
#define LOAD_ACQ(p) (*(p))
#define STORE_REL(p, v) do { __asm__ __volatile__("" ::: "memory"); *(p) = (v); } while (0)
#define INIT(p, v) (*(p) = (v))
#else
#include <stdatomic.h>
typedef atomic_uint shared_u32;
#define LOAD(p) atomic_load_explicit((p), memory_order_relaxed)
#define STORE(p, v) atomic_store_explicit((p), (v), memory_order_relaxed)
#define LOAD_ACQ(p) atomic_load_explicit((p), memory_order_acquire)
#define STORE_REL(p, v) atomic_store_explicit((p), (v), memory_order_release)
#define INIT(p, v) atomic_init((p), (v))
#endif

#define CMD_RING 128
#define FIN_RING 64
#define IMA_BLOCK 1024
#define MP3_DECODERS 2
#define OUT_RATE 44100

enum { CMD_PLAY = 1, CMD_STOP, CMD_STOP_ALL, CMD_VOLUME };

typedef struct cmd {
    uint8_t type;
    uint8_t slot;
    uint32_t id, sound, start, end;
    int plays;
    int32_t gl, gr; /* gains, 8.8 fixed point */
} cmd;

typedef struct voice {
    uint32_t id;      /* 0 = idle */
    const swf_sound *s;
    uint32_t pos;     /* next source frame to read */
    uint32_t start, end;
    int plays;
    uint32_t frac, step;
    int32_t prev[2], cur[2];
    int32_t gl, gr;
    /* IMA */
    int32_t block;
    int16_t dec[IMA_BLOCK * 2];
    /* MP3 */
    int mp3;          /* decoder index, -1 = none */
    uint32_t mp3_off;
    int16_t pcm[MP3_MAX_SAMPLES_PER_FRAME];
    uint32_t pcm_frames, pcm_index, pcm_channels;
} voice;

struct swf_mixer {
    const swf_assets *a;
    cmd ring[CMD_RING];
    shared_u32 cmd_head, cmd_tail;       /* render reads head, control writes tail */
    shared_u32 fin[FIN_RING];
    shared_u32 fin_head, fin_tail;       /* control reads head, render writes tail */
    shared_u32 pub_id[SWF_MIXER_VOICES], pub_pos[SWF_MIXER_VOICES];
    /* control thread */
    uint32_t next_id;
    uint32_t slot_id[SWF_MIXER_VOICES];
    bool slot_mp3[SWF_MIXER_VOICES];
    bool warned;
    /* render thread */
    voice v[SWF_MIXER_VOICES];
    mp3_decoder *mp3[MP3_DECODERS];
    bool mp3_busy[MP3_DECODERS];
    int32_t acc[1024 * 2];
};

swf_mixer *swf_mixer_new(const swf_assets *a)
{
    swf_mixer *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    m->a = a;
    INIT(&m->cmd_head, 0);
    INIT(&m->cmd_tail, 0);
    INIT(&m->fin_head, 0);
    INIT(&m->fin_tail, 0);
    for (int i = 0; i < FIN_RING; i++) INIT(&m->fin[i], 0);
    for (int i = 0; i < SWF_MIXER_VOICES; i++) {
        INIT(&m->pub_id[i], 0);
        INIT(&m->pub_pos[i], 0);
        m->v[i].mp3 = -1;
    }
    bool need_mp3 = false;
    for (uint32_t i = 0; i < a->sound_count; i++) need_mp3 |= a->sounds[i].format == SWF_SOUND_MP3;
    for (int i = 0; need_mp3 && i < MP3_DECODERS; i++) {
        m->mp3[i] = mp3_decoder_new();
        if (!m->mp3[i]) {
            swf_mixer_free(m);
            return NULL;
        }
    }
    return m;
}

void swf_mixer_free(swf_mixer *m)
{
    if (!m) return;
    for (int i = 0; i < MP3_DECODERS; i++) mp3_decoder_free(m->mp3[i]);
    free(m);
}

/* --- control thread ------------------------------------------------------------------ */

static bool push(swf_mixer *m, const cmd *c)
{
    uint32_t tail = LOAD(&m->cmd_tail), head = LOAD_ACQ(&m->cmd_head);
    if (tail - head >= CMD_RING) {
        if (!m->warned) {
            m->warned = true;
            plat_log("mixer: command queue full; sound commands dropped");
        }
        return false;
    }
    m->ring[tail % CMD_RING] = *c;
    STORE_REL(&m->cmd_tail, tail + 1);
    return true;
}

static int32_t gain(float g)
{
    if (!(g > 0)) return 0;
    if (g > 4) g = 4;
    return (int32_t)(g * 256.0f + 0.5f);
}

static int slot_of(const swf_mixer *m, uint32_t id)
{
    if (!id) return -1;
    for (int i = 0; i < SWF_MIXER_VOICES; i++)
        if (m->slot_id[i] == id) return i;
    return -1;
}

uint32_t swf_mixer_play(swf_mixer *m, uint32_t sound, uint32_t start, uint32_t end, int plays, float left,
                        float right)
{
    if (!m || sound >= m->a->sound_count) return 0;
    const swf_sound *s = &m->a->sounds[sound];
    if (s->frames == 0 || start >= s->frames || s->rate == 0) return 0;
    bool mp3 = s->format == SWF_SOUND_MP3;
    int free_slot = -1, mp3_in_use = 0;
    for (int i = 0; i < SWF_MIXER_VOICES; i++) {
        if (!m->slot_id[i] && free_slot < 0) free_slot = i;
        if (m->slot_id[i] && m->slot_mp3[i]) mp3_in_use++;
    }
    if (free_slot < 0 || (mp3 && mp3_in_use >= MP3_DECODERS)) return 0;
    uint32_t id = ++m->next_id;
    if (!id) id = ++m->next_id;
    cmd c = {CMD_PLAY, (uint8_t)free_slot, id, sound, start, end, plays, gain(left), gain(right)};
    if (!push(m, &c)) return 0;
    m->slot_id[free_slot] = id;
    m->slot_mp3[free_slot] = mp3;
    return id;
}

void swf_mixer_stop(swf_mixer *m, uint32_t voice)
{
    int slot = m ? slot_of(m, voice) : -1;
    if (slot < 0) return;
    cmd c = {CMD_STOP, (uint8_t)slot, voice, 0, 0, 0, 0, 0, 0};
    if (push(m, &c)) m->slot_id[slot] = 0;
}

void swf_mixer_stop_all(swf_mixer *m)
{
    if (!m) return;
    cmd c = {CMD_STOP_ALL, 0, 0, 0, 0, 0, 0, 0, 0};
    if (push(m, &c)) memset(m->slot_id, 0, sizeof m->slot_id);
}

void swf_mixer_set_volume(swf_mixer *m, uint32_t voice, float left, float right)
{
    int slot = m ? slot_of(m, voice) : -1;
    if (slot < 0) return;
    cmd c = {CMD_VOLUME, (uint8_t)slot, voice, 0, 0, 0, 0, gain(left), gain(right)};
    (void)push(m, &c);
}

bool swf_mixer_active(const swf_mixer *m, uint32_t voice)
{
    return m && slot_of(m, voice) >= 0;
}

uint32_t swf_mixer_position(const swf_mixer *m, uint32_t voice)
{
    int slot = m ? slot_of(m, voice) : -1;
    if (slot < 0) return 0;
    swf_mixer *mm = (swf_mixer *)m;
    if (LOAD(&mm->pub_id[slot]) != voice) return 0;
    return LOAD(&mm->pub_pos[slot]);
}

uint32_t swf_mixer_poll_finished(swf_mixer *m, uint32_t *ids, uint32_t max)
{
    if (!m) return 0;
    uint32_t n = 0, head = LOAD(&m->fin_head), tail = LOAD_ACQ(&m->fin_tail);
    while (head != tail && n < max) {
        uint32_t id = LOAD(&m->fin[head % FIN_RING]);
        head++;
        int slot = slot_of(m, id);
        if (slot >= 0) {
            m->slot_id[slot] = 0;
            ids[n++] = id;
        }
    }
    STORE_REL(&m->fin_head, head);
    return n;
}

/* --- render thread ------------------------------------------------------------------ */

static void ima_decode_block(voice *v, uint32_t block)
{
    const swf_sound *s = v->s;
    uint32_t ch = s->channels ? s->channels : 1, block_bytes = 4 + IMA_BLOCK / 2;
    size_t off = (size_t)block * block_bytes * ch;
    static const int16_t STEP[89] = {
        7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
        73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449,
        494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272,
        2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493,
        10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
    static const int8_t INDEX[8] = {-1, -1, -1, -1, 2, 4, 6, 8};
    for (uint32_t c = 0; c < ch; c++) {
        const uint8_t *b = s->data + off + c * block_bytes;
        if (off + (c + 1) * block_bytes > s->data_size) {
            for (uint32_t k = 0; k < IMA_BLOCK; k++) v->dec[k * ch + c] = 0;
            continue;
        }
        int32_t pred = (int16_t)(uint16_t)(b[0] | (b[1] << 8)), idx = b[2] > 88 ? 88 : b[2];
        for (uint32_t k = 0; k < IMA_BLOCK; k++) {
            uint8_t byte = b[4 + k / 2], nib = (k & 1) ? (uint8_t)(byte >> 4) : (uint8_t)(byte & 15);
            int32_t step = STEP[idx], diff = step >> 3;
            if (nib & 1) diff += step >> 2;
            if (nib & 2) diff += step >> 1;
            if (nib & 4) diff += step;
            pred = (nib & 8) ? pred - diff : pred + diff;
            if (pred > 32767) pred = 32767;
            if (pred < -32768) pred = -32768;
            idx += INDEX[nib & 7];
            if (idx < 0) idx = 0;
            if (idx > 88) idx = 88;
            v->dec[k * ch + c] = (int16_t)pred;
        }
    }
    v->block = (int32_t)block;
}

static bool mp3_next_frame(swf_mixer *m, voice *v)
{
    const swf_sound *s = v->s;
    while (v->mp3_off < s->data_size) {
        uint32_t used = 0, channels = 0;
        int n = mp3_decode(m->mp3[v->mp3], s->data + v->mp3_off, s->data_size - v->mp3_off, v->pcm, &used,
                           &channels);
        if (used == 0) return false;
        v->mp3_off += used;
        if (n > 0) {
            v->pcm_frames = (uint32_t)n;
            v->pcm_channels = channels ? channels : 1;
            v->pcm_index = 0;
            return true;
        }
    }
    return false;
}

/* Positions an MP3 voice at `frame`: frames before it are skipped by their headers, and
 * one frame ahead is decoded and dropped to fill the bit reservoir. */
static void mp3_seek(swf_mixer *m, voice *v, uint32_t frame)
{
    mp3_decoder_reset(m->mp3[v->mp3]);
    const swf_sound *s = v->s;
    uint32_t off = 0, at = 0, prev_off = 0, prev_at = 0;
    while (off < s->data_size) {
        uint32_t len = 0, samples = 0;
        if (!mp3_frame_info(s->data + off, s->data_size - off, &len, &samples)) break;
        if (at + samples > frame) break;
        prev_off = off;
        prev_at = at;
        off += len;
        at += samples;
    }
    v->mp3_off = at > 0 ? prev_off : off;
    uint32_t skip = frame - (at > 0 ? prev_at : at);
    v->pcm_frames = v->pcm_index = 0;
    while (skip > 0 && mp3_next_frame(m, v)) {
        uint32_t take = skip < v->pcm_frames ? skip : v->pcm_frames;
        v->pcm_index = take;
        skip -= take;
    }
}

static void seek(swf_mixer *m, voice *v, uint32_t frame)
{
    v->pos = frame;
    v->block = -1;
    if (v->s->format == SWF_SOUND_MP3) mp3_seek(m, v, frame);
}

static bool read_frame(swf_mixer *m, voice *v, int32_t out[2])
{
    const swf_sound *s = v->s;
    uint32_t limit = v->end && v->end < s->frames ? v->end : s->frames;
    if (v->pos >= limit) {
        if (v->plays <= 1) return false;
        v->plays--;
        seek(m, v, v->start);
    }
    uint32_t ch = s->channels == 2 ? 2 : 1;
    switch (s->format) {
    case SWF_SOUND_PCM16: {
        size_t at = (size_t)v->pos * ch * 2;
        if (at + ch * 2 > s->data_size) return false;
        const uint8_t *p = s->data + at;
        out[0] = (int16_t)(uint16_t)(p[0] | (p[1] << 8));
        out[1] = ch == 2 ? (int16_t)(uint16_t)(p[2] | (p[3] << 8)) : out[0];
        break;
    }
    case SWF_SOUND_IMA4: {
        uint32_t block = v->pos / IMA_BLOCK;
        if ((int32_t)block != v->block) ima_decode_block(v, block);
        uint32_t k = v->pos % IMA_BLOCK;
        out[0] = v->dec[k * ch];
        out[1] = ch == 2 ? v->dec[k * ch + 1] : out[0];
        break;
    }
    case SWF_SOUND_MP3:
        if (v->pcm_index >= v->pcm_frames && !mp3_next_frame(m, v)) return false;
        out[0] = v->pcm[v->pcm_index * v->pcm_channels];
        out[1] = v->pcm_channels == 2 ? v->pcm[v->pcm_index * 2 + 1] : out[0];
        v->pcm_index++;
        break;
    default:
        return false;
    }
    v->pos++;
    return true;
}

static void release(swf_mixer *m, int slot, bool finished)
{
    voice *v = &m->v[slot];
    if (v->mp3 >= 0) m->mp3_busy[v->mp3] = false;
    v->mp3 = -1;
    if (finished && v->id) {
        uint32_t tail = LOAD(&m->fin_tail), head = LOAD_ACQ(&m->fin_head);
        if (tail - head < FIN_RING) {
            STORE(&m->fin[tail % FIN_RING], v->id);
            STORE_REL(&m->fin_tail, tail + 1);
        }
    }
    v->id = 0;
    STORE(&m->pub_id[slot], 0);
}

static void start(swf_mixer *m, const cmd *c)
{
    voice *v = &m->v[c->slot];
    if (v->id) release(m, c->slot, false);
    const swf_sound *s = &m->a->sounds[c->sound];
    v->s = s;
    v->start = c->start;
    v->end = c->end;
    v->plays = c->plays < 1 ? 1 : c->plays;
    v->gl = c->gl;
    v->gr = c->gr;
    v->mp3 = -1;
    if (s->format == SWF_SOUND_MP3) {
        for (int i = 0; i < MP3_DECODERS; i++)
            if (m->mp3[i] && !m->mp3_busy[i]) {
                v->mp3 = i;
                m->mp3_busy[i] = true;
                break;
            }
        if (v->mp3 < 0) {
            v->id = c->id;
            release(m, c->slot, true);
            return;
        }
    }
    v->step = (uint32_t)(((uint64_t)s->rate << 16) / OUT_RATE);
    v->frac = 0;
    v->id = c->id;
    seek(m, v, c->start);
    if (!read_frame(m, v, v->cur)) {
        release(m, c->slot, true);
        return;
    }
    v->prev[0] = v->cur[0];
    v->prev[1] = v->cur[1];
    STORE(&m->pub_pos[c->slot], v->pos);
    STORE(&m->pub_id[c->slot], v->id);
}

static void apply_commands(swf_mixer *m)
{
    uint32_t head = LOAD(&m->cmd_head), tail = LOAD_ACQ(&m->cmd_tail);
    while (head != tail) {
        const cmd *c = &m->ring[head % CMD_RING];
        switch (c->type) {
        case CMD_PLAY: start(m, c); break;
        case CMD_STOP:
            if (m->v[c->slot].id == c->id) release(m, c->slot, false);
            break;
        case CMD_STOP_ALL:
            for (int i = 0; i < SWF_MIXER_VOICES; i++)
                if (m->v[i].id) release(m, i, false);
            break;
        case CMD_VOLUME:
            if (m->v[c->slot].id == c->id) {
                m->v[c->slot].gl = c->gl;
                m->v[c->slot].gr = c->gr;
            }
            break;
        default: break;
        }
        head++;
    }
    STORE_REL(&m->cmd_head, head);
}

static void mix_voice(swf_mixer *m, int slot, int32_t *acc, int frames)
{
    voice *v = &m->v[slot];
    for (int i = 0; i < frames; i++) {
        while (v->frac >= 65536u) {
            v->prev[0] = v->cur[0];
            v->prev[1] = v->cur[1];
            if (!read_frame(m, v, v->cur)) {
                release(m, slot, true);
                return;
            }
            v->frac -= 65536u;
        }
        int32_t f = (int32_t)v->frac;
        int32_t l = v->prev[0] + (int32_t)(((int64_t)(v->cur[0] - v->prev[0]) * f) >> 16);
        int32_t r = v->prev[1] + (int32_t)(((int64_t)(v->cur[1] - v->prev[1]) * f) >> 16);
        acc[i * 2] += (l * v->gl) >> 8;
        acc[i * 2 + 1] += (r * v->gr) >> 8;
        v->frac += v->step;
    }
    STORE(&m->pub_pos[slot], v->pos);
}

void swf_mixer_render(int16_t *out, int frames, void *mixer)
{
    swf_mixer *m = mixer;
    apply_commands(m);
    while (frames > 0) {
        int n = frames > 1024 ? 1024 : frames;
        memset(m->acc, 0, (size_t)n * 2 * sizeof m->acc[0]);
        for (int s = 0; s < SWF_MIXER_VOICES; s++)
            if (m->v[s].id) mix_voice(m, s, m->acc, n);
        for (int i = 0; i < n * 2; i++) {
            int32_t x = m->acc[i];
            out[i] = (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
        }
        out += n * 2;
        frames -= n;
    }
}
