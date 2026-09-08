#include "ime_pinyin.h"

#include <assert.h>
#include <string.h>

int main(void) {
    PinyinIme *ime = ime_create("romfs/pinyin_dict.bin");
    assert(ime != NULL);

    ime_input(ime, 'z');
    ime_input(ime, 'h');
    ime_input(ime, 'o');
    ime_input(ime, 'n');
    ime_input(ime, 'g');
    assert(ime_active(ime));
    assert(ime_candidate_count(ime) > 0);
    assert(ime_candidate(ime, 0) != NULL);
    assert(strlen(ime_buffer(ime)) == 5);

    ime_backspace(ime);
    assert(strlen(ime_buffer(ime)) == 4);
    ime_clear(ime);
    assert(!ime_active(ime));
    ime_destroy(ime);
    return 0;
}
