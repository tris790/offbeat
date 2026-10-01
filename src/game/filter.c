#include "filter.h"
#include "search.h"
#include <string.h>

static u32 game_filter_fold(u32 cp) {
    u8 latin = search_fold(cp);
    return latin == 0xFF ? cp : latin;
}

void game_filter_compile(Game_Filter *filter, Core_String query) {
    memset(filter, 0, sizeof(*filter));
    u32 n = 0;
    b32 in_word = false;
    for (u64 at = 0; at < query.len && n < 256; ) {
        Core_Utf8Decode d = core_utf8_decode(query, at);
        at += d.size;
        if (d.codepoint <= ' ' || d.codepoint == 0xA0) { in_word = false; continue; }
        if (!in_word) {
            if (filter->word_count == 128) break;
            filter->start[filter->word_count++] = (u16)n;
            in_word = true;
        }
        u32 word = filter->word_count - 1, start = filter->start[word];
        u32 length = filter->length[word];
        filter->text[n] = game_filter_fold(d.codepoint);
        if (length) {
            u32 j = filter->fallback[n - 1];
            while (j && filter->text[start + j] != filter->text[n]) j = filter->fallback[start + j - 1];
            if (filter->text[start + j] == filter->text[n]) j++;
            filter->fallback[n] = (u16)j;
        }
        filter->length[word]++;
        n++;
    }
}

b32 game_filter_matches(const Game_Filter *filter, const Core_String *fields, u32 field_count) {
    for (u32 w = 0; w < filter->word_count; w++) {
        u32 start = filter->start[w], length = filter->length[w];
        b32 found = false;
        for (u32 f = 0; f < field_count && !found; f++) {
            u32 j = 0;
            for (u64 at = 0; at < fields[f].len; ) {
                Core_Utf8Decode d = core_utf8_decode(fields[f], at);
                at += d.size;
                u32 cp = game_filter_fold(d.codepoint);
                while (j && filter->text[start + j] != cp) j = filter->fallback[start + j - 1];
                if (filter->text[start + j] == cp) j++;
                if (j == length) { found = true; break; }
            }
        }
        if (!found) return false;
    }
    return true;
}
