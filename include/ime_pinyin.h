#pragma once

/* Compact pinyin lookup adapted from the MIT-licensed Fishason/DSSH design.
 * The dictionary is optional at runtime; the search field has an ASCII
 * fallback when the ROMFS asset is unavailable. */

#define IME_BUFFER_MAX 31
#define IME_MAX_CANDIDATES 128

typedef struct PinyinIme PinyinIme;

PinyinIme *ime_create(const char *path);
void ime_destroy(PinyinIme *ime);
void ime_input(PinyinIme *ime, char letter);
void ime_backspace(PinyinIme *ime);
void ime_clear(PinyinIme *ime);
const char *ime_buffer(const PinyinIme *ime);
int ime_active(const PinyinIme *ime);
int ime_matched_length(const PinyinIme *ime);
int ime_candidate_count(const PinyinIme *ime);
const char *ime_candidate(const PinyinIme *ime, int index);
const char *ime_commit(PinyinIme *ime, int index);
