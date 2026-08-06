#include "net.h"

#include <3ds.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <malloc.h>

/* ======================================================================
 * Network layer for 3DS using libcurl
 * Ported from ClouDS-Music-FA's net.c — same init sequence, same
 * memalign-based SOC buffer, same curl configuration pattern.
 * ====================================================================== */

#define SOC_ALIGN       0x1000U
#define SOC_BUFFER_SIZE (1024U * 1024U)   /* 1 MiB */
#define CA_BUNDLE_PATH  "romfs:/cacert.pem"

static u32 *soc_buffer = NULL;
static bool net_initialized = false;
static char last_curl_error[CURL_ERROR_SIZE];

/* ======================================================================
 * Initialization — mirrors ClouDS-Music-FA exactly
 * ====================================================================== */

int net_init(void) {
    if (net_initialized) return NET_OK;

    /* Verify CA cert bundle is present in RomFS */
    FILE *ca_file = fopen(CA_BUNDLE_PATH, "rb");
    if (!ca_file) {
        /* Non-fatal: radio-browser.info uses plain HTTP anyway.
         * We'll just skip TLS verification for HTTPS URLs. */
    } else {
        fclose(ca_file);
    }

    /* Initialize wireless AC service (not fatal if it fails) */
    acInit();

    /* Allocate SOC buffer with proper 0x1000-byte alignment.
     * ClouDS-Music-FA uses memalign for this — linearAlloc only
     * guarantees 0x80 alignment which causes socInit to corrupt
     * the heap when we pass the full SOC_BUFFER_SIZE. */
    soc_buffer = (u32 *)memalign(SOC_ALIGN, SOC_BUFFER_SIZE);
    if (!soc_buffer) {
        return NET_ERROR_MEMORY;
    }

    Result ret = socInit(soc_buffer, SOC_BUFFER_SIZE);
    if (R_FAILED(ret)) {
        free(soc_buffer);
        soc_buffer = NULL;
        return NET_ERROR_TRANSPORT;
    }

    CURLcode code = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (code != CURLE_OK) {
        socExit();
        free(soc_buffer);
        soc_buffer = NULL;
        return NET_ERROR_TRANSPORT;
    }

    net_initialized = true;
    return NET_OK;
}

void net_exit(void) {
    if (!net_initialized) return;

    curl_global_cleanup();
    socExit();
    acExit();

    if (soc_buffer) {
        free(soc_buffer);
        soc_buffer = NULL;
    }
    net_initialized = false;
}

/* ======================================================================
 * WiFi status — tolerant of emulators
 * ====================================================================== */

bool net_wifi_status(void) {
    /* In emulators, ACU_GetWifiStatus may return 0 even when networking
     * works through host passthrough. If SOC is initialized, assume
     * network is available. */
    if (net_initialized) return true;

    u32 wifi_status = 0;
    Result result = ACU_GetWifiStatus(&wifi_status);
    if (R_FAILED(result)) return false;
    return wifi_status != 0;
}

/* ======================================================================
 * Memory buffer write callback (libcurl → heap)
 * ====================================================================== */

struct MemoryBuffer {
    char *data;
    size_t size;
    size_t capacity;
    bool overflow;
};

#define HTTP_MAX_RESPONSE (2U * 1024U * 1024U)  /* 2 MiB cap */

static size_t write_memory_cb(void *contents, size_t size, size_t nmemb,
                               void *userp) {
    struct MemoryBuffer *mem = (struct MemoryBuffer *)userp;
    if (size != 0 && nmemb > SIZE_MAX / size) return 0;
    size_t realsize = size * nmemb;

    /* Cap total response size */
    if (realsize > HTTP_MAX_RESPONSE - mem->size) {
        mem->overflow = true;
        return 0;
    }

    size_t required = mem->size + realsize + 1;
    if (required > mem->capacity) {
        size_t capacity = mem->capacity ? mem->capacity : 16384;
        const size_t maximum = HTTP_MAX_RESPONSE + 1U;
        while (capacity < required) {
            if (capacity > maximum / 2U) {
                capacity = maximum;
                break;
            }
            capacity *= 2U;
        }
        char *grown = (char *)realloc(mem->data, capacity);
        if (!grown) return 0;
        mem->data = grown;
        mem->capacity = capacity;
    }

    memcpy(&(mem->data[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->data[mem->size] = 0;
    return realsize;
}

/* ======================================================================
 * Header write callback (discards headers for simple GET)
 * ====================================================================== */

static size_t write_header_cb(char *buffer, size_t size, size_t nitems,
                               void *userdata) {
    (void)buffer;
    (void)userdata;
    return size * nitems;
}

/* ======================================================================
 * Curl handle setup — common configuration
 * ====================================================================== */

static CURL *setup_curl(const char *url, NetCancelFn cancel, void *cancel_data) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
                     "3DSRadio/1.0 (Nintendo 3DS)");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "identity");
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, write_header_cb);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, last_curl_error);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);

    /* Enable SSL certificate verification when CA bundle is present */
    FILE *ca = fopen(CA_BUNDLE_PATH, "rb");
    if (ca) {
        fclose(ca);
        curl_easy_setopt(curl, CURLOPT_CAINFO, CA_BUNDLE_PATH);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
    } else {
        /* CA bundle missing — disable verification as fallback.
         * radio-browser.info uses plain HTTP so this is fine for
         * the primary use case. */
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }

    if (cancel) {
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, cancel);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, cancel_data);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    }

    return curl;
}

/* ======================================================================
 * Transient error detection for retry logic
 * ====================================================================== */

static bool is_transient_error(CURLcode res, long http_code) {
    if (res != CURLE_OK) {
        switch (res) {
            case CURLE_OPERATION_TIMEDOUT:
            case CURLE_COULDNT_CONNECT:
            case CURLE_COULDNT_RESOLVE_HOST:
            case CURLE_COULDNT_RESOLVE_PROXY:
            case CURLE_SEND_ERROR:
            case CURLE_RECV_ERROR:
            case CURLE_GOT_NOTHING:
            case CURLE_PARTIAL_FILE:
            case CURLE_SSL_CONNECT_ERROR:
                return true;
            default:
                return false;
        }
    }
    /* Retryable HTTP status codes */
    return http_code == 408 || http_code == 429 ||
           http_code == 500 || http_code == 502 ||
           http_code == 503 || http_code == 504;
}

/* ======================================================================
 * Request execution with single retry on transient failures
 * ====================================================================== */

static NetError perform_with_retry(CURL *curl, struct MemoryBuffer *chunk,
                                    NetCancelFn cancel, void *cancel_data) {
    for (int attempt = 0; attempt < 2; attempt++) {
        CURLcode res = curl_easy_perform(curl);
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

        if (res == CURLE_OK && http_code >= 200 && http_code < 300)
            return NET_OK;

        if (is_transient_error(res, http_code) && attempt == 0) {
            /* Wait 250ms before retry, checking for cancellation */
            for (int i = 0; i < 10; i++) {
                svcSleepThread(25000000LL);  /* 25 ms */
                if (cancel && cancel(cancel_data))
                    return NET_ERROR_CANCELLED;
            }
            /* Reset chunk for retry */
            chunk->size = 0;
            continue;
        }

        /* Map curl/HTTP error to our error enum */
        if (res == CURLE_SSL_CONNECT_ERROR ||
            res == CURLE_PEER_FAILED_VERIFICATION ||
            res == CURLE_SSL_CERTPROBLEM ||
            res == CURLE_SSL_CIPHER ||
            res == CURLE_SSL_CACERT)
            return NET_ERROR_TLS_VERIFY;
        if (res == CURLE_OPERATION_TIMEDOUT)
            return NET_ERROR_TIMEOUT;
        if (res == CURLE_COULDNT_RESOLVE_HOST ||
            res == CURLE_COULDNT_RESOLVE_PROXY ||
            res == CURLE_COULDNT_CONNECT ||
            res == CURLE_SEND_ERROR ||
            res == CURLE_RECV_ERROR ||
            res == CURLE_GOT_NOTHING)
            return NET_ERROR_TRANSPORT;
        if (res == CURLE_ABORTED_BY_CALLBACK)
            return NET_ERROR_CANCELLED;
        if (http_code == 401 || http_code == 403)
            return NET_ERROR_AUTH;
        return NET_ERROR_HTTP;
    }
    return NET_ERROR_TRANSPORT;
}

/* ======================================================================
 * Public API: HTTP GET (with and without cancellation)
 * ====================================================================== */

int net_get(const char *url, char **buffer, size_t *buffer_size,
            char *error, size_t error_size) {
    return net_get_controlled(url, buffer, buffer_size, NULL, NULL,
                              error, error_size);
}

int net_get_controlled(const char *url, char **buffer, size_t *buffer_size,
                        NetCancelFn cancel, void *cancel_data,
                        char *error, size_t error_size) {
    if (!url || !buffer || !buffer_size)
        return NET_ERROR_HTTP;

    CURL *curl = setup_curl(url, cancel, cancel_data);
    if (!curl) {
        if (error) snprintf(error, error_size, "Failed to init curl");
        return NET_ERROR_MEMORY;
    }

    struct MemoryBuffer chunk = {0};
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_memory_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);

    NetError result = perform_with_retry(curl, &chunk, cancel, cancel_data);

    if (result == NET_OK) {
        if (!chunk.data) {
            chunk.data = (char *)calloc(1, 1);
            if (!chunk.data) {
                curl_easy_cleanup(curl);
                return NET_ERROR_MEMORY;
            }
        }
        *buffer = chunk.data;
        *buffer_size = chunk.size;
    } else {
        free(chunk.data);
        *buffer = NULL;
        *buffer_size = 0;
        if (error) {
            if (chunk.overflow) {
                snprintf(error, error_size, "Response exceeds 2 MiB limit");
            } else {
                snprintf(error, error_size, "Request failed (error %d)", result);
            }
        }
    }

    curl_easy_cleanup(curl);
    return result;
}

/* ======================================================================
 * Public API: HTTP streaming GET (for audio stream proxying)
 * ====================================================================== */

struct StreamState {
    NetResponseWrite write_cb;
    void *write_data;
    NetCancelFn cancel;
    void *cancel_data;
    bool aborted;
};

static size_t stream_write_cb(void *contents, size_t size, size_t nmemb,
                               void *userp) {
    struct StreamState *state = (struct StreamState *)userp;
    if (state->aborted) return 0;

    if (state->cancel && state->cancel(state->cancel_data)) {
        state->aborted = true;
        return 0;
    }

    if (state->write_cb) {
        int ret = state->write_cb((const char *)contents, size * nmemb,
                                   state->write_data);
        if (ret != 0) {
            state->aborted = true;
            return 0;
        }
    }
    return size * nmemb;
}

int net_get_stream(const char *url,
                    NetResponseWrite write_cb, void *write_data,
                    NetCancelFn cancel, void *cancel_data,
                    char *error, size_t error_size) {
    if (!url || !write_cb) return NET_ERROR_HTTP;
    (void)error;
    (void)error_size;

    CURL *curl = setup_curl(url, cancel, cancel_data);
    if (!curl) return NET_ERROR_MEMORY;

    /* No overall timeout for streaming — keep connection alive */
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);

    struct StreamState state = {
        .write_cb = write_cb,
        .write_data = write_data,
        .cancel = cancel,
        .cancel_data = cancel_data,
        .aborted = false
    };

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&state);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (state.aborted) return NET_ERROR_CANCELLED;
    if (res != CURLE_OK) {
        if (res == CURLE_SSL_CONNECT_ERROR)
            return NET_ERROR_TLS_VERIFY;
        if (res == CURLE_OPERATION_TIMEDOUT)
            return NET_ERROR_TIMEOUT;
        return NET_ERROR_TRANSPORT;
    }
    return NET_OK;
}

/* ======================================================================
 * Error string helpers
 * ====================================================================== */

const char *net_strerror(NetError err) {
    switch (err) {
        case NET_OK:           return "OK";
        case NET_ERROR_AUTH:   return "Authentication failed";
        case NET_ERROR_HTTP:   return "HTTP error";
        case NET_ERROR_TRANSPORT: return "Transport error";
        case NET_ERROR_TLS_VERIFY: return "TLS certificate verification failed";
        case NET_ERROR_CANCELLED: return "Cancelled";
        case NET_ERROR_MEMORY: return "Out of memory";
        case NET_ERROR_TIMEOUT: return "Connection timed out";
        default:               return "Unknown error";
    }
}

const char *net_last_curl_error(void) {
    return last_curl_error;
}
