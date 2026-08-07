#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "net.h"   /* NetCancelFn for radio_set_cancel_hook */

/* ======================================================================
 * Radio-browser.info API client
 * Fetches station lists, tags, countries and stream URLs
 * ====================================================================== */

/* Maximum sizes */
#define RADIO_MAX_STATIONS 200
#define RADIO_MAX_NAME 256
#define RADIO_MAX_URL 512
#define RADIO_MAX_TAG 64
#define RADIO_MAX_COUNTRY 64
#define RADIO_MAX_LANGUAGE 32
#define RADIO_MAX_CODE 16
#define RADIO_API_BASE "http://de1.api.radio-browser.info"

/* A single radio station */
typedef struct {
    char stationuuid[37];    /* UUID string */
    char name[RADIO_MAX_NAME];
    char url[RADIO_MAX_URL];
    char url_resolved[RADIO_MAX_URL];
    char homepage[RADIO_MAX_URL];
    char favicon[RADIO_MAX_URL];
    char tags[RADIO_MAX_TAG * 4];  /* Comma-separated tags */
    char country[RADIO_MAX_COUNTRY];
    char countrycode[RADIO_MAX_CODE];
    char state[RADIO_MAX_COUNTRY];
    char language[RADIO_MAX_LANGUAGE];
    char codec[16];          /* MP3, AAC, etc. */
    int bitrate;             /* kbps */
    int votes;
    int clickcount;
    bool is_https;
    bool lastcheckok;
    int clicktrend;          /* 0 = stable, 1 = up, -1 = down */
} RadioStation;

/* A tag (genre) with station count */
typedef struct {
    char name[RADIO_MAX_TAG];
    int stationcount;
} RadioTag;

/* A language with station count */
typedef struct {
    char name[RADIO_MAX_TAG];
    int stationcount;
} RadioLanguage;

/* Initialize the radio API (sets up HTTP user agent, etc.) */
void radio_init(void);

/* Install a cancellation hook (NetCancelFn from net.h) so an in-flight
 * request can be aborted from another thread. Pass NULL to clear.
 * Used by the async loader to let a B-press / timeout cancel the request. */
void radio_set_cancel_hook(NetCancelFn fn, void *data);

/* Fetch top tags (genres). Returns count or negative on error. */
int radio_fetch_tags(RadioTag *tags, int max_tags,
                     char *error, size_t error_size);

/* Fetch stations by tag. Returns count or negative on error. */
int radio_fetch_by_tag(const char *tag, RadioStation *stations, int max_stations,
                       char *error, size_t error_size);

/* Fetch top-clicked stations. Returns count or negative on error. */
int radio_fetch_topclick(RadioStation *stations, int max_stations,
                         char *error, size_t error_size);

/* Fetch stations by country. Returns count or negative on error. */
int radio_fetch_by_country(const char *country, RadioStation *stations,
                           int max_stations, char *error, size_t error_size);

/* Search stations by name. Returns count or negative on error. */
int radio_search_by_name(const char *name, RadioStation *stations,
                         int max_stations, char *error, size_t error_size);

/* Fetch available languages. Returns count or negative on error. */
int radio_fetch_languages(RadioLanguage *languages, int max_languages,
                          char *error, size_t error_size);

/* Fetch stations by language. Returns count or negative on error. */
int radio_fetch_by_language(const char *language, RadioStation *stations,
                            int max_stations, char *error, size_t error_size);

/* Get a playable stream URL for a station (increments click counter).
 * Returns NET_OK on success. The url buffer receives the stream URL. */
int radio_get_stream_url(const char *stationuuid, char *url, size_t url_size,
                         char *error, size_t error_size);

/* Free resources */
void radio_exit(void);
