/* MP3 decoding for the mixer: minimp3 (CC0, third_party/minimp3) behind a small API, plus
 * a Layer III frame header parser used to seek without decoding. */
#ifndef SWFRT_MP3_H
#define SWFRT_MP3_H

#include <stdbool.h>
#include <stdint.h>

#define MP3_MAX_SAMPLES_PER_FRAME (1152 * 2)

typedef struct mp3_decoder mp3_decoder;

mp3_decoder *mp3_decoder_new(void);
void mp3_decoder_free(mp3_decoder *d);
void mp3_decoder_reset(mp3_decoder *d);
/* Decodes the next frame of `data` into `pcm` (interleaved). Returns samples per channel
 * (0 when the bytes held no audio yet); `*used` is 0 when no further frame was found. */
int mp3_decode(mp3_decoder *d, const uint8_t *data, uint32_t size, int16_t *pcm, uint32_t *used,
               uint32_t *channels);
/* Length in bytes and samples per channel of the Layer III frame starting at `p`. */
bool mp3_frame_info(const uint8_t *p, uint32_t size, uint32_t *len, uint32_t *samples);

#endif
