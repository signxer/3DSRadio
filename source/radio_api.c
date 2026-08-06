#include "radio_api.h"
#include "json.h"
#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <3ds.h>

/* ======================================================================
 * Radio-browser.info API client implementation
 * Supports multiple API mirrors with automatic failover
 * ====================================================================== */

#define JSON_TOKEN_CAPACITY 1024

/* API server mirrors - tried in order, first working one wins */
static const char *API_SERVERS[] = {
    "de1.api.radio-browser.info",
    "de2.api.radio-browser.info",
    "nl1.api.radio-browser.info",
    "at1.api.radio-browser.info",
    "all.api.radio-browser.info",
};
#define NUM_API_SERVERS 5
static int current_server = 0; /* Last working server index */

/* API path templates - {server} gets replaced at runtime */
#define TAG_LIST_PATH       "/json/tags?limit=50&order=stationcount&reverse=true&hidebroken=true"
#define TAG_STATIONS_PATH   "/json/stations/bytagexact/%s?limit=%d&order=clickcount&reverse=true&hidebroken=true"
#define TOPCLICK_PATH       "/json/stations/topclick/%d?hidebroken=true"
#define COUNTRY_STATIONS_PATH "/json/stations/bycountrycodeexact/%s?limit=%d&order=clickcount&reverse=true&hidebroken=true"
#define SEARCH_PATH         "/json/stations/search?name=%s&limit=%d&order=clickcount&reverse=true&hidebroken=true"
#define LANGUAGE_LIST_PATH  "/json/languages?limit=50&order=stationcount&reverse=true&hidebroken=true"
#define LANGUAGE_STATIONS_PATH "/json/stations/bylanguageexact/%s?limit=%d&order=clickcount&reverse=true&hidebroken=true"
#define STREAM_PATH         "/json/url/%s"

static char user_agent[64];
static bool initialized = false;

void radio_init(void) {
    snprintf(user_agent, sizeof(user_agent), "3DSRadio/1.0");
    initialized = true;
}

/* Build a full URL from a server hostname and path */
static void build_url(const char *server, const char *path, char *url, size_t size) {
    snprintf(url, size, "http://%s%s", server, path);
}

/* HTTP GET with automatic server failover.
 * Tries current_server first, then falls back to others.
 * On success, updates current_server to the working one. */
static int radio_http_get(const char *path, char **buffer, size_t *buffer_size,
                          char *error, size_t error_size) {
    char last_error[256] = {0};
    int last_ret = NET_OK;

    /* Try current (last known good) server first */
    int start = current_server;
    for (int i = 0; i < NUM_API_SERVERS; i++) {
        int idx = (start + i) % NUM_API_SERVERS;
        char url[512];
        build_url(API_SERVERS[idx], path, url, sizeof(url));

        int ret = net_get(url, buffer, buffer_size, last_error, sizeof(last_error));
        if (ret == NET_OK && *buffer && *buffer_size > 0) {
            /* Success - remember this server for next time */
            current_server = idx;
            return NET_OK;
        }
        last_ret = ret;
        /* Brief pause between server attempts */
        if (i < NUM_API_SERVERS - 1) {
            svcSleepThread(50000000LL); /* 50ms */
        }
    }

    if (error) {
        snprintf(error, error_size, "All servers failed (last: %s)",
                 last_error[0] ? last_error : "transport error");
    }
    return last_ret;
}

/* URL-encode a string (simplified - handles spaces and basic chars) */
static void url_encode(const char *src, char *dst, size_t dst_size) {
    size_t pos = 0;
    for (const char *s = src; *s && pos < dst_size - 1; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == ' ') {
            if (pos + 3 > dst_size) break;
            dst[pos++] = '%';
            dst[pos++] = '2';
            dst[pos++] = '0';
        } else if (c == '&') {
            if (pos + 3 > dst_size) break;
            dst[pos++] = '%';
            dst[pos++] = '2';
            dst[pos++] = '6';
        } else if (c == '=') {
            if (pos + 3 > dst_size) break;
            dst[pos++] = '%';
            dst[pos++] = '3';
            dst[pos++] = 'D';
        } else if (c == '/') {
            if (pos + 3 > dst_size) break;
            dst[pos++] = '%';
            dst[pos++] = '2';
            dst[pos++] = 'F';
        } else if (c < 0x20 || c > 0x7E) {
            if (pos + 3 > dst_size) break;
            dst[pos++] = '%';
            static const char hex[] = "0123456789ABCDEF";
            dst[pos++] = hex[c >> 4];
            dst[pos++] = hex[c & 0xF];
        } else {
            dst[pos++] = c;
        }
    }
    dst[pos] = '\0';
}

/* Parse a single station from a JSON object */
static bool parse_station(const JsonDoc *doc, RadioStation *station) {
    if (!doc || !station) return false;

    memset(station, 0, sizeof(RadioStation));

    /* stationuuid */
    int tok = json_obj_get(doc, 0, "stationuuid");
    if (tok >= 0) json_string(doc, tok, station->stationuuid, sizeof(station->stationuuid));

    /* name */
    tok = json_obj_get(doc, 0, "name");
    if (tok >= 0) json_string(doc, tok, station->name, sizeof(station->name));

    /* url */
    tok = json_obj_get(doc, 0, "url");
    if (tok >= 0) json_string(doc, tok, station->url, sizeof(station->url));

    /* url_resolved */
    tok = json_obj_get(doc, 0, "url_resolved");
    if (tok >= 0) json_string(doc, tok, station->url_resolved, sizeof(station->url_resolved));

    /* homepage */
    tok = json_obj_get(doc, 0, "homepage");
    if (tok >= 0) json_string(doc, tok, station->homepage, sizeof(station->homepage));

    /* favicon */
    tok = json_obj_get(doc, 0, "favicon");
    if (tok >= 0) json_string(doc, tok, station->favicon, sizeof(station->favicon));

    /* tags */
    tok = json_obj_get(doc, 0, "tags");
    if (tok >= 0) json_string(doc, tok, station->tags, sizeof(station->tags));

    /* country */
    tok = json_obj_get(doc, 0, "country");
    if (tok >= 0) json_string(doc, tok, station->country, sizeof(station->country));

    /* countrycode */
    tok = json_obj_get(doc, 0, "countrycode");
    if (tok >= 0) json_string(doc, tok, station->countrycode, sizeof(station->countrycode));

    /* state */
    tok = json_obj_get(doc, 0, "state");
    if (tok >= 0) json_string(doc, tok, station->state, sizeof(station->state));

    /* language */
    tok = json_obj_get(doc, 0, "language");
    if (tok >= 0) json_string(doc, tok, station->language, sizeof(station->language));

    /* codec */
    tok = json_obj_get(doc, 0, "codec");
    if (tok >= 0) json_string(doc, tok, station->codec, sizeof(station->codec));

    /* bitrate */
    tok = json_obj_get(doc, 0, "bitrate");
    if (tok >= 0) {
        int64_t val;
        if (json_i64(doc, tok, &val) == 0) station->bitrate = (int)val;
    }

    /* votes */
    tok = json_obj_get(doc, 0, "votes");
    if (tok >= 0) {
        int64_t val;
        if (json_i64(doc, tok, &val) == 0) station->votes = (int)val;
    }

    /* clickcount */
    tok = json_obj_get(doc, 0, "clickcount");
    if (tok >= 0) {
        int64_t val;
        if (json_i64(doc, tok, &val) == 0) station->clickcount = (int)val;
    }

    /* is_https */
    tok = json_obj_get(doc, 0, "is_https");
    if (tok >= 0 && !json_is_null(doc, tok)) {
        int64_t val;
        if (json_i64(doc, tok, &val) == 0) station->is_https = (val != 0);
    }

    /* lastcheckok */
    tok = json_obj_get(doc, 0, "lastcheckok");
    if (tok >= 0 && !json_is_null(doc, tok)) {
        int64_t val;
        if (json_i64(doc, tok, &val) == 0) station->lastcheckok = (val != 0);
    }

    /* clicktrend */
    tok = json_obj_get(doc, 0, "clicktrend");
    if (tok >= 0) {
        int64_t val;
        if (json_i64(doc, tok, &val) == 0) station->clicktrend = (int)val;
    }

    return true;
}

/* Visitor callback for parsing stations from JSON array */
struct StationVisitorData {
    RadioStation *stations;
    int max_stations;
    int count;
};

static int station_visitor(const JsonDoc *doc, void *userdata) {
    struct StationVisitorData *data = (struct StationVisitorData *)userdata;
    if (data->count >= data->max_stations) return 1; /* Stop iteration */

    if (parse_station(doc, &data->stations[data->count])) {
        data->count++;
    }
    return 0;
}

/* Fetch and parse a list of stations from a path, with server failover */
static int fetch_stations(const char *path, RadioStation *stations, int max_stations,
                           char *error, size_t error_size) {
    if (!initialized) {
        snprintf(error, error_size, "Radio API not initialized");
        return -1;
    }

    char *response = NULL;
    size_t response_size = 0;

    int ret = radio_http_get(path, &response, &response_size, error, error_size);
    if (ret != NET_OK) {
        if (error && !error[0])
            snprintf(error, error_size, "HTTP error: %d", ret);
        return -1;
    }

    if (!response || response_size == 0) {
        if (error) snprintf(error, error_size, "Empty response");
        free(response);
        return -1;
    }

    JsonToken tokens[JSON_TOKEN_CAPACITY];
    struct StationVisitorData data = {
        .stations = stations,
        .max_stations = max_stations,
        .count = 0
    };

    /* The response is a JSON array of objects */
    int visited = json_visit_array_objects(response, NULL, tokens, JSON_TOKEN_CAPACITY,
                                           station_visitor, &data);

    free(response);

    if (visited < 0 && visited != JSON_VISIT_NOT_FOUND) {
        if (error) snprintf(error, error_size, "JSON parse error: %d", visited);
        return -1;
    }

    return data.count;
}

/* Helper: URL-encode a param and build the station path */
static int fetch_stations_by_param(const char *pattern, const char *param,
                                    RadioStation *stations, int max_stations,
                                    char *error, size_t error_size) {
    char encoded_param[256];
    url_encode(param, encoded_param, sizeof(encoded_param));

    char path[512];
    snprintf(path, sizeof(path), pattern, encoded_param, max_stations);

    return fetch_stations(path, stations, max_stations, error, error_size);
}

int radio_fetch_tags(RadioTag *tags, int max_tags,
                      char *error, size_t error_size) {
    if (!initialized) {
        snprintf(error, error_size, "Radio API not initialized");
        return -1;
    }

    char *response = NULL;
    size_t response_size = 0;

    int ret = radio_http_get(TAG_LIST_PATH, &response, &response_size, error, error_size);
    if (ret != NET_OK) {
        if (error) snprintf(error, error_size, "HTTP error: %d", ret);
        return -1;
    }

    if (!response || response_size == 0) {
        free(response);
        return 0;
    }

    /* Parse JSON array of tag objects */
    JsonToken tokens[JSON_TOKEN_CAPACITY];
    JsonDoc doc;
    int count = json_parse(&doc, response, tokens, JSON_TOKEN_CAPACITY);
    if (count < 0) {
        free(response);
        if (error) snprintf(error, error_size, "JSON parse error");
        return -1;
    }

    int tag_count = 0;
    int array = 0; /* Root element is the array */

    for (int i = 0; i < json_arr_size(&doc, array) && tag_count < max_tags; i++) {
        int obj = json_arr_get(&doc, array, i);
        if (obj < 0) continue;

        int name_tok = json_obj_get(&doc, obj, "name");
        int count_tok = json_obj_get(&doc, obj, "stationcount");

        if (name_tok >= 0 && count_tok >= 0) {
            RadioTag *tag = &tags[tag_count];
            json_string(&doc, name_tok, tag->name, sizeof(tag->name));
            int64_t val;
            if (json_i64(&doc, count_tok, &val) == 0) {
                tag->stationcount = (int)val;
            }
            tag_count++;
        }
    }

    free(response);
    return tag_count;
}

int radio_fetch_by_tag(const char *tag, RadioStation *stations, int max_stations,
                        char *error, size_t error_size) {
    return fetch_stations_by_param(TAG_STATIONS_PATH, tag, stations, max_stations,
                                    error, error_size);
}

int radio_fetch_topclick(RadioStation *stations, int max_stations,
                          char *error, size_t error_size) {
    char path[256];
    snprintf(path, sizeof(path), TOPCLICK_PATH, max_stations);
    return fetch_stations(path, stations, max_stations, error, error_size);
}

int radio_fetch_by_country(const char *country, RadioStation *stations,
                            int max_stations, char *error, size_t error_size) {
    return fetch_stations_by_param(COUNTRY_STATIONS_PATH, country, stations,
                                    max_stations, error, error_size);
}

int radio_search_by_name(const char *name, RadioStation *stations,
                          int max_stations, char *error, size_t error_size) {
    return fetch_stations_by_param(SEARCH_PATH, name, stations, max_stations,
                                    error, error_size);
}

int radio_fetch_languages(RadioLanguage *languages, int max_languages,
                           char *error, size_t error_size) {
    if (!initialized) {
        snprintf(error, error_size, "Radio API not initialized");
        return -1;
    }

    char *response = NULL;
    size_t response_size = 0;

    int ret = radio_http_get(LANGUAGE_LIST_PATH, &response, &response_size, error, error_size);
    if (ret != NET_OK) {
        if (error) snprintf(error, error_size, "HTTP error: %d", ret);
        return -1;
    }

    if (!response || response_size == 0) {
        free(response);
        return 0;
    }

    /* Parse JSON array of language objects */
    JsonToken tokens[JSON_TOKEN_CAPACITY];
    JsonDoc doc;
    int count = json_parse(&doc, response, tokens, JSON_TOKEN_CAPACITY);
    if (count < 0) {
        free(response);
        if (error) snprintf(error, error_size, "JSON parse error");
        return -1;
    }

    int lang_count = 0;
    int array = 0; /* Root element is the array */

    for (int i = 0; i < json_arr_size(&doc, array) && lang_count < max_languages; i++) {
        int obj = json_arr_get(&doc, array, i);
        if (obj < 0) continue;

        int name_tok = json_obj_get(&doc, obj, "name");
        int count_tok = json_obj_get(&doc, obj, "stationcount");

        if (name_tok >= 0 && count_tok >= 0) {
            RadioLanguage *lang = &languages[lang_count];
            json_string(&doc, name_tok, lang->name, sizeof(lang->name));
            int64_t val;
            if (json_i64(&doc, count_tok, &val) == 0) {
                lang->stationcount = (int)val;
            }
            lang_count++;
        }
    }

    free(response);
    return lang_count;
}

int radio_fetch_by_language(const char *language, RadioStation *stations,
                             int max_stations, char *error, size_t error_size) {
    return fetch_stations_by_param(LANGUAGE_STATIONS_PATH, language, stations,
                                    max_stations, error, error_size);
}

int radio_get_stream_url(const char *stationuuid, char *url, size_t url_size,
                          char *error, size_t error_size) {
    if (!initialized || !stationuuid || !url) return -1;

    char path[256];
    snprintf(path, sizeof(path), STREAM_PATH, stationuuid);

    char *response = NULL;
    size_t response_size = 0;

    int ret = radio_http_get(path, &response, &response_size, error, error_size);
    if (ret != NET_OK) {
        if (error) snprintf(error, error_size, "HTTP error: %d", ret);
        return -1;
    }

    if (!response || response_size == 0) {
        free(response);
        return -1;
    }

    /* Parse JSON response */
    JsonToken tokens[64];
    JsonDoc doc;
    int count = json_parse(&doc, response, tokens, 64);
    if (count < 0) {
        free(response);
        return -1;
    }

    /* The response is an object with "url" field */
    int url_tok = json_obj_get(&doc, 0, "url");
    if (url_tok < 0) {
        free(response);
        if (error) snprintf(error, error_size, "No URL in response");
        return -1;
    }

    json_string(&doc, url_tok, url, (int)url_size);
    free(response);
    return NET_OK;
}

void radio_exit(void) {
    initialized = false;
}
