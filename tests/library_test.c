/*
 * Library + covers test. Standalone: includes the app sources it needs.
 *
 *  1. Tag parsing on synthetic files (ID3v2.2/2.3/2.4, unsync, UTF-16,
 *     ID3v1, Xing/CBR durations, FLAC, Ogg Vorbis).
 *  2. The real library (~/Music or $OFFBEAT_MUSIC): cold + warm scan, cache
 *     write/load timing, sample tracks, cover signature check.
 *  3. Covers worker path with a stubbed renderer: thumbnail generation, disk
 *     cache hits, now-playing art + palette, memory peak.
 *
 * Everything is written under /tmp (never ~/.cache).
 */

#include "core/memory.c"
#include "core/image.c"
#include "core/string.c"
#include "platform/platform_posix.c"
#include "game/library.c"
#include "game/covers.c"

#include <stdio.h>
#include <stdlib.h>

/* ---- renderer stub: the covers module only needs these four ----
   Uploads land in CPU mirrors so the test can inspect what the GPU would
   get (plain static memory: stands in for VRAM, not app memory). */

static u8  g_atlas_mirror[1024 * 1024 * 4];
static u8  g_art_mirror[2][COVER_ART_SIZE * COVER_ART_SIZE * 4];
static u32 g_tex_next = 1, g_tex_uploads, g_mip_calls;

Core_Texture core_texture_create(Core_Renderer *r, u32 format, u32 w, u32 h, const void *pixels, b32 mipmapped) {
    CORE_UNUSED(r); CORE_UNUSED(pixels);
    return (Core_Texture){ .id = g_tex_next++, .width = w, .height = h, .format = format, .mip_levels = mipmapped ? 10 : 1 };
}

void core_texture_update(Core_Renderer *r, Core_Texture tex, u32 x, u32 y, u32 w, u32 h, const void *pixels) {
    CORE_UNUSED(r);
    g_tex_uploads++;
    u8 *dst = tex.width == 1024 ? g_atlas_mirror : tex.id == 2 ? g_art_mirror[0] : g_art_mirror[1];
    for (u32 row = 0; row < h; row++)
        memcpy(dst + ((u64)(y + row) * tex.width + x) * 4, (const u8 *)pixels + (u64)row * w * 4, (u64)w * 4);
}

void core_texture_generate_mips(Core_Renderer *r, Core_Texture tex) { CORE_UNUSED(r); CORE_UNUSED(tex); g_mip_calls++; }
void core_texture_destroy(Core_Renderer *r, Core_Texture tex) { CORE_UNUSED(r); CORE_UNUSED(tex); }

static int g_failures;

int stbi_write_png(char const *filename, int w, int h, int comp, const void *data, int stride_in_bytes);

#define CHECK(cond) do { \
    if (!(cond)) { fprintf(stderr, "library test failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); g_failures++; } \
} while (0)

static b32 str_is(Core_String s, const char *lit) { return core_str_eq(s, core_str(lit)); }

static f64 ms_since(f64 t0) { return (platform_time_seconds() - t0) * 1000.0; }

/* ---- synthetic file builder ---- */

typedef struct { u8 data[65536]; u32 len; } Buf;

static void put(Buf *b, const void *p, u32 n) { memcpy(b->data + b->len, p, n); b->len += n; }
static void put_s(Buf *b, const char *s) { put(b, s, (u32)strlen(s)); }
static void put_u8(Buf *b, u32 v) { b->data[b->len++] = (u8)v; }
static void put_be32(Buf *b, u32 v) { put_u8(b, v >> 24); put_u8(b, v >> 16); put_u8(b, v >> 8); put_u8(b, v); }
static void put_le32(Buf *b, u32 v) { put_u8(b, v); put_u8(b, v >> 8); put_u8(b, v >> 16); put_u8(b, v >> 24); }
static void put_ss(Buf *b, u32 v) { put_u8(b, (v >> 21) & 0x7F); put_u8(b, (v >> 14) & 0x7F); put_u8(b, (v >> 7) & 0x7F); put_u8(b, v & 0x7F); }

static void id3_frame(Buf *b, u32 ver, const char *id, const void *body, u32 n) {
    put_s(b, id);
    if (ver == 4) put_ss(b, n); else put_be32(b, n);
    put_u8(b, 0); put_u8(b, 0);
    put(b, body, n);
}

/* MPEG1 layer III 128 kbps 44.1 kHz frames of 417 bytes; the first may
   carry a Xing header with a frame count. */
static void mpeg_frames(Buf *b, u32 count, u32 xing_frames) {
    for (u32 i = 0; i < count; i++) {
        u32 start = b->len;
        put_u8(b, 0xFF); put_u8(b, 0xFB); put_u8(b, 0x90); put_u8(b, 0x64);
        memset(b->data + b->len, 0, 413);
        if (i == 0 && xing_frames) {
            memcpy(b->data + start + 36, "Xing", 4);
            u8 *p = b->data + start + 40;
            p[3] = 1;
            p[4] = (u8)(xing_frames >> 24); p[5] = (u8)(xing_frames >> 16);
            p[6] = (u8)(xing_frames >> 8);  p[7] = (u8)xing_frames;
        }
        b->len = start + 417;
    }
}

static void write_buf(const char *path, Buf *b) { CHECK(platform_file_write_all(path, b->data, b->len)); }

static void ogg_page(Buf *b, u32 type, u64 granule, u32 seq, const u8 *packet, u32 n) {
    put_s(b, "OggS"); put_u8(b, 0); put_u8(b, type);
    put_le32(b, (u32)granule); put_le32(b, (u32)(granule >> 32));
    put_le32(b, 0x1234); put_le32(b, seq); put_le32(b, 0); /* crc unchecked */
    u32 segs = n / 255 + 1;
    put_u8(b, segs);
    for (u32 i = 0; i < segs; i++) put_u8(b, i + 1 < segs ? 255 : n % 255);
    put(b, packet, n);
}

static void test_synthetic(void) {
    const char *dir = "/tmp/offbeat_test/synth";
    platform_make_dirs(dir);
    Core_Arena a = {0};
    core_arena_init(&a, CORE_MB(64));
    char path[512];
    Lib_TagInfo t;
    static Buf b, tag;

    /* ID3v2.4: UTF-16 title with BOM, multi-value UTF-8 artist, numeric
       genre, TLEN, APIC (JPEG signature), Xing header. */
    tag.len = 0;
    {
        Buf f = {0};
        u8 bom16[] = { 1, 0xFF, 0xFE, 'R', 0, 0xDC, 0, 'F', 0, 0xDC, 0, 'S', 0, ' ', 0, 0x6D, 0x30, 0, 0 };
        id3_frame(&tag, 4, "TIT2", bom16, sizeof(bom16));
        put_u8(&f, 3); put_s(&f, "Alpha"); put_u8(&f, 0); put_s(&f, "Beta"); put_u8(&f, 0);
        id3_frame(&tag, 4, "TPE1", f.data, f.len);
        f.len = 0; put_u8(&f, 0); put_s(&f, "(17)");
        id3_frame(&tag, 4, "TCON", f.data, f.len);
        f.len = 0; put_u8(&f, 0); put_s(&f, "185000");
        id3_frame(&tag, 4, "TLEN", f.data, f.len);
        f.len = 0; put_u8(&f, 0); put_s(&f, "image/jpeg"); put_u8(&f, 0); put_u8(&f, 3);
        put_s(&f, "cover"); put_u8(&f, 0);
        u8 jpg[] = { 0xFF, 0xD8, 0xFF, 0xE0, 1, 2, 3, 4, 5, 6 };
        put(&f, jpg, sizeof(jpg));
        id3_frame(&tag, 4, "APIC", f.data, f.len);
    }
    b.len = 0;
    put_s(&b, "ID3"); put_u8(&b, 4); put_u8(&b, 0); put_u8(&b, 0); put_ss(&b, tag.len + 64);
    put(&b, tag.data, tag.len);
    memset(b.data + b.len, 0, 64); b.len += 64; /* padding */
    u32 tag_end = b.len;
    mpeg_frames(&b, 10, 1000);
    snprintf(path, sizeof(path), "%s/v24.mp3", dir);
    write_buf(path, &b);
    CHECK(library_read_tags(&a, path, &t));
    CHECK(t.format == LIB_FORMAT_MP3);
    CHECK(str_is(t.title, "R\xC3\x9C" "F\xC3\x9C" "S \xE3\x81\xAD"));
    CHECK(str_is(t.artist, "Alpha, Beta"));
    CHECK(str_is(t.genre, "Rock"));
    CHECK(t.duration_ms == 185000);
    CHECK(t.cover_size == 10 && b.data[t.cover_offset] == 0xFF && b.data[t.cover_offset + 1] == 0xD8);

    /* Same tag without TLEN -> Xing frame count: 1000 * 1152 / 44100 s. */
    b.len = 0;
    put_s(&b, "ID3"); put_u8(&b, 3); put_u8(&b, 0); put_u8(&b, 0); put_ss(&b, 15);
    { u8 body[] = { 0, 'X', 'i', 'n', 'g' }; id3_frame(&b, 3, "TIT2", body, sizeof(body)); }
    mpeg_frames(&b, 10, 1000);
    snprintf(path, sizeof(path), "%s/xing.mp3", dir);
    write_buf(path, &b);
    CHECK(library_read_tags(&a, path, &t));
    CHECK(str_is(t.title, "Xing"));
    CHECK(t.duration_ms == 26122);
    CHECK(str_is(t.artist, "Unknown Artist"));
    CHECK(str_is(t.genre, "synth"));

    /* ID3v2.3 with tag-level unsynchronisation (0xFF 0x00 in the title). */
    b.len = 0;
    {
        u8 body[] = { 0, 'a', 0xFF, 0x00, 'b' };     /* on disk (unsynced) */
        put_s(&b, "ID3"); put_u8(&b, 3); put_u8(&b, 0); put_u8(&b, 0x80); put_ss(&b, 10 + sizeof(body));
        put_s(&b, "TIT2"); put_be32(&b, 4); put_u8(&b, 0); put_u8(&b, 0);
        put(&b, body, sizeof(body));
    }
    mpeg_frames(&b, 20, 0);
    snprintf(path, sizeof(path), "%s/unsync.mp3", dir);
    write_buf(path, &b);
    CHECK(library_read_tags(&a, path, &t));
    CHECK(str_is(t.title, "a\xC3\xBF" "b"));
    CHECK(t.duration_ms == 20 * 417 * 8 / 128); /* CBR estimate */

    /* ID3v2.2 + ID3v1 fallback for the album. */
    b.len = 0;
    {
        Buf f = {0};
        put_s(&f, "TT2"); put_u8(&f, 0); put_u8(&f, 0); put_u8(&f, 7); put_u8(&f, 0); put_s(&f, "Old v2");
        put_s(&f, "TP1"); put_u8(&f, 0); put_u8(&f, 0); put_u8(&f, 5); put_u8(&f, 0); put_s(&f, "Band");
        u8 pic[] = { 0, 'P', 'N', 'G', 3, 0, 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n', 9, 9 };
        put_s(&f, "PIC"); put_u8(&f, 0); put_u8(&f, 0); put_u8(&f, sizeof(pic)); put(&f, pic, sizeof(pic));
        put_s(&b, "ID3"); put_u8(&b, 2); put_u8(&b, 0); put_u8(&b, 0); put_ss(&b, f.len);
        put(&b, f.data, f.len);
    }
    mpeg_frames(&b, 5, 0);
    {
        u8 v1[128] = { 'T', 'A', 'G' };
        memcpy(v1 + 3, "v1 title", 8);
        memcpy(v1 + 63, "v1 album", 8);
        v1[127] = 8;
        put(&b, v1, 128);
    }
    snprintf(path, sizeof(path), "%s/v22.mp3", dir);
    write_buf(path, &b);
    CHECK(library_read_tags(&a, path, &t));
    CHECK(str_is(t.title, "Old v2"));
    CHECK(str_is(t.artist, "Band"));
    CHECK(str_is(t.album, "v1 album"));
    CHECK(str_is(t.genre, "Jazz"));
    CHECK(t.cover_size == 10 && b.data[t.cover_offset] == 0x89);

    /* ID3v1 only. */
    b.len = 0;
    mpeg_frames(&b, 5, 0);
    {
        u8 v1[128] = { 'T', 'A', 'G' };
        memcpy(v1 + 3, "Caf\xE9", 4);
        memcpy(v1 + 33, "Only V1", 7);
        v1[127] = 255;
        put(&b, v1, 128);
    }
    snprintf(path, sizeof(path), "%s/Only V1 File.mp3", dir);
    write_buf(path, &b);
    CHECK(library_read_tags(&a, path, &t));
    CHECK(str_is(t.title, "Caf\xC3\xA9"));
    CHECK(str_is(t.artist, "Only V1"));

    /* No tags at all: fallbacks. */
    b.len = 0;
    mpeg_frames(&b, 5, 0);
    snprintf(path, sizeof(path), "%s/Bare Name.mp3", dir);
    write_buf(path, &b);
    CHECK(library_read_tags(&a, path, &t));
    CHECK(str_is(t.title, "Bare Name"));
    CHECK(str_is(t.artist, "Unknown Artist"));
    CHECK(str_is(t.genre, "synth"));
    CHECK(t.cover_size == 0);

    /* FLAC: STREAMINFO + VORBIS_COMMENT + PICTURE. */
    b.len = 0;
    put_s(&b, "fLaC");
    put_u8(&b, 0); put_u8(&b, 0); put_u8(&b, 0); put_u8(&b, 34);
    {
        u8 si[34] = {0};
        u32 sr = 44100; u64 total = 44100ull * 10;
        si[10] = (u8)(sr >> 12); si[11] = (u8)(sr >> 4); si[12] = (u8)((sr & 0xF) << 4) | (1 << 1);
        si[13] = (u8)(0x70 | ((total >> 32) & 0x0F));
        si[14] = (u8)(total >> 24); si[15] = (u8)(total >> 16); si[16] = (u8)(total >> 8); si[17] = (u8)total;
        put(&b, si, 34);
    }
    {
        Buf c = {0};
        put_le32(&c, 3); put_s(&c, "enc"); put_le32(&c, 3);
        put_le32(&c, 16); put_s(&c, "TITLE=Flac Title");
        put_le32(&c, 18); put_s(&c, "artist=Flac Artist");
        put_le32(&c, 15); put_s(&c, "Genre=Ambient  ");
        put_u8(&b, 4); put_u8(&b, 0); put_u8(&b, 0); put_u8(&b, c.len);
        put(&b, c.data, c.len);
    }
    u32 pic_data = 0;
    {
        Buf p = {0};
        put_be32(&p, 3); put_be32(&p, 9); put_s(&p, "image/png"); put_be32(&p, 0);
        put_be32(&p, 1); put_be32(&p, 1); put_be32(&p, 24); put_be32(&p, 0);
        put_be32(&p, 12);
        u32 before = p.len;
        put(&p, "\x89PNG\r\n\x1a\n1234", 12);
        put_u8(&b, 0x80 | 6); put_u8(&b, 0); put_u8(&b, 0); put_u8(&b, p.len);
        pic_data = b.len + before;
        put(&b, p.data, p.len);
    }
    snprintf(path, sizeof(path), "%s/a.flac", dir);
    write_buf(path, &b);
    CHECK(library_read_tags(&a, path, &t));
    CHECK(t.format == LIB_FORMAT_FLAC);
    CHECK(str_is(t.title, "Flac Title"));
    CHECK(str_is(t.artist, "Flac Artist"));
    CHECK(str_is(t.genre, "Ambient"));
    CHECK(t.duration_ms == 10000);
    CHECK(t.cover_offset == pic_data && t.cover_size == 12);

    /* Ogg Vorbis: id header, comment header, last page granule. */
    b.len = 0;
    {
        u8 id[30] = { 1, 'v', 'o', 'r', 'b', 'i', 's' };
        id[11] = 2;
        id[12] = 0x44; id[13] = 0xAC; /* 44100 */
        id[29] = 1;
        ogg_page(&b, 2, 0, 0, id, 30);
        Buf c = {0};
        put_u8(&c, 3); put_s(&c, "vorbis");
        put_le32(&c, 3); put_s(&c, "enc"); put_le32(&c, 2);
        put_le32(&c, 22); put_s(&c, "TITLE=Ogg \xE6\x97\xA5\xE6\x9C\xAC Title");
        put_le32(&c, 14); put_s(&c, "ALBUM=Ogg Alb.");
        put_u8(&c, 1);
        ogg_page(&b, 0, 0, 1, c.data, c.len);
        u8 audio[10] = {0};
        ogg_page(&b, 4, 44100 * 5 + 22050, 2, audio, 10);
    }
    snprintf(path, sizeof(path), "%s/b.ogg", dir);
    write_buf(path, &b);
    CHECK(library_read_tags(&a, path, &t));
    CHECK(t.format == LIB_FORMAT_OGG);
    CHECK(str_is(t.title, "Ogg \xE6\x97\xA5\xE6\x9C\xAC Title"));
    CHECK(str_is(t.album, "Ogg Alb."));
    CHECK(t.duration_ms == 5500);

    /* Not audio. */
    snprintf(path, sizeof(path), "%s/notes.txt", dir);
    CHECK(platform_file_write_all(path, "hello", 5));
    CHECK(!library_read_tags(&a, path, &t));
    CORE_UNUSED(tag_end);

    core_arena_release(&a);
    printf("synthetic tag tests done\n");
}

/* ---- real library ---- */

static Library *scan_blocking(const char *music, const char *cache, const Library *prev, b32 *changed, f64 *ms) {
    f64 t0 = platform_time_seconds();
    Lib_Scanner *s = library_scan_start(music, cache, prev);
    Library *lib = 0;
    while (!(lib = library_scanner_poll(s, changed))) {
        if (!library_scanner_busy(s)) break;
        platform_sleep(0.001);
    }
    *ms = ms_since(t0);
    library_scanner_destroy(s);
    return lib;
}

static const char *fmt_dur(u32 ms, char *buf) {
    snprintf(buf, 16, "%u:%02u", ms / 60000, (ms / 1000) % 60);
    return buf;
}

static Library *test_real_library(const char *music, const char *cache) {
    b32 changed = false;
    f64 cold_ms, warm_ms;
    remove(cache);

    Library *lib = scan_blocking(music, cache, 0, &changed, &cold_ms);
    CHECK(lib && changed);
    if (!lib) return 0;
    printf("scan (no previous, parses every file): %u tracks, %u artists in %.1f ms\n",
           lib->track_count, lib->artist_count, cold_ms);

    Library *lib2 = scan_blocking(music, cache, lib, &changed, &warm_ms);
    CHECK(lib2 && !changed);
    printf("scan (warm, previous snapshot, no file opens): %.1f ms, changed=%d\n", warm_ms, changed);
    if (lib2) library_free(lib2);

    f64 t0 = platform_time_seconds();
    Library *cached = library_load_cache(cache);
    f64 load_ms = ms_since(t0);
    CHECK(cached != 0);
    Platform_FileInfo fi = platform_file_info(cache);
    printf("cache: %llu bytes, load %.3f ms\n", (unsigned long long)fi.size, load_ms);
    if (cached) {
        CHECK(cached->track_count == lib->track_count);
        CHECK(cached->artist_count == lib->artist_count);
        CHECK(cached->genre_count == lib->genre_count);
        CHECK(core_str_eq(cached->music_dir, core_str(music)));
        for (u32 i = 0; i < lib->track_count; i++) {
            const Lib_Track *x = &lib->tracks[i], *y = &cached->tracks[i];
            if (!core_str_eq(x->path, y->path) || !core_str_eq(x->title, y->title) ||
                !core_str_eq(x->artist, y->artist) || x->cover_offset != y->cover_offset ||
                x->duration_ms != y->duration_ms || lib->by_title[i] != cached->by_title[i] ||
                y->path.str[y->path.len] != 0) { CHECK(!"cache mismatch"); break; }
            CHECK(library_find(cached, y->path_hash) == (s64)i);
        }
        /* Warm scan against the cache-loaded snapshot (the startup path). */
        Library *lib3 = scan_blocking(music, cache, cached, &changed, &warm_ms);
        CHECK(lib3 && !changed);
        printf("scan (warm, previous = cache-loaded): %.1f ms\n", warm_ms);
        if (lib3) library_free(lib3);
        library_free(cached);
    }

    /* Corrupt cache must be rejected, not crash. */
    {
        Core_Arena a = {0};
        core_arena_init(&a, CORE_GB(1));
        Core_String data = platform_file_read_all(&a, cache);
        char bad[600];
        snprintf(bad, sizeof(bad), "%s.bad", cache);
        if (data.len > 1000) {
            data.str[data.len / 2] ^= 0x5A;
            data.str[data.len / 3] ^= 0xFF;
            platform_file_write_all(bad, data.str, data.len / 2 + 7);
            CHECK(library_load_cache(bad) == 0);
        }
        core_arena_release(&a);
    }

    /* Sorted order sanity + artist grouping. */
    for (u32 i = 1; i < lib->track_count; i++)
        CHECK(core_str_cmp_ascii_ci(lib->tracks[lib->by_title[i - 1]].title, lib->tracks[lib->by_title[i]].title) <= 0);
    u32 sum = 0;
    for (u32 i = 0; i < lib->artist_count; i++) {
        sum += lib->artists[i].track_count;
        if (i) CHECK(core_str_cmp_ascii_ci(lib->artists[i - 1].name, lib->artists[i].name) < 0);
    }
    CHECK(sum == lib->track_count);

    /* Genres = top-level folder under the music dir; slices cover every track once. */
    sum = 0;
    for (u32 i = 0; i < lib->genre_count; i++) {
        const Lib_Genre *g = &lib->genres[i];
        sum += g->track_count;
        if (i) CHECK(core_str_cmp_ascii_ci(lib->genres[i - 1].name, g->name) < 0);
        for (u32 k = 0; k < g->track_count; k++) {
            const Lib_Track *t = &lib->tracks[g->tracks[k]];
            CHECK(t->genre_index == i);
            CHECK(core_str_eq(t->genre, g->name));
            if (!core_str_eq(g->name, core_str_lit("Unsorted"))) {
                Core_String pre = core_str_substr(t->path, lib->music_dir.len + 1, g->name.len);
                CHECK(core_str_eq(pre, g->name) && t->path.str[lib->music_dir.len + 1 + g->name.len] == '/');
            }
        }
    }
    CHECK(sum == lib->track_count);
    printf("genres: %u\n", lib->genre_count);

    /* Cover offsets point at image signatures; count unknown durations. */
    u32 covers = 0, bad_cover = 0, no_dur = 0, non_ascii = 0;
    for (u32 i = 0; i < lib->track_count; i++) {
        const Lib_Track *t = &lib->tracks[i];
        if (!t->duration_ms) no_dur++;
        for (u64 k = 0; k < t->title.len; k++) if (t->title.str[k] >= 0x80) { non_ascii++; break; }
        if (!t->cover_size) continue;
        covers++;
        Platform_File f = platform_file_open_read((const char *)t->path.str);
        u8 sig[8] = {0};
        platform_file_read_at(f, t->cover_offset, sig, 8);
        platform_file_close(f);
        if (!lib_image_signature(sig, 8)) bad_cover++;
    }
    printf("covers: %u/%u embedded, %u bad signatures; %u without duration; %u non-ASCII titles\n",
           covers, lib->track_count, bad_cover, no_dur, non_ascii);
    CHECK(bad_cover == 0);

    /* A few samples, including non-ASCII ones. */
    char d[16];
    u32 shown = 0;
    for (u32 i = 0; i < lib->track_count && shown < 4; i += lib->track_count / 4 + 1, shown++) {
        const Lib_Track *t = &lib->tracks[lib->by_title[i]];
        printf("  %-40.*s | %-24.*s | %-12.*s | %s | cover %u B\n", (int)t->title.len, t->title.str,
               (int)t->artist.len, t->artist.str, (int)t->genre.len, t->genre.str,
               fmt_dur(t->duration_ms, d), t->cover_size);
    }
    shown = 0;
    for (u32 i = 0; i < lib->track_count && shown < 4; i++) {
        const Lib_Track *t = &lib->tracks[i];
        b32 wide = false;
        for (u64 k = 0; k < t->title.len + t->artist.len; k++) {
            u8 c = k < t->title.len ? t->title.str[k] : t->artist.str[k - t->title.len];
            if (c >= 0x80) { wide = true; break; }
        }
        if (!wide) continue;
        shown++;
        printf("  %.*s | %.*s | %s\n", (int)t->title.len, t->title.str,
               (int)t->artist.len, t->artist.str, fmt_dur(t->duration_ms, d));
    }
    return lib;
}

/* ---- covers ---- */

static b32 rm_visit(void *user, const char *path, u64 size, s64 mtime_ns) {
    CORE_UNUSED(user); CORE_UNUSED(size); CORE_UNUSED(mtime_ns);
    remove(path);
    return true;
}

/* Simulate frames showing `count` rows starting at `first` (title order)
   until all are resolved. Returns ms, or -1 on timeout. */
static f64 show_until_ready(Covers *c, const Library *lib, u32 first, u32 count) {
    f64 t0 = platform_time_seconds();
    for (u32 frame = 0; frame < 20000; frame++) {
        u32 done = 0;
        for (u32 i = 0; i < count; i++) {
            Cover_Thumb th = covers_thumb(c, &lib->tracks[lib->by_title[(first + i) % lib->track_count]]);
            done += th.ready || th.missing;
        }
        if (done == count) return ms_since(t0);
        covers_update(c, 1.0f / 240);
        platform_sleep(0.001);
    }
    return -1;
}

static void dump_atlas_rows(Covers *c, const Library *lib, u32 first, u32 count, const char *png) {
    /* Gather the visible thumbnails into one strip for eyeballing. */
    static u8 strip[COVER_THUMB_SIZE * 24 * COVER_THUMB_SIZE * 4];
    count = CORE_MIN(count, 24u);
    memset(strip, 0, sizeof(strip));
    u32 nonzero = 0;
    for (u32 i = 0; i < count; i++) {
        Cover_Thumb th = covers_thumb(c, &lib->tracks[lib->by_title[first + i]]);
        if (!th.ready) continue;
        u32 sx = (u32)(th.uv0.x * 1024), sy = (u32)(th.uv0.y * 1024);
        CHECK(sx % 64 == 0 && sy % 64 == 0);
        CHECK(th.uv1.x > th.uv0.x && th.uv1.x * 1024 < sx + 64);
        for (u32 y = 0; y < 64; y++) {
            memcpy(strip + ((u64)y * 64 * count + i * 64) * 4, g_atlas_mirror + ((u64)(sy + y) * 1024 + sx) * 4, 64 * 4);
            for (u32 x = 0; x < 64 * 4; x++) nonzero += g_atlas_mirror[((u64)(sy + y) * 1024 + sx) * 4 + x] != 0;
        }
    }
    CHECK(nonzero > 0);
    stbi_write_png(png, 64 * count, 64, 4, strip, 64 * count * 4);
}

static void test_covers(const Library *lib) {
    const char *cache_dir = "/tmp/offbeat_test/cache";
    platform_make_dirs("/tmp/offbeat_test/cache/thumbs");
    platform_walk_dir("/tmp/offbeat_test/cache/thumbs", rm_visit, 0);
    u64 heap_before = core_mem_stats().heap_live;

    Covers *c = covers_create(0, cache_dir, 2);
    CHECK(c != 0);
    u32 first = 0, rows = 24;

    /* 1. cold: decode embedded art, downscale, write the disk cache */
    f64 cold = show_until_ready(c, lib, first, rows);
    CHECK(cold >= 0);
    u64 gen = c->stat_generated;
    printf("thumbs cold: %u rows ready in %.1f ms (2 workers); per cover %.2f ms worker time (%llu generated)\n",
           rows, cold, gen ? c->stat_generate_s * 1000.0 / gen : 0.0, (unsigned long long)gen);
    dump_atlas_rows(c, lib, first, rows, "/tmp/offbeat_test/thumbs_cold.png");
    Covers_Stats st = covers_stats(c);
    CHECK(st.resident == rows && st.capacity == 256);

    /* 2. warm: fresh instance, everything from the disk cache */
    covers_destroy(c);
    c = covers_create(0, cache_dir, 2);
    f64 warm = show_until_ready(c, lib, first, rows);
    CHECK(warm >= 0);
    u64 hits = c->stat_cache_hits;
    printf("thumbs from disk cache: %u rows ready in %.1f ms; per cover %.3f ms (%llu hits, %llu generated)\n",
           rows, warm, hits ? c->stat_cache_hit_s * 1000.0 / hits : 0.0,
           (unsigned long long)hits, (unsigned long long)c->stat_generated);
    CHECK(hits == rows && c->stat_generated == 0);
    dump_atlas_rows(c, lib, first, rows, "/tmp/offbeat_test/thumbs_warm.png");

    /* 3. now-playing art: cold decode, crossfade pair, cache hit */
    const Lib_Track *ta = &lib->tracks[lib->by_title[0]], *tb = &lib->tracks[lib->by_title[1]];
    f64 t0 = platform_time_seconds();
    covers_set_art(c, ta);
    while (!covers_art(c).ready && !covers_art(c).missing && ms_since(t0) < 5000) { covers_update(c, 1.0f / 240); platform_sleep(0.0005); }
    Cover_Art art = covers_art(c);
    CHECK(art.ready && art.track_hash == ta->path_hash && art.tex.mip_levels > 1);
    printf("art 512 cold: %.1f ms; palette:", ms_since(t0));
    for (u32 i = 0; i < 4; i++)
        printf(" #%02x%02x%02x", (int)(art.palette[i].r * 255), (int)(art.palette[i].g * 255), (int)(art.palette[i].b * 255));
    printf("\n");
    stbi_write_png("/tmp/offbeat_test/art.png", COVER_ART_SIZE, COVER_ART_SIZE, 4, g_art_mirror[art.tex.id == 2 ? 0 : 1], COVER_ART_SIZE * 4);

    covers_set_art(c, tb);
    CHECK(covers_art_prev(c).track_hash == ta->path_hash && covers_art_prev(c).ready);
    CHECK(!covers_art(c).ready);
    t0 = platform_time_seconds();
    while (!covers_art(c).ready && ms_since(t0) < 5000) { covers_update(c, 1.0f / 240); platform_sleep(0.0005); }
    CHECK(covers_art(c).ready && covers_art(c).tex.id != covers_art_prev(c).tex.id);
    covers_set_art(c, ta); /* back to A: from the _512 disk cache */
    t0 = platform_time_seconds();
    while (!covers_art(c).ready && ms_since(t0) < 5000) { covers_update(c, 1.0f / 240); platform_sleep(0.0002); }
    printf("art 512 from disk cache: %.2f ms (incl. frame polling)\n", ms_since(t0));
    CHECK(covers_art(c).ready && covers_art(c).track_hash == ta->path_hash);
    CHECK(covers_art_prev(c).track_hash == tb->path_hash);

    /* 4. fast scrolling across 600 uncached rows: stale requests must be
       dropped, residency bounded, and the final screen still resolves. */
    u32 max_pending = 0;
    t0 = platform_time_seconds();
    u32 pos = 100;
    for (u32 frame = 0; frame < 200; frame++, pos += 3) {
        for (u32 i = 0; i < rows; i++) covers_thumb(c, &lib->tracks[lib->by_title[(pos + i) % lib->track_count]]);
        covers_update(c, 1.0f / 240);
        st = covers_stats(c);
        max_pending = CORE_MAX(max_pending, st.pending);
        CHECK(st.resident <= st.capacity);
        platform_sleep(1.0 / 240);
    }
    f64 settle = show_until_ready(c, lib, pos, rows);
    CHECK(settle >= 0);
    st = covers_stats(c);
    printf("scroll 600 rows @240fps: max pending %u, resident %u/%u, final screen settled in %.1f ms\n",
           max_pending, st.resident, st.capacity, settle);
    CHECK(max_pending <= COV_REQ_CAP + COV_DONE_CAP + 4);

    /* 5. LRU: touching > 256 cached thumbnails keeps residency at capacity */
    for (u32 k = 0; k < 12; k++) show_until_ready(c, lib, 100 + k * rows, rows);
    st = covers_stats(c);
    printf("after LRU churn: resident %u/%u\n", st.resident, st.capacity);
    CHECK(st.resident <= 256);
    CHECK(show_until_ready(c, lib, first, rows) >= 0);

    /* 6. background prefetch over a slice of the library */
    Library sub = *lib;
    sub.track_count = CORE_MIN(lib->track_count, 900u);
    t0 = platform_time_seconds();
    covers_prefetch_library(c, &sub);
    u32 start_left = covers_stats(c).prefetch_left;
    while (covers_stats(c).prefetch_left && ms_since(t0) < 60000) { covers_update(c, 1.0f / 60); platform_sleep(0.005); }
    printf("prefetch %u tracks (one worker, most already cached): %.0f ms, generated total %llu\n",
           start_left, ms_since(t0), (unsigned long long)c->stat_generated);
    CHECK(covers_stats(c).prefetch_left == 0);

    Core_MemStats ms = core_mem_stats();
    printf("covers heap: live %.2f MB (instance), peak %.2f MB\n", (ms.heap_live - heap_before) / 1048576.0, ms.heap_peak / 1048576.0);
    CHECK(ms.heap_peak < CORE_MB(24));
    covers_destroy(c);
    CHECK(core_mem_stats().heap_live == heap_before);
}

int main(void) {
    platform_make_dirs("/tmp/offbeat_test");
    test_synthetic();

    const char *music = platform_env("OFFBEAT_MUSIC");
    char music_buf[512];
    if (!music) {
        const char *home = platform_env("HOME");
        snprintf(music_buf, sizeof(music_buf), "%s/Music", home ? home : "/tmp");
        music = music_buf;
    }
    Library *lib = 0;
    if (platform_file_info(music).is_dir) lib = test_real_library(music, "/tmp/offbeat_test/library.bin");
    else printf("no music dir at %s, skipping real-library tests\n", music);

    if (lib && lib->track_count >= 1000) test_covers(lib);
    if (lib) library_free(lib);
    Core_MemStats ms = core_mem_stats();
    printf("memory: heap live %llu B, heap peak %.2f MB, arena committed %.2f MB\n",
           (unsigned long long)ms.heap_live, ms.heap_peak / 1048576.0, ms.arena_committed / 1048576.0);
    if (g_failures) { fprintf(stderr, "%d failure(s)\n", g_failures); return 1; }
    printf("library test passed\n");
    return 0;
}
