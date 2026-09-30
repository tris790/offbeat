#ifndef GAME_DOWNLOAD_H
#define GAME_DOWNLOAD_H

#include "../core/types.h"
#include "../core/string.h"

/*
 * Song downloader: searches YouTube through yt-dlp and turns picked results
 * into tagged MP3s (title, artist, album, year, genre, track, source, cover).
 *
 * Nothing here touches the UI thread's time. A search thread runs the lookup
 * (one at a time, a new search cancels the old one) and streams results into
 * a list the UI polls; a few worker threads each run one job at a time:
 *
 *   yt-dlp  -> <work>/<id>.mp3 + <id>.jpg        (audio + cover)
 *   ffmpeg  -> <folder>/<Title>.mp3              (all tags, cover attached)
 *
 * <folder> is part of the library: the folder the library already keeps that
 * artist in (<music>/<Genre>/<Artist>), else a genre folder matching YouTube's
 * genre tag, else <dest>/<Artist> (the "new artists" folder). A song the
 * library already has (same artist, same title modulo "(2011 Remaster)" and
 * the like) is never fetched twice.
 *
 * Jobs survive closing the app: the queue is written to a small text file on
 * every change, a job that was running comes back as queued, and yt-dlp keeps
 * its partial download in the per-job work folder, so it resumes where it
 * stopped. Failures retry with a growing delay before they are given up on.
 *
 * Both tools are found in PATH (OFFBEAT_YTDLP / OFFBEAT_FFMPEG override);
 * downloads_tools() tells the UI which are missing so it can say so.
 *
 * All public functions are thread-safe. The pure text helpers at the bottom
 * are exposed for tests.
 */

#define DL_TEXT       160
#define DL_MAX_RESULTS 128

typedef enum { DL_QUEUED = 0, DL_ACTIVE, DL_DONE, DL_FAILED } Dl_State;
/* What an ACTIVE job is doing right now. */
typedef enum { DL_PHASE_START = 0, DL_PHASE_FETCH, DL_PHASE_CONVERT, DL_PHASE_TAG } Dl_Phase;

typedef struct {
    u64  uid;                   /* stable, unique, persisted                 */
    char vid[16];               /* YouTube video id                          */
    char title[DL_TEXT];        /* from the search result, then from yt-dlp  */
    char artist[DL_TEXT];       /* primary artist (folder + artist tag)      */
    char album[DL_TEXT];
    char genre[64];
    u32  year, track, duration_s;
    u8   state;                 /* Dl_State                                  */
    u8   phase;                 /* Dl_Phase, while ACTIVE                    */
    u8   attempts;              /* failed attempts so far                    */
    u8   existed;               /* DONE because the file was already there   */
    f32  progress;              /* 0..1 over fetch + convert + tag           */
    char error[DL_TEXT];        /* last failure                              */
    char path[1024];            /* final file, once DONE                     */
} Dl_Job;

/* ---- tools ---- */

typedef struct {
    b32  ytdlp, ffmpeg;         /* found and runnable                        */
    char ytdlp_path[512];
} Dl_Tools;

/* ---- search ---- */

typedef enum { DL_QUERY_SONG = 0, DL_QUERY_ARTIST } Dl_QueryKind;
typedef enum { DL_SEARCH_IDLE = 0, DL_SEARCH_BUSY, DL_SEARCH_DONE, DL_SEARCH_FAILED } Dl_SearchState;

typedef struct {
    char vid[16];
    char title[DL_TEXT];
    char channel[DL_TEXT];      /* uploader; empty for artist lookups        */
    u32  duration_s;            /* 0 = unknown                               */
    u64  views;                 /* 0 = unknown                               */
    u8   music;                 /* a YouTube Music song: real album/year tags */
    char artist[DL_TEXT];       /* the song's artist, once known (see below) */
} Dl_Result;

typedef struct {
    u32  state;                 /* Dl_SearchState                            */
    u32  kind;                  /* Dl_QueryKind                              */
    u32  count;                 /* results so far                            */
    u32  generation;            /* bumps with every new search               */
    char query[256];
    char error[DL_TEXT];
} Dl_SearchInfo;

typedef struct {
    u32 total;                  /* jobs in the list                          */
    u32 queued, active, done, failed;
    u32 session_done;           /* finished since the app started (done counts older ones too) */
    f32 progress;               /* 0..1 over the current batch               */
    u32 batch_total, batch_done;
    b32 paused;
    u32 version;                /* bumps whenever a job is added/changes state */
} Dl_Summary;

typedef struct Downloads Downloads;

/* `state_path`: where the queue is persisted. `work_dir`: scratch space for
   in-flight downloads (kept across runs so partial files can resume). Loads
   the saved queue and starts the threads. */
Downloads *downloads_create(const char *state_path, const char *work_dir);
/* Stops running downloads (they resume next time), saves, joins, frees. */
void       downloads_destroy(Downloads *d);

/* Where songs of artists the library doesn't have yet go (created on demand)
   and how many download at once. */
void       downloads_configure(Downloads *d, const char *dest_dir, u32 parallel);
/* The library's root: finished songs are filed into its artist folders. */
void       downloads_set_library(Downloads *d, const char *music_dir);

/* Look for yt-dlp and ffmpeg again (after the user installed them). */
void       downloads_check_tools(Downloads *d);
Dl_Tools   downloads_tools(Downloads *d);

/* Start a search, cancelling any running one. Artist searches list the
   artist's top songs (up to 100); song searches list the best matching
   YouTube Music songs (clean studio versions with full tags) followed by
   YouTube video results (live versions, remixes, lyric videos, ...). The
   same song released twice ("Hypnotize" / "Hypnotize (2007 Remaster)") is
   listed once. YouTube Music hits come without an artist: it is looked up
   right after and filled in (`Dl_Result.artist`, and the real title), which
   may also drop a hit that turned out to be a duplicate. */
void          downloads_search(Downloads *d, u32 kind, const char *query);
void          downloads_search_cancel(Downloads *d);
Dl_SearchInfo downloads_search_info(Downloads *d);
b32           downloads_result(Downloads *d, u32 index, Dl_Result *out);

/* Queue results for download. `artist_hint` (may be 0) names the artist the
   list was searched for; it is used until yt-dlp reports the real one. Skips
   songs already queued, running, or downloaded. Returns how many were added. */
u32        downloads_enqueue(Downloads *d, const Dl_Result *results, u32 count, const char *artist_hint);

/* Cheap per-frame view of the whole list (no strings), same order as
   downloads_job(). Returns the number written. */
typedef struct { u64 uid; u8 state, phase, attempts; f32 progress; } Dl_Brief;
u32        downloads_briefs(Downloads *d, Dl_Brief *out, u32 max);

Dl_Summary downloads_summary(Downloads *d);
u32        downloads_job_count(Downloads *d);
b32        downloads_job(Downloads *d, u32 index, Dl_Job *out);
/* Is this video queued, running or already downloaded? (for result rows) */
b32        downloads_has(Downloads *d, const char *vid, Dl_State *state);

void       downloads_set_paused(Downloads *d, b32 paused);
void       downloads_retry(Downloads *d, u64 uid);        /* failed -> queued      */
void       downloads_retry_failed(Downloads *d);
void       downloads_remove(Downloads *d, u64 uid);       /* cancels if running    */
void       downloads_clear_finished(Downloads *d);        /* drops done + failed   */
void       downloads_cancel_all(Downloads *d);            /* drops queued, stops running */
/* Number of songs finished since the last call (the app rescans its library). */
u32        downloads_take_finished(Downloads *d);

/* ---- text helpers (exposed for tests) ---- */

/* Replace control characters and the field separators with spaces and trim. */
void dl_clean_field(char *s);
/* "Artist feat. X & Y" -> "Artist" (collaborations group under the first). */
void dl_primary_artist(const char *artist, char *out, u32 cap);
/* "Name - Topic" / "NameVEVO" -> "Name". */
void dl_clean_channel(const char *channel, char *out, u32 cap);
/* Video title -> song title: drops a leading "Artist - " and trailing
   "(Official Video)"-style tags. */
void dl_clean_title(const char *title, const char *artist, char *out, u32 cap);
/* Comparison key of a song title: lowercase letters and digits, without the
   tags that only describe a release ("(2011 Remaster)", "[Explicit]",
   "(feat. X)", " - Single Version", "(Official Video)") but keeping the ones
   that make a different song ("(Live)", "(Acoustic)", "(Remix)"). */
void dl_song_key(const char *title, char *out, u32 cap);
/* Comparison key of an artist: primary artist, lowercase letters and digits,
   no leading "the" ("The Notorious B.I.G." == "notorious big"). */
void dl_artist_key(const char *artist, char *out, u32 cap);
/* Could `lib_artist` be the artist of a result? `known` is what is known about
   it (may be empty), `raw_title` the result's title (YouTube titles often say
   "Artist - Song"). */
b32  dl_artist_plausible(const char *lib_artist, const char *known, const char *raw_title);
/* One path component: no separators/control characters, never empty, not "."
   or "..". */
void dl_path_segment(const char *in, char *out, u32 cap);
/* "<dest>/<Artist>/<Title>.mp3" */
void dl_dest_path(const char *dest_dir, const char *artist, const char *title, char *out, u32 cap);

/* Parse one line printed by the search / download commands. */
b32  dl_parse_result_line(const char *line, Dl_Result *out);
/* "OBP <downloaded> <total> <estimate>" -> fraction 0..1, or false if unknown. */
b32  dl_parse_progress_line(const char *line, f32 *fraction);
/* The lookup line for a YouTube Music hit: sets vid, artist, title, duration. */
b32  dl_parse_enrich_line(const char *line, Dl_Result *out);
/* yt-dlp's metadata line for a job; fills title/artist/album/genre/year/... */
b32  dl_parse_meta_line(const char *line, Dl_Job *job);

/* Queue persistence. `parse` returns the number of jobs read (running jobs
   come back queued). */
u64  dl_jobs_format(const Dl_Job *jobs, u32 count, u64 next_uid, char *buf, u64 cap);
u32  dl_jobs_parse(Core_String text, Dl_Job *out, u32 max, u64 *next_uid);

#endif /* GAME_DOWNLOAD_H */
