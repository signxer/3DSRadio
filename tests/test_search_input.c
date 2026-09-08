#include "search_input.h"

#include <assert.h>
#include <string.h>

int main(void) {
    SearchInput input;
    search_input_init(&input);

    assert(search_input_append_ascii(&input, 'z'));
    assert(search_input_append_ascii(&input, 'h'));
    assert(search_input_append_ascii(&input, 'o'));
    assert(search_input_append_ascii(&input, 'n'));
    assert(search_input_append_ascii(&input, 'g'));
    assert(search_input_candidate_count(&input) == 4);
    assert(search_input_commit_candidate(&input, 1));
    assert(strcmp(search_input_query(&input), "中国") == 0);

    assert(search_input_backspace(&input));
    assert(strcmp(search_input_query(&input), "中") == 0);
    search_input_clear(&input);
    assert(search_input_query(&input)[0] == '\0');
    return 0;
}
