#ifndef GAME_SEARCH_H
#define GAME_SEARCH_H

#include "library.h"

/*
 * Fuzzy track search for the command palette.
 *
 * The query is split on whitespace; every word must match at least one of the
 * track's title, artist or album ("one more daft" finds Daft Punk - One More
 * Time). Each word is scored against each field, keeping the best (title >
 * artist > album at equal quality), and the track's score is the mean of its
 * words. The best `max_results` are kept in a small sorted array -- no
 * allocation, O(tracks * words * len). Uses the same compiled query and
 * per-field fuzzy matcher as the tab filters.
 *
 * Matching is case-insensitive and accent-folded for Latin text, so "rufus"
 * finds "RUFUS DU SOL". Per field, ranking is: prefix, word-start match,
 * substring, then in-order subsequence (fewer gaps is better).
 *
 * Every hit says WHICH fields matched and WHICH characters, so the UI can
 * highlight them: `mask[f]` has bit i set when the i-th codepoint of field f
 * (first 64 only) was matched by a query word. A word is marked in every field
 * it matches; `best_field` is the one that scored highest.
 */

enum {
    SEARCH_TITLE = 0,
    SEARCH_ARTIST,
    SEARCH_ALBUM,
    SEARCH_FIELDS,
};

typedef struct {
    u32 track;
    s32 score;
    u8  best_field;             /* SEARCH_*: field contributing the most      */
    u8  fields;                 /* bitmask of (1 << SEARCH_*) that matched    */
    u64 mask[SEARCH_FIELDS];    /* matched codepoints per field               */
} Search_Hit;

/* Returns the number of hits written to `out` (sorted by score, best first). */
u32 search_tracks(const Library *lib, Core_String query, Search_Hit *out, u32 max_results);

/* Fold Latin text to lowercase ASCII-ish bytes; 0xFF = preserve codepoint. */
u8 search_fold(u32 cp);

#endif /* GAME_SEARCH_H */
