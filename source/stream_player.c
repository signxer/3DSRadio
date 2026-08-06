#include "stream_player.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

/* minimp3 decoder - single header library */
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_STDIO
#include "minimp3.h"

/* ======================================================================
 * Streaming Audio Player
 *
 * Architecture:
 * - Background thread downloads raw MP3 data from HTTP stream
 * - Main thread (stream_player_update) decodes MP3 → PCM → ndsp
 * - 4 wave buffers for double/triple buffering
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
        /* If buffer full, wait (spin briefly) */
        if (next_write == p->mp3_read_pos) {
            /* Buffer full - wait a bit for decoder to consume */
            svcSleepThread(10000); /* 10 us */
            next_write = (p->mp3_write_pos + 1) % DOWNLOAD_BUF_SIZE;
            if (next_write == p->mp3_read_pos) {
                /* Still full - drop data */
                return total; /* Just skip this chunk */
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

    /* Allocate PCM buffers from linear memory (for ndsp DMA) */
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

    /* Initialize ndsp */
    ndspInit();
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
    ndspChnSetRate(0, 44100);
    ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
    float mix_init[12] = {1.0f, 1.0f};
    ndspChnSetMix(0, mix_init); /* Full volume both channels */

    /* Initialize MP3 decoder */
    mp3dec_init(&p->mp3d);

    p->volume = 0.8f;
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
    p->active_channels = 2; /* Assume stereo */
    p->sample_rate = 44100;
    p->download_error[0] = '\0';

    /* Initialize ndsp wave buffers */
    memset(&p->wave_bufs, 0, sizeof(p->wave_bufs));
    for (int i = 0; i < NUM_WAVE_BUFS; i++) {
        memset(p->pcm_data[i], 0, PCM_BUF_SAMPLES * sizeof(int16_t) * 2);
        p->wave_bufs[i].data_vaddr = p->pcm_data[i];
        p->wave_bufs[i].nsamples = PCM_BUF_SAMPLES;
        p->wave_bufs[i].status = NDSP_WBUF_FREE;
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

    /* Start download thread */
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    p->download_thread = threadCreate(download_thread_func, p, 32 * 1024, prio - 1, -2, true);

    return 0;
}

void stream_player_update(StreamPlayer *p) {
    if (!p || !p->playing || p->paused) return;

    /* Check if we need to fill more wave buffers */
    for (int i = 0; i < NUM_WAVE_BUFS; i++) {
        int buf_idx = (p->next_buf + i) % NUM_WAVE_BUFS;
        ndspWaveBuf *buf = &p->wave_bufs[buf_idx];

        /* If this buffer is done playing, refill it */
        if (buf->status == NDSP_WBUF_DONE) {
            buf->status = NDSP_WBUF_FREE;
        }

        if (buf->status != NDSP_WBUF_FREE) continue;

        /* Decode MP3 frames into this buffer */
        int16_t *out = p->pcm_data[buf_idx];
        int total_samples = 0;
        int max_samples = PCM_BUF_SAMPLES * 2; /* stereo */

        while (total_samples < max_samples) {
            /* Check if we have MP3 data to decode */
            if (p->mp3_read_pos == p->mp3_write_pos && !p->mp3_eof) {
                /* Buffer empty, wait for more data */
                if (total_samples == 0) {
                    p->buffering = true;
                    /* No data at all yet - skip this buffer */
                    svcSleepThread(1000);
                }
                break;
            }

            /* Read MP3 frame from ring buffer */
            uint8_t mp3_frame[4096];
            size_t frame_size = 0;

            /* Try to read a complete MP3 frame */
            /* First, find sync word (0xFFE0) */
            while (p->mp3_read_pos != p->mp3_write_pos || p->mp3_eof) {
                if (frame_size >= sizeof(mp3_frame)) break;
                if (p->mp3_read_pos == p->mp3_write_pos && !p->mp3_eof) break;
                if (p->mp3_read_pos == p->mp3_write_pos && p->mp3_eof) {
                    /* EOF - decode remaining data */
                    break;
                }
                mp3_frame[frame_size++] = p->mp3_buffer[p->mp3_read_pos];
                p->mp3_read_pos = (p->mp3_read_pos + 1) % DOWNLOAD_BUF_SIZE;
            }

            if (frame_size == 0) break;

            /* Decode the MP3 frame */
            mp3dec_frame_info_t info;
            int samples = mp3dec_decode_frame(&p->mp3d, mp3_frame, frame_size, out + total_samples, &info);

            if (samples > 0) {
                total_samples += samples * info.channels;
                if (!p->decoder_initialized) {
                    p->active_channels = info.channels;
                    p->sample_rate = info.hz;
                    p->decoder_initialized = true;

                    /* Configure ndsp for the stream format */
                    ndspChnSetRate(0, (float)info.hz);
                    if (info.channels == 2) {
                        ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
                    } else {
                        ndspChnSetFormat(0, NDSP_FORMAT_MONO_PCM16);
                    }
                }
            }

            /* If EOF and no more data, stop */
            if (frame_size == 0 || (p->mp3_eof && p->mp3_read_pos == p->mp3_write_pos)) {
                break;
            }
        }

        if (total_samples == 0) {
            /* No data decoded - skip this buffer for now */
            if (p->mp3_eof) {
                /* Stream ended */
                p->playing = false;
                return;
            }
            continue;
        }

        /* Configure and queue the wave buffer */
        buf->data_vaddr = out;
        buf->nsamples = total_samples / p->active_channels;
        buf->looping = false;
        buf->status = NDSP_WBUF_QUEUED;

        /* Set volume */
	    float set_mix[12] = {0};
	    set_mix[0] = p->volume;
	    set_mix[1] = p->volume;
	    ndspChnSetMix(0, set_mix);

        p->buffering = false;
    }
}

void stream_player_toggle_pause(StreamPlayer *p) {
    if (!p) return;
    p->paused = !p->paused;
    if (p->paused) {
        ndspChnSetPaused(0, true);
    } else {
        ndspChnSetPaused(0, false);
    }
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

    /* Stop ndsp */
    ndspChnWaveBufClear(0);
    ndspChnSetPaused(0, false);

    /* Reset wave buffers */
    for (int i = 0; i < NUM_WAVE_BUFS; i++) {
        p->wave_bufs[i].status = NDSP_WBUF_FREE;
    }

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
    p->volume = vol < 0.0f ? 0.0f : (vol > 1.0f ? 1.0f : vol);
}

float stream_player_get_volume(StreamPlayer *p) {
    return p ? p->volume : 0.0f;
}

const char *stream_player_error(StreamPlayer *p) {
    return p ? p->download_error : NULL;
}
