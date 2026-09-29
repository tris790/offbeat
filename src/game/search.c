#include "search.h"

#include <string.h>

#define FOLD_MAX 256

u8 search_fold(u32 cp) {
    if (cp < 0x80) {
        if (cp >= 'A' && cp <= 'Z') return (u8)(cp + 32);
        return (u8)cp;
    }
    /* Latin-1 supplement + Latin Extended-A letters -> base letter
       (generated from Unicode NFKD decompositions; '?' = no base letter). */
    static const char latin1[] =
        "aaaaaaaceeeeiiiidnooooo?ouuuuyts"
        "aaaaaaaceeeeiiiidnooooo?ouuuuyty";
    static const char ext_a[] =
        "aaaaaaccccccccddddeeeeeeeeeegggggggghhhhiiiiiiiiiiiijjkkklllllll"
        "lllnnnnnnnnnoooooooorrrrrrssssssssttttttuuuuuuuuuuuuwwyyyzzzzzzs";
    if (cp >= 0xC0 && cp <= 0xFF) {
        char c = latin1[cp - 0xC0];
        return c == '?' ? 0xFF : (u8)c;
    }
    if (cp >= 0x100 && cp <= 0x17F) {
        char c = ext_a[cp - 0x100];
        return c == '?' ? 0xFF : (u8)c;
    }
    return 0xFF; /* other scripts: keep as a non-matching-but-present char */
}

/* Fold a UTF-8 string into `out` (ASCII-lowercase-ish), returning its length. */
static u32 fold_str(Core_String s, u8 *out, u32 cap) {
    u32 n = 0;
    for (u64 at = 0; at < s.len && n < cap; ) {
        Core_Utf8Decode d = core_utf8_decode(s, at);
        at += d.size;
        u8 c = search_fold(d.codepoint);
        if (c) out[n++] = c;
    }
    return n;
}

CORE_INLINE b32 is_word_start(const u8 *s, u32 i) {
    if (i == 0) return true;
    u8 p = s[i - 1];
    return !((p >= 'a' && p <= 'z') || (p >= '0' && p <= '9'));
}

/* Score `q` against folded text `s`. 0 = no match. `mask` receives the bits
   of the characters that matched. */
static s32 score_text(const u8 *s, u32 n, const u8 *q, u32 qn, u64 *mask) {
    *mask = 0;
    if (qn == 0 || qn > n) return 0;
    /* substring search, preferring word starts */
    s32 best = 0;
    u32 best_at = 0;
    for (u32 i = 0; i + qn <= n; i++) {
        if (s[i] != q[0] || memcmp(s + i, q, qn) != 0) continue;
        s32 sc;
        if (i == 0)                 sc = 1000;
        else if (is_word_start(s, i)) sc = 800 - (s32)CORE_MIN(i, 100u);
        else                        sc = 600 - (s32)CORE_MIN(i, 100u);
        if (sc > best) { best = sc; best_at = i; }
        if (i == 0) break;
    }
    if (best) {
        for (u32 k = best_at; k < best_at + qn && k < 64; k++) *mask |= 1ull << k;
        return best;
    }

    /* in-order subsequence; penalize gaps, reward word-start hits */
    u32 qi = 0;
    s32 sc = 300;
    s32 last = -1;
    u64 m = 0;
    for (u32 i = 0; i < n && qi < qn; i++) {
        if (s[i] == q[qi]) {
            if (last >= 0) sc -= (s32)CORE_MIN((u32)(i - (u32)last - 1), 20u) * 4;
            if (is_word_start(s, i)) sc += 10;
            if (i < 64) m |= 1ull << i;
            last = (s32)i;
            qi++;
        }
    }
    if (qi < qn) return 0;
    *mask = m;
    return CORE_MAX(sc, 1);
}

#define QUERY_WORDS_MAX 6

typedef struct { const u8 *s; u32 n; } Word;

/* Split the folded query into space-separated words. */
static u32 split_words(const u8 *q, u32 qn, Word *out) {
    u32 count = 0;
    for (u32 i = 0; i < qn && count < QUERY_WORDS_MAX; ) {
        while (i < qn && q[i] == ' ') i++;
        u32 start = i;
        while (i < qn && q[i] != ' ') i++;
        if (i > start) out[count++] = (Word){ q + start, i - start };
    }
    return count;
}

/* Field weights (/8): a title hit beats the same hit in the artist, which
   beats the album. */
static const s32 FIELD_WEIGHT[SEARCH_FIELDS] = { 8, 6, 5 };

u32 search_tracks(const Library *lib, Core_String query, Search_Hit *out, u32 max_results) {
    if (!lib || max_results == 0) return 0;
    u8 q[FOLD_MAX];
    u32 qn = fold_str(query, q, FOLD_MAX);
    Word words[QUERY_WORDS_MAX];
    u32 nwords = split_words(q, qn, words);
    if (nwords == 0) return 0;

    u32 count = 0;
    u8 buf[SEARCH_FIELDS][FOLD_MAX];
    for (u32 k = 0; k < lib->track_count; k++) {
        u32 ti = lib->by_title ? lib->by_title[k] : k;
        const Lib_Track *t = &lib->tracks[ti];
        const Core_String text[SEARCH_FIELDS] = { t->title, t->artist, t->album };
        u32 len[SEARCH_FIELDS];
        for (u32 f = 0; f < SEARCH_FIELDS; f++) len[f] = fold_str(text[f], buf[f], FOLD_MAX);

        Search_Hit hit = { .track = ti };
        s32 field_score[SEARCH_FIELDS] = { 0 };
        s32 total = 0;
        u32 w;
        for (w = 0; w < nwords; w++) {
            s32 best = 0;
            u32 best_f = 0;
            u64 m[SEARCH_FIELDS];
            for (u32 f = 0; f < SEARCH_FIELDS; f++) {
                s32 sc = score_text(buf[f], len[f], words[w].s, words[w].n, &m[f]);
                sc = sc * FIELD_WEIGHT[f] / 8;
                if (sc > best) { best = sc; best_f = f; }
                /* highlight the word wherever it matches, not just where it
                   scored best, so the UI shows every field that matched */
                if (sc > 0) { hit.mask[f] |= m[f]; hit.fields |= (u8)(1u << f); }
            }
            if (best <= 0) break; /* every word must match somewhere */
            total += best;
            field_score[best_f] += best;
        }
        if (w < nwords) continue;

        for (u32 f = 1; f < SEARCH_FIELDS; f++)
            if (field_score[f] > field_score[hit.best_field]) hit.best_field = (u8)f;

        s32 score = total / (s32)nwords;
        /* shorter titles first among equals */
        hit.score = score * 4 - (s32)CORE_MIN(t->title.len, 60u) / 8;

        /* insert into the sorted top-N */
        if (count == max_results && hit.score <= out[count - 1].score) continue;
        u32 pos = count < max_results ? count++ : max_results - 1;
        while (pos > 0 && out[pos - 1].score < hit.score) {
            out[pos] = out[pos - 1];
            pos--;
        }
        out[pos] = hit;
    }
    return count;
}
