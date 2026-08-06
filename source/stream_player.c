#include "stream_player.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

/* minimp3 decoder - single header library */
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_STDIO
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#include "minimp3.h"
#pragma GCC diagnostic pop

/* ======================================================================
 * Streaming Audio Player
 *
 * Architecture (fixed to match ClouDS-Music-FA's proven approach):
 * - Background thread downloads raw MP3 data from HTTP stream
 * - Main thread (stream_player_update) decodes MP3 → PCM → ndsp
 * - 4 wave buffers for double/triple buffering
 * - CRITICAL: DSP_FlushDataCache + ndspChnWaveBufAdd required for sound
 * ====================================================================== */

#define NUM_WAVE_BUFS 4
#define PCM_BUF_SAMPLES 8192   /* Samples per wave buffer (per channel) */
#define DOWNLOAD_BUF_SIZE (256 * 1024)  /* 256 KB raw MP3 download buffer */

struct StreamPlayer {
    /* Download state */
    CURL *curl;
    volatile bool download_active;
    volatile bool download_done;
    Thread download_thread;
    char download_error[256];

    /* Raw MP3 ring buffer */
    uint8_t *mp3_buffer;
    volatile size_t mp3_write_pos;
    volatile size_t mp3_read_pos;
    volatile bool mp3_eof;

    /* Decoder */
    mp3dec_t mp3d;
    bool decoder_initialized;

    /* NDSP audio output */
    ndspWaveBuf wave_bufs[NUM_WAVE_BUFS];
    int16_t *pcm_data[NUM_WAVE_BUFS];  /* linearAlloc'd PCM buffers */
    int next_buf;          /* Next wave buffer to fill */
    int active_channels;   /* 1 or 2 */
    int sample_rate;       /* e.g. 44100 */

    /* Decode staging buffer: accumulates raw MP3 bytes from the
     * ring buffer. After each decode, only info.frame_bytes bytes
     * are consumed — NOT the entire read. This prevents the data
     * discard bug that caused "chalk-writing noise." */
    uint8_t stage[65536];
    size_t stage_len;
    size_t stage_off;

    /* State */
    volatile bool playing;
    volatile bool paused;
    volatile bool buffering;
    float volume;
};

/* Write callback for curl download */
struct DownloadCtx {
    StreamPlayer *player;
};

static size_t download_write_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    struct DownloadCtx *ctx = (struct DownloadCtx *)userp;
    StreamPlayer *p = ctx->player;
    size_t total = size * nmemb;
    uint8_t *data = (uint8_t *)contents;

    for (size_t i = 0; i < total; i++) {
        size_t next_write = (p->mp3_write_pos + 1) % DOWNLOAD_BUF_SIZE;
        /* If buffer full, wait briefly for decoder to consume */
        if (next_write == p->mp3_read_pos) {
            svcSleepThread(10000); /* 10 us */
            next_write = (p->mp3_write_pos + 1) % DOWNLOAD_BUF_SIZE;
            if (next_write == p->mp3_read_pos) {
                /* Still full - drop this chunk to avoid blocking curl */
                return total;
            }
        }
        p->mp3_buffer[p->mp3_write_pos] = data[i];
        p->mp3_write_pos = next_write;
    }
    return total;
}

static void download_thread_func(void *arg) {
    StreamPlayer *p = (StreamPlayer *)arg;
    CURLcode res = curl_easy_perform(p->curl);
    if (res != CURLE_OK) {
        snprintf(p->download_error, sizeof(p->download_error),
                 "Download error: %s", curl_easy_strerror(res));
    }
    p->mp3_eof = true;
    p->download_active = false;
}

/* ======================================================================
 * Configure NDSP channel for the detected audio format.
 * Mirrors ClouDS-Music-FA's configure_channel() pattern:
 * reset → interp → rate → format → mix
 * ====================================================================== */

static void configure_ndsp_channel(StreamPlayer *p) {
    ndspChnReset(0);
    ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
    ndspChnSetRate(0, (float)p->sample_rate);
    ndspChnSetFormat(0, p->active_channels == 2 ?
                     NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);

    /* Full 12-element mix array — remaining entries must be zero */
    float mix[12] = {0};
    mix[0] = p->volume;
    mix[1] = p->volume;
    ndspChnSetMix(0, mix);
}

/* ======================================================================
 * Submit a filled PCM buffer to the NDSP hardware.
 *
 * CRITICAL: DSP_FlushDataCache is REQUIRED before ndspChnWaveBufAdd.
 * The ARM9 CPU and DSP have separate data caches. Without flushing,
 * the DSP may read stale or zeroed memory, resulting in silence.
 *
 * CRITICAL: ndspChnWaveBufAdd is REQUIRED to actually submit the
 * buffer to the DSP for playback. Setting buf->status manually does
 * nothing — it only updates a struct field without telling the hardware.
 * ====================================================================== */

static void submit_wave_buffer(ndspWaveBuf *buf, int16_t *pcm_data,
                                int total_samples, int channels) {
    memset(buf, 0, sizeof(*buf));
    buf->data_pcm16 = pcm_data;
    buf->nsamples = (u32)(total_samples / channels);
    buf->looping = false;

    /* Flush CPU data cache so the DSP sees our PCM data */
    DSP_FlushDataCache(pcm_data, (u32)total_samples * sizeof(int16_t));

    /* Actually submit to the NDSP channel 0 hardware queue */
    ndspChnWaveBufAdd(0, buf);
}

/* ======================================================================
 * Public API
 * ====================================================================== */

StreamPlayer *stream_player_create(void) {
    StreamPlayer *p = calloc(1, sizeof(StreamPlayer));
    if (!p) return NULL;

    /* Allocate MP3 download ring buffer */
    p->mp3_buffer = malloc(DOWNLOAD_BUF_SIZE);
    if (!p->mp3_buffer) {
        free(p);
        return NULL;
    }

    /* Allocate PCM buffers from linear memory (required for NDSP DMA).
     * ndspWaveBuf data MUST be in linear memory, not regular heap. */
    for (int i = 0; i < NUM_WAVE_BUFS; i++) {
        p->pcm_data[i] = (int16_t *)linearAlloc(PCM_BUF_SAMPLES * sizeof(int16_t) * 2);
        if (!p->pcm_data[i]) {
            for (int j = 0; j < i; j++)
                linearFree(p->pcm_data[j]);
            free(p->mp3_buffer);
            free(p);
            return NULL;
        }
    }

    /* Initialize ndsp — this requires DSP firmware to be available.
     * On real hardware, DSP firmware is extracted from a donor console.
     * On emulators (Azahar/Citra), it's bundled with the emulator. */
    Result ndsp_result = ndspInit();
    if (R_FAILED(ndsp_result)) {
        for (int j = 0; j < NUM_WAVE_BUFS; j++)
            linearFree(p->pcm_data[j]);
        free(p->mp3_buffer);
        free(p);
        return NULL;
    }

    ndspSetOutputMode(NDSP_OUTPUT_STEREO);

    /* Initialize MP3 decoder */
    mp3dec_init(&p->mp3d);

    p->volume = 0.8f;
    p->active_channels = 2;
    p->sample_rate = 44100;
    return p;
}

int stream_player_play(StreamPlayer *p, const char *url) {
    if (!p || !url) return -1;

    /* Stop any current playback */
    if (p->playing) {
        stream_player_stop(p);
    }

    /* Reset state */
    p->mp3_write_pos = 0;
    p->mp3_read_pos = 0;
    p->mp3_eof = false;
    p->download_active = false;
    p->download_done = false;
    p->decoder_initialized = false;
    p->next_buf = 0;
    p->active_channels = 2; /* Assume stereo until first frame decoded */
    p->sample_rate = 44100;
    p->download_error[0] = '\0';

    /* Reset the MP3 decoder state between streams.
     * Without this, the synthesis filterbank carries stale
     * state from the previous stream, causing audio artifacts. */
    mp3dec_init(&p->mp3d);

    /* Reset the decode staging buffer for the new stream */
    p->stage_len = 0;
    p->stage_off = 0;

    /* Reset NDSP channel for the new stream */
    ndspChnReset(0);
    ndspChnWaveBufClear(0);
    ndspChnSetPaused(0, false);

    /* Clear all wave buffers */
    memset(&p->wave_bufs, 0, sizeof(p->wave_bufs));
    for (int i = 0; i < NUM_WAVE_BUFS; i++) {
        memset(p->pcm_data[i], 0, PCM_BUF_SAMPLES * sizeof(int16_t) * 2);
    }

    /* Start HTTP download */
    p->curl = curl_easy_init();
    if (!p->curl) return -1;

    curl_easy_setopt(p->curl, CURLOPT_URL, url);
    curl_easy_setopt(p->curl, CURLOPT_USERAGENT, "3DSRadio/1.0");
    curl_easy_setopt(p->curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(p->curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(p->curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(p->curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(p->curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(p->curl, CURLOPT_TIMEOUT, 0L); /* No timeout for streaming */

    struct DownloadCtx *ctx = malloc(sizeof(struct DownloadCtx));
    if (!ctx) {
        curl_easy_cleanup(p->curl);
        p->curl = NULL;
        return -1;
    }
    ctx->player = p;

    curl_easy_setopt(p->curl, CURLOPT_WRITEFUNCTION, download_write_cb);
    curl_easy_setopt(p->curl, CURLOPT_WRITEDATA, ctx);
    curl_easy_setopt(p->curl, CURLOPT_PRIVATE, ctx);

    p->download_active = true;
    p->playing = true;
    p->buffering = true;

    /* Start download thread at slightly higher priority than main */
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    p->download_thread = threadCreate(download_thread_func, p,
                                       32 * 1024, prio - 1, -2, true);

    return 0;
}

void stream_player_update(StreamPlayer *p) {
    if (!p || !p->playing || p->paused) return;

    uint8_t *stage = p->stage;
    size_t *stage_len = &p->stage_len;
    size_t *stage_off = &p->stage_off;

    /* Refill staging buffer from the ring buffer.
     * Compact first: move unconsumed data to the front. */
    if (*stage_off > 0 && *stage_off < *stage_len) {
        memmove(stage, stage + *stage_off, *stage_len - *stage_off);
    }
    *stage_len -= *stage_off;
    *stage_off = 0;

    /* Read new bytes from ring buffer into the staging buffer tail */
    while (*stage_len < sizeof(p->stage)) {
        if (p->mp3_read_pos == p->mp3_write_pos) {
            if (p->mp3_eof) break;
            break;
        }
        stage[(*stage_len)++] = p->mp3_buffer[p->mp3_read_pos];
        p->mp3_read_pos = (p->mp3_read_pos + 1) % DOWNLOAD_BUF_SIZE;
    }

    /* Check if we need to fill more wave buffers */
    for (int i = 0; i < NUM_WAVE_BUFS; i++) {
        int buf_idx = (p->next_buf + i) % NUM_WAVE_BUFS;
        ndspWaveBuf *buf = &p->wave_bufs[buf_idx];

        if (buf->status != NDSP_WBUF_DONE &&
            buf->status != NDSP_WBUF_FREE) {
            continue;
        }

        int16_t *out = p->pcm_data[buf_idx];
        int total_samples = 0;
        int max_samples = PCM_BUF_SAMPLES * 2;

        while (total_samples < max_samples) {
            /* Refill staging buffer if running low */
            if (*stage_len - *stage_off < 4096 &&
                !(p->mp3_eof && p->mp3_read_pos == p->mp3_write_pos)) {
                if (*stage_off > 0 && *stage_off < *stage_len) {
                    memmove(stage, stage + *stage_off,
                            *stage_len - *stage_off);
                }
                *stage_len -= *stage_off;
                *stage_off = 0;

                while (*stage_len < sizeof(p->stage)) {
                    if (p->mp3_read_pos == p->mp3_write_pos) {
                        if (p->mp3_eof) break;
                        break;
                    }
                    stage[(*stage_len)++] = p->mp3_buffer[p->mp3_read_pos];
                    p->mp3_read_pos = (p->mp3_read_pos + 1) % DOWNLOAD_BUF_SIZE;
                }
            }

            if (*stage_len - *stage_off == 0) {
                if (total_samples == 0) {
                    p->buffering = true;
                }
                break;
            }

            mp3dec_frame_info_t info;
            memset(&info, 0, sizeof(info));
            int samples = mp3dec_decode_frame(&p->mp3d,
                                              stage + *stage_off,
                                              (int)(*stage_len - *stage_off),
                                              out + total_samples,
                                              &info);

            if (samples > 0) {
                *stage_off += (size_t)info.frame_bytes;
                total_samples += samples * info.channels;

                if (!p->decoder_initialized) {
                    p->active_channels = info.channels;
                    p->sample_rate = info.hz;
                    p->decoder_initialized = true;
                    /* Always reconfigure: ndspChnReset() defaults
                     * may not match the actual stream format. */
                    configure_ndsp_channel(p);
                }
            } else if (info.frame_bytes > 0) {
                /* No sync found in scanned region — skip and retry */
                *stage_off += (size_t)info.frame_bytes;
            } else {
                /* Need more data for a full frame */
                break;
            }

            if (p->mp3_eof && p->mp3_read_pos == p->mp3_write_pos &&
                *stage_len - *stage_off < 1024) {
                break;
            }
        }

        if (total_samples == 0) {
            if (p->mp3_eof && p->mp3_read_pos == p->mp3_write_pos &&
                *stage_len - *stage_off == 0) {
                bool any_playing = false;
                for (int j = 0; j < NUM_WAVE_BUFS; j++) {
                    u32 status = p->wave_bufs[j].status;
                    if (status == NDSP_WBUF_QUEUED ||
                        status == NDSP_WBUF_PLAYING) {
                        any_playing = true;
                        break;
                    }
                }
                if (!any_playing) {
                    p->playing = false;
                    return;
                }
            }
            continue;
        }

        submit_wave_buffer(buf, out, total_samples, p->active_channels);

        float set_mix[12] = {0};
        set_mix[0] = p->volume;
        set_mix[1] = p->volume;
        ndspChnSetMix(0, set_mix);

        p->buffering = false;
        p->next_buf = (buf_idx + 1) % NUM_WAVE_BUFS;
    }
}

void stream_player_toggle_pause(StreamPlayer *p) {
    if (!p) return;
    p->paused = !p->paused;
    ndspChnSetPaused(0, p->paused);
}

void stream_player_stop(StreamPlayer *p) {
    if (!p) return;

    p->playing = false;

    /* Stop download thread */
    if (p->download_active) {
        p->download_active = false;
        threadJoin(p->download_thread, 10000000);
        threadFree(p->download_thread);
    }

    /* Clean up curl */
    if (p->curl) {
        struct DownloadCtx *ctx;
        curl_easy_getinfo(p->curl, CURLINFO_PRIVATE, &ctx);
        if (ctx) free(ctx);
        curl_easy_cleanup(p->curl);
        p->curl = NULL;
    }

    /* Stop ndsp channel */
    ndspChnWaveBufClear(0);
    ndspChnSetPaused(0, false);

    /* Reset wave buffers */
    memset(&p->wave_bufs, 0, sizeof(p->wave_bufs));

    p->paused = false;
    p->buffering = false;
}

void stream_player_destroy(StreamPlayer *p) {
    if (!p) return;

    stream_player_stop(p);

    ndspExit();

    for (int i = 0; i < NUM_WAVE_BUFS; i++) {
        if (p->pcm_data[i]) linearFree(p->pcm_data[i]);
    }
    free(p->mp3_buffer);
    free(p);
}

bool stream_player_is_playing(StreamPlayer *p) {
    return p && p->playing && !p->paused;
}

bool stream_player_is_paused(StreamPlayer *p) {
    return p && p->paused;
}

bool stream_player_is_buffering(StreamPlayer *p) {
    return p && p->buffering;
}

bool stream_player_is_finished(StreamPlayer *p) {
    return p && !p->playing && !p->download_active;
}

void stream_player_set_volume(StreamPlayer *p, float vol) {
    if (!p) return;
    if (vol < 0.0f) vol = 0.0f;
    if (vol > 1.0f) vol = 1.0f;
    p->volume = vol;
    /* Update ndsp master volume directly (like ClouDS does) */
    ndspSetMasterVol(vol);
}

float stream_player_get_volume(StreamPlayer *p) {
    return p ? p->volume : 0.0f;
}

const char *stream_player_error(StreamPlayer *p) {
    return p ? p->download_error : NULL;
}
