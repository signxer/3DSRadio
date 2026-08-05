#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ======================================================================
 * Lightweight JSON parser for embedded/3DS use
 * Adapted from ClouDS-Music's json.h
 * ====================================================================== */

typedef enum {
    JSON_UNDEFINED = 0,
    JSON_OBJECT,
    JSON_ARRAY,
    JSON_STRING,
    JSON_PRIMITIVE
} JsonType;

typedef struct {
    JsonType type;
    int start;
    int end;
    int size;
    int parent;
} JsonToken;

typedef struct {
    const char *text;
    JsonToken *tokens;
    int count;
} JsonDoc;

typedef int (*JsonObjectVisitor)(const JsonDoc *doc, void *userdata);

enum {
    JSON_VISIT_TOKENS_EXHAUSTED = -1,
    JSON_VISIT_INVALID = -2,
    JSON_VISIT_NOT_FOUND = -3,
    JSON_VISIT_CALLBACK_FAILED = -4,
};

/* Parse JSON text into tokens. Returns token count or negative on error. */
int json_parse(JsonDoc *doc, const char *text, JsonToken *tokens, int capacity);

/* Look up a key in an object. Returns child token index or -1. */
int json_obj_get(const JsonDoc *doc, int object, const char *key);

/* Access array element by index. Returns child token index or -1. */
int json_arr_get(const JsonDoc *doc, int array, int index);

/* Get array size. Returns count or -1. */
int json_arr_size(const JsonDoc *doc, int array);

/* Copy string value to buffer. Returns string length or -1. */
int json_string(const JsonDoc *doc, int token, char *out, size_t out_size);

/* Parse primitive as int64. Returns 0 on success, -1 on error. */
int json_i64(const JsonDoc *doc, int token, int64_t *out);

/* Check if a token is JSON null. */
bool json_is_null(const JsonDoc *doc, int token);

/* Visit each object in an array identified by key.
 * Calls visitor for each, reusing the token buffer.
 * Returns count visited, or negative on error. */
int json_visit_array_objects(char *text, const char *key,
                             JsonToken *tokens, int capacity,
                             JsonObjectVisitor visitor, void *userdata);
