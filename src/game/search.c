#include "search.h"

#include "filter.h"

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
    return 0xFF; /* caller preserves the original codepoint for other scripts */
}

/* Field weights (/8): a title hit beats the same hit in the artist, which
   beats the album. */
static const s32 FIELD_WEIGHT[SEARCH_FIELDS] = { 8, 6, 5 };

u32 search_tracks(const Library *lib, Core_String query, Search_Hit *out, u32 max_results) {
    if (!lib || max_results == 0) return 0;
    Game_Filter compiled;
    game_filter_compile(&compiled, query);
    u32 nwords = compiled.word_count;
    if (nwords == 0) return 0;

    u32 count = 0;
    for (u32 k = 0; k < lib->track_count; k++) {
        u32 ti = lib->by_title ? lib->by_title[k] : k;
        const Lib_Track *t = &lib->tracks[ti];
        const Core_String text[SEARCH_FIELDS] = { t->title, t->artist, t->album };

        Search_Hit hit = { .track = ti };
        s32 field_score[SEARCH_FIELDS] = { 0 };
        s32 total = 0;
        u32 w;
        for (w = 0; w < nwords; w++) {
            s32 best = 0;
            u32 best_f = 0;
            u64 m[SEARCH_FIELDS];
            for (u32 f = 0; f < SEARCH_FIELDS; f++) {
                s32 sc = game_filter_score_word(&compiled, w, text[f], &m[f]);
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
