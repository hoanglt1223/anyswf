/* minimp3 is compiled here with its own settings (see runtime/CMakeLists.txt). */
#include "mp3.h"

#include <stdlib.h>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#include "minimp3.h"

struct mp3_decoder {
    mp3dec_t dec;
};

mp3_decoder *mp3_decoder_new(void)
{
    mp3_decoder *d = malloc(sizeof *d);
    if (d) mp3dec_init(&d->dec);
    return d;
}

void mp3_decoder_free(mp3_decoder *d)
{
    free(d);
}

void mp3_decoder_reset(mp3_decoder *d)
{
    mp3dec_init(&d->dec);
}

int mp3_decode(mp3_decoder *d, const uint8_t *data, uint32_t size, int16_t *pcm, uint32_t *used,
               uint32_t *channels)
{
    mp3dec_frame_info_t info;
    int n = mp3dec_decode_frame(&d->dec, data, (int)size, pcm, &info);
    *used = info.frame_bytes > 0 ? (uint32_t)info.frame_bytes : 0;
    *channels = info.channels > 0 ? (uint32_t)info.channels : 1;
    return n;
}

bool mp3_frame_info(const uint8_t *p, uint32_t size, uint32_t *len, uint32_t *samples)
{
    static const uint16_t BR_V1[15] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
    static const uint16_t BR_V2[15] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};
    static const uint32_t SR[3][3] = {{44100, 48000, 32000}, {22050, 24000, 16000}, {11025, 12000, 8000}};
    if (size < 4 || p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) return false;
    int version = (p[1] >> 3) & 3, layer = (p[1] >> 1) & 3;
    int bri = p[2] >> 4, sri = (p[2] >> 2) & 3, pad = (p[2] >> 1) & 1;
    if (version == 1 || layer != 1 || bri == 0 || bri == 15 || sri == 3) return false;
    bool v1 = version == 3;
    uint32_t rate = SR[v1 ? 0 : version == 2 ? 1 : 2][sri];
    uint32_t br = (v1 ? BR_V1 : BR_V2)[bri] * 1000u;
    *len = (v1 ? 144u : 72u) * br / rate + (uint32_t)pad;
    *samples = v1 ? 1152u : 576u;
    return *len > 4 && *len <= size;
}
