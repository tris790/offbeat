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
    CHECK(!matches("dft", "", "Daft Punk", "")); /* substring, not fuzzy */
    CHECK(matches("electro", "Electronic", "", "")); /* group names */
    CHECK(matches("夜", "夜に駆ける", "", ""));
    CHECK(!matches("月", "夜に駆ける", "", "")); /* non-Latin letters stay distinct */
    CHECK(matches("ababac", "ababababac", "", "")); /* overlapping KMP fallback */
    CHECK(!matches("ababac", "ababababab", "", ""));

    /* No title truncation: matches beyond the palette's 256-codepoint limit. */
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

    printf(failures ? "filter_test: %d FAILED\n" : "filter_test: ok\n", failures);
    return failures != 0;
}
