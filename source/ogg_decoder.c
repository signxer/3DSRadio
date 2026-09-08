#include "ogg_decoder.h"

#include <math.h>
#include <string.h>

#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_INTEGER_CONVERSION
#define STB_VORBIS_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wtautological-compare"
#pragma GCC diagnostic ignored "-Wshadow"
#include "stb_vorbis.c"
#pragma GCC diagnostic pop

void ogg_decoder_init(OggDecoder *decoder) {
    if (decoder) memset(decoder, 0, sizeof(*decoder));
}

void ogg_decoder_close(OggDecoder *decoder) {
    if (!decoder) return;
    if (decoder->handle) stb_vorbis_close((stb_vorbis *)decoder->handle);
    memset(decoder, 0, sizeof(*decoder));
}

static int16_t float_to_pcm16(float value) {
    if (value > 1.0f) value = 1.0f;
    if (value < -1.0f) value = -1.0f;
    return (int16_t)(value * (value < 0.0f ? 32768.0f : 32767.0f));
}

int ogg_decoder_decode(OggDecoder *decoder, const uint8_t *data, size_t length,
                       size_t *used, int16_t *output, int max_samples) {
    int consumed = 0;
    int channels = 0;
    int frames = 0;
    float **samples = NULL;

    if (used) *used = 0;
    if (!decoder || !data || !length || !output || max_samples <= 0) return 0;

    if (!decoder->handle) {
        int error = 0;
        stb_vorbis *vorbis = stb_vorbis_open_pushdata(
            data, (int)length, &consumed, &error, NULL);
        if (!vorbis) return 0;
        decoder->handle = vorbis;
        stb_vorbis_info info = stb_vorbis_get_info(vorbis);
        decoder->channels = info.channels > 0 ? info.channels : 2;
        decoder->sample_rate = info.sample_rate > 0 ? info.sample_rate : 44100;
    }

    int header_used = consumed;
    if (header_used >= (int)length) {
        if (used) *used = (size_t)header_used;
        return 0;
    }

    consumed = stb_vorbis_decode_frame_pushdata(
        (stb_vorbis *)decoder->handle, data + header_used,
        (int)length - header_used,
        &channels, &samples, &frames);
    if (consumed < 0) consumed = 0;
    if (used) *used = (size_t)header_used + (size_t)consumed;
    if (frames <= 0 || !samples) return 0;

    if (channels <= 0) channels = decoder->channels;
    if (channels > 2) channels = 2;
    if (frames * channels > max_samples)
        frames = max_samples / channels;

    for (int frame = 0; frame < frames; frame++) {
        for (int channel = 0; channel < channels; channel++) {
            output[frame * channels + channel] =
                float_to_pcm16(samples[channel][frame]);
        }
    }
    return frames * channels;
}
