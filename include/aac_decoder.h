#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A small push-data wrapper around FAAD2.  The wrapper deliberately keeps
 * the rest of the player independent from FAAD2 so an MP3/OGG-only toolchain
 * remains a valid build. */
typedef struct {
    void *handle;
    bool initialized;
    int channels;
    int sample_rate;
} AacDecoder;

void aac_decoder_init(AacDecoder *decoder);
void aac_decoder_close(AacDecoder *decoder);
bool aac_decoder_available(void);

/* Decode one or more ADTS frames from a push-data block.
 * `used` is advanced by bytes consumed by FAAD2.  Return value is the number
 * of interleaved PCM16 samples written to `output`. */
int aac_decoder_decode(AacDecoder *decoder, const uint8_t *data, size_t length,
                       size_t *used, int16_t *output, int max_samples);

