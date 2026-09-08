#include "stream_player.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <curl/curl.h>

#include "ogg_decoder.h"
#include "aac_decoder.h"

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
 * - Download thread pulls raw stream bytes from the HTTP stream into a ring
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
 *
 * The six-wave large preset and explicit prebuffer gate follow the proven
 * progressive-player approach used by ClouDS-Music-FA, adapted here for a
 * never-ending live radio stream rather than a finite local file.
 * ====================================================================== */

#define NUM_WAVE_BUFS_MAX 6
#define PCM_BUF_SAMPLES_MAX 16384  /* Max samples per wave buffer (per channel) */
#define DOWNLOAD_BUF_MAX (512 * 1024)  /* 512 KB max raw MP3 download buffer */
#define STAGE_BUF_MAX 131072  /* 128 KB max staging buffer */
#define STREAM_MAX_RETRIES 3

/* Buffer size presets (indexed by StreamBufSize enum) */
static const struct {
    int num_wave_bufs;
    int pcm_buf_samples;
    size_t download_buf_size;
    size_t stage_buf_size;
    size_t prebuffer_bytes;
} buf_configs[] = {
    [STREAM_BUF_SMALL]  = { 3,  4096, 128 * 1024, 32768,  16 * 1024 },
    [STREAM_BUF_MEDIUM] = { 4,  8192, 256 * 1024, 65536,  32 * 1024 },
    /* Six queued waves follows the reference player's queue depth.  The
     * extra wave is deliberately kept only for the large/weak-WiFi preset. */
    [STREAM_BUF_LARGE]  = { 6, 16384, 512 * 1024, 131072, 64 * 1024 },
};

struct StreamPlayer {
    /* Buffer configuration */
    StreamBufSize bufsize;
    int num_wave_bufs;
    int pcm_buf_samples;
    size_t download_buf_size;
    size_t prebuffer_bytes;

    /* Download state */
    volatile bool download_active;
    volatile bool download_done;
    Thread download_thread;
    char download_error[256];
    char last_url[512];
    StreamCodec codec;
    volatile StreamPlayerState state;
    int retry_count;

    /* Incremented on every stream; lets a stale (leaked) download thread
     * from a previous stream detect that its ring buffer is gone. */
    volatile u32 stream_epoch;

    /* Raw MP3 ring buffer */
    uint8_t *mp3_buffer;
    volatile size_t mp3_write_pos;
    volatile size_t mp3_read_pos;
    volatile bool mp3_eof;

    /* Decoder */
    mp3dec_t mp3d;
    OggDecoder oggd;
    AacDecoder aacd;
    bool decoder_initialized;
    unsigned int decoder_no_progress;

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
    size_t icy_metaint;
    size_t icy_audio_remaining;
    size_t icy_metadata_remaining;
};

static void memory_barrier(void) {
    __sync_synchronize();
}

static size_t ring_available(const StreamPlayer *p) {
    size_t write;
    size_t read;
    if (!p || p->download_buf_size == 0) return 0;
    memory_barrier();
    write = p->mp3_write_pos;
    read = p->mp3_read_pos;
    return (write + p->download_buf_size - read) % p->download_buf_size;
}

static size_t ring_free(const StreamPlayer *p) {
    if (!p || p->download_buf_size < 2) return 0;
    return p->download_buf_size - ring_available(p) - 1;
}

static char ascii_lower(char c) {
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

static bool contains_ci(const char *text, const char *needle) {
    if (!text || !needle || !needle[0]) return false;
    for (const char *p = text; *p; p++) {
        const char *a = p;
        const char *b = needle;
        while (*a && *b && ascii_lower(*a) == ascii_lower(*b)) {
            a++;
            b++;
        }
        if (!*b) return true;
    }
    return false;
}

static bool codec_hint_is_auto(const char *codec) {
    return !codec || !codec[0] || contains_ci(codec, "unknown") ||
           contains_ci(codec, "auto") || contains_ci(codec, "undefined");
}

static StreamCodec codec_from_string(const char *codec) {
    if (codec_hint_is_auto(codec)) return STREAM_CODEC_UNKNOWN;
    /* Radio-Browser may report values such as AAC+,H.264. Treat the audio
     * part as authoritative and let the player reject genuinely unsupported
     * formats instead of rejecting every compound value. */
    if (contains_ci(codec, "mp3") || contains_ci(codec, "mpeg"))
        return STREAM_CODEC_MP3;
    if (contains_ci(codec, "ogg") || contains_ci(codec, "vorbis"))
        return STREAM_CODEC_OGG;
    if (contains_ci(codec, "aac") || contains_ci(codec, "mp4a"))
        return STREAM_CODEC_AAC;
    return STREAM_CODEC_UNKNOWN;
}

static StreamCodec codec_from_url(const char *url) {
    if (contains_ci(url, ".ogg") || contains_ci(url, ".oga"))
        return STREAM_CODEC_OGG;
    if (contains_ci(url, ".aac") || contains_ci(url, "aacp") ||
        contains_ci(url, ".m4a") || contains_ci(url, "mp4a"))
        return STREAM_CODEC_AAC;
    if (contains_ci(url, ".mp3") || contains_ci(url, ".mpeg"))
        return STREAM_CODEC_MP3;
    return STREAM_CODEC_UNKNOWN;
}

static bool looks_like_adts(const uint8_t *data, size_t length) {
    return data && length >= 2 && data[0] == 0xff &&
           (data[1] & 0xf6) == 0xf0;
}

static void set_player_error(StreamPlayer *p, const char *message) {
    if (!p) return;
    snprintf(p->download_error, sizeof(p->download_error), "%s",
             message && message[0] ? message : "Unknown audio error");
    p->playing = false;
    p->download_active = false;
    p->buffering = false;
    memory_barrier();
    p->state = STREAM_STATE_ERROR;
}

/* Write callback context for the curl download */
struct DownloadCtx {
    StreamPlayer *player;
    u32 epoch; /* stream_epoch captured when the download started */
};

static size_t stream_header_cb(char *buffer, size_t size, size_t nitems,
                               void *userdata) {
    struct DownloadCtx *ctx = (struct DownloadCtx *)userdata;
    size_t length = size * nitems;
    const char *prefix = "icy-metaint:";
    char header[128];
    size_t copy = length < sizeof(header) - 1 ? length : sizeof(header) - 1;
    memcpy(header, buffer, copy);
    header[copy] = '\0';
    if (copy > 13 && strncasecmp(header, "content-type:", 13) == 0 &&
        ctx->player->codec == STREAM_CODEC_UNKNOWN) {
        const char *type = header + 13;
        if (contains_ci(type, "aac") || contains_ci(type, "mp4"))
            ctx->player->codec = STREAM_CODEC_AAC;
        else if (contains_ci(type, "ogg") || contains_ci(type, "vorbis"))
            ctx->player->codec = STREAM_CODEC_OGG;
        else if (contains_ci(type, "mpeg") || contains_ci(type, "mp3"))
            ctx->player->codec = STREAM_CODEC_MP3;
    }
    if (copy > strlen(prefix) && strncasecmp(header, prefix, strlen(prefix)) == 0) {
        unsigned long value = strtoul(header + strlen(prefix), NULL, 10);
        if (value > 0 && value < 1024 * 1024) {
            ctx->player->icy_metaint = (size_t)value;
            ctx->player->icy_audio_remaining = (size_t)value;
            ctx->player->icy_metadata_remaining = 0;
        }
    }
    return length;
}

/* Arguments captured at thread creation so the download thread owns its own
 * curl handle and context, and can free them at exit without reading shared
 * player state (a stale thread must never touch a newer stream's curl). */
struct DownloadThreadArg {
    StreamPlayer *player;
    CURL *curl;
    struct curl_slist *headers;
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

    size_t write;
    size_t read;
    size_t avail;
    memory_barrier();
    write = p->mp3_write_pos;
    read = p->mp3_read_pos;
    avail = (write + p->download_buf_size - read) % p->download_buf_size;
    if (avail == 0) return;

    size_t to_copy = avail < free_space ? avail : free_space;
    size_t first = p->download_buf_size - read;  /* bytes until ring wrap */

    if (to_copy <= first) {
        memcpy(p->stage + *stage_len, p->mp3_buffer + read, to_copy);
    } else {
        memcpy(p->stage + *stage_len, p->mp3_buffer + read, first);
        memcpy(p->stage + *stage_len + first, p->mp3_buffer, to_copy - first);
    }

    *stage_len += to_copy;
    memory_barrier();
    p->mp3_read_pos = (read + to_copy) % p->download_buf_size;
    memory_barrier();
}

static bool write_ring_bytes(StreamPlayer *p, const uint8_t *data, size_t total,
                             u32 epoch) {
    size_t offset = 0;
    while (offset < total) {
        size_t free_bytes = ring_free(p);
        if (free_bytes == 0) {
            if (!p->download_active || epoch != p->stream_epoch) return false;
            svcSleepThread(20000);
            continue;
        }

        size_t write = p->mp3_write_pos;
        size_t contiguous = p->download_buf_size - write;
        size_t chunk = total - offset;
        if (chunk > free_bytes) chunk = free_bytes;
        if (chunk > contiguous) chunk = contiguous;

        memcpy(p->mp3_buffer + write, data + offset, chunk);
        memory_barrier();
        p->mp3_write_pos = (write + chunk) % p->download_buf_size;
        memory_barrier();
        offset += chunk;
    }
    return true;
}

/* Write callback for curl download.  It strips ICY metadata blocks before
 * the bytes reach the codec, while never discarding audio bytes. */
static size_t download_write_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    struct DownloadCtx *ctx = (struct DownloadCtx *)userp;
    StreamPlayer *p = ctx->player;
    size_t total = size * nmemb;
    size_t offset = 0;
    uint8_t *data = (uint8_t *)contents;

    if (!p->download_active || ctx->epoch != p->stream_epoch)
        return 0;

    while (offset < total) {
        if (p->icy_metaint == 0) {
            if (!write_ring_bytes(p, data + offset, total - offset, ctx->epoch)) return 0;
            break;
        }

        if (p->icy_audio_remaining > 0) {
            size_t chunk = total - offset;
            if (chunk > p->icy_audio_remaining) chunk = p->icy_audio_remaining;
            if (!write_ring_bytes(p, data + offset, chunk, ctx->epoch)) return 0;
            offset += chunk;
            p->icy_audio_remaining -= chunk;
            continue;
        }

        if (p->icy_metadata_remaining == 0) {
            p->icy_metadata_remaining = (size_t)data[offset++] * 16U;
            if (p->icy_metadata_remaining == 0)
                p->icy_audio_remaining = p->icy_metaint;
            continue;
        }

        size_t skip = total - offset;
        if (skip > p->icy_metadata_remaining) skip = p->icy_metadata_remaining;
        offset += skip;
        p->icy_metadata_remaining -= skip;
        if (p->icy_metadata_remaining == 0) {
            p->icy_audio_remaining = p->icy_metaint;
        }
    }
    return total;
}

static bool retryable_curl_error(CURLcode res) {
    return res == CURLE_OPERATION_TIMEDOUT ||
           res == CURLE_COULDNT_CONNECT ||
           res == CURLE_COULDNT_RESOLVE_HOST ||
           res == CURLE_RECV_ERROR ||
           res == CURLE_GOT_NOTHING ||
           res == CURLE_PARTIAL_FILE;
}

static void download_thread_func(void *arg) {
    struct DownloadThreadArg *ta = (struct DownloadThreadArg *)arg;
    StreamPlayer *p = ta->player;
    CURLcode res = CURLE_OK;
    int attempt = 0;

    do {
        /* Each retry starts a fresh HTTP response. */
        p->icy_metaint = 0;
        p->icy_audio_remaining = 0;
        p->icy_metadata_remaining = 0;
        res = curl_easy_perform(ta->curl);
        if (res == CURLE_OK || !p->download_active ||
            ta->ctx->epoch != p->stream_epoch ||
            res == CURLE_ABORTED_BY_CALLBACK ||
            !retryable_curl_error(res) || attempt >= STREAM_MAX_RETRIES) {
            break;
        }

        p->retry_count = attempt + 1;
        p->state = STREAM_STATE_RECONNECTING;
        p->buffering = true;
        svcSleepThread((u64)(250 + attempt * 500) * 1000000ULL);
        attempt++;
    } while (p->download_active && ta->ctx->epoch == p->stream_epoch);

    if (res != CURLE_OK && p->download_active &&
        ta->ctx->epoch == p->stream_epoch) {
        snprintf(p->download_error, sizeof(p->download_error),
                 "Download error: %s", curl_easy_strerror(res));
        p->playing = false;
        p->buffering = false;
        p->state = STREAM_STATE_ERROR;
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
    if (ta->headers) curl_slist_free_all(ta->headers);
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

static void decode_pass_ogg(StreamPlayer *p) {
    size_t *stage_len = &p->stage_len;
    size_t *stage_off = &p->stage_off;

    /* Do not start Vorbis on a tiny first packet.  This mirrors the MP3
     * prebuffer gate and prevents an initial Wi-Fi hiccup from becoming a
     * DSP underrun. */
    if (p->state == STREAM_STATE_BUFFERING &&
        ring_available(p) + (*stage_len - *stage_off) < p->prebuffer_bytes &&
        !p->mp3_eof) {
        p->buffering = true;
        return;
    }

    ring_to_stage(p);

    for (int i = 0; i < p->num_wave_bufs; i++) {
        ndspWaveBuf *buf = &p->wave_bufs[i];
        if (buf->status != NDSP_WBUF_DONE && buf->status != NDSP_WBUF_FREE)
            continue;

        int16_t *out = p->pcm_data[i];
        int total_samples = 0;
        int max_samples = p->pcm_buf_samples * 2;
        while (total_samples < max_samples) {
            if (*stage_len - *stage_off < 4096)
                ring_to_stage(p);
            if (*stage_len - *stage_off == 0) break;

            size_t used = 0;
            int samples = ogg_decoder_decode(
                &p->oggd, p->stage + *stage_off,
                *stage_len - *stage_off, &used,
                out + total_samples, max_samples - total_samples);
            *stage_off += used;
            total_samples += samples;

            if (samples > 0) {
                p->active_channels = p->oggd.channels;
                p->sample_rate = p->oggd.sample_rate;
                if (!p->decoder_initialized) {
                    p->decoder_initialized = true;
                    configure_ndsp_channel(p);
                }
            } else if (used == 0) {
                break;
            }
        }

        if (total_samples > 0) {
            p->decoder_no_progress = 0;
            submit_wave_buffer(buf, out, total_samples, p->active_channels);
            p->buffering = false;
            if (p->state == STREAM_STATE_BUFFERING ||
                p->state == STREAM_STATE_RECONNECTING)
                p->state = STREAM_STATE_PLAYING;
        } else if (p->mp3_eof && ring_available(p) == 0 &&
                   *stage_len - *stage_off == 0) {
            p->playing = false;
            if (p->state != STREAM_STATE_ERROR)
                p->state = STREAM_STATE_ENDED;
        } else {
            if (*stage_len - *stage_off > 16384 && ++p->decoder_no_progress > 120) {
                set_player_error(p, "Unsupported or invalid OGG stream");
                return;
            }
            p->buffering = true;
            if (p->state != STREAM_STATE_ERROR &&
                p->state != STREAM_STATE_RECONNECTING)
                p->state = STREAM_STATE_BUFFERING;
        }
    }
}

static void decode_pass_aac(StreamPlayer *p) {
    size_t *stage_len = &p->stage_len;
    size_t *stage_off = &p->stage_off;

    if (p->state == STREAM_STATE_BUFFERING &&
        ring_available(p) + (*stage_len - *stage_off) < p->prebuffer_bytes &&
        !p->mp3_eof) {
        p->buffering = true;
        return;
    }

    ring_to_stage(p);
    for (int i = 0; i < p->num_wave_bufs; i++) {
        ndspWaveBuf *buf = &p->wave_bufs[i];
        if (buf->status != NDSP_WBUF_DONE && buf->status != NDSP_WBUF_FREE)
            continue;

        int16_t *out = p->pcm_data[i];
        int total_samples = 0;
        int max_samples = p->pcm_buf_samples * 2;
        while (total_samples < max_samples) {
            if (*stage_len - *stage_off < 4096) ring_to_stage(p);
            if (*stage_len - *stage_off == 0) break;

            size_t used = 0;
            int samples = aac_decoder_decode(
                &p->aacd, p->stage + *stage_off,
                *stage_len - *stage_off, &used,
                out + total_samples, max_samples - total_samples);
            *stage_off += used;
            total_samples += samples;

            if (samples > 0) {
                int previous_channels = p->active_channels;
                int previous_rate = p->sample_rate;
                int channels = p->aacd.channels;
                if (channels < 1) channels = 2;
                if (channels > 2) channels = 2;
                p->active_channels = channels;
                if (p->aacd.sample_rate > 0)
                    p->sample_rate = p->aacd.sample_rate;
                if (!p->decoder_initialized ||
                    p->sample_rate != previous_rate ||
                    p->active_channels != previous_channels) {
                    p->decoder_initialized = true;
                    configure_ndsp_channel(p);
                }
            } else if (used == 0) {
                break;
            }
        }

        if (total_samples > 0) {
            p->decoder_no_progress = 0;
            submit_wave_buffer(buf, out, total_samples, p->active_channels);
            p->buffering = false;
            if (p->state == STREAM_STATE_BUFFERING ||
                p->state == STREAM_STATE_RECONNECTING)
                p->state = STREAM_STATE_PLAYING;
        } else if (p->mp3_eof && ring_available(p) == 0 &&
                   *stage_len - *stage_off == 0) {
            p->playing = false;
            if (p->state != STREAM_STATE_ERROR) p->state = STREAM_STATE_ENDED;
        } else {
            if (*stage_len - *stage_off > 16384 && ++p->decoder_no_progress > 120) {
                set_player_error(p, "Unsupported or invalid AAC stream");
                return;
            }
            p->buffering = true;
            if (p->state != STREAM_STATE_ERROR &&
                p->state != STREAM_STATE_RECONNECTING)
                p->state = STREAM_STATE_BUFFERING;
        }
    }
}

/* ======================================================================
 * One decode pass: refill every finished wave buffer from the MP3 staging
 * buffer. Runs on the decode thread, independent of the render loop.
 * ====================================================================== */

static void decode_pass(StreamPlayer *p) {
    if (p->codec == STREAM_CODEC_OGG) {
        decode_pass_ogg(p);
        return;
    }
    if (p->codec == STREAM_CODEC_AAC) {
        if (!aac_decoder_available()) {
            set_player_error(p, "AAC decoder is not included in this build");
            return;
        }
        decode_pass_aac(p);
        return;
    }
    size_t *stage_len = &p->stage_len;
    size_t *stage_off = &p->stage_off;

    /* Do not start the DSP on the first few kilobytes.  A short network
     * scheduling hiccup immediately after connect should become a visible
     * buffering state, not a burst of underrun noise. */
    if (p->state == STREAM_STATE_BUFFERING &&
        ring_available(p) < p->prebuffer_bytes && !p->mp3_eof) {
        p->buffering = true;
        return;
    }

    /* Compact + top up the staging buffer from the ring */
    ring_to_stage(p);

    /* Some directory entries report a generic codec or a stale MP3 label.
     * Recognize an Ogg page before handing bytes to minimp3 so the stream
     * does not enter a misleading silent state. */
    if ((p->codec == STREAM_CODEC_MP3 || p->codec == STREAM_CODEC_UNKNOWN) &&
        *stage_len - *stage_off >= 4 &&
        memcmp(p->stage + *stage_off, "OggS", 4) == 0) {
        p->codec = STREAM_CODEC_OGG;
        ogg_decoder_close(&p->oggd);
        ogg_decoder_init(&p->oggd);
        p->decoder_initialized = false;
        decode_pass_ogg(p);
        return;
    }

    /* ADTS AAC has a distinctive sync/header pattern.  Fail early with a
     * useful diagnostic when the station metadata was incomplete. */
    if ((p->codec == STREAM_CODEC_MP3 || p->codec == STREAM_CODEC_UNKNOWN) &&
        looks_like_adts(p->stage + *stage_off, *stage_len - *stage_off)) {
        if (!aac_decoder_available()) {
            set_player_error(p, "AAC decoder is not included in this build");
            return;
        }
        p->codec = STREAM_CODEC_AAC;
        aac_decoder_close(&p->aacd);
        aac_decoder_init(&p->aacd);
        p->decoder_initialized = false;
        decode_pass_aac(p);
        return;
    }

    /* No OggS/ADTS signature means the directory's AUTO entry is most
     * likely an MPEG audio stream. Commit that decision once enough bytes
     * have arrived so the UI can report the detected format. */
    if (p->codec == STREAM_CODEC_UNKNOWN)
        p->codec = STREAM_CODEC_MP3;

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
                if (total_samples == 0) {
                    p->buffering = true;
                    if (p->state != STREAM_STATE_ERROR &&
                        p->state != STREAM_STATE_RECONNECTING)
                        p->state = STREAM_STATE_BUFFERING;
                }
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
            if (*stage_len - *stage_off > 16384 && ++p->decoder_no_progress > 120) {
                set_player_error(p, "Unsupported or invalid MP3 stream");
                return;
            }
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
                    if (p->state != STREAM_STATE_ERROR)
                        p->state = STREAM_STATE_ENDED;
                    return;
                }
            }
            continue;
        }

        submit_wave_buffer(buf, out, total_samples, p->active_channels);
        p->decoder_no_progress = 0;
        p->buffering = false;
        if (p->state == STREAM_STATE_BUFFERING ||
            p->state == STREAM_STATE_RECONNECTING)
            p->state = STREAM_STATE_PLAYING;
    }
}

/* Dedicated decode thread: keeps ndsp fed regardless of what the render
 * thread is doing. Runs at higher priority than the main thread. */
static void decode_thread_func(void *arg) {
    StreamPlayer *p = (StreamPlayer *)arg;
    while (p->playing) {
        if (p->paused) {
            p->state = STREAM_STATE_PAUSED;
            svcSleepThread(20 * 1000 * 1000); /* 20 ms */
            continue;
        }
        if (p->state == STREAM_STATE_PAUSED)
            p->state = p->buffering ? STREAM_STATE_BUFFERING : STREAM_STATE_PLAYING;
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
    p->prebuffer_bytes = buf_configs[bufsize].prebuffer_bytes;

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
    ogg_decoder_init(&p->oggd);
    aac_decoder_init(&p->aacd);

    p->volume = 0.8f;
    p->active_channels = 2;
    p->sample_rate = 44100;
    p->codec = STREAM_CODEC_UNKNOWN;
    p->state = STREAM_STATE_IDLE;
    ndspSetMasterVol(p->volume);
    return p;
}

StreamPlayer *stream_player_create(void) {
    return stream_player_create_with_bufsize(STREAM_BUF_MEDIUM);
}

int stream_player_play(StreamPlayer *p, const char *url) {
    return stream_player_play_with_codec(p, url, NULL);
}

int stream_player_play_with_codec(StreamPlayer *p, const char *url,
                                  const char *codec_name) {
    StreamCodec codec;
    if (!p || !url || !url[0]) return -1;

    codec = codec_from_string(codec_name);
    if (codec == STREAM_CODEC_UNKNOWN && codec_hint_is_auto(codec_name))
        codec = codec_from_url(url);
    if (codec == STREAM_CODEC_UNKNOWN && !codec_hint_is_auto(codec_name)) {
        set_player_error(p, "Unsupported stream codec");
        p->codec = codec;
        return -2;
    }
    if (codec == STREAM_CODEC_AAC && !aac_decoder_available()) {
        set_player_error(p, "AAC support requires the FAAD2 backend");
        p->codec = codec;
        return -2;
    }

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
        if (threadJoin(p->download_thread, 1000000) == 0) {
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

    strncpy(p->last_url, url, sizeof(p->last_url) - 1);
    p->last_url[sizeof(p->last_url) - 1] = '\0';
    p->codec = codec;
    p->state = STREAM_STATE_CONNECTING;
    p->retry_count = 0;

    /* Reset state */
    p->mp3_write_pos = 0;
    p->mp3_read_pos = 0;
    p->mp3_eof = false;
    p->download_active = false;
    p->download_done = false;
    p->decoder_initialized = false;
    p->decoder_no_progress = 0;
    p->active_channels = 2; /* Assume stereo until first frame decoded */
    p->sample_rate = 44100;
    p->download_error[0] = '\0';
    p->icy_metaint = 0;
    p->icy_audio_remaining = 0;
    p->icy_metadata_remaining = 0;

    /* Reset the MP3 decoder state between streams.
     * Without this, the synthesis filterbank carries stale
     * state from the previous stream, causing audio artifacts. */
    mp3dec_init(&p->mp3d);
    ogg_decoder_close(&p->oggd);
    aac_decoder_close(&p->aacd);
    aac_decoder_init(&p->aacd);

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
    if (!curl) {
        set_player_error(p, "Failed to initialize network stream");
        return -1;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "3DSRadio/1.1");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    FILE *ca = fopen("romfs:/cacert.pem", "rb");
    if (ca) {
        fclose(ca);
        curl_easy_setopt(curl, CURLOPT_CAINFO, "romfs:/cacert.pem");
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    } else {
        /* Keep legacy stations playable when the optional CA bundle was not
         * packaged, while still verifying every normal CI/release build. */
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L); /* No timeout for streaming */
    /* Abort if the stream stalls (guarantees the download thread always
     * winds down within ~15s, even mid-transfer with no data flowing). */
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 15L);

    struct DownloadCtx *ctx = malloc(sizeof(struct DownloadCtx));
    if (!ctx) {
        curl_easy_cleanup(curl);
        set_player_error(p, "Not enough memory for stream");
        return -1;
    }
    ctx->player = p;
    ctx->epoch = p->stream_epoch;

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, download_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, ctx);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, stream_header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, ctx);
    struct curl_slist *headers = curl_slist_append(NULL, "Icy-MetaData: 1");
    if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    struct DownloadThreadArg *ta = malloc(sizeof(struct DownloadThreadArg));
    if (!ta) {
        curl_easy_cleanup(curl);
        if (headers) curl_slist_free_all(headers);
        free(ctx);
        set_player_error(p, "Not enough memory for stream thread");
        return -1;
    }
    ta->player = p;
    ta->curl = curl;
    ta->headers = headers;
    ta->ctx = ctx;

    p->download_active = true;
    p->playing = true;
    p->buffering = true;
    p->state = STREAM_STATE_BUFFERING;

    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);

    /* Download thread: LOWER priority than main. Curl must never outrun
     * the decoder (which used to fill the ring and drop MP3 chunks). */
    p->download_thread = threadCreate(download_thread_func, ta,
                                      32 * 1024, prio + 1, -2, true);
    if (!p->download_thread) {
        curl_easy_cleanup(ta->curl);
        if (ta->headers) curl_slist_free_all(ta->headers);
        free(ta->ctx);
        free(ta);
        p->playing = false;
        p->state = STREAM_STATE_ERROR;
        snprintf(p->download_error, sizeof(p->download_error),
                 "%s", "Failed to create download thread");
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
        set_player_error(p, "Failed to create audio thread");
        return -1;
    }

    return 0;
}

void stream_player_update(StreamPlayer *p) {
    /* Decoding now runs on a dedicated decode thread (see decode_thread_func).
     * This hook is kept for the main loop's frame cadence; nothing to do. */
}

void stream_player_toggle_pause(StreamPlayer *p) {
    if (!p || !p->playing || p->state == STREAM_STATE_ERROR ||
        p->state == STREAM_STATE_ENDED) return;
    p->paused = !p->paused;
    ndspChnSetPaused(0, p->paused);
    p->state = p->paused ? STREAM_STATE_PAUSED :
        (p->buffering ? STREAM_STATE_BUFFERING : STREAM_STATE_PLAYING);
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
    p->state = STREAM_STATE_IDLE;
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

    ogg_decoder_close(&p->oggd);
    aac_decoder_close(&p->aacd);
    ndspExit();

    for (int i = 0; i < p->num_wave_bufs; i++) {
        if (p->pcm_data[i]) linearFree(p->pcm_data[i]);
    }
    free(p->mp3_buffer);
    free(p);
}

bool stream_player_is_playing(StreamPlayer *p) {
    return p && p->state == STREAM_STATE_PLAYING;
}

bool stream_player_is_paused(StreamPlayer *p) {
    return p && p->state == STREAM_STATE_PAUSED;
}

bool stream_player_is_buffering(StreamPlayer *p) {
    return p && (p->state == STREAM_STATE_BUFFERING ||
                 p->state == STREAM_STATE_RECONNECTING);
}

bool stream_player_is_finished(StreamPlayer *p) {
    return p && (p->state == STREAM_STATE_ENDED ||
                 p->state == STREAM_STATE_ERROR);
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
    memory_barrier();
    return p ? p->download_error : NULL;
}

int stream_player_retry(StreamPlayer *p) {
    if (!p || !p->last_url[0]) return -1;
    return stream_player_play_with_codec(p, p->last_url,
                                         p->codec == STREAM_CODEC_MP3 ? "MP3" :
                                         p->codec == STREAM_CODEC_OGG ? "OGG" :
                                         p->codec == STREAM_CODEC_AAC ? "AAC" : NULL);
}

StreamPlayerState stream_player_get_state(StreamPlayer *p) {
    memory_barrier();
    return p ? p->state : STREAM_STATE_ERROR;
}

StreamCodec stream_player_get_codec(StreamPlayer *p) {
    return p ? p->codec : STREAM_CODEC_UNKNOWN;
}

int stream_player_get_sample_rate(StreamPlayer *p) {
    return p ? p->sample_rate : 0;
}

int stream_player_get_channels(StreamPlayer *p) {
    return p ? p->active_channels : 0;
}

int stream_player_get_buffer_percent(StreamPlayer *p) {
    if (!p || p->download_buf_size < 2) return 0;
    size_t available = ring_available(p);
    size_t percent = available * 100 / (p->download_buf_size - 1);
    return percent > 100 ? 100 : (int)percent;
}

bool stream_player_codec_supported(const char *codec) {
    StreamCodec type = codec_from_string(codec);
    if (codec_hint_is_auto(codec)) return true;
    if (type == STREAM_CODEC_AAC) return aac_decoder_available();
    return type == STREAM_CODEC_MP3 || type == STREAM_CODEC_OGG;
}
