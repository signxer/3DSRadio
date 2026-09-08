#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    void *handle;
    int channels;
    int sample_rate;
} OggDecoder;

void ogg_decoder_init(OggDecoder *decoder);
void ogg_decoder_close(OggDecoder *decoder);

/* Decode from a contiguous push-data block.  `used` is always advanced by
 * the amount stb_vorbis consumed, even when more bytes are needed.  The
 * return value is interleaved PCM16 sample count (not frame count). */
int ogg_decoder_decode(OggDecoder *decoder, const uint8_t *data, size_t length,
                       size_t *used, int16_t *output, int max_samples);
