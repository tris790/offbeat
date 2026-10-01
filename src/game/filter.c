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

static b32 game_filter_word_char(u32 cp) {
    return (cp >= 'a' && cp <= 'z') || (cp >= '0' && cp <= '9');
}

s32 game_filter_score_word(const Game_Filter *filter, u32 word, Core_String text, u64 *mask) {
    u32 start = filter->start[word], length = filter->length[word];
    *mask = 0;
    if (!length) return 0;

    /* KMP finds contiguous matches while the same pass scores a subsequence.
       Retain word boundaries for possible substring starts, without limiting
       how much of a long title can be searched. */
    u8 word_start[256];
    u32 j = 0, qi = 0, previous = 0;
    u64 i = 0, last = 0, best_at = 0, fuzzy_mask = 0;
    s32 best = 0, fuzzy = 300;
    for (u64 at = 0; at < text.len; i++) {
        Core_Utf8Decode d = core_utf8_decode(text, at);
        at += d.size;
        u32 cp = game_filter_fold(d.codepoint);
        b32 boundary = i == 0 || !game_filter_word_char(previous);
        word_start[i % length] = boundary;
        previous = cp;

        while (j && filter->text[start + j] != cp) j = filter->fallback[start + j - 1];
        if (filter->text[start + j] == cp) j++;
        if (j == length) {
            u64 match_at = i + 1 - length;
            s32 score = match_at == 0 ? 1000 :
                (word_start[match_at % length] ? 800 : 600) - (s32)CORE_MIN(match_at, 100u);
            if (score > best) { best = score; best_at = match_at; }
            if (match_at == 0) break;
            j = filter->fallback[start + j - 1];
        }

        if (qi < length && cp == filter->text[start + qi]) {
            if (qi) fuzzy -= (s32)CORE_MIN(i - last - 1, 20u) * 4;
            if (boundary) fuzzy += 10;
            if (i < 64) fuzzy_mask |= 1ull << i;
            last = i;
            qi++;
        }
    }
    if (best) {
        for (u64 k = best_at; k < best_at + length && k < 64; k++) *mask |= 1ull << k;
        return best;
    }
    if (qi < length) return 0;
    *mask = fuzzy_mask;
    return CORE_MAX(fuzzy, 1);
}

b32 game_filter_matches(const Game_Filter *filter, const Core_String *fields, u32 field_count) {
    for (u32 w = 0; w < filter->word_count; w++) {
        b32 found = false;
        for (u32 f = 0; f < field_count && !found; f++) {
            u64 mask;
            found = game_filter_score_word(filter, w, fields[f], &mask) > 0;
        }
        if (!found) return false;
    }
    return true;
}
