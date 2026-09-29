#ifndef CORE_STRING_H
#define CORE_STRING_H

#include "types.h"
#include "memory.h"

/*
 * UTF-8 strings. `Core_String` is a length-prefixed view (not necessarily
 * null-terminated) over UTF-8 bytes. It is cheap to copy and slice; ownership
 * lives with whatever arena holds the backing bytes.
 */

typedef struct {
    u8 *str;
    u64 len; /* length in bytes */
} Core_String;

/* Decoded single codepoint plus how many bytes it consumed. */
typedef struct {
    u32 codepoint;
    u32 size; /* 1..4, or 1 with codepoint=0xFFFD on malformed input */
} Core_Utf8Decode;

/* ---- construction ---- */

#define core_str_lit(s) ((Core_String){ .str = (u8 *)(s), .len = sizeof(s) - 1 })

Core_String core_str(const char *cstr);                          /* from null-terminated */
Core_String core_str_n(const u8 *bytes, u64 len);
Core_String core_str_copy(Core_Arena *arena, Core_String s);     /* owned copy in arena   */
char       *core_str_to_cstr(Core_Arena *arena, Core_String s);  /* null-terminated copy  */

/* ---- inspection ---- */

b32 core_str_eq(Core_String a, Core_String b);
u64 core_str_count_codepoints(Core_String s); /* number of UTF-8 codepoints */

/* ---- slicing (returns views into the same backing memory) ---- */

Core_String core_str_substr(Core_String s, u64 byte_off, u64 byte_len);
Core_String core_str_prefix(Core_String s, u64 byte_len);
Core_String core_str_suffix(Core_String s, u64 byte_off);

/* ---- UTF-8 codec ---- */

/* Decode the codepoint at byte offset `at` (must be < s.len). */
Core_Utf8Decode core_utf8_decode(Core_String s, u64 at);
/* Encode `codepoint` into `out` (needs <=4 bytes). Returns bytes written. */
u32 core_utf8_encode(u8 *out, u32 codepoint);

/* ---- builder: grows by appending into an arena ---- */

typedef struct {
    Core_Arena *arena;
    u8 *str;
    u64 len;
    u64 cap;
} Core_StringBuilder;

Core_StringBuilder core_sb_begin(Core_Arena *arena, u64 initial_cap);
void               core_sb_append(Core_StringBuilder *sb, Core_String s);
void               core_sb_append_cstr(Core_StringBuilder *sb, const char *cstr);
void               core_sb_append_codepoint(Core_StringBuilder *sb, u32 codepoint);
void               core_sb_append_u64(Core_StringBuilder *sb, u64 value);
Core_String        core_sb_finish(Core_StringBuilder *sb);

#endif /* CORE_STRING_H */
