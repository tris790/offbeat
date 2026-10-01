/*
 * Command palette search test: matches title, artist and album, reports which
 * fields/characters matched, folds accents, and requires every word to match.
 */

#include "core/memory.c"
#include "core/string.c"
#include "game/search.c"
#include "game/filter.c"

#include <stdio.h>
#include <stdlib.h>

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static Lib_Track mk(const char *title, const char *artist, const char *album) {
    return (Lib_Track){ .title = core_str(title), .artist = core_str(artist), .album = core_str(album) };
}

static u32 run(const Library *lib, const char *q, Search_Hit *out) {
    return search_tracks(lib, core_str(q), out, 8);
}

int main(void) {
    Lib_Track tracks[] = {
        mk("One More Time", "Daft Punk", "Discovery"),
        mk("Innerbloom", "RÜFÜS DU SOL", "Bloom"),
        mk("Harder Better", "Daft Punk", "Discovery"),
        mk("Blue", "Eiffel 65", ""),
    };
    Library lib = { .tracks = tracks, .track_count = 4 };
    Search_Hit h[8];

    /* title only */
    CHECK(run(&lib, "one more", h) == 1);
    CHECK(h[0].track == 0 && h[0].best_field == SEARCH_TITLE && h[0].fields == (1u << SEARCH_TITLE));
    CHECK(h[0].mask[SEARCH_TITLE] == 0xF7); /* "One" + "More", not the space */

    /* artist: both Daft Punk tracks, marked in the artist field */
    u32 n = run(&lib, "daft", h);
    CHECK(n == 2);
    CHECK(h[0].best_field == SEARCH_ARTIST && h[0].mask[SEARCH_ARTIST] == 0xF && h[0].mask[SEARCH_TITLE] == 0);

    /* album: "discovery" matches both tracks by album only */
    n = run(&lib, "discov", h);
    CHECK(n == 2);
    CHECK(h[0].best_field == SEARCH_ALBUM && h[0].fields == (1u << SEARCH_ALBUM) && h[0].mask[SEARCH_ALBUM] == 0x3F);

    /* a word is marked in every field it matches; the album prefix ("Bloom")
       outscores the mid-word title hit ("Innerbloom") */
    n = run(&lib, "bloom", h);
    CHECK(n == 1 && h[0].track == 1);
    CHECK(h[0].fields == ((1u << SEARCH_TITLE) | (1u << SEARCH_ALBUM)));
    CHECK(h[0].best_field == SEARCH_ALBUM && h[0].mask[SEARCH_TITLE] == 0x3E0);

    /* words may hit different fields */
    n = run(&lib, "more daft", h);
    CHECK(n == 1 && h[0].track == 0);
    CHECK(h[0].fields == ((1u << SEARCH_TITLE) | (1u << SEARCH_ARTIST)));
    n = run(&lib, "harder discovery", h);
    CHECK(n == 1 && h[0].track == 2);

    /* every word must match */
    CHECK(run(&lib, "daft zzzz", h) == 0);

    /* accent folding, marks stay on the original codepoints */
    n = run(&lib, "rufus", h);
    CHECK(n == 1 && h[0].track == 1 && h[0].best_field == SEARCH_ARTIST && h[0].mask[SEARCH_ARTIST] == 0x1F);

    /* fuzzy subsequence still works */
    CHECK(run(&lib, "dscvry", h) == 2);

    /* empty / blank queries match nothing */
    CHECK(run(&lib, "", h) == 0);
    CHECK(run(&lib, "   ", h) == 0);

    printf(g_fail ? "search_test: %d FAILED\n" : "search_test: ok\n", g_fail);
    return g_fail != 0;
}
