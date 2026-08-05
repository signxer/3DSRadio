#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ======================================================================
 * Network layer for 3DS using libcurl
 * Adapted from ClouDS-Music's net.h
 * ====================================================================== */

/* Maximum response body size */
#define NET_MAX_RESPONSE (2 * 1024 * 1024)

/* Error codes */
typedef enum {
    NET_OK = 0,
    NET_ERROR_AUTH = -1,
    NET_ERROR_HTTP = -2,
    NET_ERROR_TRANSPORT = -3,
    NET_ERROR_TLS_VERIFY = -4,
    NET_ERROR_CANCELLED = -5,
    NET_ERROR_MEMORY = -6,
    NET_ERROR_TIMEOUT = -7,
} NetError;

/* Callback for cancellation: return non-zero to cancel */
typedef int (*NetCancelFn)(void *userdata);

/* Callback for streaming response data: return non-zero to abort */
typedef int (*NetResponseWrite)(const char *data, size_t len, void *userdata);

/* Initialize networking (SOC, AC, curl). Returns 0 on success. */
int net_init(void);

/* Shutdown networking */
void net_exit(void);

/* Check WiFi status. Returns true if connected. */
bool net_wifi_status(void);

/* HTTP GET request into a memory buffer.
 * buffer will be allocated via malloc/realloc, caller must free.
 * Returns NET_OK on success, negative on error. */
int net_get(const char *url,
            char **buffer, size_t *buffer_size,
            char *error, size_t error_size);

/* HTTP GET with cancellation support */
int net_get_controlled(const char *url,
                       char **buffer, size_t *buffer_size,
                       NetCancelFn cancel, void *cancel_data,
                       char *error, size_t error_size);

/* HTTP GET with streaming response */
int net_get_stream(const char *url,
                   NetResponseWrite write_cb, void *write_data,
                   NetCancelFn cancel, void *cancel_data,
                   char *error, size_t error_size);

/* Get a human-readable error string for a NetError code */
const char *net_strerror(NetError err);

/* Get the last curl error string */
const char *net_last_curl_error(void);
