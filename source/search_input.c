#include "search_input.h"

#include <ctype.h>
#include <string.h>

typedef struct {
    const char *pinyin;
    const char *candidates[SEARCH_CANDIDATE_COUNT];
    int count;
} PinyinEntry;

/* A deliberately small, ROM-friendly dictionary covering common radio
 * searches.  Latin search remains available for the complete station index. */
static const PinyinEntry PINYIN_TABLE[] = {
    {"bei", {"北", "北京", "北方", "贝"}, 4},
    {"dian", {"电", "电台", "电子", "点"}, 4},
    {"guang", {"广", "广东", "广州", "光"}, 4},
    {"guo", {"国", "国际", "国内", "过"}, 4},
    {"jing", {"经", "经典", "经济", "京"}, 4},
    {"liu", {"流", "流行", "柳", "六"}, 4},
    {"nan", {"南", "南方", "南京", "难"}, 4},
    {"qi", {"气", "器", "奇", "其"}, 4},
    {"radio", {"Radio", "电台", "广播", "收音机"}, 4},
    {"shang", {"上", "上海", "商", "尚"}, 4},
    {"tian", {"天", "天津", "天气", "田"}, 4},
    {"xiang", {"香", "香港", "乡", "响"}, 4},
    {"yin", {"音", "音乐", "音乐台", "银"}, 4},
    {"yue", {"乐", "乐队", "粤", "月"}, 4},
    {"zhong", {"中", "中国", "中文", "中央"}, 4},
};

static const PinyinEntry *find_entry(const char *pinyin) {
    for (size_t i = 0; i < sizeof(PINYIN_TABLE) / sizeof(PINYIN_TABLE[0]); i++) {
        if (strcmp(PINYIN_TABLE[i].pinyin, pinyin) == 0)
            return &PINYIN_TABLE[i];
    }
    return NULL;
}

static bool append_bytes(char *dst, size_t dst_size, const char *src) {
    size_t used = strlen(dst);
    size_t len = strlen(src);
    if (used + len >= dst_size) return false;
    memcpy(dst + used, src, len + 1);
    return true;
}

void search_input_init(SearchInput *input) {
    if (!input) return;
    memset(input, 0, sizeof(*input));
    input->ime = ime_create("romfs:/pinyin_dict.bin");
}

void search_input_destroy(SearchInput *input) {
    if (!input) return;
    ime_destroy(input->ime);
    input->ime = NULL;
}

bool search_input_append_ascii(SearchInput *input, char ch) {
    char text[2] = {ch, '\0'};
    if (!input || ch < 0x20 || ch > 0x7e) return false;
    if (!append_bytes(input->query, sizeof(input->query), text)) return false;

    /* Keep an ASCII composition so the same keyboard can also produce a
     * Chinese candidate. A space commits the currently composed candidate. */
    if (ch == ' ') {
        input->pinyin[0] = '\0';
        if (input->ime) ime_clear(input->ime);
    } else if (strlen(input->pinyin) < SEARCH_PINYIN_MAX) {
        char lower = ch;
        if (lower >= 'A' && lower <= 'Z') lower = (char)(lower - 'A' + 'a');
        if (lower >= 'a' && lower <= 'z') {
            size_t n = strlen(input->pinyin);
            input->pinyin[n] = lower;
            input->pinyin[n + 1] = '\0';
            if (input->ime) ime_input(input->ime, lower);
        }
    }
    input->candidate_page = 0;
    return true;
}

bool search_input_backspace(SearchInput *input) {
    if (!input) return false;
    size_t len = strlen(input->query);
    if (len == 0) return false;

    /* Remove one UTF-8 codepoint from the committed query. */
    do {
        --len;
    } while (len > 0 && ((unsigned char)input->query[len] & 0xc0) == 0x80);
    input->query[len] = '\0';

    size_t pinyin_len = strlen(input->pinyin);
    if (pinyin_len > 0) {
        input->pinyin[pinyin_len - 1] = '\0';
        if (input->ime) ime_backspace(input->ime);
    }
    input->candidate_page = 0;
    return true;
}

void search_input_clear(SearchInput *input) {
    if (!input) return;
    memset(input->query, 0, sizeof(input->query));
    memset(input->pinyin, 0, sizeof(input->pinyin));
    input->candidate_page = 0;
    if (input->ime) ime_clear(input->ime);
}

int search_input_candidate_count(const SearchInput *input) {
    const PinyinEntry *entry;
    if (!input || input->pinyin[0] == '\0') return 0;
    if (input->ime) return ime_candidate_count(input->ime);
    entry = find_entry(input->pinyin);
    return entry ? entry->count : 0;
}

const char *search_input_candidate(const SearchInput *input, int index) {
    const PinyinEntry *entry;
    if (!input || index < 0 || index >= SEARCH_CANDIDATE_COUNT) return "";
    if (input->ime) {
        const char *candidate = ime_candidate(input->ime, index);
        return candidate ? candidate : "";
    }
    entry = find_entry(input->pinyin);
    if (!entry || index >= entry->count) return "";
    return entry->candidates[index];
}

bool search_input_commit_candidate(SearchInput *input, int index) {
    const char *candidate;
    size_t query_len;
    size_t pinyin_len;

    if (!input) return false;
    candidate = search_input_candidate(input, index);
    if (!candidate[0]) return false;

    query_len = strlen(input->query);
    pinyin_len = strlen(input->pinyin);
    if (query_len < pinyin_len) return false;
    if (query_len - pinyin_len + strlen(candidate) >= sizeof(input->query))
        return false;
    input->query[query_len - pinyin_len] = '\0';
    if (!append_bytes(input->query, sizeof(input->query), candidate)) return false;
    input->pinyin[0] = '\0';
    input->candidate_page = 0;
    if (input->ime) ime_commit(input->ime, index);
    return true;
}

const char *search_input_query(const SearchInput *input) {
    return input ? input->query : "";
}

const char *search_input_composition(const SearchInput *input) {
    if (!input) return "";
    if (input->ime && ime_active(input->ime)) return ime_buffer(input->ime);
    return input->pinyin;
}
