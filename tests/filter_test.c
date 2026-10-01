#include "core/memory.c"
#include "core/string.c"
#include "game/search.c"
#include "game/filter.c"

#include <stdio.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static b32 matches(const char *query, const char *title, const char *artist, const char *album) {
    Game_Filter filter;
    game_filter_compile(&filter, core_str(query));
    Core_String fields[] = {core_str(title), core_str(artist), core_str(album)};
    return game_filter_matches(&filter, fields, 3);
}

int main(void) {
    CHECK(matches("", "Anything", "", ""));
    CHECK(matches(" \t\n ", "Anything", "", ""));
    CHECK(matches("RUFUS bloom", "Innerbloom", "RÜFÜS DU SOL", ""));
    CHECK(matches("more discovery", "One More Time", "Daft Punk", "Discovery"));
    CHECK(!matches("more zzz", "One More Time", "Daft Punk", "Discovery"));
    CHECK(matches("dft", "", "Daft Punk", "")); /* palette fuzzy subsequence */
    CHECK(matches("dscvry", "", "", "Discovery"));
    CHECK(matches("on mr", "One More Time", "", ""));
    CHECK(!matches("tfd", "", "Daft Punk", "")); /* order still matters */
    CHECK(matches("electro", "Electronic", "", "")); /* group names */
    CHECK(matches("夜", "夜に駆ける", "", ""));
    CHECK(!matches("月", "夜に駆ける", "", "")); /* non-Latin letters stay distinct */
    CHECK(matches("ababac", "ababababac", "", "")); /* overlapping KMP fallback */
    CHECK(!matches("ababac", "ababababab", "", ""));

    /* Shared matcher searches long titles without truncating them. */
    char long_title[1024];
    memset(long_title, 'a', sizeof(long_title));
    memcpy(long_title + 900, " Needle", 8);
    long_title[908] = 0;
    CHECK(matches("needle", long_title, "", ""));

    Game_Filter filter;
    char long_query[513];
    memset(long_query, 'a', sizeof(long_query) - 1);
    long_query[512] = 0;
    game_filter_compile(&filter, core_str(long_query));
    CHECK(filter.word_count == 1 && filter.length[0] == 256);
    for (u32 i = 0; i < 512; i++) long_query[i] = i % 2 ? ' ' : 'a';
    game_filter_compile(&filter, core_str(long_query));
    CHECK(filter.word_count == 128);

    /* A tab only supplies its own name/title, never unrelated metadata. */
    Core_String title = core_str("One More Time");
    Core_String artist = core_str("Daft Punk");
    game_filter_compile(&filter, core_str("dft"));
    CHECK(!game_filter_matches(&filter, &title, 1));
    CHECK(game_filter_matches(&filter, &artist, 1));

    /* Single-field tab matching agrees with palette matching, including
       whitespace, accents, Unicode and fields longer than 256 codepoints. */
    const char *queries[] = {"one", "mr", "on mr", "time one", "zzz", "rufus", "月", "夜", "needle", "one\tmore"};
    const char *titles[] = {"One More Time", "RÜFÜS DU SOL", "夜に駆ける", long_title};
    for (u32 t = 0; t < sizeof(titles) / sizeof(titles[0]); t++) {
        Lib_Track track = {.title = core_str(titles[t])};
        Library lib = {.tracks = &track, .track_count = 1};
        for (u32 q = 0; q < sizeof(queries) / sizeof(queries[0]); q++) {
            Search_Hit hit;
            game_filter_compile(&filter, core_str(queries[q]));
            CHECK(game_filter_matches(&filter, &track.title, 1) ==
                  (search_tracks(&lib, core_str(queries[q]), &hit, 1) == 1));
        }
    }

    /* Keep the palette's score ordering and character highlights. */
    u64 mask;
    game_filter_compile(&filter, core_str("bloom"));
    s32 prefix = game_filter_score_word(&filter, 0, core_str("Bloom"), &mask);
    CHECK(prefix == 1000 && mask == 0x1F);
    s32 boundary = game_filter_score_word(&filter, 0, core_str("The Bloom"), &mask);
    CHECK(boundary == 796 && mask == 0x1F0);
    s32 substring = game_filter_score_word(&filter, 0, core_str("Innerbloom"), &mask);
    CHECK(substring == 595 && mask == 0x3E0);
    s32 fuzzy = game_filter_score_word(&filter, 0, core_str("Below Loom"), &mask);
    CHECK(fuzzy > 0 && fuzzy < substring);
    CHECK(prefix > boundary && boundary > substring);

    printf(failures ? "filter_test: %d FAILED\n" : "filter_test: ok\n", failures);
    return failures != 0;
}
