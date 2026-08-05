#include "json.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* ======================================================================
 * Lightweight JSON parser for embedded/3DS use
 * Adapted from ClouDS-Music's json.c
 * ====================================================================== */

static int token_new(JsonDoc *doc, int capacity, JsonType type,
                     int start, int parent) {
    if (doc->count >= capacity) return -1;
    int index = doc->count++;
    JsonToken *token = &doc->tokens[index];
    token->type = type;
    token->start = start;
    token->end = -1;
    token->size = 0;
    token->parent = parent;
    if (parent >= 0) doc->tokens[parent].size++;
    return index;
}

static int parse_string(JsonDoc *doc, int capacity, int *position,
                        int parent) {
    const char *text = doc->text;
    int start = *position + 1;
    for (int pos = start; text[pos]; pos++) {
        unsigned char c = (unsigned char)text[pos];
        if (c == '"') {
            int index = token_new(doc, capacity, JSON_STRING, start, parent);
            if (index < 0) return -1;
            doc->tokens[index].end = pos;
            *position = pos;
            return index;
        }
        if (c < 0x20) return -2;
        if (c != '\\') continue;
        c = (unsigned char)text[++pos];
        if (!c) return -2;
        if (c == '"' || c == '/' || c == '\\' || c == 'b' || c == 'f' ||
            c == 'n' || c == 'r' || c == 't') continue;
        if (c != 'u') return -2;
        for (int i = 0; i < 4; i++) {
            c = (unsigned char)text[++pos];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                  (c >= 'A' && c <= 'F'))) return -2;
        }
    }
    return -2;
}

static int parse_primitive(JsonDoc *doc, int capacity, int *position,
                           int parent) {
    const char *text = doc->text;
    int start = *position;
    int pos = start;
    while (text[pos] && text[pos] != ' ' && text[pos] != '\t' &&
           text[pos] != '\r' && text[pos] != '\n' && text[pos] != ',' &&
           text[pos] != ']' && text[pos] != '}') {
        unsigned char c = (unsigned char)text[pos];
        if (c < 0x20 || c == ':' || c == '[' || c == '{' || c == '"')
            return -2;
        pos++;
    }
    if (pos == start) return -2;
    int index = token_new(doc, capacity, JSON_PRIMITIVE, start, parent);
    if (index < 0) return -1;
    doc->tokens[index].end = pos;
    *position = pos - 1;
    return index;
}

int json_parse(JsonDoc *doc, const char *text, JsonToken *tokens, int capacity) {
    if (!doc || !text || !tokens || capacity <= 0) return -2;
    doc->text = text;
    doc->tokens = tokens;
    doc->count = 0;
    int parent = -1;

    for (int pos = 0; text[pos]; pos++) {
        char c = text[pos];
        if (c == '{' || c == '[') {
            JsonType type = c == '{' ? JSON_OBJECT : JSON_ARRAY;
            int index = token_new(doc, capacity, type, pos, parent);
            if (index < 0) return -1;
            parent = index;
        } else if (c == '}' || c == ']') {
            JsonType expected = c == '}' ? JSON_OBJECT : JSON_ARRAY;
            if (parent < 0 || doc->tokens[parent].type != expected) return -2;
            doc->tokens[parent].end = pos + 1;
            parent = doc->tokens[parent].parent;
        } else if (c == '"') {
            int result = parse_string(doc, capacity, &pos, parent);
            if (result < 0) return result;
        } else if (c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
                   c == ':' || c == ',') {
            continue;
        } else {
            int result = parse_primitive(doc, capacity, &pos, parent);
            if (result < 0) return result;
        }
    }
    if (parent >= 0 || doc->count == 0) return -2;
    return doc->count;
}

int json_obj_get(const JsonDoc *doc, int object, const char *key) {
    if (!doc || object < 0 || object >= doc->count ||
        doc->tokens[object].type != JSON_OBJECT) return -1;
    int pending_key = -1;
    for (int i = object + 1; i < doc->count; i++) {
        const JsonToken *token = &doc->tokens[i];
        if (token->start >= doc->tokens[object].end) break;
        if (token->parent != object) continue;
        if (pending_key < 0) {
            if (token->type != JSON_STRING) return -1;
            pending_key = i;
        } else {
            const JsonToken *kt = &doc->tokens[pending_key];
            size_t klen = (size_t)(kt->end - kt->start);
            if (strlen(key) == klen &&
                memcmp(doc->text + kt->start, key, klen) == 0) {
                return i;
            }
            pending_key = -1;
        }
    }
    return -1;
}

int json_arr_get(const JsonDoc *doc, int array, int index) {
    if (!doc || array < 0 || array >= doc->count || index < 0 ||
        doc->tokens[array].type != JSON_ARRAY) return -1;
    int found = 0;
    for (int i = array + 1; i < doc->count; i++) {
        const JsonToken *token = &doc->tokens[i];
        if (token->start >= doc->tokens[array].end) break;
        if (token->parent == array) {
            if (found == index) return i;
            found++;
        }
    }
    return -1;
}

int json_arr_size(const JsonDoc *doc, int array) {
    if (!doc || array < 0 || array >= doc->count ||
        doc->tokens[array].type != JSON_ARRAY) return -1;
    int count = 0;
    for (int i = array + 1; i < doc->count; i++) {
        if (doc->tokens[i].start >= doc->tokens[array].end) break;
        if (doc->tokens[i].parent == array) count++;
    }
    return count;
}

int json_string(const JsonDoc *doc, int token, char *out, size_t out_size) {
    if (!doc || !out || out_size == 0 || token < 0 || token >= doc->count ||
        doc->tokens[token].type != JSON_STRING) return -1;
    const JsonToken *t = &doc->tokens[token];
    size_t used = 0;
    for (int i = t->start; i < t->end; i++) {
        unsigned char c = (unsigned char)doc->text[i];
        if (c != '\\') {
            if (used + 2 > out_size) return -1;
            out[used++] = (char)c;
            continue;
        }
        if (++i >= t->end) return -1;
        c = (unsigned char)doc->text[i];
        if (c == '"' || c == '\\' || c == '/') {
            if (used + 2 > out_size) return -1;
            out[used++] = (char)c;
        } else if (c == 'b' || c == 'f' || c == 'n' || c == 'r' || c == 't') {
            static const char escaped[] = "\b\f\n\r\t";
            static const char names[] = "bfnrt";
            const char *found = strchr(names, (int)c);
            if (!found || used + 2 > out_size) return -1;
            out[used++] = escaped[found - names];
        } else if (c == 'u') {
            if (i + 4 >= t->end) return -1;
            // Simplified: just copy raw for now
            uint32_t cp = 0;
            for (int j = 0; j < 4; j++) {
                unsigned char h = (unsigned char)doc->text[i + 1 + j];
                cp <<= 4;
                if (h >= '0' && h <= '9') cp |= (uint32_t)(h - '0');
                else if (h >= 'a' && h <= 'f') cp |= (uint32_t)(h - 'a' + 10);
                else if (h >= 'A' && h <= 'F') cp |= (uint32_t)(h - 'A' + 10);
            }
            i += 4;
            if (cp < 0x80) {
                if (used + 2 > out_size) return -1;
                out[used++] = (char)cp;
            } else if (cp < 0x800) {
                if (used + 3 > out_size) return -1;
                out[used++] = (char)(0xC0 | (cp >> 6));
                out[used++] = (char)(0x80 | (cp & 0x3F));
            } else {
                if (used + 4 > out_size) return -1;
                out[used++] = (char)(0xE0 | (cp >> 12));
                out[used++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                out[used++] = (char)(0x80 | (cp & 0x3F));
            }
        } else return -1;
    }
    out[used] = '\0';
    return (int)used;
}

int json_i64(const JsonDoc *doc, int token, int64_t *out) {
    if (!doc || !out || token < 0 || token >= doc->count ||
        doc->tokens[token].type != JSON_PRIMITIVE) return -1;
    const JsonToken *t = &doc->tokens[token];
    size_t len = (size_t)(t->end - t->start);
    if (len == 0 || len >= 48) return -1;
    char buffer[48];
    memcpy(buffer, doc->text + t->start, len);
    buffer[len] = '\0';
    errno = 0;
    char *end = NULL;
    long long value = strtoll(buffer, &end, 10);
    if (errno != 0 || !end || *end != '\0') return -1;
    *out = (int64_t)value;
    return 0;
}

bool json_is_null(const JsonDoc *doc, int token) {
    if (!doc || token < 0 || token >= doc->count ||
        doc->tokens[token].type != JSON_PRIMITIVE) return false;
    const JsonToken *t = &doc->tokens[token];
    size_t len = (size_t)(t->end - t->start);
    return len == 4 && memcmp(doc->text + t->start, "null", 4) == 0;
}
