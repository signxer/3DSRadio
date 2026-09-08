#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ======================================================================
 * Streaming Audio Player for 3DSRadio
 * Uses minimp3, stb_vorbis and an optional FAAD2 backend + ndsp audio
 * ====================================================================== */

/* Opaque player handle */
typedef struct StreamPlayer StreamPlayer;

typedef enum {
    STREAM_STATE_IDLE = 0,
    STREAM_STATE_CONNECTING,
    STREAM_STATE_BUFFERING,
    STREAM_STATE_PLAYING,
    STREAM_STATE_PAUSED,
    STREAM_STATE_RECONNECTING,
    STREAM_STATE_ENDED,
    STREAM_STATE_ERROR,
} StreamPlayerState;

typedef enum {
    STREAM_CODEC_UNKNOWN = 0,
    STREAM_CODEC_MP3,
    STREAM_CODEC_OGG,
    STREAM_CODEC_AAC,
} StreamCodec;

/* Buffer size presets for audio streaming.
 * Larger buffers reduce stutter but increase memory usage and latency. */
typedef enum {
    STREAM_BUF_SMALL  = 0,  /*  4K samples, 3 wave bufs, 128 KB download */
    STREAM_BUF_MEDIUM = 1,  /*  8K samples, 4 wave bufs, 256 KB download */
    STREAM_BUF_LARGE  = 2,  /* 16K samples, 6 wave bufs, 512 KB download */
} StreamBufSize;

/* Create a new stream player with the specified buffer size.
 * Pass STREAM_BUF_MEDIUM for the default (balanced) configuration. */
StreamPlayer *stream_player_create_with_bufsize(StreamBufSize bufsize);

/* Deprecated: create with default medium buffer size. */
StreamPlayer *stream_player_create(void);

/* Start playing a stream URL. Returns 0 on success. */
int stream_player_play(StreamPlayer *player, const char *url);

/* Start a stream with the codec reported by radio-browser. Empty, AUTO and
 * UNKNOWN codec names are probed from the URL, headers and stream bytes. */
int stream_player_play_with_codec(StreamPlayer *player, const char *url,
                                  const char *codec);

/* Retry the last stream URL after a recoverable failure. */
int stream_player_retry(StreamPlayer *player);

/* Toggle pause */
void stream_player_toggle_pause(StreamPlayer *player);

/* Stop playback and close stream */
void stream_player_stop(StreamPlayer *player);

/* Destroy player and free all resources */
void stream_player_destroy(StreamPlayer *player);

/* Kept for main-loop compatibility. Decoding now runs on a dedicated
 * decode thread inside the player, so this is a lightweight no-op. */
void stream_player_update(StreamPlayer *player);

/* State queries */
bool stream_player_is_playing(StreamPlayer *player);
bool stream_player_is_paused(StreamPlayer *player);
bool stream_player_is_buffering(StreamPlayer *player);
bool stream_player_is_finished(StreamPlayer *player);

StreamPlayerState stream_player_get_state(StreamPlayer *player);
StreamCodec stream_player_get_codec(StreamPlayer *player);
int stream_player_get_sample_rate(StreamPlayer *player);
int stream_player_get_channels(StreamPlayer *player);
int stream_player_get_buffer_percent(StreamPlayer *player);
bool stream_player_codec_supported(const char *codec);

/* Volume control (0.0 - 1.0) */
void stream_player_set_volume(StreamPlayer *player, float vol);
float stream_player_get_volume(StreamPlayer *player);

/* Get player error string */
const char *stream_player_error(StreamPlayer *player);
