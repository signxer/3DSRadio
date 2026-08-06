#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ======================================================================
 * Streaming Audio Player for 3DSRadio
 * Uses minimp3 decoder + ndsp hardware audio
 * ====================================================================== */

/* Opaque player handle */
typedef struct StreamPlayer StreamPlayer;

/* Buffer size presets for audio streaming.
 * Larger buffers reduce stutter but increase memory usage and latency. */
typedef enum {
    STREAM_BUF_SMALL  = 0,  /*  4K samples, 3 wave bufs, 128 KB download */
    STREAM_BUF_MEDIUM = 1,  /*  8K samples, 4 wave bufs, 256 KB download */
    STREAM_BUF_LARGE  = 2,  /* 16K samples, 5 wave bufs, 512 KB download */
} StreamBufSize;

/* Create a new stream player with the specified buffer size.
 * Pass STREAM_BUF_MEDIUM for the default (balanced) configuration. */
StreamPlayer *stream_player_create_with_bufsize(StreamBufSize bufsize);

/* Deprecated: create with default medium buffer size. */
StreamPlayer *stream_player_create(void);

/* Start playing a stream URL. Returns 0 on success. */
int stream_player_play(StreamPlayer *player, const char *url);

/* Toggle pause */
void stream_player_toggle_pause(StreamPlayer *player);

/* Stop playback and close stream */
void stream_player_stop(StreamPlayer *player);

/* Destroy player and free all resources */
void stream_player_destroy(StreamPlayer *player);

/* Must be called each frame in main loop - decodes audio, feeds ndsp */
void stream_player_update(StreamPlayer *player);

/* State queries */
bool stream_player_is_playing(StreamPlayer *player);
bool stream_player_is_paused(StreamPlayer *player);
bool stream_player_is_buffering(StreamPlayer *player);
bool stream_player_is_finished(StreamPlayer *player);

/* Volume control (0.0 - 1.0) */
void stream_player_set_volume(StreamPlayer *player, float vol);
float stream_player_get_volume(StreamPlayer *player);

/* Get player error string */
const char *stream_player_error(StreamPlayer *player);
