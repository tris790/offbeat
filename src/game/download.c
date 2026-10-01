/*
 * Song downloader: see download.h for the overview.
 *
 * Locking: one mutex guards the job list, the search results and the tool
 * paths. Threads only hold it for short bookkeeping (never while waiting on a
 * child process), so the UI thread, which takes it once per frame to read,
 * never stalls behind a download.
 */

#include "download.h"

#include "../platform/platform.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DL_MAX_WORKERS   3
#define DL_MAX_ATTEMPTS  3
#define DL_ENRICH_MAX    6    /* YouTube Music hits looked up at once           */
#define DL_KEEP_DONE     300  /* finished jobs remembered in the list          */
#define DL_STALL_SECONDS 300.0 /* a child silent this long is considered hung   */

#define US "\x1f" /* field separator in the lines yt-dlp prints for us */

/* ------------------------------------------------------------------------- */
/* text helpers                                                              */
/* ------------------------------------------------------------------------- */

static void copy_to(char *dst, u32 cap, const char *src) {
    if (!cap) return;
    u32 n = (u32)strlen(src);
    if (n >= cap) {
        n = cap - 1;
        while (n > 0 && ((u8)src[n] & 0xC0) == 0x80) n--; /* never cut a UTF-8 sequence */
    }
    memcpy(dst, src, n);
    dst[n] = 0;
}

static b32 ci_prefix(const char *s, const char *prefix) {
    for (; *prefix; s++, prefix++)
        if (tolower((u8)*s) != tolower((u8)*prefix)) return false;
    return true;
}

static b32 ci_suffix(const char *s, const char *suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && ci_prefix(s + n - m, suffix);
}

static void trim(char *s) {
    size_t n = strlen(s), a = 0;
    while (n && (s[n - 1] == ' ')) s[--n] = 0;
    while (s[a] == ' ') a++;
    if (a) memmove(s, s + a, n - a + 1);
}

void dl_clean_field(char *s) {
    for (u8 *p = (u8 *)s; *p; p++)
        if (*p < 0x20 || *p == 0x7f) *p = ' ';
    trim(s);
}

/* yt-dlp prints "NA" for a missing field. */
static const char *na(const char *s) { return strcmp(s, "NA") == 0 ? "" : s; }

static u32 split_fields(char *line, char sep, char **out, u32 max) {
    u32 n = 0;
    char *p = line;
    while (n < max) {
        out[n++] = p;
        char *e = strchr(p, sep);
        if (!e) break;
        *e = 0;
        p = e + 1;
    }
    return n;
}

/* Does a collaboration separator start at `t`? Returns its length, else 0.
   Matches the rules of the user's artist normalizer: "feat."/"ft."/"vs"/
   "and"/"with"/"presents" between spaces, "," "&" "×", " x ", " / ", " + ". */
static u32 separator_at(const char *t) {
    static const char *const words[] = { "feat.", "feat", "ft.", "ft", "vs.", "vs", "and", "with", "presents", "present" };
    if (*t == ' ') {
        const char *w = t;
        while (*w == ' ') w++;
        for (u32 i = 0; i < CORE_ARRAY_COUNT(words); i++) {
            size_t n = strlen(words[i]);
            if (ci_prefix(w, words[i]) && (w[n] == ' ' || w[n] == ':')) {
                const char *e = w + n;
                if (*e == ':') e++;
                while (*e == ' ') e++;
                if (e > w + n || w[n] == ' ') return (u32)(e - t);
            }
        }
        if ((w[0] == 'x' || w[0] == 'X' || w[0] == '/' || w[0] == '+') && w[1] == ' ' && w > t) {
            const char *e = w + 1;
            while (*e == ' ') e++;
            return (u32)(e - t);
        }
    }
    const char *w = t;
    while (*w == ' ') w++;
    if (*w == ',' || *w == '&') {
        const char *e = w + 1;
        while (*e == ' ') e++;
        return (u32)(e - t);
    }
    if ((u8)w[0] == 0xC3 && (u8)w[1] == 0x97) { /* × */
        const char *e = w + 2;
        while (*e == ' ') e++;
        return (u32)(e - t);
    }
    return 0;
}

void dl_primary_artist(const char *artist, char *out, u32 cap) {
    char tmp[DL_TEXT];
    copy_to(tmp, sizeof(tmp), artist);
    trim(tmp);
    size_t n = strlen(tmp);

    /* "Project (A + B)" / "Project [A x B]": the bracketed credit is not part of the name */
    if (n && (tmp[n - 1] == ')' || tmp[n - 1] == ']')) {
        char open = tmp[n - 1] == ')' ? '(' : '[';
        char *o = strrchr(tmp, open);
        if (o && o > tmp && o[-1] == ' ') {
            b32 collab = false;
            for (char *q = o + 1; *q && !collab; q++)
                collab = *q == '+' || *q == ',' || *q == '&' || separator_at(q) != 0;
            if (collab) { o[-1] = 0; trim(tmp); }
        }
    }
    u32 depth = 0;
    for (size_t i = 0; tmp[i]; i++) {
        if (tmp[i] == '(' || tmp[i] == '[') depth++;
        else if ((tmp[i] == ')' || tmp[i] == ']') && depth) depth--;
        if (!depth && i > 0 && separator_at(tmp + i)) { tmp[i] = 0; break; }
    }
    trim(tmp);
    copy_to(out, cap, tmp[0] ? tmp : artist);
}

void dl_clean_channel(const char *channel, char *out, u32 cap) {
    char tmp[DL_TEXT];
    copy_to(tmp, sizeof(tmp), channel);
    trim(tmp);
    if (ci_suffix(tmp, " - Topic")) tmp[strlen(tmp) - 8] = 0;
    else if (strlen(tmp) > 4 && ci_suffix(tmp, "VEVO")) tmp[strlen(tmp) - 4] = 0;
    trim(tmp);
    copy_to(out, cap, tmp);
}

static b32 junk_tag_word(const char *w, size_t n) {
    static const char *const junk[] = {
        "official", "music", "video", "audio", "lyric", "lyrics", "lyrical", "visualizer", "visualiser",
        "hd", "hq", "4k", "1080p", "720p", "mv", "clip", "with", "explicit", "full", "song",
    };
    for (u32 i = 0; i < CORE_ARRAY_COUNT(junk); i++)
        if (strlen(junk[i]) == n && strncasecmp(w, junk[i], n) == 0) return true;
    return false;
}

/* Is the bracket group text[open..close] made only of "Official Video"-style words? */
static b32 junk_group(const char *s, size_t open, size_t close) {
    b32 any = false;
    size_t i = open + 1;
    while (i < close) {
        while (i < close && !isalnum((u8)s[i])) i++;
        size_t st = i;
        while (i < close && isalnum((u8)s[i])) i++;
        if (i == st) break;
        if (!junk_tag_word(s + st, i - st)) return false;
        any = true;
    }
    return any;
}

void dl_clean_title(const char *title, const char *artist, char *out, u32 cap) {
    char t[DL_TEXT * 2];
    copy_to(t, sizeof(t), title);
    trim(t);
    const char *start = t;
    size_t alen = artist ? strlen(artist) : 0;
    if (alen && ci_prefix(t, artist)) {
        const char *p = t + alen;
        while (*p == ' ') p++;
        if (*p == '-' || *p == ':' || ((u8)p[0] == 0xE2 && (u8)p[1] == 0x80 && ((u8)p[2] == 0x93 || (u8)p[2] == 0x94))) {
            p += (*p == '-' || *p == ':') ? 1 : 3;
            while (*p == ' ') p++;
            if (*p) start = p;
        }
    }
    char work[DL_TEXT * 2];
    copy_to(work, sizeof(work), start);
    for (;;) {
        size_t n = strlen(work);
        while (n && work[n - 1] == ' ') work[--n] = 0;
        if (!n || (work[n - 1] != ')' && work[n - 1] != ']')) break;
        char open = work[n - 1] == ')' ? '(' : '[';
        size_t o = n - 1;
        while (o > 0 && work[o] != open) o--;
        if (work[o] != open || o == 0 || !junk_group(work, o, n - 1)) break;
        work[o] = 0;
    }
    trim(work);
    copy_to(out, cap, work[0] ? work : t);
}

/* ---- song / artist keys (what "the same song" means) ---- */

/* Words that describe a release rather than a different song. */
static b32 release_word(const char *w, size_t n) {
    static const char *const words[] = {
        "remaster", "remastered", "remastering", "digital", "deluxe", "explicit", "clean", "version", "original",
        "album", "single", "bonus", "track", "mono", "stereo", "edition", "anniversary", "expanded",
    };
    b32 digits = n > 0;
    for (size_t i = 0; i < n; i++) digits = digits && isdigit((u8)w[i]); /* a year */
    if (digits || junk_tag_word(w, n)) return true;
    for (u32 i = 0; i < CORE_ARRAY_COUNT(words); i++)
        if (strlen(words[i]) == n && strncasecmp(w, words[i], n) == 0) return true;
    return false;
}

/* "2011 Remaster", "Explicit", "Single Version": nothing but release words. */
static b32 release_tags(const char *s, size_t len) {
    b32 any = false;
    size_t i = 0;
    while (i < len) {
        while (i < len && !isalnum((u8)s[i])) i++;
        size_t st = i;
        while (i < len && isalnum((u8)s[i])) i++;
        if (i == st) break;
        if (!release_word(s + st, i - st)) return false;
        any = true;
    }
    return any;
}

/* Recording variants belong to the same song family. Keep unknown subtitles:
   "Sweets (Soda Pop)" and "Pista (Fresh Start)" are meaningful title text. */
static b32 variant_tags(const char *s, size_t len) {
    static const char *const markers[] = {
        "instrumental", "acapella", "accapella", "cappella", "acoustic", "live",
        "remix", "mix", "edit", "edited", "cleaned", "dirty", "karaoke", "demo",
        "vip", "vocal", "radio", "extended", "remaster", "remastered",
    };
    if (release_tags(s, len)) return true;
    for (size_t i = 0; i < len;) {
        while (i < len && !isalnum((u8)s[i])) i++;
        size_t st = i;
        while (i < len && isalnum((u8)s[i])) i++;
        size_t n = i - st;
        for (u32 j = 0; j < CORE_ARRAY_COUNT(markers); j++)
            if (strlen(markers[j]) == n && !strncasecmp(s + st, markers[j], n)) return true;
    }
    return false;
}

/* Balanced brackets, including nested release labels. */
static size_t song_group_end(const char *s, size_t start) {
    char stack[64];
    u32 depth = 0;
    for (size_t i = start; s[i]; i++) {
        if (s[i] == '(' || s[i] == '[') {
            if (depth == CORE_ARRAY_COUNT(stack)) return start;
            stack[depth++] = s[i] == '(' ? ')' : ']';
        } else if (s[i] == ')' || s[i] == ']') {
            if (!depth || s[i] != stack[depth - 1]) return start;
            if (--depth == 0) return i;
        }
    }
    return start;
}

/* "feat. X & Y": a credit, not part of the song's name. */
static b32 credit_words(const char *s, size_t len) {
    size_t i = 0;
    while (i < len && !isalnum((u8)s[i])) i++;
    size_t st = i;
    while (i < len && isalnum((u8)s[i])) i++;
    size_t n = i - st;
    return (n == 2 && strncasecmp(s + st, "ft", 2) == 0) || (n == 4 && strncasecmp(s + st, "feat", 4) == 0) ||
           (n == 9 && strncasecmp(s + st, "featuring", 9) == 0);
}

/* Lowercase letters and digits (and any UTF-8 bytes) of `s`. */
static u32 fold_append(const char *s, char *out, u32 w, u32 cap) {
    for (const u8 *p = (const u8 *)s; *p && w + 1 < cap; p++)
        if (isalnum(*p) || *p >= 0x80) out[w++] = (char)tolower(*p);
    out[w] = 0;
    return w;
}

void dl_song_key(const char *title, char *out, u32 cap) {
    if (!cap) return;
    char t[DL_TEXT * 2];
    copy_to(t, sizeof(t), title);
    /* " - Remastered 2011" / " - Single Version" / " feat. X" tails */
    char *dash = 0;
    for (char *p = t; (p = strstr(p, " - ")) != 0; p += 3) dash = p;
    if (dash && variant_tags(dash + 3, strlen(dash + 3))) *dash = 0;
    static const char *const feats[] = { " feat. ", " feat ", " ft. ", " ft ", " featuring " };
    for (u32 i = 0; i < CORE_ARRAY_COUNT(feats); i++) {
        size_t fl = strlen(feats[i]);
        for (char *p = t; *p; p++)
            if (strncasecmp(p, feats[i], fl) == 0) { *p = 0; break; }
    }
    static const char *const tails[] = { " instrumental", " acapella", " accapella", " a cappella", " radio edit" };
    for (u32 i = 0; i < CORE_ARRAY_COUNT(tails); i++)
        if (strlen(t) > strlen(tails[i]) && ci_suffix(t, tails[i])) t[strlen(t) - strlen(tails[i])] = 0;
    u32 w = 0;
    for (size_t i = 0; t[i] && w + 1 < cap; i++) {
        u8 c = (u8)t[i];
        if (c == '(' || c == '[') {
            size_t end = song_group_end(t, i);
            if (end > i && (credit_words(t + i + 1, end - i - 1) || variant_tags(t + i + 1, end - i - 1)))
                i = end;
            continue;
        }
        if (isalnum(c) || c >= 0x80) out[w++] = (char)tolower(c);
    }
    out[w] = 0;
    if (!w) fold_append(title, out, 0, cap); /* a title that is all tags */
}

void dl_artist_key(const char *artist, char *out, u32 cap) {
    char p[DL_TEXT];
    dl_primary_artist(artist, p, sizeof(p));
    fold_append(p, out, 0, cap);
    if (strncmp(out, "the", 3) == 0 && strlen(out) > 5) memmove(out, out + 3, strlen(out + 3) + 1);
}

b32 dl_artist_plausible(const char *lib_artist, const char *known, const char *raw_title) {
    char a[DL_TEXT], k[DL_TEXT], hay[DL_TEXT * 2];
    dl_artist_key(lib_artist, a, sizeof(a));
    size_t al = strlen(a);
    if (al < 2) return false;
    dl_artist_key(known, k, sizeof(k));
    if (k[0] && !strcmp(a, k)) return true;
    if (al < 3) return false;
    if (strlen(k) >= 5 && al >= 5 && (strstr(a, k) || strstr(k, a))) return true;
    fold_append(raw_title, hay, 0, sizeof(hay));
    return strstr(hay, a) != 0;
}

void dl_path_segment(const char *in, char *out, u32 cap) {
    /* illegal characters become " - " (like the user's own downloader), then
       runs of spaces collapse */
    char tmp[320];
    u32 w = 0;
    b32 space = true; /* swallows leading spaces */
    for (const u8 *p = (const u8 *)in; *p && w < sizeof(tmp) - 8; p++) {
        b32 bad = *p < 0x20 || *p == 0x7f || strchr("<>:\"/\\|?*", *p) != 0;
        if (bad) {
            if (!space) tmp[w++] = ' ';
            tmp[w++] = '-';
            tmp[w++] = ' ';
            space = true;
        } else if (*p == ' ') {
            if (!space) tmp[w++] = ' ';
            space = true;
        } else {
            tmp[w++] = (char)*p;
            space = false;
        }
    }
    tmp[w] = 0;
    /* file systems cap names at 255 bytes; leave room for ".mp3.part" */
    if (w > 150) { w = 150; while (w && ((u8)tmp[w] & 0xC0) == 0x80) w--; tmp[w] = 0; }
    for (;;) {
        while (w && (tmp[w - 1] == ' ' || tmp[w - 1] == '.')) tmp[--w] = 0;
        if (w >= 2 && tmp[w - 2] == ' ' && tmp[w - 1] == '-') tmp[w -= 2] = 0; /* "What?" -> "What" */
        else break;
    }
    char *s = tmp;
    while (*s == '.' || *s == ' ') s++; /* a leading dot would make the file hidden (the library skips those) */
    if (!*s || !strcmp(s, "-")) { copy_to(out, cap, "_"); return; }
    if ((ci_prefix(s, "con") || ci_prefix(s, "prn") || ci_prefix(s, "aux") || ci_prefix(s, "nul")) &&
        (s[3] == 0 || s[3] == '.')) {
        char buf[340];
        snprintf(buf, sizeof(buf), "_%s", s);
        copy_to(out, cap, buf);
        return;
    }
    copy_to(out, cap, s);
}

void dl_dest_path(const char *dest_dir, const char *artist, const char *title, char *out, u32 cap) {
    char a[256], t[256];
    dl_path_segment(artist[0] ? artist : "Unknown Artist", a, sizeof(a));
    dl_path_segment(title, t, sizeof(t));
    snprintf(out, cap, "%s/%s/%s.mp3", dest_dir, a, t);
}

/* ------------------------------------------------------------------------- */
/* parsing what yt-dlp prints                                                */
/* ------------------------------------------------------------------------- */

static b32 valid_video_id(const char *id) {
    if (strlen(id) != 11) return false;
    for (u32 i = 0; i < 11; i++)
        if (!isalnum((u8)id[i]) && id[i] != '_' && id[i] != '-') return false;
    return true;
}

b32 dl_parse_result_line(const char *line, Dl_Result *out) {
    char buf[1024];
    copy_to(buf, sizeof(buf), line);
    char *f[5] = {0};
    u32 n = split_fields(buf, 0x1f, f, 5);
    if (n < 2 || !valid_video_id(f[0])) return false;
    memset(out, 0, sizeof(*out));
    memcpy(out->vid, f[0], 12);
    for (u32 i = 1; i < n; i++) { dl_clean_field(f[i]); f[i] = (char *)na(f[i]); }
    if (!f[1][0]) return false;
    copy_to(out->title, sizeof(out->title), f[1]);
    if (n > 2) copy_to(out->channel, sizeof(out->channel), f[2]);
    if (n > 3 && atof(f[3]) > 0) out->duration_s = (u32)atof(f[3]);
    if (n > 4 && atof(f[4]) > 0) out->views = (u64)atof(f[4]);
    return true;
}

b32 dl_parse_enrich_line(const char *line, Dl_Result *out) {
    if (strncmp(line, "OBE" US, 4) != 0) return false;
    char buf[1024];
    copy_to(buf, sizeof(buf), line + 4);
    char *f[4] = {0};
    u32 n = split_fields(buf, 0x1f, f, 4);
    if (n < 4 || !valid_video_id(f[0])) return false;
    for (u32 i = 1; i < n; i++) { dl_clean_field(f[i]); f[i] = (char *)na(f[i]); }
    memset(out, 0, sizeof(*out));
    memcpy(out->vid, f[0], 12);
    if (f[1][0]) dl_primary_artist(f[1], out->artist, sizeof(out->artist));
    if (atof(f[2]) > 0) out->duration_s = (u32)atof(f[2]);
    copy_to(out->title, sizeof(out->title), f[3]);
    return true;
}

b32 dl_parse_progress_line(const char *line, f32 *fraction) {
    if (strncmp(line, "OBP ", 4) != 0) return false;
    char a[32], b[32], c[32];
    if (sscanf(line + 4, "%31s %31s %31s", a, b, c) < 2) return false;
    f64 done = atof(a);
    f64 total = strcmp(b, "NA") && atof(b) > 0 ? atof(b) : (strcmp(c, "NA") == 0 ? 0 : atof(c));
    if (total <= 0 || !strcmp(a, "NA")) return false;
    f64 fr = done / total;
    *fraction = (f32)(fr < 0 ? 0 : fr > 1 ? 1 : fr);
    return true;
}

b32 dl_parse_meta_line(const char *line, Dl_Job *job) {
    if (strncmp(line, "OBM" US, 4) != 0) return false;
    char buf[2048];
    copy_to(buf, sizeof(buf), line + 4);
    char *f[12] = {0};
    u32 n = split_fields(buf, 0x1f, f, 12);
    if (n < 11) return false;
    for (u32 i = 0; i < n; i++) { dl_clean_field(f[i]); f[i] = (char *)na(f[i]); }
    /* 0 track, 1 title, 2 artist, 3 album, 4 release year, 5 upload year,
       6 duration, 7 genre, 8 channel, 9 track number, 10 uploader */
    char credit[DL_TEXT];
    if (f[2][0]) copy_to(credit, sizeof(credit), f[2]);
    else if (f[8][0]) dl_clean_channel(f[8], credit, sizeof(credit));
    else dl_clean_channel(f[10], credit, sizeof(credit));
    if (credit[0]) dl_primary_artist(credit, job->artist, sizeof(job->artist));
    if (f[0][0]) copy_to(job->title, sizeof(job->title), f[0]);
    else if (f[1][0]) dl_clean_title(f[1], job->artist, job->title, sizeof(job->title));
    if (f[3][0]) copy_to(job->album, sizeof(job->album), f[3]);
    u32 year = (u32)atoi(f[4][0] ? f[4] : f[5]);
    if (year >= 1000 && year <= 2999) job->year = year;
    if (atof(f[6]) > 0) job->duration_s = (u32)atof(f[6]);
    if (f[7][0]) copy_to(job->genre, sizeof(job->genre), f[7]);
    if (atoi(f[9]) > 0) job->track = (u32)atoi(f[9]);
    return true;
}

/* ------------------------------------------------------------------------- */
/* queue persistence                                                         */
/* ------------------------------------------------------------------------- */

u64 dl_jobs_format(const Dl_Job *jobs, u32 count, u64 next_uid, char *buf, u64 cap) {
    u64 n = 0;
    int w = snprintf(buf, cap, "offbeat-downloads 1\nnext_uid %llu\n", (unsigned long long)next_uid);
    if (w > 0) n = (u64)w;
    for (u32 i = 0; i < count && n + 1 < cap; i++) {
        const Dl_Job *j = &jobs[i];
        w = snprintf(buf + n, cap - n, "job\t%llu\t%u\t%u\t%s\t%u\t%u\t%u\t%s\t%s\t%s\t%s\t%s\t%s\n",
                     (unsigned long long)j->uid, j->state, j->attempts, j->vid, j->year, j->track,
                     j->duration_s, j->title, j->artist, j->album, j->genre, j->path, j->error);
        if (w < 0 || (u64)w >= cap - n) break; /* out of room: drop the rest rather than half a line */
        n += (u64)w;
    }
    return n;
}

u32 dl_jobs_parse(Core_String text, Dl_Job *out, u32 max, u64 *next_uid) {
    u32 count = 0;
    u64 at = 0;
    while (at < text.len) {
        u64 end = at;
        while (end < text.len && text.str[end] != '\n') end++;
        char line[2600];
        u64 n = CORE_MIN(end - at, (u64)sizeof(line) - 1);
        memcpy(line, text.str + at, n);
        line[n] = 0;
        at = end + 1;

        unsigned long long uid;
        if (sscanf(line, "next_uid %llu", &uid) == 1) { if (next_uid) *next_uid = uid; continue; }
        if (strncmp(line, "job\t", 4) != 0 || count >= max) continue;
        char *f[14] = {0};
        if (split_fields(line + 4, '\t', f, 14) < 13) continue;
        if (!valid_video_id(f[3])) continue;
        Dl_Job *j = &out[count];
        memset(j, 0, sizeof(*j));
        j->uid = strtoull(f[0], 0, 10);
        u32 st = (u32)atoi(f[1]);
        /* a job that was running when the app closed starts over as queued */
        j->state = st == DL_DONE ? DL_DONE : st == DL_FAILED ? DL_FAILED : DL_QUEUED;
        j->attempts = (u8)CORE_MIN((u32)atoi(f[2]), 255u);
        memcpy(j->vid, f[3], 12);
        j->year = (u32)atoi(f[4]);
        j->track = (u32)atoi(f[5]);
        j->duration_s = (u32)atoi(f[6]);
        copy_to(j->title, sizeof(j->title), f[7]);
        copy_to(j->artist, sizeof(j->artist), f[8]);
        copy_to(j->album, sizeof(j->album), f[9]);
        copy_to(j->genre, sizeof(j->genre), f[10]);
        copy_to(j->path, sizeof(j->path), f[11]);
        copy_to(j->error, sizeof(j->error), f[12]);
        j->progress = j->state == DL_DONE ? 1.0f : 0.0f;
        if (!j->uid) continue;
        count++;
    }
    return count;
}

/* ------------------------------------------------------------------------- */
/* manager                                                                   */
/* ------------------------------------------------------------------------- */

typedef struct {
    Dl_Job j;
    f64    not_before;  /* retry backoff: earliest start (monotonic seconds) */
    u32    batch;
    f64    stat_at;     /* when the finished file was last checked for, and ... */
    b32    file_ok;     /* ... whether it was there (deleted songs can be fetched again) */
} Dl_Slot;

typedef struct {
    Downloads        *d;
    Platform_Thread  *thread;
    Platform_Process *proc;   /* running child, guarded by d->mu */
    u64               uid;    /* job being worked on, 0 = idle   */
    b32               cancel; /* the job was removed while running */
} Dl_Worker;

struct Downloads {
    Platform_Mutex mu;
    Platform_Cond  cv;

    char state_path[1024], work_dir[1024], dest_dir[1024], music_dir[1024];
    char ffmpeg_path[512];
    Dl_Tools tools;
    u32  parallel;
    b32  quit, paused;

    Dl_Slot *slots;
    u32  count, cap;
    u64  next_uid;
    u32  version, batch_id, finished, session_done;
    f64  next_start_at;
    u32  rng;
    f64  backoff_base;   /* seconds before the first retry (x4 each time) */
    f64  stagger_min, stagger_span; /* gap between job starts */

    Dl_Worker workers[DL_MAX_WORKERS];

    /* search */
    Platform_Thread  *search_thread;
    Platform_Process *search_proc;
    Platform_Process *enrich_procs[DL_ENRICH_MAX]; /* artist lookups of YouTube Music hits */
    b32   search_pending;
    u32   search_kind;
    char  search_query[256];
    Dl_SearchInfo sinfo;
    Dl_Result results[DL_MAX_RESULTS];
    char  keys[DL_MAX_RESULTS][DL_TEXT]; /* dl_song_key of each result's title */
};

static u32 rng_next(Downloads *d) {
    d->rng ^= d->rng << 13; d->rng ^= d->rng >> 17; d->rng ^= d->rng << 5;
    return d->rng;
}

static f64 now_s(void) { return platform_time_seconds(); }

static Dl_Slot *find_slot(Downloads *d, u64 uid) {
    for (u32 i = 0; i < d->count; i++)
        if (d->slots[i].j.uid == uid) return &d->slots[i];
    return 0;
}

static void save_locked(Downloads *d) {
    u64 cap = 4096 + (u64)d->count * 2600;
    char *buf = core_heap_alloc(cap);
    Dl_Job *jobs = core_heap_alloc(sizeof(Dl_Job) * (d->count ? d->count : 1));
    for (u32 i = 0; i < d->count; i++) jobs[i] = d->slots[i].j;
    u64 n = dl_jobs_format(jobs, d->count, d->next_uid, buf, cap);
    platform_file_write_all(d->state_path, buf, n);
    core_heap_free(jobs);
    core_heap_free(buf);
    d->version++;
}

/* PATH first, then the places tools usually live that a desktop launcher's
   PATH often lacks (pipx/pip --user installs go to ~/.local/bin). */
static b32 find_tool(const char *name, char *out, u64 cap) {
    if (platform_find_executable(name, out, cap)) return true;
    if (strchr(name, '/')) return false; /* an explicit path that does not work: no fallbacks */
    const char *home = platform_env("HOME");
    const char *dirs[] = { "/usr/local/bin", "/usr/bin", "/opt/homebrew/bin", "/snap/bin", 0, 0, 0 };
    char a[512], b[512], c[512];
    if (home) {
        snprintf(a, sizeof(a), "%s/.local/bin", home);
        snprintf(b, sizeof(b), "%s/.nix-profile/bin", home);
        snprintf(c, sizeof(c), "%s/bin", home);
        dirs[4] = a; dirs[5] = b; dirs[6] = c;
    }
    for (u32 i = 0; i < CORE_ARRAY_COUNT(dirs); i++) {
        if (!dirs[i]) continue;
        char cand[1100];
        snprintf(cand, sizeof(cand), "%s/%s", dirs[i], name);
        if (platform_find_executable(cand, out, cap)) return true;
    }
    return false;
}

static void probe_tools(Downloads *d) {
    const char *y = platform_env("OFFBEAT_YTDLP");
    const char *f = platform_env("OFFBEAT_FFMPEG");
    char tmp[512];
    d->tools.ytdlp = find_tool(y ? y : "yt-dlp", tmp, sizeof(tmp));
    copy_to(d->tools.ytdlp_path, sizeof(d->tools.ytdlp_path), d->tools.ytdlp ? tmp : "yt-dlp");
    d->tools.ffmpeg = find_tool(f ? f : "ffmpeg", tmp, sizeof(tmp));
    copy_to(d->ffmpeg_path, sizeof(d->ffmpeg_path), d->tools.ffmpeg ? tmp : "ffmpeg");
}

static u32 active_count(Downloads *d) {
    u32 n = 0;
    for (u32 i = 0; i < d->count; i++) n += d->slots[i].j.state == DL_ACTIVE;
    return n;
}

/* Oldest queued job that may start now; marks it active. Caller holds mu. */
static u64 claim_locked(Downloads *d) {
    if (d->quit || d->paused || !d->tools.ytdlp || !d->tools.ffmpeg) return 0;
    f64 now = now_s();
    if (active_count(d) >= d->parallel || now < d->next_start_at) return 0;
    for (u32 i = 0; i < d->count; i++) {
        Dl_Slot *s = &d->slots[i];
        if (s->j.state != DL_QUEUED || s->not_before > now) continue;
        s->j.state = DL_ACTIVE;
        s->j.phase = DL_PHASE_START;
        s->j.progress = 0.02f;
        s->j.error[0] = 0;
        /* stagger starts a little so a big batch doesn't hammer the server */
        d->next_start_at = now + d->stagger_min + d->stagger_span * (f64)(rng_next(d) % 1000) / 1000.0;
        d->version++;
        return s->j.uid;
    }
    return 0;
}

/* ---- job bookkeeping (each takes the lock itself) ---- */

static void job_progress(Downloads *d, u64 uid, u32 phase, f32 progress) {
    platform_mutex_lock(&d->mu);
    Dl_Slot *s = find_slot(d, uid);
    if (s && s->j.state == DL_ACTIVE) {
        if (s->j.phase != phase) d->version++;
        s->j.phase = (u8)phase;
        s->j.progress = progress;
    }
    platform_mutex_unlock(&d->mu);
}

static void job_requeue(Downloads *d, u64 uid) {
    platform_mutex_lock(&d->mu);
    Dl_Slot *s = find_slot(d, uid);
    if (s) { s->j.state = DL_QUEUED; s->j.progress = 0; s->j.phase = 0; d->version++; save_locked(d); }
    platform_mutex_unlock(&d->mu);
}

static void work_path(Downloads *d, const char *vid, const char *name, char *out, u32 cap);

static void job_fail(Downloads *d, u64 uid, const char *error) {
    char give_up_work[1100] = {0};
    platform_mutex_lock(&d->mu);
    Dl_Slot *s = find_slot(d, uid);
    if (s) {
        s->j.attempts++;
        copy_to(s->j.error, sizeof(s->j.error), error[0] ? error : "Download failed");
        s->j.progress = 0;
        s->j.phase = 0;
        if (s->j.attempts >= DL_MAX_ATTEMPTS) {
            s->j.state = DL_FAILED;
            work_path(d, s->j.vid, 0, give_up_work, sizeof(give_up_work)); /* a retry starts over: no use keeping it */
        } else {
            s->j.state = DL_QUEUED;
            s->not_before = now_s() + d->backoff_base * (f64)(1u << (2 * (s->j.attempts - 1))); /* 5s, 20s */
        }
        save_locked(d);
    }
    platform_mutex_unlock(&d->mu);
    if (give_up_work[0]) platform_remove_tree(give_up_work);
    platform_cond_broadcast(&d->cv);
}

static void job_done(Downloads *d, u64 uid, const char *path, b32 existed) {
    platform_mutex_lock(&d->mu);
    Dl_Slot *s = find_slot(d, uid);
    if (s) {
        s->j.state = DL_DONE;
        s->j.progress = 1;
        s->j.error[0] = 0;
        s->j.existed = (u8)existed;
        copy_to(s->j.path, sizeof(s->j.path), path);
        d->finished++;
        d->session_done++;
        save_locked(d);
    }
    platform_mutex_unlock(&d->mu);
}

static void job_remove_locked(Downloads *d, u64 uid) {
    for (u32 i = 0; i < d->count; i++) {
        if (d->slots[i].j.uid != uid) continue;
        memmove(&d->slots[i], &d->slots[i + 1], sizeof(Dl_Slot) * (d->count - i - 1));
        d->count--;
        return;
    }
}

/* ---- where a song goes: into the library ---- */

typedef struct { char key[DL_TEXT]; char found[1100]; } Dup_Probe;

/* Is this file the song we are looking for? (same title modulo release tags) */
static b32 dup_visit(void *user, const char *path, u64 size, s64 mtime_ns) {
    CORE_UNUSED(mtime_ns);
    Dup_Probe *pr = user;
    static const char *const exts[] = { ".mp3", ".flac", ".ogg", ".opus", ".m4a" };
    b32 audio = false;
    for (u32 i = 0; i < CORE_ARRAY_COUNT(exts); i++) audio = audio || ci_suffix(path, exts[i]);
    if (!audio || size == 0) return true;
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    char base[320];
    copy_to(base, sizeof(base), name);
    char *dot = strrchr(base, '.');
    if (dot) *dot = 0;
    char k[DL_TEXT];
    dl_song_key(base, k, sizeof(k));
    if (strcmp(k, pr->key) != 0) return true;
    copy_to(pr->found, sizeof(pr->found), path);
    return false;
}

/* The folder the library keeps `artist` in (<music>/<Genre>/<Artist>), else a
   genre folder YouTube's tag names, else <fallback>/<Artist>. Songs of one
   artist always end up together, wherever the user filed the first one. */
static void artist_folder(const char *music_dir, const char *fallback, const Dl_Job *j, char *dir, u32 cap,
                          b32 *existing) {
    char akey[DL_TEXT], gkey[128];
    dl_artist_key(j->artist[0] ? j->artist : "Unknown Artist", akey, sizeof(akey));
    gkey[0] = 0;
    fold_append(j->genre, gkey, 0, sizeof(gkey));
    char seg[256];
    dl_path_segment(j->artist[0] ? j->artist : "Unknown Artist", seg, sizeof(seg));

    char in_fallback[1100] = {0}, by_genre[1100] = {0};
    *existing = false;
    dir[0] = 0;
    Core_Arena a;
    if (music_dir[0] && core_arena_init(&a, CORE_MB(16))) {
        const char **genres;
        u32 gn = platform_list_dirs(&a, music_dir, &genres);
        for (u32 g = 0; g < gn && !dir[0]; g++) {
            char gpath[1100];
            snprintf(gpath, sizeof(gpath), "%s/%s", music_dir, genres[g]);
            b32 is_fallback = strcmp(gpath, fallback) == 0;
            const char **artists;
            u32 an = platform_list_dirs(&a, gpath, &artists);
            for (u32 i = 0; i < an; i++) {
                char k[DL_TEXT];
                dl_artist_key(artists[i], k, sizeof(k));
                if (strcmp(k, akey) != 0) continue;
                char found[1100];
                snprintf(found, sizeof(found), "%s/%s", gpath, artists[i]);
                if (is_fallback) copy_to(in_fallback, sizeof(in_fallback), found);
                else copy_to(dir, cap, found);
                break;
            }
            /* "Hip-Hop/Rap" lands in a folder called "Hip Hop" (either name may be the longer one) */
            char dk[128];
            dk[0] = 0;
            fold_append(genres[g], dk, 0, sizeof(dk));
            if (!by_genre[0] && !is_fallback && gkey[0] && strlen(dk) >= 3 && (strstr(gkey, dk) || strstr(dk, gkey)))
                snprintf(by_genre, sizeof(by_genre), "%s/%s", gpath, seg);
        }
        core_arena_release(&a);
    }
    if (dir[0]) { *existing = true; return; }
    if (in_fallback[0]) { copy_to(dir, cap, in_fallback); *existing = true; return; }
    if (by_genre[0]) { copy_to(dir, cap, by_genre); return; }
    snprintf(dir, cap, "%s/%s", fallback, seg);
    *existing = platform_file_info(dir).is_dir;
}

/* Final path of a job's file. Returns true when the library already has this
   song (`dest` is then that file). */
static b32 plan_dest(const Dl_Job *j, const char *music_dir, const char *fallback, char *dest, u32 cap) {
    char dir[1100], seg[256];
    b32 existing;
    artist_folder(music_dir, fallback, j, dir, sizeof(dir), &existing);
    dl_path_segment(j->title, seg, sizeof(seg));
    snprintf(dest, cap, "%s/%s.mp3", dir, seg);
    if (!existing) return false;
    if (platform_file_info(dest).exists && platform_file_info(dest).size > 0) return true;
    Dup_Probe pr;
    memset(&pr, 0, sizeof(pr));
    dl_song_key(j->title, pr.key, sizeof(pr.key));
    platform_walk_dir(dir, dup_visit, &pr);
    if (!pr.found[0]) return false;
    copy_to(dest, cap, pr.found);
    return true;
}

/* ---- running a job ---- */

static void work_path(Downloads *d, const char *vid, const char *name, char *out, u32 cap) {
    if (name) snprintf(out, cap, "%s/%s/%s", d->work_dir, vid, name);
    else snprintf(out, cap, "%s/%s", d->work_dir, vid);
}

static void set_proc(Dl_Worker *w, Platform_Process *p) {
    platform_mutex_lock(&w->d->mu);
    w->proc = p;
    if (p && (w->cancel || w->d->quit)) platform_process_kill(p);
    platform_mutex_unlock(&w->d->mu);
}

/* What yt-dlp says is for developers; say what to do about the common ones. */
static const char *friendly_error(const char *msg) {
    static const struct { const char *needle, *say; } map[] = {
        { "Sign in to confirm",          "YouTube wants a sign-in: update yt-dlp, or give it your cookies" },
        { "HTTP Error 429",              "YouTube is rate limiting: try fewer downloads at once" },
        { "Too Many Requests",           "YouTube is rate limiting: try fewer downloads at once" },
        { "JavaScript runtime",          "yt-dlp needs a JavaScript runtime (deno or node) installed" },
        { "Requested format is not",     "No audio format found: update yt-dlp" },
        { "Unable to download webpage",  "Could not reach YouTube: check the connection" },
        { "Temporary failure in name",   "Could not reach YouTube: check the connection" },
        { "Name or service not known",   "Could not reach YouTube: check the connection" },
        { "Private video",               "This video is private" },
        { "copyright",                   "Blocked for copyright reasons" },
        { "not available in your country", "Not available in your country" },
    };
    for (u32 i = 0; i < CORE_ARRAY_COUNT(map); i++)
        if (strstr(msg, map[i].needle)) return map[i].say;
    return msg;
}

static void take_error_line(const char *line, char *err, u32 cap) {
    if (strncmp(line, "ERROR:", 6) != 0) return;
    const char *m = line + 6;
    while (*m == ' ') m++;
    if (*m == '[') { /* "[youtube] abc: Video unavailable" -> "Video unavailable" */
        const char *c = strstr(m, "] ");
        if (c) {
            const char *colon = strstr(c, ": ");
            m = colon ? colon + 2 : c + 2;
        }
    }
    copy_to(err, cap, friendly_error(m));
}

/* ffmpeg: attach the cover and write every tag. Returns true on success. */
static b32 tag_file(Downloads *d, const Dl_Job *j, const char *audio, const char *cover, const char *out,
                    char *err, u32 err_cap) {
    char a_title[DL_TEXT + 8], a_artist[DL_TEXT + 16], a_album[DL_TEXT + 8], a_date[32], a_genre[80],
         a_track[32], a_comment[128], a_vid[32], a_url[64], a_aa[DL_TEXT + 16];
    snprintf(a_title, sizeof(a_title), "title=%s", j->title);
    snprintf(a_artist, sizeof(a_artist), "artist=%s", j->artist);
    snprintf(a_aa, sizeof(a_aa), "album_artist=%s", j->artist);
    snprintf(a_album, sizeof(a_album), "album=%s", j->album);
    snprintf(a_date, sizeof(a_date), "date=%u", j->year);
    snprintf(a_genre, sizeof(a_genre), "genre=%s", j->genre);
    snprintf(a_track, sizeof(a_track), "track=%u", j->track);
    snprintf(a_url, sizeof(a_url), "source_url=https://www.youtube.com/watch?v=%s", j->vid);
    snprintf(a_vid, sizeof(a_vid), "youtube_id=%s", j->vid);
    snprintf(a_comment, sizeof(a_comment), "comment=Downloaded with Offbeat from YouTube (%s)", j->vid);

    const char *argv[64]; /* at most ~51 with every tag present */
    u32 n = 0;
    b32 have_cover = cover && cover[0];
    argv[n++] = d->ffmpeg_path;
    argv[n++] = "-hide_banner"; argv[n++] = "-loglevel"; argv[n++] = "error"; argv[n++] = "-y";
    argv[n++] = "-i"; argv[n++] = audio;
    if (have_cover) { argv[n++] = "-i"; argv[n++] = cover; }
    argv[n++] = "-map"; argv[n++] = "0:a";
    if (have_cover) {
        argv[n++] = "-map"; argv[n++] = "1:v";
        argv[n++] = "-disposition:v:0"; argv[n++] = "attached_pic";
        argv[n++] = "-metadata:s:v"; argv[n++] = "title=Album cover";
        argv[n++] = "-metadata:s:v"; argv[n++] = "comment=Cover (front)";
    }
    argv[n++] = "-c"; argv[n++] = "copy";
    argv[n++] = "-map_metadata"; argv[n++] = "-1";
    argv[n++] = "-id3v2_version"; argv[n++] = "3";
    argv[n++] = "-write_id3v1"; argv[n++] = "1";
    argv[n++] = "-metadata"; argv[n++] = a_title;
    argv[n++] = "-metadata"; argv[n++] = a_artist;
    argv[n++] = "-metadata"; argv[n++] = a_aa;
    if (j->album[0]) { argv[n++] = "-metadata"; argv[n++] = a_album; }
    if (j->year) { argv[n++] = "-metadata"; argv[n++] = a_date; }
    if (j->genre[0]) { argv[n++] = "-metadata"; argv[n++] = a_genre; }
    if (j->track) { argv[n++] = "-metadata"; argv[n++] = a_track; }
    argv[n++] = "-metadata"; argv[n++] = a_comment;
    argv[n++] = "-metadata"; argv[n++] = a_vid;
    argv[n++] = "-metadata"; argv[n++] = a_url;
    argv[n++] = "-f"; argv[n++] = "mp3";
    argv[n++] = out;
    argv[n] = 0;

    Platform_Process *p = platform_process_spawn(argv);
    if (!p) { copy_to(err, err_cap, "Could not start ffmpeg"); return false; }
    char line[512], last[DL_TEXT] = {0};
    for (;;) {
        s32 r = platform_process_read_line(p, line, sizeof(line), 0.5);
        if (r == -1) break;
        if (r > 0) copy_to(last, sizeof(last), line);
    }
    s32 code = platform_process_finish(p);
    if (code != 0) {
        snprintf(err, err_cap, "ffmpeg: %s", last[0] ? last : "tagging failed");
        return false;
    }
    return true;
}

static const char *const COVER_EXTS[] = { "jpg", "jpeg", "png", "webp" };

static void run_job(Downloads *d, Dl_Worker *w, u64 uid) {
    Dl_Job j;
    platform_mutex_lock(&d->mu);
    Dl_Slot *slot = find_slot(d, uid);
    if (!slot) { platform_mutex_unlock(&d->mu); return; }
    j = slot->j;
    char dest_dir[1024], music_dir[1024];
    copy_to(dest_dir, sizeof(dest_dir), d->dest_dir);
    copy_to(music_dir, sizeof(music_dir), d->music_dir);
    char ytdlp[512], ffmpeg[512];
    copy_to(ytdlp, sizeof(ytdlp), d->tools.ytdlp_path);
    copy_to(ffmpeg, sizeof(ffmpeg), d->ffmpeg_path);
    platform_mutex_unlock(&d->mu);

    char work[1100], out_tpl[1200], url[64], audio[1200], cover[1200], part[1700];
    work_path(d, j.vid, 0, work, sizeof(work));
    platform_make_dirs(work);
    snprintf(out_tpl, sizeof(out_tpl), "%s/%%(id)s.%%(ext)s", work);
    snprintf(url, sizeof(url), "https://www.youtube.com/watch?v=%s", j.vid);
    snprintf(audio, sizeof(audio), "%s/%s.mp3", work, j.vid);

    char err[DL_TEXT] = {0};
    b32 existed = false;
    char dest[1600] = {0};

    /* the hint may already name a song the library has (an earlier download) */
    if (j.title[0] && j.artist[0]) existed = plan_dest(&j, music_dir, dest_dir, dest, sizeof(dest));

    if (!existed) {
        job_progress(d, uid, DL_PHASE_START, 0.03f);
        const char *argv[] = {
            ytdlp, "--no-playlist", "--no-warnings", "--no-colors", "--ffmpeg-location", ffmpeg,
            "-x", "--audio-format", "mp3", "--audio-quality", "0",
            "--write-thumbnail", "--convert-thumbnails", "jpg",
            "--retries", "5", "--fragment-retries", "5", "--extractor-retries", "2", "--socket-timeout", "20",
            "--newline", "--progress",
            "--progress-template", "download:OBP %(progress.downloaded_bytes)s %(progress.total_bytes)s %(progress.total_bytes_estimate)s",
            "--print", "before_dl:OBM" US "%(track)s" US "%(title)s" US "%(artist)s" US "%(album)s" US "%(release_year)s"
                       US "%(upload_date>%Y)s" US "%(duration)s" US "%(genre)s" US "%(channel)s" US "%(track_number)s"
                       US "%(uploader)s",
            "-o", out_tpl, url, 0,
        };
        Platform_Process *p = platform_process_spawn(argv);
        if (!p) {
            job_fail(d, uid, "Could not start yt-dlp");
            return;
        }
        set_proc(w, p);
        char line[2200];
        f64 last_output = now_s();
        b32 skip_existing = false;
        f32 frac = 0;
        for (;;) {
            s32 r = platform_process_read_line(p, line, sizeof(line), 0.5);
            if (r == -1) break;
            if (r == -2) {
                if (now_s() - last_output > DL_STALL_SECONDS) {
                    copy_to(err, sizeof(err), "Timed out");
                    platform_process_kill(p);
                }
                continue;
            }
            last_output = now_s();
            if (dl_parse_progress_line(line, &frac)) {
                if (frac >= 0.999f) job_progress(d, uid, DL_PHASE_CONVERT, 0.88f);
                else job_progress(d, uid, DL_PHASE_FETCH, 0.05f + 0.80f * frac);
            } else if (strncmp(line, "OBM", 3) == 0) {
                platform_mutex_lock(&d->mu);
                Dl_Slot *s = find_slot(d, uid);
                if (s) {
                    dl_parse_meta_line(line, &s->j);
                    j = s->j;
                    d->version++;
                }
                platform_mutex_unlock(&d->mu);
                if (plan_dest(&j, music_dir, dest_dir, dest, sizeof(dest))) {
                    skip_existing = true;
                    platform_process_kill(p); /* no point fetching a song we already have */
                }
            } else {
                take_error_line(line, err, sizeof(err));
            }
        }
        set_proc(w, 0); /* before finish(): it frees the handle other threads may still kill */
        s32 code = platform_process_finish(p);
        if (w->cancel) { goto cancelled; }
        if (d->quit) { job_requeue(d, uid); return; }
        if (skip_existing) existed = true;
        else if (code != 0) {
            job_fail(d, uid, err);
            goto cleanup_partial;
        }
    }

    if (existed) {
        job_done(d, uid, dest, true);
        platform_remove_tree(work);
        return;
    }

    /* ---- tag: audio + cover -> final file ---- */
    if (!platform_file_info(audio).exists) {
        job_fail(d, uid, err[0] ? err : "yt-dlp produced no audio file");
        return;
    }
    cover[0] = 0;
    for (u32 i = 0; i < CORE_ARRAY_COUNT(COVER_EXTS); i++) {
        char c[1200];
        snprintf(c, sizeof(c), "%s/%s.%s", work, j.vid, COVER_EXTS[i]);
        if (platform_file_info(c).exists) { copy_to(cover, sizeof(cover), c); break; }
    }
    job_progress(d, uid, DL_PHASE_TAG, 0.94f);
    if (plan_dest(&j, music_dir, dest_dir, dest, sizeof(dest))) { /* another job saved it meanwhile */
        job_done(d, uid, dest, true);
        platform_remove_tree(work);
        return;
    }
    char dir[1600];
    copy_to(dir, sizeof(dir), dest);
    {
        char *slash = strrchr(dir, '/');
        if (slash) { *slash = 0; platform_make_dirs(dir); }
    }
    snprintf(part, sizeof(part), "%s.part", dest);
    /* the half-written file sits in the library folder: note where, so a crash can't leave it there for good */
    char marker[1200], note[1800];
    work_path(d, j.vid, "tagging", marker, sizeof(marker));
    snprintf(note, sizeof(note), "%s\n", part);
    platform_file_write_all(marker, note, strlen(note));
    if (!tag_file(d, &j, audio, cover, part, err, sizeof(err))) {
        platform_file_remove(part);
        platform_remove_dir_if_empty(dir);
        if (w->cancel) goto cancelled;
        if (d->quit) { job_requeue(d, uid); return; }
        job_fail(d, uid, err);
        return;
    }
    if (!platform_file_rename(part, dest)) {
        platform_file_remove(part);
        platform_remove_dir_if_empty(dir);
        job_fail(d, uid, "Could not save the file");
        return;
    }
    job_done(d, uid, dest, false);
    platform_remove_tree(work);
    return;

cleanup_partial:
    /* keep the work folder: yt-dlp resumes a partial download on the retry */
    return;

cancelled:
    platform_mutex_lock(&d->mu);
    job_remove_locked(d, uid);
    save_locked(d);
    platform_mutex_unlock(&d->mu);
    platform_remove_tree(work);
}

static void worker_main(void *arg) {
    Dl_Worker *w = arg;
    Downloads *d = w->d;
    platform_thread_set_background(); /* children inherit it: conversions never starve the UI */
    platform_mutex_lock(&d->mu);
    while (!d->quit) {
        u64 uid = claim_locked(d);
        if (!uid) {
            platform_cond_wait_timeout(&d->cv, &d->mu, 0.5);
            continue;
        }
        w->uid = uid;
        platform_mutex_unlock(&d->mu);
        run_job(d, w, uid);
        platform_mutex_lock(&d->mu);
        w->uid = 0;
        w->cancel = false;
    }
    platform_mutex_unlock(&d->mu);
}

/* ---- search ---- */

static void url_encode(const char *s, char *out, u32 cap) {
    static const char hex[] = "0123456789ABCDEF";
    u32 w = 0;
    for (const u8 *p = (const u8 *)s; *p && w + 4 < cap; p++) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') out[w++] = (char)*p;
        else if (*p == ' ') out[w++] = '+';
        else { out[w++] = '%'; out[w++] = hex[*p >> 4]; out[w++] = hex[*p & 15]; }
    }
    out[w] = 0;
}

/* Runs one yt-dlp listing. Returns the number of results added. `gen` is the
   generation this search belongs to; a newer one aborts it. `limit` hits are
   asked for and listing stops once `keep` of them survived the de-duplication
   (a song's remasters are listed as many times), so ask for more than you keep. */
static u32 search_run(Downloads *d, u32 gen, const char *target, u32 limit, u32 keep, b32 dedupe_songs, b32 music,
                      char *err, u32 err_cap) {
    platform_mutex_lock(&d->mu);
    char ytdlp[512];
    copy_to(ytdlp, sizeof(ytdlp), d->tools.ytdlp_path);
    platform_mutex_unlock(&d->mu);

    char end[16];
    snprintf(end, sizeof(end), "%u", limit);
    const char *argv[] = {
        ytdlp, "--flat-playlist", "--no-warnings", "--ignore-errors", "--no-colors", "--socket-timeout", "15",
        "--playlist-end", end,
        "--print", "%(id)s" US "%(title)s" US "%(channel,uploader)s" US "%(duration)s" US "%(view_count)s",
        target, 0,
    };
    Platform_Process *p = platform_process_spawn(argv);
    if (!p) { copy_to(err, err_cap, "Could not start yt-dlp"); return 0; }

    platform_mutex_lock(&d->mu);
    d->search_proc = p;
    if (d->sinfo.generation != gen) platform_process_kill(p);
    platform_mutex_unlock(&d->mu);

    u32 added = 0;
    f64 last_output = now_s();
    char line[2200];
    for (;;) {
        s32 r = platform_process_read_line(p, line, sizeof(line), 0.25);
        if (r == -1) break;
        if (r == -2) {
            if (now_s() - last_output > 40.0) {
                copy_to(err, err_cap, "The search timed out");
                platform_process_kill(p);
            }
            continue;
        }
        last_output = now_s();
        Dl_Result res;
        if (dl_parse_result_line(line, &res)) {
            res.music = (u8)music;
            platform_mutex_lock(&d->mu);
            if (d->sinfo.generation == gen && d->sinfo.count < DL_MAX_RESULTS) {
                b32 dup = false;
                char key[DL_TEXT];
                dl_song_key(res.title, key, sizeof(key));
                for (u32 i = 0; i < d->sinfo.count && !dup; i++)
                    dup = !strcmp(d->results[i].vid, res.vid) || (dedupe_songs && !strcmp(key, d->keys[i]));
                if (!dup) {
                    copy_to(d->keys[d->sinfo.count], DL_TEXT, key);
                    d->results[d->sinfo.count++] = res;
                    added++;
                }
            }
            platform_mutex_unlock(&d->mu);
            if (added >= keep) platform_process_kill(p);
        } else {
            take_error_line(line, err, err_cap);
        }
    }
    platform_mutex_lock(&d->mu);
    d->search_proc = 0;
    platform_mutex_unlock(&d->mu);
    platform_process_finish(p);
    return added;
}

static void kill_enrich_locked(Downloads *d) {
    for (u32 i = 0; i < DL_ENRICH_MAX; i++)
        if (d->enrich_procs[i]) platform_process_kill(d->enrich_procs[i]);
}

/* A flat YouTube Music listing has no artist, and its titles hide the
   release ("Hypnotize" is really "Hypnotize (2014 Remaster)"). Look each hit
   up (in parallel, they take a few seconds each), fill the artist and real
   title in as they arrive, then drop the hits that turn out to be the same
   song by the same artist twice. */
static void enrich_music(Downloads *d, u32 gen) {
    char ytdlp[512];
    char vids[DL_ENRICH_MAX][16];
    u32 n = 0;
    platform_mutex_lock(&d->mu);
    copy_to(ytdlp, sizeof(ytdlp), d->tools.ytdlp_path);
    if (d->sinfo.generation == gen)
        for (u32 i = 0; i < d->sinfo.count && n < DL_ENRICH_MAX; i++)
            if (d->results[i].music && !d->results[i].artist[0]) copy_to(vids[n++], sizeof(vids[0]), d->results[i].vid);
    platform_mutex_unlock(&d->mu);
    if (!n) return;

    Platform_Process *procs[DL_ENRICH_MAX] = {0};
    u32 alive = 0;
    for (u32 i = 0; i < n; i++) {
        char url[64];
        snprintf(url, sizeof(url), "https://www.youtube.com/watch?v=%s", vids[i]);
        const char *argv[] = {
            ytdlp, "--skip-download", "--no-playlist", "--no-warnings", "--ignore-errors", "--no-colors",
            "--socket-timeout", "15",
            "--print", "OBE" US "%(id)s" US "%(artist,channel,uploader)s" US "%(duration)s" US "%(track,title)s",
            url, 0,
        };
        procs[i] = platform_process_spawn(argv);
        alive += procs[i] != 0;
    }
    platform_mutex_lock(&d->mu);
    for (u32 i = 0; i < n; i++) d->enrich_procs[i] = procs[i];
    if (d->sinfo.generation != gen) kill_enrich_locked(d);
    platform_mutex_unlock(&d->mu);

    f64 deadline = now_s() + 45.0;
    while (alive) {
        for (u32 i = 0; i < n; i++) {
            if (!procs[i]) continue;
            char line[1100];
            s32 r = platform_process_read_line(procs[i], line, sizeof(line), 0.02);
            if (r == -1) {
                platform_mutex_lock(&d->mu);
                d->enrich_procs[i] = 0; /* before finish(): it frees the handle */
                platform_mutex_unlock(&d->mu);
                platform_process_finish(procs[i]);
                procs[i] = 0;
                alive--;
                continue;
            }
            Dl_Result e;
            if (r <= 0 || !dl_parse_enrich_line(line, &e)) continue;
            platform_mutex_lock(&d->mu);
            if (d->sinfo.generation == gen) {
                for (u32 k = 0; k < d->sinfo.count; k++) {
                    Dl_Result *res = &d->results[k];
                    if (strcmp(res->vid, e.vid) != 0) continue;
                    copy_to(res->artist, sizeof(res->artist), e.artist);
                    if (e.title[0]) copy_to(res->title, sizeof(res->title), e.title);
                    if (e.duration_s) res->duration_s = e.duration_s;
                    dl_song_key(res->title, d->keys[k], DL_TEXT);
                }
            }
            platform_mutex_unlock(&d->mu);
        }
        if (now_s() > deadline) {
            platform_mutex_lock(&d->mu);
            kill_enrich_locked(d);
            platform_mutex_unlock(&d->mu);
            deadline = now_s() + 1e9;
        }
    }

    /* the same song twice (single, album, remaster...) -> keep the first */
    platform_mutex_lock(&d->mu);
    if (d->sinfo.generation == gen) {
        for (u32 i = 0; i < d->sinfo.count; i++) {
            if (!d->results[i].music || !d->results[i].artist[0]) continue;
            char ak[DL_TEXT];
            dl_artist_key(d->results[i].artist, ak, sizeof(ak));
            for (u32 k = i + 1; k < d->sinfo.count;) {
                char bk[DL_TEXT];
                dl_artist_key(d->results[k].artist, bk, sizeof(bk));
                if (d->results[k].music && !strcmp(ak, bk) && !strcmp(d->keys[i], d->keys[k])) {
                    memmove(&d->results[k], &d->results[k + 1], sizeof(Dl_Result) * (d->sinfo.count - k - 1));
                    memmove(&d->keys[k], &d->keys[k + 1], sizeof(d->keys[0]) * (d->sinfo.count - k - 1));
                    d->sinfo.count--;
                } else k++;
            }
        }
    }
    platform_mutex_unlock(&d->mu);
}

static void search_main(void *arg) {
    Downloads *d = arg;
    platform_thread_set_background();
    platform_mutex_lock(&d->mu);
    while (!d->quit) {
        if (!d->search_pending) { platform_cond_wait(&d->cv, &d->mu); continue; }
        d->search_pending = false;
        u32 gen = d->sinfo.generation, kind = d->search_kind;
        char query[256];
        copy_to(query, sizeof(query), d->search_query);
        platform_mutex_unlock(&d->mu);

        char err[DL_TEXT] = {0};
        u32 found = 0;
        char target[1024];
        if (kind == DL_QUERY_ARTIST) {
            /* YouTube Music's "Songs" filter: clean titles of actual songs, most popular first */
            char q[768];
            url_encode(query, q, sizeof(q));
            snprintf(target, sizeof(target),
                     "https://music.youtube.com/search?q=%s&sp=EgWKAQIIAWoKEAoQAxAEEAkQBQ%%3D%%3D", q);
            found = search_run(d, gen, target, 200, 100, true, true, err, sizeof(err));
            platform_mutex_lock(&d->mu);
            b32 current = d->sinfo.generation == gen;
            platform_mutex_unlock(&d->mu);
            if (!found && current) { /* the filter link may have changed: fall back to a plain search */
                snprintf(target, sizeof(target), "ytsearch60:%s topic", query);
                err[0] = 0;
                found = search_run(d, gen, target, 60, 60, true, false, err, sizeof(err));
            }
        } else {
            /* the studio versions first (they carry album, year and track number) ... */
            char q[768];
            url_encode(query, q, sizeof(q));
            snprintf(target, sizeof(target),
                     "https://music.youtube.com/search?q=%s&sp=EgWKAQIIAWoKEAoQAxAEEAkQBQ%%3D%%3D", q);
            found = search_run(d, gen, target, 5, 5, false, true, err, sizeof(err));
            platform_mutex_lock(&d->mu);
            b32 current = d->sinfo.generation == gen;
            platform_mutex_unlock(&d->mu);
            /* ... then whatever else is on YouTube */
            if (current) {
                char err2[DL_TEXT] = {0};
                snprintf(target, sizeof(target), "ytsearch12:%s", query);
                found += search_run(d, gen, target, 12, 12, false, false, err2, sizeof(err2));
                if (!found) copy_to(err, sizeof(err), err2);
                enrich_music(d, gen);
            }
        }

        platform_mutex_lock(&d->mu);
        if (d->sinfo.generation == gen) {
            d->sinfo.state = found ? DL_SEARCH_DONE : DL_SEARCH_FAILED;
            if (!found) copy_to(d->sinfo.error, sizeof(d->sinfo.error), err[0] ? err : "No results");
        }
    }
    platform_mutex_unlock(&d->mu);
}

/* ------------------------------------------------------------------------- */
/* public API                                                                */
/* ------------------------------------------------------------------------- */

static void sweep_work_dir(Downloads *d);

Downloads *downloads_create(const char *state_path, const char *work_dir) {
    Downloads *d = core_heap_calloc(sizeof(*d));
    platform_mutex_init(&d->mu);
    platform_cond_init(&d->cv);
    copy_to(d->state_path, sizeof(d->state_path), state_path);
    copy_to(d->work_dir, sizeof(d->work_dir), work_dir);
    d->parallel = 2;
    d->next_uid = 1;
    d->rng = 0x9e3779b9u ^ (u32)(now_s() * 1000.0);
    d->backoff_base = 5.0;
    d->stagger_min = 2.0;   /* a batch of 100 is polite, not a burst */
    d->stagger_span = 3.0;
    if (platform_env("OFFBEAT_DL_FAST")) { /* tests and demos: no politeness delays */
        d->backoff_base = 0.2;
        d->stagger_min = 0.05;
        d->stagger_span = 0.1;
    }
    probe_tools(d);

    Core_Arena scratch;
    if (core_arena_init(&scratch, CORE_MB(64))) {
        Core_String text = platform_file_read_all(&scratch, state_path);
        if (text.len) {
            u32 max = 4096;
            Dl_Job *jobs = core_heap_alloc(sizeof(Dl_Job) * max);
            u32 n = dl_jobs_parse(text, jobs, max, &d->next_uid);
            d->cap = n + 64;
            d->slots = core_heap_calloc(sizeof(Dl_Slot) * d->cap);
            for (u32 i = 0; i < n; i++) {
                d->slots[i].j = jobs[i];
                if (jobs[i].uid >= d->next_uid) d->next_uid = jobs[i].uid + 1;
            }
            d->count = n;
            core_heap_free(jobs);
        }
        core_arena_release(&scratch);
    }
    if (!d->slots) {
        d->cap = 64;
        d->slots = core_heap_calloc(sizeof(Dl_Slot) * d->cap);
    }
    d->batch_id = 1;
    for (u32 i = 0; i < d->count; i++) d->slots[i].batch = 1;
    sweep_work_dir(d);

    for (u32 i = 0; i < DL_MAX_WORKERS; i++) {
        d->workers[i].d = d;
        d->workers[i].thread = platform_thread_start(worker_main, &d->workers[i], "dl-worker");
    }
    d->search_thread = platform_thread_start(search_main, d, "dl-search");
    return d;
}

void downloads_destroy(Downloads *d) {
    if (!d) return;
    platform_mutex_lock(&d->mu);
    d->quit = true;
    for (u32 i = 0; i < DL_MAX_WORKERS; i++)
        if (d->workers[i].proc) platform_process_kill(d->workers[i].proc);
    if (d->search_proc) platform_process_kill(d->search_proc);
    kill_enrich_locked(d);
    platform_mutex_unlock(&d->mu);
    platform_cond_broadcast(&d->cv);
    for (u32 i = 0; i < DL_MAX_WORKERS; i++) platform_thread_join(d->workers[i].thread);
    platform_thread_join(d->search_thread);
    platform_mutex_lock(&d->mu);
    save_locked(d); /* jobs that were running are written as queued by the parser */
    platform_mutex_unlock(&d->mu);
    core_heap_free(d->slots);
    core_heap_free(d);
}

void downloads_configure(Downloads *d, const char *dest_dir, u32 parallel) {
    platform_mutex_lock(&d->mu);
    copy_to(d->dest_dir, sizeof(d->dest_dir), dest_dir);
    d->parallel = CORE_CLAMP(parallel, 1u, (u32)DL_MAX_WORKERS);
    platform_mutex_unlock(&d->mu);
    platform_cond_broadcast(&d->cv);
}

/* Leftovers of an earlier run: scratch folders of jobs that are gone, and a
   half-written .part file a crash left in a library folder (its path was
   noted in <work>/tagging). Folders of jobs still in the queue stay, so
   yt-dlp can resume them. */
static void sweep_work_dir(Downloads *d) {
    Core_Arena a;
    if (!core_arena_init(&a, CORE_MB(4))) return;
    const char **names;
    u32 n = platform_list_dirs(&a, d->work_dir, &names);
    for (u32 i = 0; i < n; i++) {
        char path[1100], marker[1200];
        work_path(d, names[i], 0, path, sizeof(path));
        work_path(d, names[i], "tagging", marker, sizeof(marker));
        Core_String note = platform_file_read_all(&a, marker);
        if (note.len) {
            char part[1100];
            u64 len = CORE_MIN(note.len, (u64)sizeof(part) - 1);
            memcpy(part, note.str, len);
            part[len] = 0;
            part[strcspn(part, "\r\n")] = 0;
            if (ci_suffix(part, ".mp3.part")) {
                platform_file_remove(part);
                char *slash = strrchr(part, '/');
                if (slash) { *slash = 0; platform_remove_dir_if_empty(part); }
            }
            platform_file_remove(marker);
        }
        b32 wanted = false;
        for (u32 k = 0; k < d->count; k++)
            wanted = wanted || (d->slots[k].j.state == DL_QUEUED && !strcmp(d->slots[k].j.vid, names[i]));
        if (!wanted) platform_remove_tree(path);
    }
    core_arena_release(&a);
}

void downloads_set_library(Downloads *d, const char *music_dir) {
    platform_mutex_lock(&d->mu);
    copy_to(d->music_dir, sizeof(d->music_dir), music_dir);
    platform_mutex_unlock(&d->mu);
}

void downloads_check_tools(Downloads *d) {
    platform_mutex_lock(&d->mu);
    probe_tools(d);
    d->version++;
    platform_mutex_unlock(&d->mu);
    platform_cond_broadcast(&d->cv);
}

Dl_Tools downloads_tools(Downloads *d) {
    platform_mutex_lock(&d->mu);
    Dl_Tools t = d->tools;
    platform_mutex_unlock(&d->mu);
    return t;
}

void downloads_search(Downloads *d, u32 kind, const char *query) {
    platform_mutex_lock(&d->mu);
    d->sinfo.generation++;
    d->sinfo.kind = kind;
    d->sinfo.count = 0;
    d->sinfo.error[0] = 0;
    copy_to(d->sinfo.query, sizeof(d->sinfo.query), query);
    d->sinfo.state = DL_SEARCH_BUSY;
    if (!d->tools.ytdlp) {
        d->sinfo.state = DL_SEARCH_FAILED;
        copy_to(d->sinfo.error, sizeof(d->sinfo.error), "yt-dlp is not installed");
    } else {
        d->search_kind = kind;
        copy_to(d->search_query, sizeof(d->search_query), query);
        d->search_pending = true;
        if (d->search_proc) platform_process_kill(d->search_proc);
        kill_enrich_locked(d);
    }
    platform_mutex_unlock(&d->mu);
    platform_cond_broadcast(&d->cv);
}

void downloads_search_cancel(Downloads *d) {
    platform_mutex_lock(&d->mu);
    d->sinfo.generation++;
    d->sinfo.state = DL_SEARCH_IDLE;
    d->sinfo.count = 0;
    d->search_pending = false;
    if (d->search_proc) platform_process_kill(d->search_proc);
    kill_enrich_locked(d);
    platform_mutex_unlock(&d->mu);
}

Dl_SearchInfo downloads_search_info(Downloads *d) {
    platform_mutex_lock(&d->mu);
    Dl_SearchInfo s = d->sinfo;
    platform_mutex_unlock(&d->mu);
    return s;
}

b32 downloads_result(Downloads *d, u32 index, Dl_Result *out) {
    platform_mutex_lock(&d->mu);
    b32 ok = index < d->sinfo.count;
    if (ok) *out = d->results[index];
    platform_mutex_unlock(&d->mu);
    return ok;
}

static Dl_Slot *find_vid(Downloads *d, const char *vid) {
    for (u32 i = d->count; i-- > 0;)
        if (!strcmp(d->slots[i].j.vid, vid)) return &d->slots[i];
    return 0;
}

/* Is this song (same artist, same title modulo release tags) already queued,
   running, or saved? Failed jobs don't count: they may be picked again. */
static b32 song_in_queue(Downloads *d, const char *artist, const char *title) {
    char ak[DL_TEXT], sk[DL_TEXT];
    dl_artist_key(artist, ak, sizeof(ak));
    dl_song_key(title, sk, sizeof(sk));
    for (u32 i = 0; i < d->count; i++) {
        const Dl_Job *j = &d->slots[i].j;
        if (j->state == DL_FAILED) continue;
        char k[DL_TEXT];
        dl_song_key(j->title, k, sizeof(k));
        if (strcmp(k, sk) != 0) continue;
        dl_artist_key(j->artist, k, sizeof(k));
        if (strcmp(k, ak) != 0) continue;
        if (j->state == DL_DONE && (!j->path[0] || !platform_file_info(j->path).exists)) continue;
        return true;
    }
    return false;
}

u32 downloads_enqueue(Downloads *d, const Dl_Result *results, u32 count, const char *artist_hint) {
    u32 added = 0;
    platform_mutex_lock(&d->mu);
    b32 idle = true;
    for (u32 i = 0; i < d->count; i++)
        if (d->slots[i].j.state == DL_QUEUED || d->slots[i].j.state == DL_ACTIVE) { idle = false; break; }
    if (idle) d->batch_id++;

    for (u32 i = 0; i < count; i++) {
        const Dl_Result *r = &results[i];
        if (!valid_video_id(r->vid)) continue;
        Dl_Slot *ex = find_vid(d, r->vid);
        if (ex) {
            if (ex->j.state == DL_QUEUED || ex->j.state == DL_ACTIVE) continue;
            if (ex->j.state == DL_DONE && ex->j.path[0] && platform_file_info(ex->j.path).exists) continue;
            /* failed, or the file was deleted: run it again */
            ex->j.state = DL_QUEUED;
            ex->j.attempts = 0;
            ex->j.progress = 0;
            ex->j.error[0] = 0;
            ex->not_before = 0;
            ex->batch = d->batch_id;
            added++;
            continue;
        }
        char artist[DL_TEXT], title[DL_TEXT];
        if (artist_hint && artist_hint[0]) dl_primary_artist(artist_hint, artist, sizeof(artist));
        else if (r->artist[0]) copy_to(artist, sizeof(artist), r->artist);
        else dl_clean_channel(r->channel, artist, sizeof(artist));
        dl_clean_title(r->title, artist, title, sizeof(title));
        if (song_in_queue(d, artist, title)) continue; /* "Hypnotize" and "Hypnotize (2007 Remaster)" */
        if (d->count == d->cap) {
            d->cap *= 2;
            d->slots = core_heap_realloc(d->slots, sizeof(Dl_Slot) * d->cap);
        }
        Dl_Slot *s = &d->slots[d->count++];
        memset(s, 0, sizeof(*s));
        s->batch = d->batch_id;
        Dl_Job *j = &s->j;
        j->uid = d->next_uid++;
        memcpy(j->vid, r->vid, 12);
        copy_to(j->artist, sizeof(j->artist), artist);
        copy_to(j->title, sizeof(j->title), title);
        j->duration_s = r->duration_s;
        added++;
    }
    /* forget the oldest finished jobs once the list gets long */
    u32 done = 0;
    for (u32 i = 0; i < d->count; i++) done += d->slots[i].j.state == DL_DONE;
    for (u32 i = 0; i < d->count && done > DL_KEEP_DONE;) {
        if (d->slots[i].j.state == DL_DONE) { job_remove_locked(d, d->slots[i].j.uid); done--; }
        else i++;
    }
    if (added) save_locked(d);
    platform_mutex_unlock(&d->mu);
    platform_cond_broadcast(&d->cv);
    return added;
}

Dl_Summary downloads_summary(Downloads *d) {
    Dl_Summary s = {0};
    platform_mutex_lock(&d->mu);
    f32 sum = 0;
    for (u32 i = 0; i < d->count; i++) {
        const Dl_Slot *sl = &d->slots[i];
        switch (sl->j.state) {
        case DL_QUEUED: s.queued++; break;
        case DL_ACTIVE: s.active++; break;
        case DL_DONE:   s.done++; break;
        default:        s.failed++; break;
        }
        if (sl->batch == d->batch_id) {
            s.batch_total++;
            b32 finished = sl->j.state == DL_DONE || sl->j.state == DL_FAILED;
            if (finished) s.batch_done++;
            sum += finished ? 1.0f : sl->j.progress;
        }
    }
    s.total = d->count;
    s.session_done = d->session_done;
    s.progress = s.batch_total ? sum / (f32)s.batch_total : 0;
    s.paused = d->paused;
    s.version = d->version;
    platform_mutex_unlock(&d->mu);
    return s;
}

u32 downloads_briefs(Downloads *d, Dl_Brief *out, u32 max) {
    platform_mutex_lock(&d->mu);
    u32 n = CORE_MIN(d->count, max);
    for (u32 i = 0; i < n; i++) {
        const Dl_Job *j = &d->slots[i].j;
        out[i] = (Dl_Brief){ .uid = j->uid, .state = j->state, .phase = j->phase, .attempts = j->attempts,
                             .progress = j->progress };
    }
    platform_mutex_unlock(&d->mu);
    return n;
}

u32 downloads_job_count(Downloads *d) {
    platform_mutex_lock(&d->mu);
    u32 n = d->count;
    platform_mutex_unlock(&d->mu);
    return n;
}

b32 downloads_job(Downloads *d, u32 index, Dl_Job *out) {
    platform_mutex_lock(&d->mu);
    b32 ok = index < d->count;
    if (ok) *out = d->slots[index].j;
    platform_mutex_unlock(&d->mu);
    return ok;
}

b32 downloads_has(Downloads *d, const char *vid, Dl_State *state) {
    platform_mutex_lock(&d->mu);
    Dl_Slot *s = find_vid(d, vid);
    b32 has = s != 0;
    if (s) {
        u32 st = s->j.state;
        /* a finished song whose file was deleted counts as not downloaded
           (checked at most every couple of seconds: the UI asks every frame) */
        if (st == DL_DONE && s->j.path[0]) {
            f64 now = now_s();
            if (now - s->stat_at > 2.0) { s->file_ok = platform_file_info(s->j.path).exists; s->stat_at = now; }
            if (!s->file_ok) has = false;
        }
        if (state) *state = (Dl_State)st;
    }
    platform_mutex_unlock(&d->mu);
    return has;
}

void downloads_set_paused(Downloads *d, b32 paused) {
    platform_mutex_lock(&d->mu);
    d->paused = paused;
    d->version++;
    platform_mutex_unlock(&d->mu);
    platform_cond_broadcast(&d->cv);
}

void downloads_retry(Downloads *d, u64 uid) {
    platform_mutex_lock(&d->mu);
    Dl_Slot *s = find_slot(d, uid);
    if (s && s->j.state == DL_FAILED) {
        s->j.state = DL_QUEUED;
        s->j.attempts = 0;
        s->j.error[0] = 0;
        s->not_before = 0;
        s->batch = d->batch_id;
        save_locked(d);
    }
    platform_mutex_unlock(&d->mu);
    platform_cond_broadcast(&d->cv);
}

void downloads_retry_failed(Downloads *d) {
    platform_mutex_lock(&d->mu);
    for (u32 i = 0; i < d->count; i++) {
        Dl_Slot *s = &d->slots[i];
        if (s->j.state != DL_FAILED) continue;
        s->j.state = DL_QUEUED;
        s->j.attempts = 0;
        s->j.error[0] = 0;
        s->not_before = 0;
        s->batch = d->batch_id;
    }
    save_locked(d);
    platform_mutex_unlock(&d->mu);
    platform_cond_broadcast(&d->cv);
}

void downloads_remove(Downloads *d, u64 uid) {
    platform_mutex_lock(&d->mu);
    Dl_Slot *s = find_slot(d, uid);
    if (s) {
        char vid[16];
        memcpy(vid, s->j.vid, sizeof(vid));
        if (s->j.state == DL_ACTIVE) {
            for (u32 i = 0; i < DL_MAX_WORKERS; i++) {
                Dl_Worker *w = &d->workers[i];
                if (w->uid != uid) continue;
                w->cancel = true; /* the worker removes the job once its child is gone */
                if (w->proc) platform_process_kill(w->proc);
            }
            d->version++;
        } else {
            b32 unfinished = s->j.state != DL_DONE;
            job_remove_locked(d, uid);
            save_locked(d);
            if (unfinished) {
                char work[1100];
                work_path(d, vid, 0, work, sizeof(work));
                platform_remove_tree(work);
            }
        }
    }
    platform_mutex_unlock(&d->mu);
}

void downloads_clear_finished(Downloads *d) {
    platform_mutex_lock(&d->mu);
    for (u32 i = 0; i < d->count;) {
        u32 st = d->slots[i].j.state;
        if (st == DL_DONE || st == DL_FAILED) {
            if (st == DL_FAILED) {
                char work[1100];
                work_path(d, d->slots[i].j.vid, 0, work, sizeof(work));
                platform_remove_tree(work);
            }
            job_remove_locked(d, d->slots[i].j.uid);
        } else i++;
    }
    save_locked(d);
    platform_mutex_unlock(&d->mu);
}

void downloads_cancel_all(Downloads *d) {
    platform_mutex_lock(&d->mu);
    for (u32 i = 0; i < DL_MAX_WORKERS; i++) {
        Dl_Worker *w = &d->workers[i];
        if (!w->uid) continue;
        w->cancel = true; /* each worker removes its own job once the child is gone */
        if (w->proc) platform_process_kill(w->proc);
    }
    for (u32 i = 0; i < d->count;) {
        if (d->slots[i].j.state == DL_QUEUED) {
            char work[1100];
            work_path(d, d->slots[i].j.vid, 0, work, sizeof(work));
            platform_remove_tree(work);
            job_remove_locked(d, d->slots[i].j.uid);
        } else i++;
    }
    save_locked(d);
    platform_mutex_unlock(&d->mu);
}

u32 downloads_take_finished(Downloads *d) {
    platform_mutex_lock(&d->mu);
    u32 n = d->finished;
    d->finished = 0;
    platform_mutex_unlock(&d->mu);
    return n;
}
