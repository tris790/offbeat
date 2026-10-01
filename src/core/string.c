#include "string.h"

#include <string.h>
#include <stdio.h>

Core_String core_str(const char *cstr) {
    Core_String s = { .str = (u8 *)cstr, .len = 0 };
    if (cstr) s.len = (u64)strlen(cstr);
    return s;
}

Core_String core_str_n(const u8 *bytes, u64 len) {
    return (Core_String){ .str = (u8 *)bytes, .len = len };
}

Core_String core_str_copy(Core_Arena *arena, Core_String s) {
    if (s.len == (u64)-1) return (Core_String){0};
    u8 *dst = core_push_array(arena, u8, s.len + 1);
    if (!dst) return (Core_String){0};
    if (s.len) memcpy(dst, s.str, s.len);
    dst[s.len] = 0;
    return (Core_String){ .str = dst, .len = s.len };
}

char *core_str_to_cstr(Core_Arena *arena, Core_String s) {
    return (char *)core_str_copy(arena, s).str;
}

char *core_str_heap_cstr(Core_String s) {
    if (s.len == (u64)-1) return 0;
    char *dst = core_heap_alloc(s.len + 1);
    if (!dst) return 0;
    if (s.len) memcpy(dst, s.str, s.len);
    dst[s.len] = 0;
    return dst;
}

Core_String core_str_prefix_utf8(Core_String s, u64 byte_len) {
    u64 n = CORE_MIN(s.len, byte_len);
    if (n < s.len)
        while (n && (s.str[n] & 0xC0) == 0x80) n--;
    return core_str_prefix(s, n);
}

u64 core_str_write_cstr(char *dst, u64 cap, Core_String s) {
    if (!cap) return 0;
    Core_String prefix = core_str_prefix_utf8(s, cap - 1);
    if (prefix.len) memmove(dst, prefix.str, prefix.len);
    dst[prefix.len] = 0;
    return prefix.len;
}

u64 core_cstr_copy(char *dst, u64 cap, const char *src) {
    if (!cap) return 0;
    u64 n = 0;
    if (src) while (n < cap && src[n]) n++;
    return core_str_write_cstr(dst, cap, core_str_n((const u8 *)src, n));
}

Core_String core_str_vfmt(Core_Arena *arena, const char *fmt, va_list args) {
    char small[512];
    va_list copy;
    va_copy(copy, args);
    int n = vsnprintf(small, sizeof(small), fmt, copy);
    va_end(copy);
    if (n < 0) return (Core_String){0};
    if ((u64)n < sizeof(small)) return core_str_copy(arena, core_str_n((u8 *)small, (u64)n));
    char *dst = core_push_array(arena, char, (u64)n + 1);
    if (!dst) return (Core_String){0};
    va_copy(copy, args);
    int written = vsnprintf(dst, (u64)n + 1, fmt, copy);
    va_end(copy);
    if (written < 0 || written != n) return (Core_String){0};
    return core_str_n((u8 *)dst, (u64)n);
}

Core_String core_str_fmt(Core_Arena *arena, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Core_String s = core_str_vfmt(arena, fmt, args);
    va_end(args);
    return s;
}

b32 core_str_eq(Core_String a, Core_String b) {
    if (a.len != b.len) return false;
    return !a.len || memcmp(a.str, b.str, a.len) == 0;
}

Core_String core_str_substr(Core_String s, u64 byte_off, u64 byte_len) {
    if (byte_off > s.len) byte_off = s.len;
    if (byte_len > s.len - byte_off) byte_len = s.len - byte_off;
    return (Core_String){ .str = s.str ? s.str + byte_off : 0, .len = byte_len };
}

Core_String core_str_prefix(Core_String s, u64 byte_len) {
    return core_str_substr(s, 0, byte_len);
}

Core_String core_str_suffix(Core_String s, u64 byte_off) {
    if (byte_off > s.len) byte_off = s.len;
    return (Core_String){ .str = s.str ? s.str + byte_off : 0, .len = s.len - byte_off };
}

u8 core_ascii_lower(u8 c) { return c >= 'A' && c <= 'Z' ? (u8)(c + 32) : c; }
b32 core_ascii_space(u8 c) { return c == ' ' || (c >= '\t' && c <= '\r'); }

s32 core_str_cmp(Core_String a, Core_String b) {
    u64 n = CORE_MIN(a.len, b.len);
    int r = n ? memcmp(a.str, b.str, n) : 0;
    if (r) return r < 0 ? -1 : 1;
    return a.len == b.len ? 0 : (a.len < b.len ? -1 : 1);
}

s32 core_str_cmp_ascii_ci(Core_String a, Core_String b) {
    u64 n = CORE_MIN(a.len, b.len);
    for (u64 i = 0; i < n; i++) {
        u8 x = core_ascii_lower(a.str[i]), y = core_ascii_lower(b.str[i]);
        if (x != y) return x < y ? -1 : 1;
    }
    return a.len == b.len ? 0 : (a.len < b.len ? -1 : 1);
}

b32 core_str_eq_ascii_ci(Core_String a, Core_String b) {
    return a.len == b.len && core_str_cmp_ascii_ci(a, b) == 0;
}

b32 core_cstr_starts_ascii_ci(const char *s, const char *prefix) {
    for (; *prefix; s++, prefix++)
        if (core_ascii_lower((u8)*s) != core_ascii_lower((u8)*prefix)) return false;
    return true;
}

b32 core_cstr_ends_ascii_ci(const char *s, const char *suffix) {
    u64 n = strlen(s), m = strlen(suffix);
    return n >= m && core_cstr_starts_ascii_ci(s + n - m, suffix);
}

Core_String core_str_trim_ascii(Core_String s) {
    u64 start = 0, end = s.len;
    while (start < end && core_ascii_space(s.str[start])) start++;
    while (end > start && core_ascii_space(s.str[end - 1])) end--;
    return core_str_substr(s, start, end - start);
}

u64 core_utf8_prev(Core_String s, u64 at) {
    if (at > s.len) at = s.len;
    if (!at) return 0;
    u64 end = at--, candidate = at;
    while (candidate && end - candidate < 4 && (s.str[candidate] & 0xC0) == 0x80) candidate--;
    Core_Utf8Decode d = core_utf8_decode(s, candidate);
    return candidate + d.size == end ? candidate : at;
}

/* ---- UTF-8 codec ---- */

Core_Utf8Decode core_utf8_decode(Core_String s, u64 at) {
    Core_Utf8Decode r = { .codepoint = 0xFFFD, .size = 1 };
    if (at >= s.len) return r;

    u8 b0 = s.str[at];
    u64 remaining = s.len - at;

    if (b0 < 0x80) {
        r.codepoint = b0;
        r.size = 1;
    } else if (b0 >= 0xC2 && b0 <= 0xDF && remaining >= 2) {
        u8 b1 = s.str[at + 1];
        if ((b1 & 0xC0) == 0x80) {
            r.codepoint = ((u32)(b0 & 0x1F) << 6) | (b1 & 0x3F);
            r.size = 2;
        }
    } else if ((b0 & 0xF0) == 0xE0 && remaining >= 3) {
        u8 b1 = s.str[at + 1], b2 = s.str[at + 2];
        if ((b1 & 0xC0) == 0x80 && (b2 & 0xC0) == 0x80 &&
            (b0 != 0xE0 || b1 >= 0xA0) && (b0 != 0xED || b1 < 0xA0)) {
            r.codepoint = ((u32)(b0 & 0x0F) << 12) | ((u32)(b1 & 0x3F) << 6) | (b2 & 0x3F);
            r.size = 3;
        }
    } else if (b0 >= 0xF0 && b0 <= 0xF4 && remaining >= 4) {
        u8 b1 = s.str[at + 1], b2 = s.str[at + 2], b3 = s.str[at + 3];
        if ((b1 & 0xC0) == 0x80 && (b2 & 0xC0) == 0x80 && (b3 & 0xC0) == 0x80 &&
            (b0 != 0xF0 || b1 >= 0x90) && (b0 != 0xF4 || b1 < 0x90)) {
            r.codepoint = ((u32)(b0 & 0x07) << 18) | ((u32)(b1 & 0x3F) << 12) |
                          ((u32)(b2 & 0x3F) << 6) | (b3 & 0x3F);
            r.size = 4;
        }
    }
    return r;
}

u32 core_utf8_encode(u8 *out, u32 cp) {
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    if (cp < 0x80) {
        out[0] = (u8)cp;
        return 1;
    } else if (cp < 0x800) {
        out[0] = (u8)(0xC0 | (cp >> 6));
        out[1] = (u8)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp < 0x10000) {
        out[0] = (u8)(0xE0 | (cp >> 12));
        out[1] = (u8)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (u8)(0x80 | (cp & 0x3F));
        return 3;
    } else {
        out[0] = (u8)(0xF0 | (cp >> 18));
        out[1] = (u8)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (u8)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (u8)(0x80 | (cp & 0x3F));
        return 4;
    }
}

b32 core_utf8_valid(Core_String s) {
    for (u64 i = 0; i < s.len;) {
        Core_Utf8Decode d = core_utf8_decode(s, i);
        if (d.size == 1 && s.str[i] >= 0x80) return false;
        i += d.size;
    }
    return true;
}

u64 core_str_count_codepoints(Core_String s) {
    u64 count = 0, at = 0;
    while (at < s.len) {
        at += core_utf8_decode(s, at).size;
        count++;
    }
    return count;
}

u32 core_unicode_fold_latin(u32 cp) {
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
        return c == '?' ? cp : (u8)c;
    }
    if (cp >= 0x100 && cp <= 0x17F) {
        char c = ext_a[cp - 0x100];
        return c == '?' ? cp : (u8)c;
    }
    return cp; /* other scripts stay distinct */
}

/* ---- builder ---- */

Core_StringBuilder core_sb_begin(Core_Arena *arena, u64 initial_cap) {
    if (initial_cap < 16) initial_cap = 16;
    Core_StringBuilder sb = { .arena = arena, .len = 0, .cap = initial_cap };
    sb.str = initial_cap <= arena->reserved ? core_push_array(arena, u8, initial_cap) : 0;
    sb.failed = !sb.str;
    return sb;
}

static b32 core_sb_ensure(Core_StringBuilder *sb, u64 extra) {
    if (sb->failed) return false;
    if (extra > (u64)-1 - sb->len) { sb->failed = true; return false; }
    u64 needed = sb->len + extra;
    if (needed > sb->arena->reserved) { sb->failed = true; return false; }
    if (needed <= sb->cap) return true;
    u64 new_cap = sb->cap;
    while (new_cap < needed) {
        if (new_cap > (u64)-1 / 2) { new_cap = needed; break; }
        new_cap *= 2;
    }
    if (new_cap > sb->arena->reserved) new_cap = sb->arena->reserved;
    /* The common append-only case can extend the last arena allocation in
       place: stable VM addresses avoid a copy and abandoned old buffers. */
    if (sb->str + sb->cap == sb->arena->base + sb->arena->used) {
        if (!core_arena_push(sb->arena, new_cap - sb->cap, 1)) {
            sb->failed = true;
            return false;
        }
    } else {
        u8 *new_str = core_push_array(sb->arena, u8, new_cap);
        if (!new_str) { sb->failed = true; return false; }
        if (sb->len) memcpy(new_str, sb->str, sb->len);
        sb->str = new_str;
    }
    sb->cap = new_cap;
    return true;
}

void core_sb_append(Core_StringBuilder *sb, Core_String s) {
    if (!core_sb_ensure(sb, s.len)) return;
    if (s.len) memmove(sb->str + sb->len, s.str, s.len);
    sb->len += s.len;
}

void core_sb_append_cstr(Core_StringBuilder *sb, const char *cstr) {
    core_sb_append(sb, core_str(cstr));
}

void core_sb_append_codepoint(Core_StringBuilder *sb, u32 cp) {
    if (!core_sb_ensure(sb, 4)) return;
    sb->len += core_utf8_encode(sb->str + sb->len, cp);
}

void core_sb_append_u64(Core_StringBuilder *sb, u64 value) {
    u8 tmp[20];
    u32 n = 0;
    if (value == 0) {
        tmp[n++] = '0';
    } else {
        while (value > 0) { tmp[n++] = (u8)('0' + (value % 10)); value /= 10; }
    }
    if (!core_sb_ensure(sb, n)) return;
    for (u32 i = 0; i < n; i++) sb->str[sb->len + i] = tmp[n - 1 - i];
    sb->len += n;
}

Core_String core_sb_finish(Core_StringBuilder *sb) {
    return sb->failed ? (Core_String){0} : (Core_String){ .str = sb->str, .len = sb->len };
}
