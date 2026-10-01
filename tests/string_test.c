#include "core/memory.c"
#include "core/string.c"

#include <stdio.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static void check_codec(void) {
    const u32 points[] = {0, 0x7F, 0x80, 0x7FF, 0x800, 0xD7FF, 0xE000, 0xFFFF, 0x10000, 0x10FFFF};
    for (u32 i = 0; i < CORE_ARRAY_COUNT(points); i++) {
        u8 bytes[4];
        u32 n = core_utf8_encode(bytes, points[i]);
        Core_String s = core_str_n(bytes, n);
        Core_Utf8Decode d = core_utf8_decode(s, 0);
        CHECK(core_utf8_valid(s));
        CHECK(d.codepoint == points[i] && d.size == n);
        CHECK(core_str_count_codepoints(s) == 1);
    }
    const char *bad[] = {"\xC0\x80", "\xC1\xBF", "\xE0\x9F\xBF", "\xED\xA0\x80", "\xF0\x8F\xBF\xBF", "\xF4\x90\x80\x80", "\xF5\x80\x80\x80", "\x80", "\xC2", "\xE2\x82", "\xF0\x9F\x98", "\xE2x\xAC"};
    for (u32 i = 0; i < CORE_ARRAY_COUNT(bad); i++) {
        Core_String s = core_str(bad[i]);
        CHECK(!core_utf8_valid(s));
        Core_Utf8Decode d = core_utf8_decode(s, 0);
        CHECK(d.codepoint == 0xFFFD && d.size == 1);
    }
    u8 out[4];
    CHECK(core_utf8_encode(out, 0xD800) == 3);
    CHECK(core_utf8_decode(core_str_n(out, 3), 0).codepoint == 0xFFFD);
    CHECK(core_utf8_encode(out, 0x110000) == 3);
    CHECK(core_utf8_valid((Core_String){0}));
}

static void check_bounded_copy(void) {
    Core_String text = core_str_lit("aé夜😀z");
    const u64 lengths[] = {0, 1, 1, 3, 3, 3, 6, 6, 6, 6, 10, 11};
    for (u64 limit = 0; limit <= text.len; limit++) {
        Core_String p = core_str_prefix_utf8(text, limit);
        CHECK(p.len == lengths[limit]);
        CHECK(core_utf8_valid(p));
        char out[32];
        memset(out, 0x55, sizeof(out));
        CHECK(core_str_write_cstr(out, limit + 1, text) == lengths[limit]);
        CHECK(out[lengths[limit]] == 0 && out[limit + 1] == 0x55);
        CHECK(core_str_eq(core_str(out), p));
        memset(out, 0x55, sizeof(out));
        CHECK(core_cstr_copy(out, limit + 1, (char *)text.str) == lengths[limit]);
        CHECK(core_str_eq(core_str(out), p));
    }
    CHECK(core_cstr_copy(0, 0, "anything") == 0);
    char out[20] = "abcdef";
    CHECK(core_cstr_copy(out + 1, 7, out) == 6 && !strcmp(out + 1, "abcdef"));
    CHECK(core_cstr_copy(out, 20, out + 1) == 6 && !strcmp(out, "abcdef"));
    CHECK(core_cstr_copy(out, 20, 0) == 0 && out[0] == 0);
    const u64 ends[] = {1, 3, 6, 10, 11};
    const u64 starts[] = {0, 1, 3, 6, 10};
    for (u32 i = 0; i < CORE_ARRAY_COUNT(ends); i++) CHECK(core_utf8_prev(text, ends[i]) == starts[i]);
    CHECK(core_utf8_prev(text, 999) == 10);
    CHECK(core_utf8_prev(core_str_lit("\x80\x80"), 2) == 1);
    CHECK(core_str_substr(text, 1, (u64)-1).len == text.len - 1);
    CHECK(core_str_eq(core_str_substr((Core_String){0}, 99, 99), (Core_String){0}));
}

static void check_arena(void) {
    Core_Arena arena;
    CHECK(core_arena_init(&arena, CORE_KB(64)));
    const u8 bytes[] = {'a', 0, 'b'};
    Core_String copy = core_str_copy(&arena, core_str_n(bytes, 3));
    CHECK(copy.len == 3 && !memcmp(copy.str, bytes, 3) && copy.str[3] == 0);
    char *heap = core_str_heap_cstr(copy);
    CHECK(heap && !memcmp(heap, bytes, 3) && heap[3] == 0);
    core_heap_free(heap);
    CHECK(core_str_eq(core_str_fmt(&arena, "%s %u", "answer", 42u), core_str_lit("answer 42")));
    char long_text[1501];
    memset(long_text, 'a', 1500); long_text[1500] = 0;
    Core_String formatted = core_str_fmt(&arena, "%s!", long_text);
    CHECK(formatted.len == 1501 && formatted.str[1500] == '!' && formatted.str[1501] == 0);
    core_arena_reset(&arena);
    Core_StringBuilder sb = core_sb_begin(&arena, 16);
    u8 *original = sb.str;
    for (u32 i = 0; i < 100; i++) core_sb_append_cstr(&sb, "test");
    CHECK(sb.str == original && sb.len == 400 && sb.cap == 512 && arena.used == sb.cap);
    core_sb_append_u64(&sb, (u64)-1);
    CHECK(core_str_eq(core_str_suffix(core_sb_finish(&sb), 400), core_str_lit("18446744073709551615")));
    core_sb_append_codepoint(&sb, 0x1F600);
    CHECK(core_utf8_valid(core_sb_finish(&sb)));
    core_arena_push(&arena, 1, 1); /* another allocation forces copy on growth */
    core_sb_append(&sb, core_str(long_text));
    CHECK(sb.str != original && sb.len == 1924 && !sb.failed);
    core_sb_append(&sb, core_sb_finish(&sb)); /* overlapping/self append */
    CHECK(sb.len == 3848 && !memcmp(sb.str, sb.str + 1924, 1924));
    core_arena_reset(&arena);
    core_arena_push(&arena, arena.reserved, 1);
    CHECK(!core_str_copy(&arena, core_str_lit("OOM")).str);
    CHECK(!core_str_to_cstr(&arena, core_str_lit("OOM")));
    CHECK(!core_str_fmt(&arena, "%s", "OOM").str);
    sb = core_sb_begin(&arena, 16);
    CHECK(sb.failed);
    core_sb_append_cstr(&sb, "cannot append");
    CHECK(!core_sb_finish(&sb).str);
    core_arena_reset(&arena);
    sb = core_sb_begin(&arena, 16);
    core_sb_append(&sb, (Core_String){.len = (u64)-1});
    CHECK(sb.failed && !core_sb_finish(&sb).str);
    core_arena_release(&arena);
}

int main(void) {
    check_codec();
    check_bounded_copy();
    check_arena();
    CHECK(core_str_eq_ascii_ci(core_str_lit("Vorbis"), core_str_lit("VORBIS")));
    CHECK(core_str_cmp_ascii_ci(core_str_lit("a"), core_str_lit("AA")) < 0);
    CHECK(core_str_cmp(core_str_lit("a"), core_str_lit("B")) > 0);
    CHECK(core_str_eq(core_str_trim_ascii(core_str_lit("\v\t hello \f\r\n")), core_str_lit("hello")));
    CHECK(core_cstr_starts_ascii_ci("Topic", "TO") && !core_cstr_starts_ascii_ci("T", "TOPIC"));
    CHECK(core_cstr_ends_ascii_ci("a - Topic", "TOPIC") && !core_cstr_ends_ascii_ci("a", "Topic"));
    CHECK(core_unicode_fold_latin('A') == 'a');
    CHECK(core_unicode_fold_latin(0xDC) == 'u');
    CHECK(core_unicode_fold_latin(0x141) == 'l');
    CHECK(core_unicode_fold_latin(0x591C) == 0x591C);
    printf(failures ? "string_test: %d FAILED\n" : "string_test: ok\n", failures);
    return failures != 0;
}
