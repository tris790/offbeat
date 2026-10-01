/*
 * Music library: tag parsing (ID3v1/v2, FLAC, Ogg Vorbis), immutable snapshot
 * building, the binary index cache and the background scanner.
 *
 * Tag parsing only ever touches file headers through a small positional read
 * buffer; embedded pictures are located (offset/size) but never read.
 *
 * Cache file layout (native endian, every section 8-byte aligned):
 *
 *   Lib_CacheHeader
 *   Lib_Track  tracks[track_count]        string pointers stored as blob offsets
 *   u32        by_title[track_count]
 *   Lib_Artist artists[artist_count]      name as blob offset, tracks as index
 *   u32        artist_tracks[track_count] every artist's tracks, back to back
 *   Lib_Genre  genres[genre_count]        same scheme as artists
 *   u32        genre_tracks[track_count]
 *   u32        hash_slots[hash_cap]
 *   u8         strings[strings_size]      NUL-terminated, deduplicated
 *
 * Loading is one read into the snapshot's arena plus an in-place pointer
 * fix-up, so every derived index comes for free.
 */

#include "library.h"
#include "../core/hash.h"
#include "../platform/platform.h"

#include <stdatomic.h>
#include <string.h>

/* ---- small helpers ---- */

static u32 lib_be16(const u8 *p) { return ((u32)p[0] << 8) | p[1]; }
static u32 lib_be24(const u8 *p) { return ((u32)p[0] << 16) | ((u32)p[1] << 8) | p[2]; }
static u32 lib_be32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
static u32 lib_le32(const u8 *p) { return ((u32)p[3] << 24) | ((u32)p[2] << 16) | ((u32)p[1] << 8) | p[0]; }
static u64 lib_le64(const u8 *p) { return ((u64)lib_le32(p + 4) << 32) | lib_le32(p); }
static u32 lib_syncsafe(const u8 *p) {
    return ((u32)(p[0] & 0x7F) << 21) | ((u32)(p[1] & 0x7F) << 14) |
           ((u32)(p[2] & 0x7F) << 7)  |  (u32)(p[3] & 0x7F);
}

/* Tag keys are ASCII; values retain their original UTF-8 bytes. */
static b32 lib_ieq(const u8 *s, u64 len, const char *lit) {
    return core_str_eq_ascii_ci(core_str_n(s, len), core_str(lit));
}

/* First 8 lowercased bytes packed big-endian: most sort comparisons end here. */
static u64 lib_sort_key(Core_String s) {
    u64 k = 0;
    for (u32 i = 0; i < 8; i++) k = (k << 8) | (i < s.len ? core_ascii_lower(s.str[i]) : 0);
    return k;
}

u64 library_hash_path(Core_String path) {
    return core_hash_bytes(path.str, path.len);
}

/* ---- buffered positional reader ----
   Tag parsing does many tiny reads that are close together; one 16 KB
   window serves nearly all of them with a single pread. */

#define LIB_READ_CHUNK (16 * 1024)

typedef struct {
    Platform_File file;
    u64 size;
    u64 buf_off;
    u64 buf_len;
    u8  buf[LIB_READ_CHUNK];
} Lib_Reader;

/* Pointer to `n` (<= LIB_READ_CHUNK) bytes at `off`, or 0 past EOF. */
static const u8 *lib_view(Lib_Reader *r, u64 off, u64 n) {
    if (n > LIB_READ_CHUNK || off > r->size || n > r->size - off) return 0;
    if (off < r->buf_off || off + n > r->buf_off + r->buf_len) {
        r->buf_off = off;
        r->buf_len = platform_file_read_at(r->file, off, r->buf, LIB_READ_CHUNK);
        if (n > r->buf_len) return 0;
    }
    return r->buf + (off - r->buf_off);
}

/* Copy up to `n` bytes at `off` into dst; returns bytes copied. */
static u64 lib_read(Lib_Reader *r, u64 off, void *dst, u64 n) {
    if (off >= r->size) return 0;
    if (n > r->size - off) n = r->size - off;
    if (n <= LIB_READ_CHUNK) {
        const u8 *p = lib_view(r, off, n);
        if (!p) return 0;
        memcpy(dst, p, n);
        return n;
    }
    return platform_file_read_at(r->file, off, dst, n);
}

/* ---- text decoding ---- */

typedef struct {
    u8  *out;
    u64  len;
    b32  sep; /* a NUL was seen: join the next value with ", " */
} Lib_TextOut;

static void lib_emit(Lib_TextOut *t, u32 cp) {
    if (cp == 0) { t->sep = true; return; }
    if (cp == 0xFEFF) return;                 /* stray BOM */
    if (cp < 0x20 || cp == 0x7F) cp = ' ';    /* newlines/tabs in titles */
    if (t->sep) {
        if (t->len) { t->out[t->len++] = ','; t->out[t->len++] = ' '; }
        t->sep = false;
    }
    t->len += core_utf8_encode(t->out + t->len, cp);
}

/* Decode tag text to UTF-8 into the arena. enc: 0 = ISO-8859-1, 1 = UTF-16
   with BOM, 2 = UTF-16BE, 3 = UTF-8. NUL-separated values are joined with
   ", ", control characters become spaces and both ends are trimmed. The
   result is NUL-terminated. */
static Core_String lib_decode_text(Core_Arena *a, u32 enc, const u8 *p, u64 n) {
    u64 cap = n * 2 + 8; /* worst case: Latin-1 -> 2 bytes per byte */
    u8 *out = core_arena_push(a, cap, 1);
    if (!out) return (Core_String){0};
    u64 mark = a->used;
    Lib_TextOut t = { .out = out };

    if (enc == 3 && !core_utf8_valid(core_str_n(p, n))) enc = 0; /* mislabeled Latin-1 */
    if (enc == 0) {
        for (u64 i = 0; i < n; i++) lib_emit(&t, p[i]);
    } else if (enc == 3) {
        Core_String s = core_str_n(p, n);
        for (u64 i = 0; i < n;) {
            Core_Utf8Decode d = core_utf8_decode(s, i);
            lib_emit(&t, d.codepoint);
            i += d.size;
        }
    } else {
        b32 be = (enc == 2), want_bom = (enc == 1);
        u64 i = 0;
        while (i + 1 < n) {
            if (want_bom) {
                want_bom = false;
                if (p[i] == 0xFF && p[i + 1] == 0xFE) { be = false; i += 2; continue; }
                if (p[i] == 0xFE && p[i + 1] == 0xFF) { be = true;  i += 2; continue; }
            }
            u32 u = be ? lib_be16(p + i) : ((u32)p[i + 1] << 8 | p[i]);
            i += 2;
            u32 cp = u;
            if (u >= 0xD800 && u < 0xDC00) {
                cp = 0xFFFD;
                if (i + 1 < n) {
                    u32 lo = be ? lib_be16(p + i) : ((u32)p[i + 1] << 8 | p[i]);
                    if (lo >= 0xDC00 && lo < 0xE000) {
                        cp = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                        i += 2;
                    }
                }
            } else if (u >= 0xDC00 && u < 0xE000) {
                cp = 0xFFFD;
            }
            lib_emit(&t, cp);
            if (u == 0 && enc == 1) want_bom = true; /* each value has a BOM */
        }
    }

    /* Give back the unused tail when we are still the last allocation. */
    if (a->used == mark) a->used = mark - cap + t.len + 1;
    Core_String s = core_str_trim_ascii((Core_String){ .str = out, .len = t.len });
    s.str[s.len] = 0;
    return s;
}

/* ---- parsed fields shared by all formats ---- */

typedef struct {
    Core_String title, artist, band, album, genre;
    u32 duration_ms;
    u32 cover_offset, cover_size;
    u32 cover_type;   /* APIC/PICTURE picture type; 3 = front cover wins */
    u32 sample_rate;  /* ogg: from the id header                          */
} Lib_Fields;

static b32 lib_image_signature(const u8 *p, u64 n) {
    if (n >= 8 && memcmp(p, "\x89PNG\r\n\x1a\n", 8) == 0) return true;
    if (n >= 3 && p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF) return true;
    if (n >= 4 && memcmp(p, "GIF8", 4) == 0) return true;
    if (n >= 2 && p[0] == 'B' && p[1] == 'M') return true;
    return false;
}

static void lib_offer_cover(Lib_Fields *f, u64 offset, u64 size, u32 type) {
    if (size == 0 || offset + size > 0xFFFFFFFFull) return;
    b32 better = f->cover_size == 0 || (type == 3 && f->cover_type != 3);
    if (!better) return;
    f->cover_offset = (u32)offset;
    f->cover_size   = (u32)size;
    f->cover_type   = type;
}

static const char *const lib_genres[] = {
    "Blues", "Classic Rock", "Country", "Dance", "Disco", "Funk", "Grunge",
    "Hip-Hop", "Jazz", "Metal", "New Age", "Oldies", "Other", "Pop", "R&B",
    "Rap", "Reggae", "Rock", "Techno", "Industrial", "Alternative", "Ska",
    "Death Metal", "Pranks", "Soundtrack", "Euro-Techno", "Ambient",
    "Trip-Hop", "Vocal", "Jazz+Funk", "Fusion", "Trance", "Classical",
    "Instrumental", "Acid", "House", "Game", "Sound Clip", "Gospel", "Noise",
    "Alternative Rock", "Bass", "Soul", "Punk", "Space", "Meditative",
    "Instrumental Pop", "Instrumental Rock", "Ethnic", "Gothic", "Darkwave",
    "Techno-Industrial", "Electronic", "Pop-Folk", "Eurodance", "Dream",
    "Southern Rock", "Comedy", "Cult", "Gangsta", "Top 40", "Christian Rap",
    "Pop/Funk", "Jungle", "Native American", "Cabaret", "New Wave",
    "Psychedelic", "Rave", "Showtunes", "Trailer", "Lo-Fi", "Tribal",
    "Acid Punk", "Acid Jazz", "Polka", "Retro", "Musical", "Rock & Roll",
    "Hard Rock", "Folk", "Folk-Rock", "National Folk", "Swing", "Fast Fusion",
    "Bebop", "Latin", "Revival", "Celtic", "Bluegrass", "Avantgarde",
    "Gothic Rock", "Progressive Rock", "Psychedelic Rock", "Symphonic Rock",
    "Slow Rock", "Big Band", "Chorus", "Easy Listening", "Acoustic", "Humour",
    "Speech", "Chanson", "Opera", "Chamber Music", "Sonata", "Symphony",
    "Booty Bass", "Primus", "Porn Groove", "Satire", "Slow Jam", "Club",
    "Tango", "Samba", "Folklore", "Ballad", "Power Ballad", "Rhythmic Soul",
    "Freestyle", "Duet", "Punk Rock", "Drum Solo", "A Cappella", "Euro-House",
    "Dance Hall",
};

static b32 lib_parse_uint(Core_String s, u32 *out) {
    if (s.len == 0 || s.len > 9) return false;
    u32 v = 0;
    for (u64 i = 0; i < s.len; i++) {
        if (s.str[i] < '0' || s.str[i] > '9') return false;
        v = v * 10 + (s.str[i] - '0');
    }
    *out = v;
    return true;
}

/* "(17)", "(17)Rock", "17", "(RX)" -> readable genre. */
static Core_String lib_genre_clean(Core_String g) {
    u32 ref = 0xFFFFFFFF, n;
    while (g.len && g.str[0] == '(') {
        u64 close = 1;
        while (close < g.len && g.str[close] != ')') close++;
        if (close >= g.len) break;
        Core_String inner = core_str_substr(g, 1, close - 1);
        if (lib_parse_uint(inner, &n)) { if (ref == 0xFFFFFFFF) ref = n; }
        else if (!lib_ieq(inner.str, inner.len, "rx") && !lib_ieq(inner.str, inner.len, "cr")) break;
        g = core_str_trim_ascii(core_str_suffix(g, close + 1));
    }
    if (lib_parse_uint(g, &n)) { ref = n; g.len = 0; }
    if (g.len == 0 && ref < CORE_ARRAY_COUNT(lib_genres)) g = core_str(lib_genres[ref]);
    return g;
}

/* ---- ID3v2 ---- */

/* Tag bytes come either straight from the file (normal case) or from a
   de-unsynchronised in-memory copy (v2.2/v2.3 tag-level unsync). */
typedef struct {
    Lib_Reader *r;
    const u8   *mem;     /* non-zero: positions are offsets into mem */
    u64         mem_len;
} Lib_Src;

static const u8 *lib_src_view(Lib_Src *s, u64 off, u64 n) {
    if (s->mem) return (off <= s->mem_len && n <= s->mem_len - off) ? s->mem + off : 0;
    return lib_view(s->r, off, n);
}

/* Removes the 0x00 stuffed after every 0xFF. Returns the new length. */
static u64 lib_unsync(u8 *dst, const u8 *src, u64 n) {
    u64 o = 0;
    for (u64 i = 0; i < n; i++) {
        dst[o++] = src[i];
        if (src[i] == 0xFF && i + 1 < n && src[i + 1] == 0x00) i++;
    }
    return o;
}

static b32 lib_id3_id_char(u8 c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }

/* A frame may start at `pos`: valid id, padding, or the end of the tag. */
static b32 lib_id3_frame_ok(Lib_Src *s, u64 pos, u64 end) {
    if (pos == end) return true;
    if (pos > end || end - pos < 4) return false;
    const u8 *p = lib_src_view(s, pos, 4);
    if (!p) return false;
    if (p[0] == 0) return true;
    return lib_id3_id_char(p[0]) && lib_id3_id_char(p[1]) && lib_id3_id_char(p[2]) && lib_id3_id_char(p[3]);
}

/* v2.2 three-letter ids we care about, mapped to their v2.3 names. */
static void lib_id3_v22_id(const u8 *id3, char out[4]) {
    static const char *const map[][2] = {
        { "TT2", "TIT2" }, { "TP1", "TPE1" }, { "TP2", "TPE2" }, { "TAL", "TALB" },
        { "TCO", "TCON" }, { "TLE", "TLEN" }, { "PIC", "APIC" },
    };
    memcpy(out, "XXXX", 4);
    for (u32 i = 0; i < CORE_ARRAY_COUNT(map); i++)
        if (memcmp(id3, map[i][0], 3) == 0) { memcpy(out, map[i][1], 4); return; }
}

static void lib_id3_text_frame(Core_Arena *a, Lib_Fields *f, const char *id, const u8 *body, u64 n) {
    if (n < 2) return;
    Core_String *dst = 0;
    if      (!memcmp(id, "TIT2", 4)) dst = &f->title;
    else if (!memcmp(id, "TPE1", 4)) dst = &f->artist;
    else if (!memcmp(id, "TPE2", 4)) dst = &f->band;
    else if (!memcmp(id, "TALB", 4)) dst = &f->album;
    else if (!memcmp(id, "TCON", 4)) dst = &f->genre;
    else if (!memcmp(id, "TLEN", 4)) {
        u8 tmp[32];
        u64 m = 0;
        for (u64 i = 1; i < n && m < sizeof(tmp); i++) {
            u8 c = body[i];
            if (c >= '0' && c <= '9') tmp[m++] = c; /* also skips UTF-16 zero bytes */
            else if (c != 0 && c != 0xFF && c != 0xFE) break;
        }
        u32 ms;
        if (lib_parse_uint(core_str_n(tmp, m), &ms) && ms > 0) f->duration_ms = ms;
        return;
    } else {
        return;
    }
    if (dst->len) return; /* first occurrence wins */
    *dst = lib_decode_text(a, body[0], body + 1, n - 1);
    if (dst == &f->genre) *dst = lib_genre_clean(*dst);
}

/* APIC / PIC: skip the small header, record where the image bytes live. */
static void lib_id3_picture(Lib_Src *s, Lib_Fields *f, b32 v22, u64 body, u64 n) {
    const u8 *p = lib_src_view(s, body, CORE_MIN(n, (u64)4096));
    if (!p) return;
    u64 avail = CORE_MIN(n, (u64)4096), i = 1;
    u32 enc = p[0];
    if (v22) {
        i += 3;                                   /* "JPG" / "PNG" */
    } else {
        while (i < avail && p[i]) i++;            /* mime, NUL-terminated */
        i++;
    }
    if (i >= avail) return;
    u32 type = p[i++];
    if (enc == 1 || enc == 2) {                   /* UTF-16 description */
        while (i + 1 < avail && (p[i] || p[i + 1])) i += 2;
        i += 2;
    } else {
        while (i < avail && p[i]) i++;
        i++;
    }
    if (i >= avail) return;
    if (!lib_image_signature(p + i, avail - i)) return; /* "-->" links etc. */
    lib_offer_cover(f, body + i, n - i, type);
}

/* Parses an ID3v2 tag at `at`. Returns its total length (0 = no tag). */
static u64 lib_parse_id3v2(Core_Arena *a, Lib_Reader *r, u64 at, Lib_Fields *f) {
    const u8 *h = lib_view(r, at, 10);
    if (!h || memcmp(h, "ID3", 3) != 0 || h[3] < 2 || h[3] > 4 || h[4] == 0xFF) return 0;
    if ((h[6] | h[7] | h[8] | h[9]) & 0x80) return 0;
    u32 ver = h[3], flags = h[5];
    u64 size  = lib_syncsafe(h + 6);
    u64 total = 10 + size + ((ver == 4 && (flags & 0x10)) ? 10 : 0);
    if (ver == 2 && (flags & 0x40)) return total; /* v2.2 compression: unusable */

    Lib_Src src = { .r = r };
    u64 pos = at + 10, end = at + 10 + size;
    b32 tag_unsync = (flags & 0x80) != 0;

    if (tag_unsync && ver < 4) {
        /* Whole-tag unsynchronisation: sizes refer to the decoded bytes, so
           decode a bounded prefix. Pictures are skipped in this mode (their
           file bytes are not the image). Rare in practice. */
        u64 n = CORE_MIN(size, (u64)CORE_MB(1));
        u8 *copy = core_arena_push(a, n, 1);
        if (!copy) return total;
        n = lib_read(r, pos, copy, n);
        src.mem = copy;
        src.mem_len = lib_unsync(copy, copy, n);
        pos = 0;
        end = src.mem_len;
    }

    if (ver >= 3 && (flags & 0x40)) {           /* extended header */
        const u8 *e = lib_src_view(&src, pos, 4);
        if (!e) return total;
        pos += (ver == 3) ? 4 + (u64)lib_be32(e) : (u64)lib_syncsafe(e);
    }

    u32 hdr_len = (ver == 2) ? 6 : 10;
    for (u32 guard = 0; guard < 4096 && pos + hdr_len <= end; guard++) {
        const u8 *fh = lib_src_view(&src, pos, hdr_len);
        if (!fh || fh[0] == 0) break;             /* padding */
        char id[4];
        u64 fsize;
        u32 fflags = 0;
        if (ver == 2) {
            if (!lib_id3_id_char(fh[0]) || !lib_id3_id_char(fh[1]) || !lib_id3_id_char(fh[2])) break;
            lib_id3_v22_id(fh, id);
            fsize = lib_be24(fh + 3);
        } else {
            if (!lib_id3_id_char(fh[0]) || !lib_id3_id_char(fh[1]) ||
                !lib_id3_id_char(fh[2]) || !lib_id3_id_char(fh[3])) break;
            memcpy(id, fh, 4);
            fflags = lib_be16(fh + 8);
            fsize = lib_be32(fh + 4);
            if (ver == 4 && !((fh[4] | fh[5] | fh[6] | fh[7]) & 0x80)) {
                /* v2.4 sizes are syncsafe, but some writers (old iTunes)
                   store plain integers: pick whichever lands on a frame. */
                u64 ss = lib_syncsafe(fh + 4);
                if (ss != fsize && (lib_id3_frame_ok(&src, pos + 10 + ss, end) ||
                                    !lib_id3_frame_ok(&src, pos + 10 + fsize, end))) fsize = ss;
            }
        }
        u64 body = pos + hdr_len;
        if (fsize > end - body) break;
        pos = body + fsize;

        /* Frame format flags. */
        b32 skip = false, unsync = false;
        u64 b = body, n = fsize;
        if (ver == 3) {
            if (fflags & 0x00C0) skip = true;     /* compressed / encrypted */
            if (fflags & 0x0020) { b += 1; n -= n ? 1 : 0; }
        } else if (ver == 4) {
            if (fflags & 0x000C) skip = true;
            if (fflags & 0x0040) { b += 1; n -= n ? 1 : 0; }
            if (fflags & 0x0001) { if (n < 4) skip = true; else { b += 4; n -= 4; } }
            unsync = (fflags & 0x0002) || tag_unsync;
        }
        if (skip || n == 0) continue;

        if (id[0] == 'T') {
            if (n > LIB_READ_CHUNK) continue;     /* no useful text is that long */
            const u8 *p = lib_src_view(&src, b, n);
            if (!p) continue;
            if (unsync) {
                u8 *tmp = core_arena_push(a, n, 1);
                if (!tmp) continue;
                n = lib_unsync(tmp, p, n);
                p = tmp;
            }
            lib_id3_text_frame(a, f, id, p, n);
        } else if (!memcmp(id, "APIC", 4) && !src.mem && !unsync) {
            lib_id3_picture(&src, f, ver == 2, b, n);
        }
    }
    return total;
}

/* ID3v1 at the end of the file: fills only fields that are still empty. */
static b32 lib_parse_id3v1(Core_Arena *a, Lib_Reader *r, Lib_Fields *f) {
    if (r->size < 128) return false;
    const u8 *t = lib_view(r, r->size - 128, 128);
    if (!t || memcmp(t, "TAG", 3) != 0) return false;
    Core_String *dst[3] = { &f->title, &f->artist, &f->album };
    for (u32 i = 0; i < 3; i++) {
        if (dst[i]->len) continue;
        const u8 *p = t + 3 + 30 * i;
        u64 n = 0;
        while (n < 30 && p[n]) n++;
        *dst[i] = lib_decode_text(a, 0, p, n);
    }
    if (!f->genre.len && t[127] < CORE_ARRAY_COUNT(lib_genres)) f->genre = core_str(lib_genres[t[127]]);
    return true;
}

/* ---- MPEG audio ---- */

typedef struct {
    u32 bitrate;     /* kbit/s */
    u32 sample_rate;
    u32 frame_len;   /* bytes  */
    u32 samples;     /* per frame */
    u32 side_info;   /* layer III side info size */
} Lib_Mpeg;

static b32 lib_mpeg_header(const u8 *h, Lib_Mpeg *m) {
    static const u16 rates[3][15] = {
        { 0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448 }, /* V1 L1 */
        { 0, 32, 48, 56,  64,  80,  96, 112, 128, 160, 192, 224, 256, 320, 384 }, /* V1 L2 */
        { 0, 32, 40, 48,  56,  64,  80,  96, 112, 128, 160, 192, 224, 256, 320 }, /* V1 L3 */
    };
    static const u16 rates2[2][15] = {
        { 0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256 },    /* V2 L1 */
        { 0,  8, 16, 24, 32, 40, 48,  56,  64,  80,  96, 112, 128, 144, 160 },    /* V2 L2/L3 */
    };
    static const u32 srates[3] = { 44100, 48000, 32000 };

    if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0) return false;
    u32 ver = (h[1] >> 3) & 3, layer = (h[1] >> 1) & 3;
    u32 bri = h[2] >> 4, sri = (h[2] >> 2) & 3, pad = (h[2] >> 1) & 1;
    if (ver == 1 || layer == 0 || bri == 0 || bri == 15 || sri == 3) return false;
    b32 v1 = (ver == 3), mono = (h[3] >> 6) == 3;
    u32 l = 3 - layer; /* 0 = L1, 1 = L2, 2 = L3 */

    m->bitrate     = v1 ? rates[l][bri] : rates2[l == 0 ? 0 : 1][bri];
    m->sample_rate = srates[sri] >> (v1 ? 0 : (ver == 2 ? 1 : 2));
    if (l == 0) {
        m->samples   = 384;
        m->frame_len = (12 * m->bitrate * 1000 / m->sample_rate + pad) * 4;
    } else {
        m->samples   = (l == 2 && !v1) ? 576 : 1152;
        m->frame_len = m->samples / 8 * m->bitrate * 1000 / m->sample_rate + pad;
    }
    m->side_info = v1 ? (mono ? 17 : 32) : (mono ? 9 : 17);
    return m->frame_len >= 24;
}

/* Finds the first MPEG frame at/after `start` and derives the duration
   (Xing/Info/VBRI frame count, else CBR estimate). Returns false if no frame
   sync was found within the first 64 KB. */
static b32 lib_mp3_duration(Lib_Reader *r, u64 start, u64 end, u32 *out_ms) {
    u64 limit = CORE_MIN(end, start + CORE_KB(64));
    for (u64 pos = start; pos + 4 <= limit; pos++) {
        const u8 *h = lib_view(r, pos, 4);
        if (!h) return false;
        if (h[0] != 0xFF) continue;
        Lib_Mpeg m;
        if (!lib_mpeg_header(h, &m)) continue;
        u8 h0[4];
        memcpy(h0, h, 4);

        /* Confirm with the next frame so a stray 0xFF in junk can't fool us. */
        u64 next = pos + m.frame_len;
        if (next + 4 <= end) {
            const u8 *n = lib_view(r, next, 4);
            Lib_Mpeg m2;
            if (!n || !lib_mpeg_header(n, &m2) || (n[1] & 0xFE) != (h0[1] & 0xFE) ||
                (n[2] & 0x0C) != (h0[2] & 0x0C)) continue;
        }

        u64 frames = 0;
        const u8 *x = lib_view(r, pos + 4 + m.side_info, 16);
        if (x && (!memcmp(x, "Xing", 4) || !memcmp(x, "Info", 4)) && (lib_be32(x + 4) & 1))
            frames = lib_be32(x + 8);
        const u8 *v = lib_view(r, pos + 36, 18);
        if (!frames && v && !memcmp(v, "VBRI", 4)) frames = lib_be32(v + 14);

        if (frames) *out_ms = (u32)(frames * m.samples * 1000 / m.sample_rate);
        else        *out_ms = (u32)((end - pos) * 8 / m.bitrate); /* bits / (kbit/s) = ms */
        return true;
    }
    return false;
}

/* ---- Vorbis comments (FLAC + Ogg) ---- */

static void lib_vorbis_comments(Core_Arena *a, const u8 *p, u64 n, Lib_Fields *f) {
    if (n < 8) return;
    u64 vendor = lib_le32(p);
    if (vendor > n - 8) return;
    u64 i = 4 + vendor;
    u32 count = lib_le32(p + i);
    i += 4;
    for (u32 c = 0; c < count && i + 4 <= n; c++) {
        u64 len = lib_le32(p + i);
        i += 4;
        if (len > n - i) break;                   /* truncated read: stop */
        const u8 *e = p + i;
        i += len;
        u64 eq = 0;
        while (eq < len && e[eq] != '=') eq++;
        if (eq >= len) continue;
        Core_String *dst = 0;
        if      (lib_ieq(e, eq, "title"))       dst = &f->title;
        else if (lib_ieq(e, eq, "artist"))      dst = &f->artist;
        else if (lib_ieq(e, eq, "albumartist")) dst = &f->band;
        else if (lib_ieq(e, eq, "album"))       dst = &f->album;
        else if (lib_ieq(e, eq, "genre"))       dst = &f->genre;
        if (!dst || dst->len) continue;
        *dst = lib_decode_text(a, 3, e + eq + 1, len - eq - 1);
    }
}

/* ---- FLAC ---- */

static void lib_parse_flac(Core_Arena *a, Lib_Reader *r, u64 at, Lib_Fields *f) {
    u64 pos = at + 4;
    for (u32 guard = 0; guard < 256; guard++) {
        const u8 *h = lib_view(r, pos, 4);
        if (!h) return;
        b32 last = (h[0] & 0x80) != 0;
        u32 type = h[0] & 0x7F;
        u64 len = lib_be24(h + 1), body = pos + 4;
        if (type == 127) return;

        if (type == 0 && len >= 18) {                        /* STREAMINFO */
            const u8 *b = lib_view(r, body, 18);
            if (b) {
                u32 sr = ((u32)b[10] << 12) | ((u32)b[11] << 4) | (b[12] >> 4);
                u64 total = ((u64)(b[13] & 0x0F) << 32) | lib_be32(b + 14);
                if (sr && total) f->duration_ms = (u32)(total * 1000 / sr);
            }
        } else if (type == 4) {                              /* VORBIS_COMMENT */
            u64 n = CORE_MIN(len, (u64)CORE_KB(256)); /* comments may hold lyrics/art */
            u8 *buf = core_arena_push(a, n, 1);
            if (buf) lib_vorbis_comments(a, buf, lib_read(r, body, buf, n), f);
        } else if (type == 6 && len >= 32) {                 /* PICTURE */
            u64 n = CORE_MIN(len, (u64)LIB_READ_CHUNK);
            const u8 *b = lib_view(r, body, n);
            if (b) {
                u32 ptype = lib_be32(b);
                u64 i = 4, mlen = lib_be32(b + i);
                i += 4 + mlen;
                if (i + 4 <= n) {
                    u64 dlen = lib_be32(b + i);
                    i += 4 + dlen + 16;
                    if (i + 4 <= n) {
                        u64 size = lib_be32(b + i);
                        i += 4;
                        if (size <= len - i && lib_image_signature(b + i, n - i))
                            lib_offer_cover(f, body + i, size, ptype);
                    }
                }
            }
        }
        if (last) return;
        pos = body + len;
    }
}

/* ---- Ogg Vorbis ---- */

static b32 lib_parse_ogg(Core_Arena *a, Lib_Reader *r, Lib_Fields *f) {
    enum { PACKET_CAP = CORE_KB(256) };
    u8 *pkt = core_arena_push(a, PACKET_CAP, 1);
    if (!pkt) return false;
    u64 plen = 0, pos = 0;
    u32 packet = 0, serial = 0;
    b32 is_vorbis = false;

    for (u32 page = 0; page < 128 && packet < 2; page++) {
        const u8 *h = lib_view(r, pos, 27);
        if (!h || memcmp(h, "OggS", 4) != 0) break;
        u32 nseg = h[26], ser = lib_le32(h + 14);
        if (page == 0) serial = ser;
        const u8 *st = lib_view(r, pos + 27, nseg);
        if (!st) break;
        u8 segs[255];
        memcpy(segs, st, nseg);
        u64 data = pos + 27 + nseg;
        for (u32 i = 0; i < nseg && ser == serial && packet < 2; i++) {
            u64 n = segs[i];
            if (plen + n <= PACKET_CAP) {
                if (lib_read(r, data, pkt + plen, n) != n) return is_vorbis;
                plen += n;
            }
            data += n;
            if (n == 255) continue;
            /* packet complete */
            if (packet == 0) {
                if (plen < 16 || memcmp(pkt, "\x01vorbis", 7) != 0) return false; /* Opus etc. */
                f->sample_rate = lib_le32(pkt + 12);
                is_vorbis = true;
            } else if (plen > 7 && !memcmp(pkt, "\x03vorbis", 7)) {
                lib_vorbis_comments(a, pkt + 7, plen - 7, f);
            }
            packet++;
            plen = 0;
        }
        for (u32 i = 0; i < nseg; i++) pos += segs[i];
        pos += 27 + nseg;
    }
    if (!is_vorbis) return false;

    /* Duration: granule position of the last page (one read at the tail). */
    if (f->sample_rate) {
        u64 n = CORE_MIN(r->size, (u64)CORE_KB(64));
        u8 *tail = core_arena_push(a, n, 1);
        if (tail && n >= 27 && lib_read(r, r->size - n, tail, n) == n) {
            for (u64 i = n - 27 + 1; i-- > 0;) {
                if (tail[i] == 'O' && !memcmp(tail + i, "OggS", 4) && lib_le32(tail + i + 14) == serial) {
                    u64 g = lib_le64(tail + i + 6);
                    if (g != ~0ull) { f->duration_ms = (u32)(g * 1000 / f->sample_rate); break; }
                }
            }
        }
    }
    return true;
}

/* ---- tag reading entry point ---- */

static Core_String lib_file_stem(Core_String path) {
    u64 slash = path.len;
    while (slash > 0 && path.str[slash - 1] != '/') slash--;
    Core_String name = core_str_suffix(path, slash);
    u64 dot = name.len;
    while (dot > 0 && name.str[dot - 1] != '.') dot--;
    if (dot > 1) name.len = dot - 1;
    return name;
}

static Core_String lib_parent_name(Core_String path) {
    u64 end = path.len;
    while (end > 0 && path.str[end - 1] != '/') end--;
    if (end == 0) return (Core_String){0};
    end--; /* the slash */
    u64 start = end;
    while (start > 0 && path.str[start - 1] != '/') start--;
    return core_str_substr(path, start, end - start);
}

/* Genre = first folder below the music root; files sitting directly in the
   root have none. The result points into `path` (or is a literal). */
static Core_String lib_genre_from_path(Core_String path, Core_String root) {
    Core_String none = core_str_lit("Unsorted");
    if (path.len <= root.len || memcmp(path.str, root.str, root.len) != 0) return none;
    u64 start = root.len;
    while (start < path.len && path.str[start] == '/') start++;
    u64 end = start;
    while (end < path.len && path.str[end] != '/') end++;
    if (end == path.len || end == start) return none; /* no folder in between */
    return core_str_substr(path, start, end - start);
}

static b32 lib_ext_is(Core_String path, const char *ext) {
    u64 n = strlen(ext);
    return path.len > n && lib_ieq(path.str + path.len - n, n, ext);
}

b32 library_read_tags(Core_Arena *arena, const char *path, Lib_TagInfo *out) {
    memset(out, 0, sizeof(*out));
    Lib_Reader rd;
    rd.file = platform_file_open_read(path);
    if (!platform_file_valid(rd.file)) return false;
    rd.size = platform_file_size(rd.file);
    rd.buf_off = rd.buf_len = 0;
    Lib_Reader *r = &rd;

    Core_String spath = core_str(path);
    Lib_Fields f = {0};
    u8 format = LIB_FORMAT_UNKNOWN;

    u64 id3 = lib_parse_id3v2(arena, r, 0, &f);
    const u8 *m = lib_view(r, id3, 4);
    if (m && !memcmp(m, "fLaC", 4)) {
        format = LIB_FORMAT_FLAC;
        lib_parse_flac(arena, r, id3, &f);
    } else if (m && !memcmp(m, "OggS", 4)) {
        if (lib_parse_ogg(arena, r, &f)) format = LIB_FORMAT_OGG;
    } else if (id3 || lib_ext_is(spath, ".mp3") || (m && m[0] == 0xFF)) {
        b32 v1 = lib_parse_id3v1(arena, r, &f);
        u64 end = r->size - (v1 ? 128 : 0);
        u32 ms = 0;
        if (lib_mp3_duration(r, id3, end, &ms)) {
            format = LIB_FORMAT_MP3;
            if (!f.duration_ms) f.duration_ms = ms;
        }
    }
    platform_file_close(r->file);
    if (format == LIB_FORMAT_UNKNOWN) return false;

    out->format       = format;
    out->title        = f.title.len  ? f.title  : core_str_copy(arena, lib_file_stem(spath));
    out->artist       = f.artist.len ? f.artist : f.band.len ? f.band : core_str_lit("Unknown Artist");
    out->album        = f.album;
    out->genre        = f.genre.len  ? f.genre  : core_str_copy(arena, lib_parent_name(spath));
    out->duration_ms  = f.duration_ms;
    out->cover_offset = f.cover_offset;
    out->cover_size   = f.cover_size;
    return true;
}

/* ---- sorting ---- */

typedef s32 (*Lib_CmpProc)(const void *ctx, u32 a, u32 b);

/* Stable merge sort of indices: insertion-sorted runs of 16, then bottom-up
   merges ping-ponging between `a` and `tmp`. */
static void lib_sort(u32 *a, u32 *tmp, u32 n, Lib_CmpProc cmp, const void *ctx) {
    enum { RUN = 16 };
    for (u32 s = 0; s < n; s += RUN) {
        u32 e = CORE_MIN(s + RUN, n);
        for (u32 i = s + 1; i < e; i++) {
            u32 v = a[i], j = i;
            while (j > s && cmp(ctx, a[j - 1], v) > 0) { a[j] = a[j - 1]; j--; }
            a[j] = v;
        }
    }
    u32 *src = a, *dst = tmp;
    for (u64 w = RUN; w < n; w *= 2) {
        for (u64 lo = 0; lo < n; lo += 2 * w) {
            u32 mid = (u32)CORE_MIN(lo + w, (u64)n), hi = (u32)CORE_MIN(lo + 2 * w, (u64)n);
            u32 i = (u32)lo, j = mid, k = (u32)lo;
            while (i < mid && j < hi) dst[k++] = (cmp(ctx, src[j], src[i]) < 0) ? src[j++] : src[i++];
            while (i < mid) dst[k++] = src[i++];
            while (j < hi)  dst[k++] = src[j++];
        }
        u32 *t = src; src = dst; dst = t;
    }
    if (src != a) memcpy(a, src, (u64)n * sizeof(u32));
}

typedef struct {
    const Lib_Track *tracks;
    const u64       *key;
} Lib_SortCtx;

static s32 lib_cmp_by_title(const void *ctx, u32 a, u32 b) {
    const Lib_SortCtx *c = ctx;
    if (c->key[a] != c->key[b]) return c->key[a] < c->key[b] ? -1 : 1;
    const Lib_Track *x = &c->tracks[a], *y = &c->tracks[b];
    s32 r = core_str_cmp_ascii_ci(x->title, y->title);
    if (!r) r = core_str_cmp_ascii_ci(x->artist, y->artist);
    if (!r) r = core_str_cmp(x->path, y->path);
    return r;
}

static s32 lib_cmp_by_artist(const void *ctx, u32 a, u32 b) {
    const Lib_SortCtx *c = ctx;
    if (c->key[a] != c->key[b]) return c->key[a] < c->key[b] ? -1 : 1;
    const Lib_Track *x = &c->tracks[a], *y = &c->tracks[b];
    s32 r = core_str_cmp_ascii_ci(x->artist, y->artist);
    if (!r) r = core_str_cmp_ascii_ci(x->title, y->title);
    if (!r) r = core_str_cmp(x->path, y->path);
    return r;
}

static s32 lib_cmp_by_genre(const void *ctx, u32 a, u32 b) {
    const Lib_SortCtx *c = ctx;
    if (c->key[a] != c->key[b]) return c->key[a] < c->key[b] ? -1 : 1;
    const Lib_Track *x = &c->tracks[a], *y = &c->tracks[b];
    s32 r = core_str_cmp_ascii_ci(x->genre, y->genre);
    if (!r) r = core_str_cmp_ascii_ci(x->artist, y->artist);
    if (!r) r = core_str_cmp_ascii_ci(x->title, y->title);
    if (!r) r = core_str_cmp(x->path, y->path);
    return r;
}

/* ---- snapshot ---- */

static Library *lib_create(void) {
    Core_Arena a;
    if (!core_arena_init(&a, CORE_GB(4))) return 0;
    Library *lib = core_push_struct(&a, Library);
    if (!lib) { core_arena_release(&a); return 0; }
    lib->arena = a; /* from here on, allocate through lib->arena only */
    return lib;
}

void library_free(Library *lib) {
    if (!lib) return;
    Core_Arena a = lib->arena; /* lib itself lives inside the arena */
    core_arena_release(&a);
}

s64 library_find(const Library *lib, u64 path_hash) {
    if (!lib || !lib->hash_cap) return -1;
    u32 mask = lib->hash_cap - 1;
    for (u32 i = (u32)path_hash & mask, probes = 0; probes < lib->hash_cap; i = (i + 1) & mask, probes++) {
        u32 slot = lib->hash_slots[i];
        if (!slot) return -1;
        if (lib->tracks[slot - 1].path_hash == path_hash) return slot - 1;
    }
    return -1;
}

/* Sort orders, artist grouping and the hash table (tracks must be final). */
static b32 lib_build_indices(Library *lib, Core_Arena *scratch) {
    u32 n = lib->track_count;
    Core_Temp temp = core_temp_begin(scratch);
    u64 *key = core_push_array(scratch, u64, n + 1);
    u32 *tmp = core_push_array(scratch, u32, n + 1);
    u32 *by_artist = core_push_array(&lib->arena, u32, n + 1);
    u32 *by_genre  = core_push_array(&lib->arena, u32, n + 1);
    lib->by_title  = core_push_array(&lib->arena, u32, n + 1);
    if (!key || !tmp || !by_artist || !by_genre || !lib->by_title) { core_temp_end(temp); return false; }
    Lib_SortCtx ctx = { .tracks = lib->tracks, .key = key };

    for (u32 i = 0; i < n; i++) { lib->by_title[i] = i; key[i] = lib_sort_key(lib->tracks[i].title); }
    lib_sort(lib->by_title, tmp, n, lib_cmp_by_title, &ctx);

    for (u32 i = 0; i < n; i++) { by_artist[i] = i; key[i] = lib_sort_key(lib->tracks[i].artist); }
    lib_sort(by_artist, tmp, n, lib_cmp_by_artist, &ctx);

    /* Runs of case-insensitively equal artists become artists; within a
       run the tracks are already sorted by title. */
    u32 count = 0;
    for (u32 i = 0; i < n; i++)
        if (i == 0 || core_str_cmp_ascii_ci(lib->tracks[by_artist[i]].artist, lib->tracks[by_artist[i - 1]].artist)) count++;
    lib->artists = core_push_array(&lib->arena, Lib_Artist, count + 1);
    if (!lib->artists) { core_temp_end(temp); return false; }
    lib->artist_count = 0;
    for (u32 i = 0; i < n; i++) {
        Lib_Track *t = &lib->tracks[by_artist[i]];
        if (i == 0 || core_str_cmp_ascii_ci(t->artist, lib->tracks[by_artist[i - 1]].artist)) {
            Lib_Artist *a = &lib->artists[lib->artist_count++];
            a->name   = t->artist;
            a->tracks = by_artist + i;
        }
        lib->artists[lib->artist_count - 1].track_count++;
        t->artist_index = lib->artist_count - 1;
    }

    /* Genres: same grouping, ordered genre > artist > title. */
    for (u32 i = 0; i < n; i++) { by_genre[i] = i; key[i] = lib_sort_key(lib->tracks[i].genre); }
    lib_sort(by_genre, tmp, n, lib_cmp_by_genre, &ctx);
    count = 0;
    for (u32 i = 0; i < n; i++)
        if (i == 0 || core_str_cmp_ascii_ci(lib->tracks[by_genre[i]].genre, lib->tracks[by_genre[i - 1]].genre)) count++;
    lib->genres = core_push_array(&lib->arena, Lib_Genre, count + 1);
    if (!lib->genres) { core_temp_end(temp); return false; }
    lib->genre_count = 0;
    for (u32 i = 0; i < n; i++) {
        Lib_Track *t = &lib->tracks[by_genre[i]];
        if (i == 0 || core_str_cmp_ascii_ci(t->genre, lib->tracks[by_genre[i - 1]].genre)) {
            Lib_Genre *g = &lib->genres[lib->genre_count++];
            g->name   = t->genre;
            g->tracks = by_genre + i;
        }
        lib->genres[lib->genre_count - 1].track_count++;
        t->genre_index = lib->genre_count - 1;
    }

    u32 cap = 16;
    while (cap < n * 2) cap *= 2;
    lib->hash_cap   = cap;
    lib->hash_slots = core_push_array(&lib->arena, u32, cap);
    if (!lib->hash_slots) { core_temp_end(temp); return false; }
    for (u32 i = 0; i < n; i++) {
        u32 s = (u32)lib->tracks[i].path_hash & (cap - 1);
        while (lib->hash_slots[s]) s = (s + 1) & (cap - 1);
        lib->hash_slots[s] = i + 1;
    }
    core_temp_end(temp);
    return true;
}

/* ---- binary cache ---- */

#define LIB_CACHE_MAGIC   0x4249424Cu /* "LBIB" */
#define LIB_CACHE_VERSION 2u

typedef struct {
    u32 magic;
    u32 version;
    u32 track_size;      /* sizeof(Lib_Track): layout guard */
    u32 artist_size;     /* sizeof(Lib_Artist)              */
    u32 track_count;
    u32 artist_count;
    u32 genre_count;
    u32 hash_cap;
    u64 file_size;
    u64 tracks_off, by_title_off, artists_off, artist_tracks_off, hash_off;
    u64 genres_off, genre_tracks_off;
    u64 strings_off, strings_size;
    u64 root_off, root_len;
} Lib_CacheHeader;

/* String blob with deduplication by (pointer, length): interned strings
   share storage in the snapshot, so equal pointers mean equal strings. */
typedef struct {
    Core_Arena *bytes;   /* contiguous blob (nothing else pushed here) */
    u8  *base;
    u64 *keys;           /* ptr ^ len*prime, 0 = empty */
    const u8 **ptrs;
    u64 *lens;
    u64 *offs;
    u64  cap;
} Lib_Blob;

static u64 lib_blob_put(Lib_Blob *b, Core_String s) {
    u64 k = ((u64)(uintptr_t)s.str ^ (s.len * 0x9E3779B97F4A7C15ull)) | 1;
    u64 i = (k * 0x9E3779B97F4A7C15ull) >> 20;
    for (;; i++) {
        i &= b->cap - 1;
        if (!b->keys[i]) break;
        if (b->keys[i] == k && b->ptrs[i] == s.str && b->lens[i] == s.len) return b->offs[i];
    }
    u64 off = b->bytes->used;
    u8 *dst = core_arena_push(b->bytes, s.len + 1, 1);
    if (!b->base) b->base = dst;
    if (s.len) memcpy(dst, s.str, s.len);
    b->keys[i] = k; b->ptrs[i] = s.str; b->lens[i] = s.len; b->offs[i] = off;
    return off;
}

static Core_String lib_str_as_off(u64 off, u64 len) {
    return (Core_String){ .str = (u8 *)(uintptr_t)off, .len = len };
}

static b32 lib_write_cache(const Library *lib, const char *cache_path) {
    Core_Arena scratch = {0}, bytes = {0};
    if (!core_arena_init(&scratch, CORE_GB(4))) return false;
    if (!core_arena_init(&bytes, CORE_GB(4))) { core_arena_release(&scratch); return false; }

    u32 n = lib->track_count;
    Lib_Blob blob = { .bytes = &bytes };
    blob.cap = 64;
    while (blob.cap < (u64)n * 8 + 8) blob.cap *= 2;
    blob.keys = core_push_array(&scratch, u64, blob.cap);
    blob.ptrs = core_push_array(&scratch, const u8 *, blob.cap);
    blob.lens = core_push_array(&scratch, u64, blob.cap);
    blob.offs = core_push_array(&scratch, u64, blob.cap);
    b32 ok = blob.keys && blob.ptrs && blob.lens && blob.offs;

    Lib_CacheHeader h = {0};
    h.magic = LIB_CACHE_MAGIC;
    h.version = LIB_CACHE_VERSION;
    h.track_size = sizeof(Lib_Track);
    h.artist_size = sizeof(Lib_Artist);
    h.track_count = n;
    h.artist_count = lib->artist_count;
    h.genre_count = lib->genre_count;
    h.hash_cap = lib->hash_cap;

    u64 at = CORE_ALIGN_UP(sizeof(Lib_CacheHeader), 16);
    h.tracks_off        = at; at = CORE_ALIGN_UP(at + (u64)n * sizeof(Lib_Track), 16);
    h.by_title_off      = at; at = CORE_ALIGN_UP(at + (u64)n * 4, 16);
    h.artists_off       = at; at = CORE_ALIGN_UP(at + (u64)lib->artist_count * sizeof(Lib_Artist), 16);
    h.artist_tracks_off = at; at = CORE_ALIGN_UP(at + (u64)n * 4, 16);
    h.genres_off        = at; at = CORE_ALIGN_UP(at + (u64)lib->genre_count * sizeof(Lib_Genre), 16);
    h.genre_tracks_off  = at; at = CORE_ALIGN_UP(at + (u64)n * 4, 16);
    h.hash_off          = at; at = CORE_ALIGN_UP(at + (u64)lib->hash_cap * 4, 16);
    h.strings_off       = at;

    u8 *file = ok ? core_arena_push(&scratch, at, 16) : 0;
    ok = ok && file;
    if (ok) {
        Lib_Track *tr = (Lib_Track *)(file + h.tracks_off);
        for (u32 i = 0; i < n; i++) {
            const Lib_Track *s = &lib->tracks[i];
            tr[i] = *s;
            tr[i].path   = lib_str_as_off(lib_blob_put(&blob, s->path),   s->path.len);
            tr[i].title  = lib_str_as_off(lib_blob_put(&blob, s->title),  s->title.len);
            tr[i].artist = lib_str_as_off(lib_blob_put(&blob, s->artist), s->artist.len);
            tr[i].album  = lib_str_as_off(lib_blob_put(&blob, s->album),  s->album.len);
            tr[i].genre  = lib_str_as_off(lib_blob_put(&blob, s->genre),  s->genre.len);
        }
        /* Artists' track lists are slices of one array (see build_indices);
           store them as offsets into artist_tracks. */
        u32 *at_out = (u32 *)(file + h.artist_tracks_off);
        Lib_Artist *ar = (Lib_Artist *)(file + h.artists_off);
        u32 filled = 0;
        for (u32 i = 0; i < lib->artist_count; i++) {
            const Lib_Artist *s = &lib->artists[i];
            ar[i].name = lib_str_as_off(lib_blob_put(&blob, s->name), s->name.len);
            ar[i].tracks = (u32 *)(uintptr_t)filled;
            ar[i].track_count = s->track_count;
            if (filled + s->track_count > n) { ok = false; break; }
            memcpy(at_out + filled, s->tracks, (u64)s->track_count * 4);
            filled += s->track_count;
        }
        u32 *gt_out = (u32 *)(file + h.genre_tracks_off);
        Lib_Genre *ge = (Lib_Genre *)(file + h.genres_off);
        filled = 0;
        for (u32 i = 0; ok && i < lib->genre_count; i++) {
            const Lib_Genre *s = &lib->genres[i];
            ge[i].name = lib_str_as_off(lib_blob_put(&blob, s->name), s->name.len);
            ge[i].tracks = (u32 *)(uintptr_t)filled;
            ge[i].track_count = s->track_count;
            if (filled + s->track_count > n) { ok = false; break; }
            memcpy(gt_out + filled, s->tracks, (u64)s->track_count * 4);
            filled += s->track_count;
        }
        if (n) memcpy(file + h.by_title_off, lib->by_title, (u64)n * 4);
        memcpy(file + h.hash_off, lib->hash_slots, (u64)lib->hash_cap * 4);
        h.root_off = lib_blob_put(&blob, lib->music_dir);
        h.root_len = lib->music_dir.len;
        h.strings_size = bytes.used;
        h.file_size = h.strings_off + h.strings_size;
        memcpy(file, &h, sizeof(h));
    }

    if (ok) {
        /* Append the blob right after the fixed sections (the arena is
           contiguous: nothing else was pushed after `file`). */
        u8 *tail = core_arena_push(&scratch, h.strings_size, 1);
        ok = tail == file + h.strings_off || (tail && h.strings_size == 0);
        if (ok && h.strings_size) memcpy(tail, blob.base, h.strings_size);
    }
    if (ok) {
        char dir[4096];
        u64 len = strlen(cache_path);
        if (len < sizeof(dir)) {
            memcpy(dir, cache_path, len + 1);
            while (len > 0 && dir[len - 1] != '/') len--;
            if (len > 1) { dir[len - 1] = 0; platform_make_dirs(dir); }
        }
        ok = platform_file_write_all(cache_path, file, h.file_size);
    }
    core_arena_release(&bytes);
    core_arena_release(&scratch);
    return ok;
}

static b32 lib_fix_str(Core_String *s, u8 *blob, u64 blob_size) {
    u64 off = (u64)(uintptr_t)s->str;
    if (off >= blob_size || s->len >= blob_size - off || blob[off + s->len] != 0) return false;
    s->str = blob + off;
    return true;
}

static b32 lib_section_ok(const Lib_CacheHeader *h, u64 off, u64 count, u64 elem) {
    if (off % 8 || off > h->file_size) return false;
    if (elem && count > (h->file_size - off) / elem) return false;
    return true;
}

Library *library_load_cache(const char *cache_path) {
    Library *lib = lib_create();
    if (!lib) return 0;
    Core_String data = platform_file_read_all(&lib->arena, cache_path);
    const Lib_CacheHeader *h = (const Lib_CacheHeader *)data.str;
    u8 *base = data.str;

    b32 ok = data.len >= sizeof(Lib_CacheHeader) && h->magic == LIB_CACHE_MAGIC &&
             h->version == LIB_CACHE_VERSION && h->track_size == sizeof(Lib_Track) &&
             h->artist_size == sizeof(Lib_Artist) && h->file_size == data.len &&
             h->hash_cap >= 16 && (h->hash_cap & (h->hash_cap - 1)) == 0 &&
             h->hash_cap >= h->track_count &&
             lib_section_ok(h, h->tracks_off, h->track_count, sizeof(Lib_Track)) &&
             lib_section_ok(h, h->by_title_off, h->track_count, 4) &&
             lib_section_ok(h, h->artists_off, h->artist_count, sizeof(Lib_Artist)) &&
             lib_section_ok(h, h->artist_tracks_off, h->track_count, 4) &&
             lib_section_ok(h, h->genres_off, h->genre_count, sizeof(Lib_Genre)) &&
             lib_section_ok(h, h->genre_tracks_off, h->track_count, 4) &&
             lib_section_ok(h, h->hash_off, h->hash_cap, 4) &&
             lib_section_ok(h, h->strings_off, h->strings_size, 1) &&
             h->strings_off + h->strings_size == h->file_size;
    if (!ok) { library_free(lib); return 0; }

    u32 n = h->track_count, na = h->artist_count, ng = h->genre_count;
    u8 *blob = base + h->strings_off;
    u64 blob_size = h->strings_size;
    lib->tracks       = (Lib_Track *)(base + h->tracks_off);
    lib->by_title     = (u32 *)(base + h->by_title_off);
    lib->artists      = (Lib_Artist *)(base + h->artists_off);
    lib->hash_slots   = (u32 *)(base + h->hash_off);
    lib->track_count  = n;
    lib->artist_count = na;
    lib->genres       = (Lib_Genre *)(base + h->genres_off);
    lib->genre_count  = ng;
    lib->hash_cap     = h->hash_cap;
    lib->music_dir    = lib_str_as_off(h->root_off, h->root_len);
    ok = lib_fix_str(&lib->music_dir, blob, blob_size);

    /* Pointer fix-up + validation of every index, so a damaged cache can
       never make the UI read out of bounds. */
    for (u32 i = 0; ok && i < n; i++) {
        Lib_Track *t = &lib->tracks[i];
        ok = lib_fix_str(&t->path, blob, blob_size) && lib_fix_str(&t->title, blob, blob_size) &&
             lib_fix_str(&t->artist, blob, blob_size) && lib_fix_str(&t->album, blob, blob_size) &&
             lib_fix_str(&t->genre, blob, blob_size) && t->artist_index < na && t->genre_index < ng &&
             lib->by_title[i] < n;
    }
    u32 *artist_tracks = (u32 *)(base + h->artist_tracks_off);
    u64 used = 0;
    for (u32 i = 0; ok && i < na; i++) {
        Lib_Artist *a = &lib->artists[i];
        u64 first = (u64)(uintptr_t)a->tracks;
        ok = lib_fix_str(&a->name, blob, blob_size) && first == used && a->track_count <= n - used;
        a->tracks = artist_tracks + first;
        used += a->track_count;
    }
    ok = ok && used == n;
    for (u32 i = 0; ok && i < n; i++) ok = artist_tracks[i] < n;
    u32 *genre_tracks = (u32 *)(base + h->genre_tracks_off);
    used = 0;
    for (u32 i = 0; ok && i < ng; i++) {
        Lib_Genre *g = &lib->genres[i];
        u64 first = (u64)(uintptr_t)g->tracks;
        ok = lib_fix_str(&g->name, blob, blob_size) && first == used && g->track_count <= n - used;
        g->tracks = genre_tracks + first;
        used += g->track_count;
    }
    ok = ok && used == n;
    for (u32 i = 0; ok && i < n; i++) ok = genre_tracks[i] < n;
    for (u32 i = 0; ok && i < h->hash_cap; i++) ok = lib->hash_slots[i] <= n;
    if (!ok) { library_free(lib); return 0; }
    return lib;
}

/* ---- scanner ---- */

struct Lib_Scanner {
    Platform_Thread *thread;
    char            *music_dir;
    char            *cache_path;
    const Library   *previous;
    Library         *result;       /* written by the thread before `done` */
    b32              changed;
    b32              delivered;    /* UI side */
    _Atomic u32      files_seen;
    _Atomic u32      done;
    _Atomic u32      cancel;
};

typedef struct {
    Core_String path;   /* NUL-terminated, in the paths arena */
    u64 size;
    s64 mtime_ns;
    u64 hash;
} Lib_ScanFile;

typedef struct {
    Lib_Scanner  *s;
    Core_Arena   *files;  /* holds only Lib_ScanFile records: a contiguous array */
    Core_Arena   *paths;
    Lib_ScanFile *first;
    u32           count;
} Lib_WalkCtx;

static b32 lib_walk_visit(void *user, const char *path, u64 size, s64 mtime_ns) {
    Lib_WalkCtx *w = user;
    if (atomic_load_explicit(&w->s->cancel, memory_order_relaxed)) return false;
    Core_String p = core_str(path);
    if (!lib_ext_is(p, ".mp3") && !lib_ext_is(p, ".flac") && !lib_ext_is(p, ".ogg")) return true;
    Lib_ScanFile *f = core_push_struct(w->files, Lib_ScanFile);
    if (!f) return false;
    if (!w->first) w->first = f;
    f->path     = core_str_copy(w->paths, p);
    f->size     = size;
    f->mtime_ns = mtime_ns;
    f->hash     = library_hash_path(p);
    w->count++;
    atomic_fetch_add_explicit(&w->s->files_seen, 1, memory_order_relaxed);
    return true;
}

static s32 lib_cmp_scan_path(const void *ctx, u32 a, u32 b) {
    const Lib_ScanFile *f = ctx;
    return core_str_cmp(f[a].path, f[b].path);
}

/* Interns repeated strings (artist/album/genre) into the snapshot arena. */
typedef struct {
    Core_Arena  *dst;
    Core_String *slots;
    u64         *hashes;
    u64          cap;
    u64          count;
} Lib_Intern;

static Core_String lib_intern(Lib_Intern *in, Core_String s) {
    if (in->count * 2 >= in->cap) return core_str_copy(in->dst, s); /* table full: just copy */
    u64 h = library_hash_path(s) | 1;
    for (u64 i = h & (in->cap - 1);; i = (i + 1) & (in->cap - 1)) {
        if (!in->hashes[i]) {
            in->hashes[i] = h;
            in->slots[i] = core_str_copy(in->dst, s);
            in->count++;
            return in->slots[i];
        }
        if (in->hashes[i] == h && core_str_eq(in->slots[i], s)) return in->slots[i];
    }
}

static Library *lib_scan_build(Lib_Scanner *s, b32 *out_changed) {
    Core_Arena files = {0}, paths = {0}, scratch = {0}, tags = {0};
    core_arena_init(&files, CORE_GB(4));
    core_arena_init(&paths, CORE_GB(4));
    core_arena_init(&scratch, CORE_GB(4));
    core_arena_init(&tags, CORE_GB(1));
    Library *lib = 0;

    Lib_WalkCtx w = { .s = s, .files = &files, .paths = &paths };
    platform_walk_dir(s->music_dir, lib_walk_visit, &w);
    if (atomic_load(&s->cancel)) goto done;

    u32 n = w.count;
    Lib_ScanFile *fs = w.first;
    u32 *order = core_push_array(&scratch, u32, n + 1);
    u32 *tmp   = core_push_array(&scratch, u32, n + 1);
    if (!order || !tmp) goto done;
    for (u32 i = 0; i < n; i++) order[i] = i;
    lib_sort(order, tmp, n, lib_cmp_scan_path, fs);

    lib = lib_create();
    if (!lib) goto done;
    lib->tracks = core_push_array(&lib->arena, Lib_Track, n + 1);
    lib->music_dir = core_str_copy(&lib->arena, core_str(s->music_dir));
    Lib_Intern in = { .dst = &lib->arena, .cap = 64 };
    while (in.cap < (u64)n * 4 + 16) in.cap *= 2;
    in.slots  = core_push_array(&scratch, Core_String, in.cap);
    in.hashes = core_push_array(&scratch, u64, in.cap);
    if (!lib->tracks || !in.slots || !in.hashes) { library_free(lib); lib = 0; goto done; }

    const Library *prev = s->previous;
    u32 count = 0, reused = 0, parsed = 0;
    for (u32 k = 0; k < n; k++) {
        if (atomic_load_explicit(&s->cancel, memory_order_relaxed)) { library_free(lib); lib = 0; goto done; }
        const Lib_ScanFile *f = &fs[order[k]];
        Lib_Track *t = &lib->tracks[count];
        s64 pi = prev ? library_find(prev, f->hash) : -1;
        const Lib_Track *pt = pi >= 0 ? &prev->tracks[pi] : 0;
        if (pt && pt->file_size == f->size && pt->mtime_ns == f->mtime_ns && core_str_eq(pt->path, f->path)) {
            *t = *pt; /* unchanged: no file access at all */
            t->title  = core_str_copy(&lib->arena, pt->title);
            t->artist = lib_intern(&in, pt->artist);
            t->album  = lib_intern(&in, pt->album);
            reused++;
        } else {
            core_arena_reset(&tags);
            Lib_TagInfo info;
            if (!library_read_tags(&tags, (const char *)f->path.str, &info)) continue;
            t->title        = core_str_copy(&lib->arena, info.title);
            t->artist       = lib_intern(&in, info.artist);
            t->album        = lib_intern(&in, info.album);
            t->cover_offset = info.cover_offset;
            t->cover_size   = info.cover_size;
            t->cover_kind   = info.cover_size ? LIB_COVER_EMBEDDED : LIB_COVER_NONE;
            t->duration_ms  = info.duration_ms;
            t->format       = info.format;
            parsed++;
        }
        t->path      = core_str_copy(&lib->arena, f->path);
        t->genre     = lib_intern(&in, lib_genre_from_path(f->path, lib->music_dir));
        t->path_hash = f->hash;
        t->file_size = f->size;
        t->mtime_ns  = f->mtime_ns;
        count++;
    }
    lib->track_count = count;
    if (!lib_build_indices(lib, &scratch)) { library_free(lib); lib = 0; goto done; }

    b32 changed = !prev || parsed > 0 || reused != prev->track_count ||
                  !core_str_eq(prev->music_dir, lib->music_dir);
    *out_changed = changed;
    if (changed || !platform_file_info(s->cache_path).exists) lib_write_cache(lib, s->cache_path);

done:
    core_arena_release(&tags);
    core_arena_release(&scratch);
    core_arena_release(&paths);
    core_arena_release(&files);
    return lib;
}

static void lib_scan_thread(void *user) {
    Lib_Scanner *s = user;
    platform_thread_set_background();
    b32 changed = true;
    s->result  = lib_scan_build(s, &changed);
    s->changed = changed;
    atomic_store_explicit(&s->done, 1, memory_order_release);
}

Lib_Scanner *library_scan_start(const char *music_dir, const char *cache_path, const Library *previous) {
    Lib_Scanner *s = core_heap_calloc(sizeof(Lib_Scanner));
    if (!s) return 0;
    s->music_dir  = core_str_heap_cstr(core_str(music_dir));
    s->cache_path = core_str_heap_cstr(core_str(cache_path));
    s->previous   = previous;
    if (s->music_dir && s->cache_path) s->thread = platform_thread_start(lib_scan_thread, s, "lib-scan");
    if (!s->thread) atomic_store(&s->done, 1); /* nothing to wait for */
    return s;
}

Library *library_scanner_poll(Lib_Scanner *s, b32 *changed) {
    if (changed) *changed = false;
    if (!s || s->delivered || !atomic_load_explicit(&s->done, memory_order_acquire)) return 0;
    s->delivered = true;
    if (changed) *changed = s->changed;
    return s->result;
}

b32 library_scanner_busy(Lib_Scanner *s) {
    return s && !atomic_load_explicit(&s->done, memory_order_acquire);
}

u32 library_scanner_files_seen(Lib_Scanner *s) {
    return s ? atomic_load_explicit(&s->files_seen, memory_order_relaxed) : 0;
}

void library_scanner_destroy(Lib_Scanner *s) {
    if (!s) return;
    atomic_store(&s->cancel, 1);
    platform_thread_join(s->thread);
    if (!s->delivered && s->result) library_free(s->result);
    core_heap_free(s->music_dir);
    core_heap_free(s->cache_path);
    core_heap_free(s);
}
