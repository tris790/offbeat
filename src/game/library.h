#ifndef GAME_LIBRARY_H
#define GAME_LIBRARY_H

#include "../core/types.h"
#include "../core/memory.h"
#include "../core/string.h"

/*
 * Music library: an immutable snapshot of tracks + derived indices.
 *
 * A Library is built once (from the on-disk index cache, or by a scan) and is
 * never mutated afterwards, so the UI can read it without locks. Rescans build
 * a NEW Library on a background thread; the UI swaps it in when ready and
 * frees the old one. Everything a Library owns lives in its own arena.
 *
 * Startup is instant: library_load_cache() reads a compact binary index in one
 * read. A background scan then walks the music dir, re-parses only files whose
 * size/mtime changed, and writes a fresh cache.
 *
 * Scales to very large collections: tracks are ~100 bytes + strings, lookups by
 * path hash are O(1), sorting happens once per snapshot.
 */

enum {
    LIB_FORMAT_UNKNOWN = 0,
    LIB_FORMAT_MP3,
    LIB_FORMAT_FLAC,
    LIB_FORMAT_OGG,
};

enum {
    LIB_COVER_NONE = 0,
    LIB_COVER_EMBEDDED, /* bytes [cover_offset, +cover_size) of the audio file */
};

/* All strings of a snapshot are NUL-terminated (len excludes the NUL), so
   `path.str` can be handed straight to file APIs. */
typedef struct {
    Core_String path;       /* absolute, UTF-8                                */
    Core_String title;      /* tag title, else file name without extension    */
    Core_String artist;     /* tag artist, else "Unknown Artist"              */
    Core_String album;      /* may be empty                                   */
    Core_String genre;      /* top-level folder under the music dir, else
                               "Unsorted" (the tag genre is ignored)          */
    u64 path_hash;          /* stable id (FNV-1a 64 of path)                  */
    u64 file_size;
    s64 mtime_ns;
    u32 cover_offset;       /* embedded picture bytes (file offset) ...       */
    u32 cover_size;         /* ... and length; 0 = no cover                   */
    u32 duration_ms;        /* 0 = unknown (from TLEN / Xing / stream info)   */
    u32 artist_index;       /* index into Library.artists                     */
    u32 genre_index;        /* index into Library.genres                      */
    u8  format;             /* LIB_FORMAT_*                                   */
    u8  cover_kind;         /* LIB_COVER_*                                    */
} Lib_Track;

typedef struct {
    Core_String name;
    u32 *tracks;            /* track indices, sorted by title                 */
    u32  track_count;
} Lib_Artist;

typedef struct {
    Core_String name;
    u32 *tracks;            /* track indices, sorted by artist then title     */
    u32  track_count;
} Lib_Genre;

typedef struct Library {
    Core_Arena  arena;          /* owns every allocation of this snapshot     */
    Lib_Track  *tracks;
    u32         track_count;
    u32        *by_title;       /* all track indices sorted by title (ci)     */
    Lib_Artist *artists;        /* sorted by name (case-insensitive)          */
    u32         artist_count;
    Lib_Genre  *genres;         /* sorted by name (case-insensitive)          */
    u32         genre_count;
    u32        *hash_slots;     /* open-addressing table: track index + 1     */
    u32         hash_cap;       /* power of two                               */
    Core_String music_dir;      /* root the snapshot was scanned from         */
} Library;

/* Load the binary index cache. Returns 0 if missing/corrupt/outdated. */
Library *library_load_cache(const char *cache_path);
void     library_free(Library *lib);

/* Track index for a path hash, or -1. */
s64      library_find(const Library *lib, u64 path_hash);

u64      library_hash_path(Core_String path);

/* ---- background scanning ---- */

typedef struct Lib_Scanner Lib_Scanner;

/* Start scanning `music_dir` on a background thread. `previous` (may be 0) is
   used read-only to skip re-parsing unchanged files; it must stay alive until
   the scanner finishes (library_scanner_poll returned the result or
   library_scanner_busy is false). The finished library is written to
   `cache_path`. */
Lib_Scanner *library_scan_start(const char *music_dir, const char *cache_path,
                                const Library *previous);
/* Returns the new Library exactly once when the scan has finished (0 before
   and after). `changed` tells whether it differs from `previous`. */
Library     *library_scanner_poll(Lib_Scanner *s, b32 *changed);
b32          library_scanner_busy(Lib_Scanner *s);
u32          library_scanner_files_seen(Lib_Scanner *s);
void         library_scanner_destroy(Lib_Scanner *s); /* joins the thread */

/* ---- tag parsing (exposed for tests) ---- */

typedef struct {
    Core_String title, artist, album, genre; /* point into `arena` */
    u32 cover_offset, cover_size;
    u32 duration_ms;
    u8  format;
} Lib_TagInfo;

/* Parse metadata of one audio file. Reads only headers (never the audio).
   Returns false if the file is not a supported audio file. */
b32 library_read_tags(Core_Arena *arena, const char *path, Lib_TagInfo *out);

#endif /* GAME_LIBRARY_H */
