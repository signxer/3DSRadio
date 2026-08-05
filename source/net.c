#include "net.h"

#include <3ds.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

/* ======================================================================
 * Network layer for 3DS using libcurl
 * Adapted from ClouDS-Music's net.c
 * ====================================================================== */

#define SOC_BUFFER_SIZE 0x100000
static u32 *soc_buffer = NULL;
static bool net_initialized = false;
static char last_curl_error[CURL_ERROR_SIZE];

/* Memory buffer write callback */
struct MemoryBuffer {
    char *data;
    size_t size;
};

static size_t write_memory_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemoryBuffer *mem = (struct MemoryBuffer *)userp;

    char *ptr = realloc(mem->data, mem->size + realsize + 1);
    if (!ptr) return 0;

    mem->data = ptr;
    memcpy(&(mem->data[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->data[mem->size] = 0;
    return realsize;
}

/* Header write callback */
static size_t write_header_cb(char *buffer, size_t size, size_t nitems, void *userdata) {
    (void)buffer;
    (void)userdata;
    return size * nitems;
}

/* Configure common curl options for HTTPS */
static CURL *setup_curl(const char *url, NetCancelFn cancel, void *cancel_data) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "3DSRadio/1.0 (Nintendo 3DS)");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "identity");
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, write_header_cb);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, last_curl_error);

    if (cancel) {
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, cancel);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, cancel_data);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    }

    return curl;
}

int net_init(void) {
    if (net_initialized) return 0;

    /* Initialize SOC service */
    soc_buffer = (u32 *)linearAlloc(SOC_BUFFER_SIZE);
    if (!soc_buffer) return NET_ERROR_MEMORY;

    /* Align to 0x1000 boundary */
    soc_buffer = (u32 *)(((uintptr_t)soc_buffer + 0xFFF) & ~0xFFF);

    acInit();

    Result ret = socInit(soc_buffer, SOC_BUFFER_SIZE);
    if (R_FAILED(ret)) {
        linearFree(soc_buffer);
        soc_buffer = NULL;
        return NET_ERROR_TRANSPORT;
    }

    curl_global_init(CURL_GLOBAL_ALL);
    net_initialized = true;
    return NET_OK;
}

void net_exit(void) {
    if (!net_initialized) return;

    curl_global_cleanup();
    socExit();
    acExit();

    if (soc_buffer) {
        linearFree(soc_buffer);
        soc_buffer = NULL;
    }
    net_initialized = false;
}

bool net_wifi_status(void) {
    u32 wifi_status = 0;
    ACU_GetWifiStatus(&wifi_status);
    return wifi_status != 0;
}

/* Check if a curl result indicates a transient error worth retrying */
static bool is_transient_error(CURLcode res, long http_code) {
    if (res != CURLE_OK) {
        if (res == CURLE_OPERATION_TIMEDOUT) return true;
        if (res == CURLE_COULDNT_CONNECT) return true;
        if (res == CURLE_SEND_ERROR) return true;
        if (res == CURLE_RECV_ERROR) return true;
        return false;
    }
    if (http_code == 408 || http_code == 429 || http_code == 500 ||
        http_code == 502 || http_code == 503 || http_code == 504) {
        return true;
    }
    return false;
}

/* Perform a request with a single retry on transient failures */
static NetError perform_with_retry(CURL *curl, struct MemoryBuffer *chunk,
                                    NetCancelFn cancel, void *cancel_data) {
    for (int attempt = 0; attempt < 2; attempt++) {
        CURLcode res = curl_easy_perform(curl);
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

        if (res == CURLE_OK && http_code == 200) return NET_OK;

        if (is_transient_error(res, http_code) && attempt == 0) {
            /* Wait before retry */
            for (int i = 0; i < 10; i++) {
                svcSleepThread(25000000LL); /* 25ms */
                if (cancel && cancel(cancel_data)) {
                    return NET_ERROR_CANCELLED;
                }
            }
            /* Reset chunk for retry */
            chunk->size = 0;
            continue;
        }

        /* Map error */
        if (res == CURLE_SSL_CONNECT_ERROR || res == CURLE_PEER_FAILED_VERIFICATION ||
            res == CURLE_SSL_CERTPROBLEM || res == CURLE_SSL_CIPHER ||
            res == CURLE_SSL_CACERT) {
            return NET_ERROR_TLS_VERIFY;
        }
        if (res == CURLE_OPERATION_TIMEDOUT) return NET_ERROR_TIMEOUT;
        if (res == CURLE_COULDNT_RESOLVE_HOST || res == CURLE_COULDNT_RESOLVE_PROXY || res == CURLE_COULDNT_CONNECT || res == CURLE_SEND_ERROR ||
            res == CURLE_RECV_ERROR || res == CURLE_GOT_NOTHING)
            return NET_ERROR_TRANSPORT;
        if (res == CURLE_ABORTED_BY_CALLBACK) return NET_ERROR_CANCELLED;
        if (http_code == 401 || http_code == 403) return NET_ERROR_AUTH;
        return NET_ERROR_HTTP;
    }
    return NET_ERROR_TRANSPORT;
}

int net_get(const char *url, char **buffer, size_t *buffer_size,
            char *error, size_t error_size) {
    return net_get_controlled(url, buffer, buffer_size, NULL, NULL, error, error_size);
}

int net_get_controlled(const char *url, char **buffer, size_t *buffer_size,
                        NetCancelFn cancel, void *cancel_data,
                        char *error, size_t error_size) {
    if (!url || !buffer || !buffer_size) return NET_ERROR_HTTP;

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
        *buffer = chunk.data;
        *buffer_size = chunk.size;
    } else {
        free(chunk.data);
        if (buffer) *buffer = NULL;
        if (buffer_size) *buffer_size = 0;
        if (error) {
            snprintf(error, error_size, "Request failed (error %d)", result);
        }
    }

    curl_easy_cleanup(curl);
    return result;
}

struct StreamState {
    NetResponseWrite write_cb;
    void *write_data;
    NetCancelFn cancel;
    void *cancel_data;
    bool aborted;
};

static size_t stream_write_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    struct StreamState *state = (struct StreamState *)userp;
    if (state->aborted) return 0;

    if (state->cancel && state->cancel(state->cancel_data)) {
        state->aborted = true;
        return 0;
    }

    if (state->write_cb) {
        int ret = state->write_cb((const char *)contents, size * nmemb, state->write_data);
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

    CURL *curl = setup_curl(url, cancel, cancel_data);
    if (!curl) {
        if (error) snprintf(error, error_size, "Failed to init curl");
        return NET_ERROR_MEMORY;
    }

    /* Longer timeout for streaming */
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L); /* No overall timeout */
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
        if (res == CURLE_SSL_CONNECT_ERROR) return NET_ERROR_TLS_VERIFY;
        if (res == CURLE_OPERATION_TIMEDOUT) return NET_ERROR_TIMEOUT;
        if (res == CURLE_COULDNT_CONNECT) return NET_ERROR_TRANSPORT;
        return NET_ERROR_TRANSPORT;
    }
    return NET_OK;
}

const char *net_strerror(NetError err) {
    switch (err) {
        case NET_OK: return "OK";
        case NET_ERROR_AUTH: return "Authentication failed";
        case NET_ERROR_HTTP: return "HTTP error";
        case NET_ERROR_TRANSPORT: return "Transport error";
        case NET_ERROR_TLS_VERIFY: return "TLS certificate verification failed";
        case NET_ERROR_CANCELLED: return "Cancelled";
        case NET_ERROR_MEMORY: return "Out of memory";
        case NET_ERROR_TIMEOUT: return "Connection timed out";
        default: return "Unknown error";
    }
}

const char *net_last_curl_error(void) {
    return last_curl_error;
}
