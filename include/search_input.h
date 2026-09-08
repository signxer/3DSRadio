#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "ime_pinyin.h"

#define SEARCH_INPUT_MAX 63
#define SEARCH_PINYIN_MAX 15
#define SEARCH_CANDIDATE_COUNT IME_MAX_CANDIDATES

typedef struct {
    char query[SEARCH_INPUT_MAX + 1];
    char pinyin[SEARCH_PINYIN_MAX + 1];
    int candidate_page;
    PinyinIme *ime;
} SearchInput;

void search_input_init(SearchInput *input);
void search_input_destroy(SearchInput *input);
bool search_input_append_ascii(SearchInput *input, char ch);
bool search_input_backspace(SearchInput *input);
void search_input_clear(SearchInput *input);
bool search_input_commit_candidate(SearchInput *input, int index);
int search_input_candidate_count(const SearchInput *input);
const char *search_input_candidate(const SearchInput *input, int index);
const char *search_input_query(const SearchInput *input);
const char *search_input_composition(const SearchInput *input);
