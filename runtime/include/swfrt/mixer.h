/* Software mixer for the movie's sounds: PCM16, IMA ADPCM and MP3 (minimp3) voices,
 * resampled to the platform's 44.1 kHz stereo output.
 *
 * Threading: one control thread (the game loop) calls the commands and status queries;
 * one render thread (the platform audio callback) calls swf_mixer_render. They share only
 * single-writer words and two single-producer rings, so no locks are needed (the PSP
 * has no LL/SC). Headless runs call swf_mixer_render on the game thread instead. */
#ifndef SWFRT_MIXER_H
#define SWFRT_MIXER_H

#include "swfrt/assets.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SWF_MIXER_VOICES 32

typedef struct swf_mixer swf_mixer;

swf_mixer *swf_mixer_new(const swf_assets *a);
void swf_mixer_free(swf_mixer *m);

/* Render thread: fills `frames` interleaved stereo s16 frames. Matches plat_audio_fn. */
void swf_mixer_render(int16_t *out, int frames, void *mixer);

/* Starts a sound at `start` (sample frames at the sound's rate), playing `plays` times
 * (at least once), ending early at `end` frames when non-zero. Returns a voice id, or 0
 * when the sound is invalid or every voice is busy. */
uint32_t swf_mixer_play(swf_mixer *m, uint32_t sound, uint32_t start, uint32_t end, int plays, float left,
                        float right);
void swf_mixer_stop(swf_mixer *m, uint32_t voice);
void swf_mixer_stop_all(swf_mixer *m);
void swf_mixer_set_volume(swf_mixer *m, uint32_t voice, float left, float right);

/* Status as last published by the render thread. */
bool swf_mixer_active(const swf_mixer *m, uint32_t voice);
/* Playback position of a voice in sample frames of its sound (0 if unknown). */
uint32_t swf_mixer_position(const swf_mixer *m, uint32_t voice);
/* Voices that ended on their own since the last call (not stopped ones). */
uint32_t swf_mixer_poll_finished(swf_mixer *m, uint32_t *ids, uint32_t max);

#ifdef __cplusplus
}
#endif

#endif
