#include "string.h"

#include <string.h>

Core_String core_str(const char *cstr) {
    Core_String s = { .str = (u8 *)cstr, .len = 0 };
    if (cstr) s.len = (u64)strlen(cstr);
    return s;
}

Core_String core_str_n(const u8 *bytes, u64 len) {
    return (Core_String){ .str = (u8 *)bytes, .len = len };
}

Core_String core_str_copy(Core_Arena *arena, Core_String s) {
    u8 *dst = core_push_array(arena, u8, s.len);
    if (s.len) memcpy(dst, s.str, s.len);
    return (Core_String){ .str = dst, .len = s.len };
}

char *core_str_to_cstr(Core_Arena *arena, Core_String s) {
    char *dst = core_push_array(arena, char, s.len + 1);
    if (s.len) memcpy(dst, s.str, s.len);
    dst[s.len] = 0;
    return dst;
}

b32 core_str_eq(Core_String a, Core_String b) {
    if (a.len != b.len) return false;
    return memcmp(a.str, b.str, a.len) == 0;
}

Core_String core_str_substr(Core_String s, u64 byte_off, u64 byte_len) {
    if (byte_off > s.len) byte_off = s.len;
    if (byte_off + byte_len > s.len) byte_len = s.len - byte_off;
    return (Core_String){ .str = s.str + byte_off, .len = byte_len };
}

Core_String core_str_prefix(Core_String s, u64 byte_len) {
    return core_str_substr(s, 0, byte_len);
}

Core_String core_str_suffix(Core_String s, u64 byte_off) {
    if (byte_off > s.len) byte_off = s.len;
    return (Core_String){ .str = s.str + byte_off, .len = s.len - byte_off };
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
    } else if ((b0 & 0xE0) == 0xC0 && remaining >= 2) {
        u8 b1 = s.str[at + 1];
        if ((b1 & 0xC0) == 0x80) {
            r.codepoint = ((u32)(b0 & 0x1F) << 6) | (b1 & 0x3F);
            r.size = 2;
        }
    } else if ((b0 & 0xF0) == 0xE0 && remaining >= 3) {
        u8 b1 = s.str[at + 1], b2 = s.str[at + 2];
        if ((b1 & 0xC0) == 0x80 && (b2 & 0xC0) == 0x80) {
            r.codepoint = ((u32)(b0 & 0x0F) << 12) | ((u32)(b1 & 0x3F) << 6) | (b2 & 0x3F);
            r.size = 3;
        }
    } else if ((b0 & 0xF8) == 0xF0 && remaining >= 4) {
        u8 b1 = s.str[at + 1], b2 = s.str[at + 2], b3 = s.str[at + 3];
        if ((b1 & 0xC0) == 0x80 && (b2 & 0xC0) == 0x80 && (b3 & 0xC0) == 0x80) {
            r.codepoint = ((u32)(b0 & 0x07) << 18) | ((u32)(b1 & 0x3F) << 12) |
                          ((u32)(b2 & 0x3F) << 6) | (b3 & 0x3F);
            r.size = 4;
        }
    }
    return r;
}

u32 core_utf8_encode(u8 *out, u32 cp) {
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

u64 core_str_count_codepoints(Core_String s) {
    u64 count = 0, at = 0;
    while (at < s.len) {
        at += core_utf8_decode(s, at).size;
        count++;
    }
    return count;
}

/* ---- builder ---- */

Core_StringBuilder core_sb_begin(Core_Arena *arena, u64 initial_cap) {
    if (initial_cap < 16) initial_cap = 16;
    Core_StringBuilder sb = { .arena = arena, .len = 0, .cap = initial_cap };
    sb.str = core_push_array(arena, u8, initial_cap);
    return sb;
}

static void core_sb_ensure(Core_StringBuilder *sb, u64 extra) {
    if (sb->len + extra <= sb->cap) return;
    u64 new_cap = sb->cap ? sb->cap * 2 : 16;
    while (new_cap < sb->len + extra) new_cap *= 2;
    u8 *new_str = core_push_array(sb->arena, u8, new_cap);
    if (sb->len) memcpy(new_str, sb->str, sb->len);
    sb->str = new_str;
    sb->cap = new_cap;
}

void core_sb_append(Core_StringBuilder *sb, Core_String s) {
    core_sb_ensure(sb, s.len);
    if (s.len) memcpy(sb->str + sb->len, s.str, s.len);
    sb->len += s.len;
}

void core_sb_append_cstr(Core_StringBuilder *sb, const char *cstr) {
    core_sb_append(sb, core_str(cstr));
}

void core_sb_append_codepoint(Core_StringBuilder *sb, u32 cp) {
    core_sb_ensure(sb, 4);
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
    core_sb_ensure(sb, n);
    for (u32 i = 0; i < n; i++) sb->str[sb->len + i] = tmp[n - 1 - i];
    sb->len += n;
}

Core_String core_sb_finish(Core_StringBuilder *sb) {
    return (Core_String){ .str = sb->str, .len = sb->len };
}
