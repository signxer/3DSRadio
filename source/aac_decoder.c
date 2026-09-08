#include "aac_decoder.h"

#include <string.h>

#ifdef HAVE_FAAD2
#include <neaacdec.h>
#endif

void aac_decoder_init(AacDecoder *decoder) {
    if (decoder) memset(decoder, 0, sizeof(*decoder));
}

void aac_decoder_close(AacDecoder *decoder) {
    if (!decoder) return;
#ifdef HAVE_FAAD2
    if (decoder->handle) NeAACDecClose((NeAACDecHandle)decoder->handle);
#endif
    memset(decoder, 0, sizeof(*decoder));
}

bool aac_decoder_available(void) {
#ifdef HAVE_FAAD2
    return true;
#else
    return false;
#endif
}

int aac_decoder_decode(AacDecoder *decoder, const uint8_t *data, size_t length,
                       size_t *used, int16_t *output, int max_samples) {
    if (used) *used = 0;
    if (!decoder || !data || length == 0 || !output || max_samples <= 0)
        return 0;

#ifdef HAVE_FAAD2
    NeAACDecHandle handle = (NeAACDecHandle)decoder->handle;
    if (!handle) {
        handle = NeAACDecOpen();
        if (!handle) return 0;

        NeAACDecConfigurationPtr config = NeAACDecGetCurrentConfiguration(handle);
        if (config) {
            config->outputFormat = FAAD_FMT_16BIT;
            config->downMatrix = 1;
            config->useOldADTSFormat = 0;
            NeAACDecSetConfiguration(handle, config);
        }

        unsigned long sample_rate = 0;
        unsigned char channels = 0;
        long init_used = NeAACDecInit(handle, (unsigned char *)data,
                                      (unsigned long)length,
                                      &sample_rate, &channels);
        if (init_used < 0) {
            NeAACDecClose(handle);
            return 0;
        }
        decoder->handle = handle;
        decoder->initialized = true;
        decoder->sample_rate = (int)sample_rate;
        decoder->channels = channels > 0 ? (int)channels : 2;
        if (used) *used = (size_t)init_used;

        /* NeAACDecInit consumes the ADTS header and may not return a frame.
         * Leave the caller with a clean staging buffer for the next pass. */
        if ((size_t)init_used >= length) return 0;
        data += init_used;
        length -= (size_t)init_used;
    }

    NeAACDecFrameInfo info;
    memset(&info, 0, sizeof(info));
    void *decoded = NeAACDecDecode(handle, &info, (unsigned char *)data,
                                   (unsigned long)length);
    if (info.bytesconsumed > 0 && used)
        *used += (size_t)info.bytesconsumed;
    if (info.error != 0 || !decoded || info.samples == 0 || info.channels == 0)
        return 0;

    decoder->sample_rate = info.samplerate > 0 ? (int)info.samplerate
                                                : decoder->sample_rate;
    decoder->channels = info.channels > 0 ? (int)info.channels
                                           : decoder->channels;

    int total_samples = (int)info.samples;
    if (total_samples > max_samples) total_samples = max_samples;
    memcpy(output, decoded, (size_t)total_samples * sizeof(int16_t));
    return total_samples;
#else
    (void)decoder;
    (void)data;
    (void)length;
    (void)output;
    (void)max_samples;
    return 0;
#endif
}
