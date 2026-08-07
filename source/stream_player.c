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
 * Architecture:
 * - Download thread pulls raw MP3 bytes from the HTTP stream into a ring
 *   buffer. It runs at LOWER priority than the main thread and NEVER drops
 *   bytes (it throttles instead), so it can't outpace the decoder and
 *   corrupt the MP3 stream with gaps.
 * - A DEDICATED decode thread decodes MP3 -> PCM and feeds the ndsp wave
 *   buffers. It runs at HIGHER priority than the render thread, so UI
 *   rendering / network / JSON work can never starve the audio feed.
 *   (Previously decoding happened on the render thread, so any main-thread
 *   stall let the DSP underrun — the "stutter/squeak" that buffer size
 *   alone cannot fix.)
 * - Priority order:  decode thread > main (render) > download thread
 * ====================================================================== */

#define NUM_WAVE_BUFS_MAX 5
#define PCM_BUF_SAMPLES_MAX 16384  /* Max samples per wave buffer (per channel) */
#define DOWNLOAD_BUF_MAX (512 * 1024)  /* 512 KB max raw MP3 download buffer */
#define STAGE_BUF_MAX 131072  /* 128 KB max staging buffer */

/* Buffer size presets (indexed by StreamBufSize enum) */
static const struct {
    int num_wave_bufs;
    int pcm_buf_samples;
    size_t download_buf_size;
    size_t stage_buf_size;
} buf_configs[] = {
    [STREAM_BUF_SMALL]  = { 3,  4096, 128 * 1024, 32768  },
    [STREAM_BUF_MEDIUM] = { 4,  8192, 256 * 1024, 65536  },
    [STREAM_BUF_LARGE]  = { 5, 16384, 512 * 1024, 131072 },
};

struct StreamPlayer {
    /* Buffer configuration */
    StreamBufSize bufsize;
    int num_wave_bufs;
    int pcm_buf_samples;
    size_t download_buf_size;

    /* Download state */
    volatile bool download_active;
    volatile bool download_done;
    Thread download_thread;
    char download_error[256];

    /* Incremented on every stream; lets a stale (leaked) download thread
     * from a previous stream detect that its ring buffer is gone. */
    u32 stream_epoch;

    /* Raw MP3 ring buffer */
    uint8_t *mp3_buffer;
    volatile size_t mp3_write_pos;
    volatile size_t mp3_read_pos;
    volatile bool mp3_eof;

    /* Decoder */
    mp3dec_t mp3d;
    bool decoder_initialized;

    /* NDSP audio output */
    ndspWaveBuf wave_bufs[NUM_WAVE_BUFS_MAX];
    int16_t *pcm_data[NUM_WAVE_BUFS_MAX];  /* linearAlloc'd PCM buffers */
    int active_channels;   /* 1 or 2 */
    int sample_rate;       /* e.g. 44100 */

    /* Decode thread */
    Thread decode_thread;

    /* Decode staging buffer: accumulates raw MP3 bytes from the ring
     * buffer. After each decode, only info.frame_bytes bytes are consumed
     * — NOT the entire read. This prevents the data-discard bug that
     * caused "chalk-writing noise." */
    uint8_t stage[STAGE_BUF_MAX];
    size_t stage_len;
    size_t stage_off;

    /* State */
    volatile bool playing;
    volatile bool paused;
    volatile bool buffering;
    float volume;
};

/* Write callback context for the curl download */
struct DownloadCtx {
    StreamPlayer *player;
    u32 epoch; /* stream_epoch captured when the download started */
};

/* Arguments captured at thread creation so the download thread owns its own
 * curl handle and context, and can free them at exit without reading shared
 * player state (a stale thread must never touch a newer stream's curl). */
struct DownloadThreadArg {
    StreamPlayer *player;
    CURL *curl;
    struct DownloadCtx *ctx;
};

/* ======================================================================
 * Ring buffer -> staging buffer.
 *
 * Compacts any unconsumed bytes to the front of the staging buffer, then
 * copies as much contiguous data as possible from the ring using memcpy
 * (the old per-byte copy with a modulo on every byte was slow enough to
 * stall the audio feed).
 * ====================================================================== */

static void ring_to_stage(StreamPlayer *p) {
    size_t *stage_len = &p->stage_len;
    size_t *stage_off = &p->stage_off;

    /* Compact: move unconsumed bytes to the front */
    if (*stage_off > 0) {
        size_t remaining = *stage_len - *stage_off;
        if (remaining > 0)
            memmove(p->stage, p->stage + *stage_off, remaining);
        *stage_len = remaining;
        *stage_off = 0;
    }

    /* Copy contiguous runs from the ring */
    size_t free_space = sizeof(p->stage) - *stage_len;
    if (free_space == 0) return;

    size_t write = p->mp3_write_pos;
    size_t read  = p->mp3_read_pos;
    size_t avail = (write + p->download_buf_size - read) % p->download_buf_size;
    if (avail == 0) return;

    size_t to_copy = avail < free_space ? avail : free_space;
    size_t first = p->download_buf_size - read;  /* bytes until ring wrap */

    if (to_copy <= first) {
        memcpy(p->stage + *stage_len, p->mp3_buffer + read, to_copy);
    } else {
        memcpy(p->stage + *stage_len, p->mp3_buffer + read, first);
        memcpy(p->stage + *stage_len + first, p->mp3_buffer, to_copy - first);
    }

    p->mp3_read_pos = (read + to_copy) % p->download_buf_size;
    *stage_len += to_copy;
}

/* Write callback for curl download.
 * Throttles instead of dropping: if the ring is full it waits (bounded),
 * and only skips a byte as a last resort. Never abandons a whole chunk. */
static size_t download_write_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    struct DownloadCtx *ctx = (struct DownloadCtx *)userp;
    StreamPlayer *p = ctx->player;

    /* Stop signal, or a stale thread from a previous stream */
    if (!p->download_active || ctx->epoch != p->stream_epoch)
        return 0; /* abort curl */

    size_t total = size * nmemb;
    uint8_t *data = (uint8_t *)contents;

    for (size_t i = 0; i < total; i++) {
        int spins = 0;
        while ((p->mp3_write_pos + 1) % p->download_buf_size == p->mp3_read_pos) {
            if (!p->download_active) return 0;
            if (++spins > 250) break;       /* ~5 ms cap */
            svcSleepThread(20000);          /* 20 us */
        }
        if ((p->mp3_write_pos + 1) % p->download_buf_size == p->mp3_read_pos) {
            /* Still full after waiting — skip a single byte; the decoder
             * will resync on the next frame header. Rare in practice. */
            continue;
        }
        p->mp3_buffer[p->mp3_write_pos] = data[i];
        p->mp3_write_pos = (p->mp3_write_pos + 1) % p->download_buf_size;
    }
    return total;
}

static void download_thread_func(void *arg) {
    struct DownloadThreadArg *ta = (struct DownloadThreadArg *)arg;
    StreamPlayer *p = ta->player;

    CURLcode res = curl_easy_perform(ta->curl);
    if (res != CURLE_OK && p->download_active &&
        ta->ctx->epoch == p->stream_epoch) {
        /* Only report genuine errors, not the abort we trigger on stop() */
        snprintf(p->download_error, sizeof(p->download_error),
                 "Download error: %s", curl_easy_strerror(res));
    }
    /* Only touch shared state if we're still the current stream. A stale
     * thread from a previous stream must never write mp3_eof into a newer
     * stream — that would cut the new stream off prematurely. */
    if (ta->ctx->epoch == p->stream_epoch) {
        p->mp3_eof = true;
        p->download_active = false;
        p->download_done = true;
    }

    /* The download thread owns its curl handle and context, and frees them
     * here so a stopped stream never leaves a dangling CURL* behind. It
     * uses the captured pointers, never p->curl — a stale thread from a
     * previous stream must not free a newer stream's handle. */
    curl_easy_cleanup(ta->curl);
    free(ta->ctx);
    free(ta);
}

/* ======================================================================
 * Configure NDSP channel for the detected audio format.
 * reset -> interp -> rate -> format -> mix
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
 * One decode pass: refill every finished wave buffer from the MP3 staging
 * buffer. Runs on the decode thread, independent of the render loop.
 * ====================================================================== */

static void decode_pass(StreamPlayer *p) {
    size_t *stage_len = &p->stage_len;
    size_t *stage_off = &p->stage_off;

    /* Compact + top up the staging buffer from the ring */
    ring_to_stage(p);

    /* Refill every wave buffer that has finished playing */
    for (int i = 0; i < p->num_wave_bufs; i++) {
        ndspWaveBuf *buf = &p->wave_bufs[i];
        if (buf->status != NDSP_WBUF_DONE && buf->status != NDSP_WBUF_FREE)
            continue;

        int16_t *out = p->pcm_data[i];
        int total_samples = 0;
        int max_samples = p->pcm_buf_samples * 2;

        while (total_samples < max_samples) {
            /* Refill staging buffer if running low */
            if (*stage_len - *stage_off < 4096)
                ring_to_stage(p);

            if (*stage_len - *stage_off == 0) {
                if (total_samples == 0)
                    p->buffering = true;
                break;
            }

            mp3dec_frame_info_t info;
            memset(&info, 0, sizeof(info));
            int samples = mp3dec_decode_frame(&p->mp3d,
                                              p->stage + *stage_off,
                                              (int)(*stage_len - *stage_off),
                                              out + total_samples, &info);

            if (samples > 0) {
                *stage_off += (size_t)info.frame_bytes;
                total_samples += samples * info.channels;

                if (!p->decoder_initialized) {
                    p->active_channels = info.channels;
                    p->sample_rate = info.hz;
                    p->decoder_initialized = true;
                    configure_ndsp_channel(p);
                } else if (info.hz != p->sample_rate ||
                           info.channels != p->active_channels) {
                    /* Stream format changed mid-flight (e.g. an ad break):
                     * resync the DSP to avoid pitch-shifted squeak. */
                    p->active_channels = info.channels;
                    p->sample_rate = info.hz;
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
                for (int j = 0; j < p->num_wave_bufs; j++) {
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
        p->buffering = false;
    }
}

/* Dedicated decode thread: keeps ndsp fed regardless of what the render
 * thread is doing. Runs at higher priority than the main thread. */
static void decode_thread_func(void *arg) {
    StreamPlayer *p = (StreamPlayer *)arg;
    while (p->playing) {
        if (p->paused) {
            svcSleepThread(20 * 1000 * 1000); /* 20 ms */
            continue;
        }
        decode_pass(p);
        svcSleepThread(4 * 1000 * 1000); /* 4 ms */
    }
}

/* ======================================================================
 * Public API
 * ====================================================================== */

StreamPlayer *stream_player_create_with_bufsize(StreamBufSize bufsize) {
    if (bufsize > STREAM_BUF_LARGE) bufsize = STREAM_BUF_MEDIUM;

    StreamPlayer *p = calloc(1, sizeof(StreamPlayer));
    if (!p) return NULL;

    /* Store buffer configuration */
    p->bufsize = bufsize;
    p->num_wave_bufs = buf_configs[bufsize].num_wave_bufs;
    p->pcm_buf_samples = buf_configs[bufsize].pcm_buf_samples;
    p->download_buf_size = buf_configs[bufsize].download_buf_size;

    /* Allocate MP3 download ring buffer */
    p->mp3_buffer = malloc(p->download_buf_size);
    if (!p->mp3_buffer) {
        free(p);
        return NULL;
    }

    /* Allocate PCM buffers from linear memory (required for NDSP DMA).
     * ndspWaveBuf data MUST be in linear memory, not regular heap. */
    for (int i = 0; i < p->num_wave_bufs; i++) {
        p->pcm_data[i] = (int16_t *)linearAlloc(p->pcm_buf_samples * sizeof(int16_t) * 2);
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
        for (int j = 0; j < p->num_wave_bufs; j++)
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

StreamPlayer *stream_player_create(void) {
    return stream_player_create_with_bufsize(STREAM_BUF_MEDIUM);
}

int stream_player_play(StreamPlayer *p, const char *url) {
    if (!p || !url) return -1;

    /* Stop any current playback and its threads */
    if (p->playing || p->decode_thread || p->download_thread)
        stream_player_stop(p);

    /* If a previous download thread is still winding down (e.g. it was
     * stopped during a stalled connect), wait for it before starting a new
     * stream — we must not overwrite the handle of a thread that still owns
     * a live curl. CURLOPT_LOW_SPEED bounds stalls to 15s, so this returns
     * in practice. If it somehow still runs, refuse to start. */
    if (p->download_thread) {
        p->download_active = false;
        if (threadJoin(p->download_thread, 15000000) == 0) {
            threadFree(p->download_thread);
            p->download_thread = NULL;
        }
        if (p->download_thread) {
            return -1;
        }
    }
    if (p->decode_thread) {
        if (threadJoin(p->decode_thread, 2000000) == 0) {
            threadFree(p->decode_thread);
            p->decode_thread = NULL;
        }
    }

    /* New epoch so any stale download thread can't write into this stream */
    p->stream_epoch++;

    /* Reset state */
    p->mp3_write_pos = 0;
    p->mp3_read_pos = 0;
    p->mp3_eof = false;
    p->download_active = false;
    p->download_done = false;
    p->decoder_initialized = false;
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
    for (int i = 0; i < p->num_wave_bufs; i++) {
        memset(p->pcm_data[i], 0, p->pcm_buf_samples * sizeof(int16_t) * 2);
    }

    /* Start HTTP download */
    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "3DSRadio/1.0");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L); /* No timeout for streaming */
    /* Abort if the stream stalls (guarantees the download thread always
     * winds down within ~15s, even mid-transfer with no data flowing). */
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 15L);

    struct DownloadCtx *ctx = malloc(sizeof(struct DownloadCtx));
    if (!ctx) {
        curl_easy_cleanup(curl);
        return -1;
    }
    ctx->player = p;
    ctx->epoch = p->stream_epoch;

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, download_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, ctx);

    struct DownloadThreadArg *ta = malloc(sizeof(struct DownloadThreadArg));
    if (!ta) {
        curl_easy_cleanup(curl);
        free(ctx);
        return -1;
    }
    ta->player = p;
    ta->curl = curl;
    ta->ctx = ctx;

    p->download_active = true;
    p->playing = true;
    p->buffering = true;

    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);

    /* Download thread: LOWER priority than main. Curl must never outrun
     * the decoder (which used to fill the ring and drop MP3 chunks). */
    p->download_thread = threadCreate(download_thread_func, ta,
                                      32 * 1024, prio + 1, -2, true);
    if (!p->download_thread) {
        curl_easy_cleanup(ta->curl);
        free(ta->ctx);
        free(ta);
        p->playing = false;
        return -1;
    }

    /* Decode thread: HIGHER priority than main so UI work can't starve
     * the audio feed — the actual source of stutter. */
    p->decode_thread = threadCreate(decode_thread_func, p,
                                    32 * 1024, prio - 2, -2, true);
    if (!p->decode_thread) {
        p->download_active = false;
        p->playing = false;
        /* Download thread frees its own curl; just wind it down. Keep the
         * handle if the join times out so a later play()/destroy() can still
         * join it (a stale thread must not be forgotten while still running). */
        if (threadJoin(p->download_thread, 1000000) == 0) {
            threadFree(p->download_thread);
            p->download_thread = NULL;
        }
        return -1;
    }

    return 0;
}

void stream_player_update(StreamPlayer *p) {
    /* Decoding now runs on a dedicated decode thread (see decode_thread_func).
     * This hook is kept for the main loop's frame cadence; nothing to do. */
}

void stream_player_toggle_pause(StreamPlayer *p) {
    if (!p) return;
    p->paused = !p->paused;
    ndspChnSetPaused(0, p->paused);
}

void stream_player_stop(StreamPlayer *p) {
    if (!p) return;

    p->playing = false;
    p->paused = false;

    /* Stop the download thread: the write callback aborts curl as soon as
     * download_active is false, so it exits promptly while data is flowing.
     * The thread owns its curl handle and frees it on exit. If the join
     * times out (curl stalled), keep the handle so play()/destroy() can
     * wind it down later. */
    if (p->download_thread) {
        p->download_active = false;
        if (threadJoin(p->download_thread, 2000000) == 0) {
            threadFree(p->download_thread);
            p->download_thread = NULL;
        }
    }

    /* Stop the decode thread (finishes its current short pass, then exits) */
    if (p->decode_thread) {
        if (threadJoin(p->decode_thread, 2000000) == 0) {
            threadFree(p->decode_thread);
            p->decode_thread = NULL;
        }
    }

    /* Stop ndsp channel */
    ndspChnWaveBufClear(0);
    ndspChnSetPaused(0, false);

    /* Reset wave buffers */
    memset(&p->wave_bufs, 0, sizeof(p->wave_bufs));

    p->buffering = false;
}

void stream_player_destroy(StreamPlayer *p) {
    if (!p) return;

    stream_player_stop(p);

    /* Give a still-running download thread more time to wind down before
     * freeing anything it might still touch. CURLOPT_LOW_SPEED bounds any
     * stall to ~15s, so this join is expected to succeed. */
    if (p->download_thread) {
        p->download_active = false;
        if (threadJoin(p->download_thread, 15000000) == 0) {
            threadFree(p->download_thread);
            p->download_thread = NULL;
        }
    }
    if (p->decode_thread) {
        if (threadJoin(p->decode_thread, 2000000) == 0) {
            threadFree(p->decode_thread);
            p->decode_thread = NULL;
        }
    }
    if (p->download_thread) {
        /* Extremely unlikely: the download thread refused to exit. Leak
         * rather than free the player out from under a running thread. */
        return;
    }

    ndspExit();

    for (int i = 0; i < p->num_wave_bufs; i++) {
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
