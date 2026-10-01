/*
 * Downloader test. Standalone: includes the sources it needs.
 *
 *  1. Text helpers: artist/title/path cleaning, line parsers, persistence.
 *  2. The whole pipeline against fake `yt-dlp` / `ffmpeg` scripts (no
 *     network): search, queue, progress, tagging, duplicates, retry, cancel,
 *     and surviving a "closed app" (queue restored, job resumes).
 *
 * Everything is written under /tmp.
 */

#include "core/memory.c"
#include "core/string.c"
#include "platform/platform_posix.c"
#include "game/download.c"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)
#define CHECK_STR(a, b) do { const char *_a = (a), *_b = (b); \
    if (strcmp(_a, _b)) { printf("FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, _a, _b); g_fail++; } } while (0)

static char artist_of(const char *in, char *out) { dl_primary_artist(in, out, 160); return out[0]; }

static void test_text(void) {
    char o[256];
    artist_of("Boris Brejcha & Deniz Bul", o);          CHECK_STR(o, "Boris Brejcha");
    artist_of("Daft Punk feat. Pharrell Williams", o);   CHECK_STR(o, "Daft Punk");
    artist_of("Daft Punk ft Pharrell", o);               CHECK_STR(o, "Daft Punk");
    artist_of("AC/DC", o);                               CHECK_STR(o, "AC/DC");
    artist_of("20/20 LDN Recordings", o);                CHECK_STR(o, "20/20 LDN Recordings");
    artist_of("Earth, Wind & Fire", o);                  CHECK_STR(o, "Earth");
    artist_of("Jedi Mind Tricks (Vinnie Paz + Stoupe)", o); CHECK_STR(o, "Jedi Mind Tricks");
    artist_of("Sigma x Rudimental", o);                  CHECK_STR(o, "Sigma");
    artist_of("Band of Horses", o);                      CHECK_STR(o, "Band of Horses");
    artist_of("Mumford & Sons", o);                      CHECK_STR(o, "Mumford");
    artist_of("Kerri Chandler × Jamie Jones", o);        CHECK_STR(o, "Kerri Chandler");
    artist_of("  Solo  ", o);                            CHECK_STR(o, "Solo");

    dl_clean_channel("Daft Punk - Topic", o, sizeof(o));  CHECK_STR(o, "Daft Punk");
    dl_clean_channel("RihannaVEVO", o, sizeof(o));        CHECK_STR(o, "Rihanna");
    dl_clean_channel("VEVO", o, sizeof(o));               CHECK_STR(o, "VEVO");
    dl_clean_channel("Cercle", o, sizeof(o));             CHECK_STR(o, "Cercle");

    dl_clean_title("Daft Punk - One More Time (Official Video)", "Daft Punk", o, sizeof(o)); CHECK_STR(o, "One More Time");
    dl_clean_title("Get Lucky [Official Audio]", "Daft Punk", o, sizeof(o));                 CHECK_STR(o, "Get Lucky");
    dl_clean_title("Song (Official Video) (HD)", "", o, sizeof(o));                          CHECK_STR(o, "Song");
    dl_clean_title("Song (Live at Wembley)", "X", o, sizeof(o));                             CHECK_STR(o, "Song (Live at Wembley)");
    dl_clean_title("Song (Remastered 2021)", "X", o, sizeof(o));                             CHECK_STR(o, "Song (Remastered 2021)");
    dl_clean_title("Daft Punk", "Daft Punk", o, sizeof(o));                                  CHECK_STR(o, "Daft Punk");
    dl_clean_title("Daft Punk: Around the World (Lyrics)", "daft punk", o, sizeof(o));       CHECK_STR(o, "Around the World");
    dl_clean_title("(Official Video)", "", o, sizeof(o));                                    CHECK_STR(o, "(Official Video)");

    /* what counts as the same song */
    char k1[256], k2[256];
    dl_song_key("Hypnotize", k1, sizeof(k1));
    dl_song_key("Hypnotize (2007 Remaster)", k2, sizeof(k2));            CHECK_STR(k1, k2);
    dl_song_key("Hypnotize - Remastered 2014", k2, sizeof(k2));          CHECK_STR(k1, k2);
    dl_song_key("Hypnotize [Explicit] (Official Video)", k2, sizeof(k2)); CHECK_STR(k1, k2);
    dl_song_key("Hypnotize (feat. Puff Daddy & Mase)", k2, sizeof(k2));  CHECK_STR(k1, k2);
    dl_song_key("Hypnotize feat. Puff Daddy", k2, sizeof(k2));           CHECK_STR(k1, k2);
    dl_song_key("Notorious B.I.G. [2005 Remaster] (feat. Lil' Kim & Puff Daddy)", k1, sizeof(k1));
    dl_song_key("Notorious B.I.G.", k2, sizeof(k2));                     CHECK_STR(k1, k2);
    dl_song_key("Hypnotize (Live)", k2, sizeof(k2));                     CHECK_STR(k2, "hypnotize");
    dl_song_key("Hypnotize (Remix)", k2, sizeof(k2));                    CHECK_STR(k2, "hypnotize");
    dl_song_key("Hypnotize - Live at Wembley", k2, sizeof(k2));          CHECK_STR(k2, "hypnotize");
    dl_song_key("God's Bathroom Floor (live in Montreal)", k1, sizeof(k1));
    dl_song_key("Gods Bathroom Floor (instrumental)", k2, sizeof(k2)); CHECK_STR(k1, k2);
    dl_song_key("Loyal To The Game (DJ Quik Remix (Explicit))", k1, sizeof(k1));
    CHECK_STR(k1, "loyaltothegame");
    dl_song_key("The Woman With the Tattooed Hands Instrumental", k1, sizeof(k1));
    CHECK_STR(k1, "thewomanwiththetattooedhands");
    dl_song_key("Sweets (Soda Pop) (extended mix)", k1, sizeof(k1));
    dl_song_key("Sweets (Soda Pop)", k2, sizeof(k2)); CHECK_STR(k1, k2);
    dl_song_key("Pista (Fresh Start)", k1, sizeof(k1));
    dl_song_key("Pista (Great Start)", k2, sizeof(k2)); CHECK(strcmp(k1, k2) != 0);
    dl_song_key("Live Forever", k1, sizeof(k1)); CHECK_STR(k1, "liveforever");
    dl_song_key("Song (Live", k1, sizeof(k1)); CHECK_STR(k1, "songlive");
    dl_song_key("Song", k1, 0); /* zero capacity writes nothing */
    dl_song_key("(Remastered)", k2, sizeof(k2));                         CHECK_STR(k2, "remastered");
    dl_artist_key("The Notorious B.I.G.", k1, sizeof(k1));
    dl_artist_key("notorious big", k2, sizeof(k2));                      CHECK_STR(k1, k2); CHECK_STR(k1, "notoriousbig");
    dl_artist_key("Daft Punk feat. Pharrell", k1, sizeof(k1));           CHECK_STR(k1, "daftpunk");
    dl_artist_key("The Who", k1, sizeof(k1));                            CHECK_STR(k1, "who");
    CHECK(dl_artist_plausible("One-T", "", "One T + Cool T The magic key (Official Music Video)"));
    CHECK(dl_artist_plausible("The Notorious B.I.G", "notorious big", "Hypnotize"));
    CHECK(dl_artist_plausible("Daft Punk", "Daft Punk - Topic", "x"));
    CHECK(!dl_artist_plausible("One-T", "billibvb", "The Magic Key (Lyrics)"));
    CHECK(!dl_artist_plausible("One-T", "", "The Magic Key"));
    CHECK(!dl_artist_plausible("Air", "Hair", "Something"));             /* too short to match inside another name */

    dl_path_segment("AC/DC", o, sizeof(o));               CHECK_STR(o, "AC - DC");
    dl_path_segment("What?", o, sizeof(o));               CHECK_STR(o, "What");
    dl_path_segment("  a   b  ", o, sizeof(o));           CHECK_STR(o, "a b");
    dl_path_segment("con", o, sizeof(o));                 CHECK_STR(o, "_con");
    dl_path_segment("..", o, sizeof(o));                  CHECK_STR(o, "_");
    dl_path_segment(".", o, sizeof(o));                   CHECK_STR(o, "_");
    dl_path_segment("...Baby One More Time", o, sizeof(o)); CHECK_STR(o, "Baby One More Time"); /* not hidden */
    dl_path_segment(".hidden", o, sizeof(o));             CHECK_STR(o, "hidden");
    dl_path_segment("", o, sizeof(o));                    CHECK_STR(o, "_");
    dl_path_segment("Ünï Cödé 音楽", o, sizeof(o));        CHECK_STR(o, "Ünï Cödé 音楽");
    {
        char longname[400];
        memset(longname, 'a', sizeof(longname)); longname[399] = 0;
        dl_path_segment(longname, o, sizeof(o));
        CHECK(strlen(o) <= 150);
        memset(longname, 0, sizeof(longname));
        for (u32 i = 0; i < 120; i++) strcat(longname, "音"); /* 3 bytes each */
        dl_path_segment(longname, o, sizeof(o));
        CHECK(strlen(o) <= 150 && strlen(o) % 3 == 0); /* never cut mid-character */
    }
    dl_dest_path("/m/Downloads", "Daft Punk", "One/Two", o, sizeof(o));  CHECK_STR(o, "/m/Downloads/Daft Punk/One - Two.mp3");

    char f[64];
    strcpy(f, " a\tb\nc "); dl_clean_field(f);             CHECK_STR(f, "a b c");
}

static void test_parsers(void) {
    Dl_Result r;
    CHECK(dl_parse_result_line("FGBhQbmPwH8\x1f" "Daft Punk - One More Time (Official Video)\x1f" "Daft Punk\x1f" "322\x1f" "619317374", &r));
    CHECK_STR(r.vid, "FGBhQbmPwH8");
    CHECK_STR(r.title, "Daft Punk - One More Time (Official Video)");
    CHECK_STR(r.channel, "Daft Punk");
    CHECK(r.duration_s == 322 && r.views == 619317374ull);
    /* missing fields (yt-dlp prints NA) */
    CHECK(dl_parse_result_line("khnokW3Mw24\x1f" "Instant Crush\x1f" "NA\x1f" "NA\x1f" "NA", &r));
    CHECK(r.channel[0] == 0 && r.duration_s == 0 && r.views == 0);
    /* a title containing characters that look like separators is still one field */
    CHECK(dl_parse_result_line("khnokW3Mw24\x1f" "A | B - C", &r) && !strcmp(r.title, "A | B - C"));
    CHECK(!dl_parse_result_line("short\x1f" "T", &r));              /* bad id */
    CHECK(!dl_parse_result_line("khnokW3Mw24\x1f" "NA", &r));       /* no title */
    CHECK(!dl_parse_result_line("WARNING: something", &r));
    CHECK(!dl_parse_result_line("", &r));

    f32 fr = -1;
    CHECK(dl_parse_progress_line("OBP 1024 2048 NA", &fr) && fr > 0.499f && fr < 0.501f);
    CHECK(dl_parse_progress_line("OBP 512 NA 2048", &fr) && fr > 0.249f && fr < 0.251f);
    CHECK(dl_parse_progress_line("OBP 9999 100 NA", &fr) && fr == 1.0f);
    CHECK(!dl_parse_progress_line("OBP NA NA NA", &fr));
    CHECK(!dl_parse_progress_line("OBP 1 NA NA", &fr));
    CHECK(!dl_parse_progress_line("[download] 50%", &fr));

    /* YouTube Music song: real track/artist/album/year */
    Dl_Job j = {0};
    strcpy(j.artist, "hint");
    CHECK(dl_parse_meta_line("OBM\x1f" "Thunderstorm\x1f" "Thunderstorm\x1f" "Boris Brejcha & Deniz Bul\x1f" "Thunderstorm EP\x1f"
                             "2020\x1f" "2023\x1f" "455\x1f" "NA\x1f" "Boris Brejcha\x1f" "3\x1f" "Boris Brejcha", &j));
    CHECK_STR(j.title, "Thunderstorm"); CHECK_STR(j.artist, "Boris Brejcha"); CHECK_STR(j.album, "Thunderstorm EP");
    CHECK(j.year == 2020 && j.duration_s == 455 && j.track == 3 && j.genre[0] == 0);
    /* plain video: title cleaned, artist from the channel, upload year */
    Dl_Job v = {0};
    CHECK(dl_parse_meta_line("OBM\x1f" "NA\x1f" "Daft Punk - One More Time (Official Video)\x1f" "NA\x1f" "NA\x1f" "NA\x1f"
                             "2009\x1f" "322\x1f" "NA\x1f" "Daft Punk - Topic\x1f" "NA\x1f" "Daft Punk", &v));
    CHECK_STR(v.title, "One More Time"); CHECK_STR(v.artist, "Daft Punk"); CHECK(v.year == 2009 && v.album[0] == 0);
    CHECK(!dl_parse_meta_line("OBM\x1f" "too\x1f" "few", &v));
    CHECK(!dl_parse_meta_line("OBP 1 2 3", &v));
}

static void test_enrich_parse(void) {
    Dl_Result r;
#define T_US "\x1f"
    CHECK(dl_parse_enrich_line("OBE" T_US "abcdefghijk" T_US "The Notorious B.I.G., Guest" T_US "230" T_US "Hypnotize (2014 Remaster)", &r));
    CHECK_STR(r.vid, "abcdefghijk"); CHECK_STR(r.artist, "The Notorious B.I.G."); CHECK(r.duration_s == 230);
    CHECK_STR(r.title, "Hypnotize (2014 Remaster)");
    CHECK(dl_parse_enrich_line("OBE" T_US "abcdefghijk" T_US "NA" T_US "NA" T_US "Song", &r) && !r.artist[0] && !r.duration_s);
    CHECK(!dl_parse_enrich_line("OBE" T_US "short" T_US "A" T_US "1" T_US "T", &r));
    CHECK(!dl_parse_enrich_line("abcdefghijk" T_US "A" T_US "1" T_US "T", &r));
#undef T_US
}

static void test_persistence(void) {
    Dl_Job a[3] = {0};
    a[0].uid = 7; a[0].state = DL_DONE; memcpy(a[0].vid, "FGBhQbmPwH8", 12);
    strcpy(a[0].title, "One More Time"); strcpy(a[0].artist, "Daft Punk"); strcpy(a[0].album, "Discovery");
    strcpy(a[0].path, "/home/me/Music/Downloads/Daft Punk/One More Time.mp3"); a[0].year = 2001; a[0].track = 1; a[0].duration_s = 321;
    a[1].uid = 8; a[1].state = DL_ACTIVE; memcpy(a[1].vid, "khnokW3Mw24", 12); strcpy(a[1].title, "Instant Crush");
    a[1].progress = 0.5f; a[1].metadata_ready = true;
    a[2].uid = 9; a[2].state = DL_FAILED; a[2].attempts = 3; memcpy(a[2].vid, "Jb6gcoR266U", 12);
    strcpy(a[2].title, "Around the World"); strcpy(a[2].error, "Video unavailable");

    char buf[8192];
    u64 n = dl_jobs_format(a, 3, 10, buf, sizeof(buf));
    CHECK(n > 0 && n < sizeof(buf));
    Dl_Job b[8];
    u64 next = 0;
    u32 cnt = dl_jobs_parse((Core_String){ .str = (u8 *)buf, .len = n }, b, 8, &next);
    CHECK(cnt == 3 && next == 10);
    CHECK(b[0].uid == 7 && b[0].state == DL_DONE && b[0].year == 2001 && b[0].track == 1 && b[0].duration_s == 321);
    CHECK_STR(b[0].path, a[0].path); CHECK_STR(b[0].album, "Discovery"); CHECK(b[0].progress == 1.0f);
    CHECK(b[1].state == DL_QUEUED && b[1].progress == 0.0f && b[1].metadata_ready);            /* running -> queued */
    CHECK(b[2].state == DL_FAILED && b[2].attempts == 3 && !strcmp(b[2].error, "Video unavailable"));
    /* Old queues load without claiming their search hints are full metadata. */
    CHECK(dl_jobs_parse(core_str("offbeat-downloads 1\njob\t1\t1\t0\tabcdefghijk\t0\t0\t0\tSong\tArtist\t\t\t\t\n"), b, 8, &next) == 1);
    CHECK(b[0].state == DL_QUEUED && !b[0].metadata_ready);
    /* truncated output never yields a half-written job */
    u32 cnt2 = dl_jobs_parse((Core_String){ .str = (u8 *)buf, .len = n - 20 }, b, 8, &next);
    CHECK(cnt2 == 2 || cnt2 == 3);
    /* garbage and unknown lines are ignored */
    CHECK(dl_jobs_parse(core_str("junk\njob\tx\n\njob\t1\t0\t0\tbadid\t0\t0\t0\ta\tb\tc\td\te\tf\n"), b, 8, &next) == 0);
    /* output too small for all jobs: writes whole lines only */
    char tiny[260];
    u64 tn = dl_jobs_format(a, 3, 10, tiny, sizeof(tiny));
    CHECK(tn < sizeof(tiny));
    u32 tc = dl_jobs_parse((Core_String){ .str = (u8 *)tiny, .len = tn }, b, 8, &next);
    CHECK(tc < 3 && (tc == 0 || b[0].uid == 7));
}

/* ---- pipeline with fake tools ---- */

static const char *FAKE_YTDLP =
    "#!/usr/bin/env bash\n"
    "# Fake yt-dlp: a search prints results, a download writes <id>.mp3/.jpg into the -o folder.\n"
    "log=\"$FAKE_DIR/ytdlp.log\"\n"
    "echo \"$*\" >> \"$log\"\n"
    "args=(\"$@\")\n"
    "for a in \"$@\"; do [[ \"$a\" == --flat-playlist ]] && search=1; done\n"
    "target=\"${args[${#args[@]}-1]}\"\n"
    "for a in \"$@\"; do [[ \"$a\" == --skip-download ]] && lookup=1; done\n"
    "if [[ -n \"$lookup\" ]]; then   # artist + real title of one YouTube Music hit\n"
    "  [[ -f \"$FAKE_DIR/enrich_slow\" ]] && { touch \"$FAKE_DIR/enrich_started\"; sleep 30; }\n"
    "  id=\"${target##*v=}\"; n=$((10#${id#vid}))\n"
    "  title=\"Song $n (2011 Remaster)\"\n"
    "  [[ -f \"$FAKE_DIR/enrich_dup\" && $n == 3 ]] && title=\"Song 2 - Remastered 2009\"\n"
    "  printf 'OBE\\x1f%s\\x1f%s\\x1f%d\\x1f%s\\n' \"$id\" 'Fake Artist' 200 \"$title\"\n"
    "  exit 0\n"
    "fi\n"
    "if [[ -n \"$search\" ]]; then\n"
    "  if [[ \"$target\" == *music.youtube.com* ]]; then n=5; else n=3; fi\n"
    "  [[ \"$target\" == *nothing* ]] && exit 0\n"
    "  [[ \"$target\" == *boom* ]] && { echo 'ERROR: Unable to download webpage: boom' >&2; exit 1; }\n"
    "  for i in $(seq 1 $n); do\n"
    "    printf 'vid%08d\\x1f%s\\x1f%s\\x1f%d\\x1f%d\\n' $i \"Song $i (Official Video)\" 'Some Channel - Topic' $((100+i)) $((i*1000))\n"
    "  done\n"
    "  # a repeated id and a repeated title must be dropped\n"
    "  printf 'vid%08d\\x1f%s\\x1f%s\\x1f%d\\x1f%d\\n' 1 'Song 1 (Official Video)' 'x' 1 1\n"
    "  exit 0\n"
    "fi\n"
    "out=''\n"
    "for ((i=0;i<${#args[@]};i++)); do [[ \"${args[$i]}\" == -o ]] && out=\"${args[$i+1]}\"; done\n"
    "id=\"${target##*v=}\"\n"
    "dir=\"$(dirname \"$out\")\"\n"
    "[[ -f \"$FAKE_DIR/fail_$id\" ]] && { echo 'WARNING: noise'; echo \"ERROR: [youtube] $id: Video unavailable\" >&2; exit 1; }\n"
    "printf 'OBM\\x1f%s\\x1f%s\\x1f%s\\x1f%s\\x1f%s\\x1f%s\\x1f%s\\x1f%s\\x1f%s\\x1f%s\\x1f%s\\n' \"Title $id\" \"Title $id\" 'Fake Artist & Guest' 'Fake Album' 2020 2021 200 NA 'Fake Artist' 4 'Fake Artist'\n"
    "[[ -f \"$dir/$id.webm.part\" ]] && echo \"resumed:$id\" >> \"$log\"\n"
    "echo partial-bytes > \"$dir/$id.webm.part\"\n"
    "slow=${FAKE_SLOW:-0}\n"
    "[[ -f \"$FAKE_DIR/slow\" ]] && slow=1\n"
    "for p in 100 2000 40000 500000 1000000; do echo \"OBP $p 1000000 NA\"; [[ $slow == 1 ]] && sleep 0.4 || sleep 0.02; done\n"
    "[[ $slow == 1 ]] && sleep 30\n"
    "echo audio-bytes > \"$dir/$id.mp3\"\n"
    "echo cover-bytes > \"$dir/$id.jpg\"\n"
    "rm -f \"$dir/$id.webm.part\"\n"
    "exit 0\n";

static const char *FAKE_FFMPEG =
    "#!/usr/bin/env bash\n"
    "# Fake ffmpeg: log the tags and write the output file (last argument).\n"
    "echo \"$@\" >> \"$FAKE_DIR/ffmpeg.log\"\n"
    "args=(\"$@\")\n"
    "out=\"${args[${#args[@]}-1]}\"\n"
    "cat \"${args[5]}\" > \"$out\"; echo \"$*\" >> \"$out\"\n"
    "[[ -f \"$FAKE_DIR/tag_slow\" ]] && { touch \"$FAKE_DIR/tag_started\"; sleep 30; }\n"
    "exit 0\n";

static void write_script(const char *path, const char *body) {
    FILE *f = fopen(path, "w");
    fputs(body, f);
    fclose(f);
    chmod(path, 0755);
}

static u64 read_file(const char *path, char *buf, u64 cap) {
    FILE *f = fopen(path, "rb");
    if (!f) { buf[0] = 0; return 0; }
    u64 n = fread(buf, 1, cap - 1, f);
    buf[n] = 0;
    fclose(f);
    return n;
}

static b32 wait_for(Downloads *d, b32 (*pred)(Downloads *, void *), void *arg, f64 secs) {
    f64 end = platform_time_seconds() + secs;
    while (platform_time_seconds() < end) {
        if (pred(d, arg)) return true;
        platform_sleep(0.02);
    }
    return pred(d, arg);
}

static b32 search_finished(Downloads *d, void *arg) { CORE_UNUSED(arg); return downloads_search_info(d).state >= DL_SEARCH_DONE; }
static b32 all_settled(Downloads *d, void *arg) {
    CORE_UNUSED(arg);
    Dl_Summary s = downloads_summary(d);
    return s.queued == 0 && s.active == 0;
}
static b32 has_active(Downloads *d, void *arg) { CORE_UNUSED(arg); return downloads_summary(d).active > 0; }
static b32 done_count_is(Downloads *d, void *arg) { return downloads_summary(d).done == (u32)(uintptr_t)arg; }
static b32 file_exists(Downloads *d, void *arg) { CORE_UNUSED(d); return platform_file_info(arg).exists; }

static void test_pipeline(void) {
    const char *root = "/tmp/offbeat-dl-test";
    platform_remove_tree(root);
    char dir[256], script[256], state[256], work[256], dest[256], p[512];
    snprintf(dir, sizeof(dir), "%s/fake", root);
    snprintf(state, sizeof(state), "%s/downloads", root);
    snprintf(work, sizeof(work), "%s/work", root);
    snprintf(dest, sizeof(dest), "%s/Music/Downloads", root);
    platform_make_dirs(dir);
    setenv("FAKE_DIR", dir, 1);
    setenv("OFFBEAT_DL_FAST", "1", 1);
    snprintf(script, sizeof(script), "%s/yt-dlp", dir);   write_script(script, FAKE_YTDLP);   setenv("OFFBEAT_YTDLP", script, 1);
    snprintf(script, sizeof(script), "%s/ffmpeg", dir);   write_script(script, FAKE_FFMPEG);  setenv("OFFBEAT_FFMPEG", script, 1);

    Downloads *d = downloads_create(state, work);
    downloads_configure(d, dest, "", 2);
    Dl_Tools tools = downloads_tools(d);
    CHECK(tools.ytdlp);

    /* song search: the music results first (5), then the videos (3 more lines, all duplicate ids here) */
    downloads_search(d, DL_QUERY_SONG, "daft punk one more time");
    CHECK(wait_for(d, search_finished, 0, 10));
    Dl_SearchInfo si = downloads_search_info(d);
    CHECK(si.state == DL_SEARCH_DONE && si.count == 5 && si.kind == DL_QUERY_SONG);
    Dl_Result r[8];
    for (u32 i = 0; i < si.count; i++) CHECK(downloads_result(d, i, &r[i]));
    CHECK_STR(r[0].vid, "vid00000001"); CHECK(r[0].views == 1000 && r[0].music);
    /* the lookup filled in the artist, the real title and the duration */
    CHECK_STR(r[0].artist, "Fake Artist"); CHECK_STR(r[0].title, "Song 1 (2011 Remaster)"); CHECK(r[0].duration_s == 200);
    CHECK(!downloads_result(d, 5, &r[5]));

    /* artist search: music.youtube.com filter link, 5 results */
    downloads_search(d, DL_QUERY_ARTIST, "Boris Brejcha");
    CHECK(wait_for(d, search_finished, 0, 10));
    si = downloads_search_info(d);
    CHECK(si.state == DL_SEARCH_DONE && si.count == 5 && si.kind == DL_QUERY_ARTIST);
    char log[8192];
    read_file("/tmp/offbeat-dl-test/fake/ytdlp.log", log, sizeof(log));
    CHECK(strstr(log, "music.youtube.com/search?q=Boris+Brejcha&sp=EgWKAQIIAWoKEAoQAxAEEAkQBQ%3D%3D"));
    CHECK(strstr(log, "--playlist-end 200"));

    /* no results / failing search surface as FAILED with a message */
    downloads_search(d, DL_QUERY_SONG, "nothing");
    CHECK(wait_for(d, search_finished, 0, 10));
    si = downloads_search_info(d);
    CHECK(si.state == DL_SEARCH_FAILED && si.error[0] && si.count == 0);
    downloads_search(d, DL_QUERY_SONG, "boom");
    CHECK(wait_for(d, search_finished, 0, 10));
    si = downloads_search_info(d);
    CHECK(si.state == DL_SEARCH_FAILED && strstr(si.error, "Could not reach YouTube")); /* translated from the tool's message */

    /* a newer search replaces an older one */
    downloads_search(d, DL_QUERY_SONG, "first");
    downloads_search(d, DL_QUERY_ARTIST, "second");
    CHECK(wait_for(d, search_finished, 0, 10));
    si = downloads_search_info(d);
    CHECK(si.kind == DL_QUERY_ARTIST && si.count == 5 && !strcmp(si.query, "second"));

    /* download three songs from the artist list */
    Dl_Result pick[3];
    for (u32 i = 0; i < 3; i++) downloads_result(d, i, &pick[i]);
    CHECK(downloads_enqueue(d, pick, 3, "Hint Artist") == 3);
    CHECK(downloads_enqueue(d, pick, 3, "Hint Artist") == 0);            /* duplicates are skipped */
    Dl_Summary sum = downloads_summary(d);
    CHECK(sum.total == 3 && sum.batch_total == 3);
    CHECK(wait_for(d, all_settled, 0, 30));
    sum = downloads_summary(d);
    CHECK(sum.done == 3 && sum.failed == 0 && sum.progress > 0.999f);
    CHECK(downloads_take_finished(d) == 3 && downloads_take_finished(d) == 0);

    Dl_Job j;
    CHECK(downloads_job(d, 0, &j));
    CHECK(j.state == DL_DONE && j.progress == 1.0f);
    CHECK_STR(j.artist, "Fake Artist");                                   /* primary artist from yt-dlp, not the hint */
    CHECK_STR(j.title, "Title vid00000001");
    snprintf(p, sizeof(p), "%s/Fake Artist/Title vid00000001.mp3", dest);
    CHECK_STR(j.path, p);
    CHECK(platform_file_info(p).exists);
    char tagged[4096];
    read_file(p, tagged, sizeof(tagged));
    CHECK(strstr(tagged, "audio-bytes"));                                 /* first input is the audio */
    CHECK(strstr(tagged, "title=Title vid00000001") && strstr(tagged, "artist=Fake Artist"));
    CHECK(strstr(tagged, "album=Fake Album") && strstr(tagged, "date=2020") && strstr(tagged, "track=4"));
    CHECK(strstr(tagged, "youtube_id=vid00000001") && strstr(tagged, "source_url=https://www.youtube.com/watch?v=vid00000001"));
    CHECK(strstr(tagged, "-id3v2_version 3") && strstr(tagged, "attached_pic") && strstr(tagged, ".jpg"));
    char wp[512]; snprintf(wp, sizeof(wp), "%s/vid00000001", work);
    CHECK(!platform_file_info(wp).exists);                                /* scratch folder cleaned up */

    /* yt-dlp was asked for the right things */
    read_file("/tmp/offbeat-dl-test/fake/ytdlp.log", log, sizeof(log));
    CHECK(strstr(log, "-x --audio-format mp3") && strstr(log, "--write-thumbnail") && strstr(log, "watch?v=vid00000002"));
    CHECK(strstr(log, "--no-playlist"));

    /* asking again for a finished song does nothing; deleting its file makes it downloadable again */
    CHECK(downloads_enqueue(d, pick, 1, "Hint Artist") == 0);
    Dl_State st;
    CHECK(downloads_has(d, "vid00000001", &st) && st == DL_DONE);
    platform_file_remove(j.path);
    platform_sleep(2.1);                                                  /* the UI-facing check is cached for 2 s */
    CHECK(!downloads_has(d, "vid00000001", &st));
    CHECK(downloads_enqueue(d, pick, 1, "Hint Artist") == 1);
    CHECK(wait_for(d, all_settled, 0, 30));
    CHECK(downloads_summary(d).done == 3 && platform_file_info(j.path).exists);

    /* a file already on disk is not downloaded again (yt-dlp is stopped after the metadata line) */
    downloads_clear_finished(d);
    CHECK(downloads_summary(d).total == 0);
    CHECK(downloads_enqueue(d, pick, 1, 0) == 1);
    CHECK(wait_for(d, all_settled, 0, 30));
    CHECK(downloads_job(d, 0, &j) && j.state == DL_DONE && j.existed);

    /* failures retry, then are given up on, with the tool's message */
    snprintf(p, sizeof(p), "%s/fail_vid00000004", dir);
    write_script(p, "x");
    Dl_Result bad = {0};
    strcpy(bad.vid, "vid00000004"); strcpy(bad.title, "Broken");
    CHECK(downloads_enqueue(d, &bad, 1, "Nobody") == 1);
    f64 t0 = platform_time_seconds();
    while (platform_time_seconds() - t0 < 40) {
        Dl_Summary s = downloads_summary(d);
        if (s.failed) break;
        platform_sleep(0.1);
    }
    sum = downloads_summary(d);
    CHECK(sum.failed == 1);
    CHECK(downloads_job(d, 1, &j) && j.state == DL_FAILED && j.attempts == 3 && !strcmp(j.error, "Video unavailable"));
    /* the file the tool would need is now there: retry succeeds */
    platform_file_remove(p);
    downloads_retry(d, j.uid);
    CHECK(wait_for(d, all_settled, 0, 30));
    CHECK(downloads_job(d, 1, &j) && j.state == DL_DONE);
    CHECK_STR(j.artist, "Fake Artist");
    downloads_clear_finished(d);

    /* ---- closing the app mid-download and coming back ---- */
    snprintf(p, sizeof(p), "%s/slow", dir);
    write_script(p, "x");
    Dl_Result more[2];
    downloads_search(d, DL_QUERY_ARTIST, "again");
    CHECK(wait_for(d, search_finished, 0, 10));
    downloads_result(d, 4, &more[0]); more[1] = more[0]; strcpy(more[1].vid, "vidRESUME01"); strcpy(more[1].title, "Another");
    CHECK(downloads_enqueue(d, more, 2, "Hint") == 2);
    CHECK(wait_for(d, has_active, 0, 10));
    platform_sleep(1.0);                                                  /* into the slow part */
    f64 t1 = platform_time_seconds();
    downloads_destroy(d);                                                 /* kills the child, joins */
    CHECK(platform_time_seconds() - t1 < 5.0);
    char partial[512];
    snprintf(partial, sizeof(partial), "%s/%s/%s.webm.part", work, more[0].vid, more[0].vid);
    CHECK(platform_file_info(partial).size > 0);

    d = downloads_create(state, work);
    platform_sleep(0.7); /* initialization can take time; no unconfigured job may start */
    CHECK(downloads_summary(d).queued == 2 && downloads_summary(d).active == 0);
    CHECK(platform_file_info(partial).size > 0);
    downloads_configure(d, dest, "", 2);
    sum = downloads_summary(d);
    CHECK(sum.total == 2 && sum.queued + sum.active == 2 && sum.done == 0); /* workers may already have picked one up */
    platform_file_remove(p);                                              /* the network is fast again */
    CHECK(wait_for(d, all_settled, 0, 30));
    sum = downloads_summary(d);
    CHECK(sum.done == 2 && sum.failed == 0);
    char resume_log[32000];
    snprintf(p, sizeof(p), "%s/ytdlp.log", dir);
    read_file(p, resume_log, sizeof(resume_log));
    CHECK(strstr(resume_log, "resumed:vid00000005") != 0);

    /* pausing holds queued jobs back; removing a running one cancels it */
    downloads_set_paused(d, true);
    Dl_Result extra[2];
    downloads_result(d, 0, &extra[0]);                                    /* results from the last search */
    strcpy(extra[0].vid, "vidPAUSE001");
    CHECK(downloads_enqueue(d, &extra[0], 1, "Hint") == 1);
    platform_sleep(1.0);
    sum = downloads_summary(d);
    CHECK(sum.queued == 1 && sum.active == 0 && sum.paused);
    downloads_set_paused(d, false);
    snprintf(p, sizeof(p), "%s/slow", dir); write_script(p, "x");
    CHECK(wait_for(d, has_active, 0, 10));
    platform_sleep(0.5);
    CHECK(downloads_job(d, downloads_job_count(d) - 1, &j) && j.state == DL_ACTIVE);
    downloads_remove(d, j.uid);
    CHECK(wait_for(d, all_settled, 0, 10));
    CHECK(downloads_summary(d).total == 2);
    platform_file_remove(p);

    /* cancel everything: queued jobs vanish, the running one is stopped */
    {
        Dl_Result many[4];
        for (u32 i = 0; i < 4; i++) { many[i] = extra[0]; snprintf(many[i].vid, sizeof(many[i].vid), "vidCANCEL%02u", i); snprintf(many[i].title, sizeof(many[i].title), "Cancel %u", i); }
        snprintf(p, sizeof(p), "%s/slow", dir); write_script(p, "x");
        u32 before = downloads_summary(d).total;
        CHECK(downloads_enqueue(d, many, 4, "Hint") == 4);
        CHECK(wait_for(d, has_active, 0, 10));
        platform_sleep(0.5);
        downloads_cancel_all(d);
        CHECK(wait_for(d, all_settled, 0, 10));
        CHECK(downloads_summary(d).total == before && downloads_summary(d).active == 0);
        platform_file_remove(p);
    }

    downloads_destroy(d);
    platform_remove_tree(root);
    extra[1] = extra[0];
}

/* The library: songs are filed into the artist's folder, duplicates are never
   fetched, and leftovers of an earlier run are cleaned up. */
static void test_library(void) {
    const char *root = "/tmp/offbeat-dl-test-lib";
    platform_remove_tree(root);
    char dir[256], script[256], state[256], work[256], music[256], dest[256], p[512];
    snprintf(dir, sizeof(dir), "%s/fake", root);
    snprintf(state, sizeof(state), "%s/downloads", root);
    snprintf(work, sizeof(work), "%s/work", root);
    snprintf(music, sizeof(music), "%s/Music", root);
    snprintf(dest, sizeof(dest), "%s/Music/Downloads", root);
    platform_make_dirs(dir);
    setenv("FAKE_DIR", dir, 1);
    snprintf(script, sizeof(script), "%s/yt-dlp", dir);   write_script(script, FAKE_YTDLP);   setenv("OFFBEAT_YTDLP", script, 1);
    snprintf(script, sizeof(script), "%s/ffmpeg", dir);   write_script(script, FAKE_FFMPEG);  setenv("OFFBEAT_FFMPEG", script, 1);

    /* the library: "Fake Artist" already lives under Rock, with one song */
    char artist_dir[300], have[400];
    snprintf(artist_dir, sizeof(artist_dir), "%s/Rock/Fake Artist", music);
    platform_make_dirs(artist_dir);
    platform_make_dirs(dest);
    snprintf(have, sizeof(have), "%s/Title vid00000009 (2005 Remaster).mp3", artist_dir);
    write_script(have, "x");
    chmod(have, 0644);

    /* leftovers of an earlier run: a scratch folder nobody needs, and a half-written file */
    char stale[400], stale_marker[500], half[400];
    snprintf(stale, sizeof(stale), "%s/vidSTALE0001", work);
    platform_make_dirs(stale);
    snprintf(half, sizeof(half), "%s/Crashed.mp3.part", artist_dir);
    write_script(half, "x");
    snprintf(stale_marker, sizeof(stale_marker), "%s/tagging", stale);
    char note[500]; snprintf(note, sizeof(note), "%s\n", half);
    platform_file_write_all(stale_marker, note, strlen(note));

    Downloads *d = downloads_create(state, work);
    downloads_configure(d, dest, music, 2);
    CHECK(!platform_file_info(stale).exists);
    CHECK(!platform_file_info(half).exists);
    CHECK(platform_file_info(artist_dir).exists);                         /* the artist folder itself stays */

    /* two hits for one song: the second is dropped once the lookup shows they are the same */
    snprintf(p, sizeof(p), "%s/enrich_dup", dir);
    write_script(p, "x");
    downloads_search(d, DL_QUERY_SONG, "dups");
    CHECK(wait_for(d, search_finished, 0, 10));
    Dl_SearchInfo si = downloads_search_info(d);
    CHECK(si.state == DL_SEARCH_DONE && si.count == 4);
    Dl_Result r;
    CHECK(downloads_result(d, 2, &r) && strcmp(r.vid, "vid00000003") != 0);
    platform_file_remove(p);

    /* the same song under two titles in one batch is fetched once */
    downloads_search(d, DL_QUERY_ARTIST, "Fake Artist");
    CHECK(wait_for(d, search_finished, 0, 10));
    Dl_Result pick[2];
    CHECK(downloads_result(d, 0, &pick[0]) && downloads_result(d, 1, &pick[1]));
    strcpy(pick[1].title, "Song 1 (2009 Remaster)");
    CHECK(downloads_enqueue(d, pick, 2, "Fake Artist") == 1);

    /* a new song lands in the artist's library folder, not the Downloads folder */
    CHECK(wait_for(d, all_settled, 0, 30));
    Dl_Job j;
    CHECK(downloads_job(d, 0, &j) && j.state == DL_DONE);
    snprintf(p, sizeof(p), "%s/Title vid00000001.mp3", artist_dir);
    CHECK_STR(j.path, p);
    CHECK(platform_file_info(p).exists);
    snprintf(p, sizeof(p), "%s/Fake Artist", dest);
    CHECK(!platform_file_info(p).exists);

    /* a song the library has (under a remaster title) is not fetched again */
    Dl_Result have_it = pick[0];
    strcpy(have_it.vid, "vid00000009");
    strcpy(have_it.title, "Title vid00000009");
    CHECK(downloads_enqueue(d, &have_it, 1, "Fake Artist") == 1);
    CHECK(wait_for(d, all_settled, 0, 30));
    CHECK(downloads_job(d, 1, &j) && j.state == DL_DONE && j.existed);
    CHECK(strstr(j.path, "Title vid00000009 (2005 Remaster).mp3") != 0);
    char vwork[400]; snprintf(vwork, sizeof(vwork), "%s/vid00000009", work);
    CHECK(!platform_file_info(vwork).exists);

    /* the same song again under another video id: skipped without a download */
    Dl_Result again = pick[0];
    strcpy(again.vid, "vid00000077");
    strcpy(again.title, "Title vid00000001 (Remastered 2011)");
    CHECK(downloads_enqueue(d, &again, 1, "Fake Artist") == 0);

    /* an artist the library doesn't know goes to the new-artists folder; no scratch left behind */
    downloads_clear_finished(d);
    Dl_Result other = pick[0];
    strcpy(other.vid, "vid00000042");
    strcpy(other.title, "Brand New");
    snprintf(p, sizeof(p), "%s/fail_vid00000042", dir);
    CHECK(downloads_enqueue(d, &other, 1, "Nobody Known") == 1);
    CHECK(wait_for(d, all_settled, 0, 30));
    CHECK(downloads_job(d, 0, &j) && j.state == DL_DONE);
    /* (the fake reports "Fake Artist" for every video: same folder, but the scratch folder must be gone) */
    char w42[400]; snprintf(w42, sizeof(w42), "%s/vid00000042", work);
    CHECK(!platform_file_info(w42).exists);

    /* giving up on a job removes its scratch folder, and the empty artist folder it made */
    snprintf(p, sizeof(p), "%s/fail_vid00000050", dir);
    write_script(p, "x");
    Dl_Result bad = pick[0];
    strcpy(bad.vid, "vid00000050"); strcpy(bad.title, "Broken");
    CHECK(downloads_enqueue(d, &bad, 1, "Nobody") == 1);
    f64 t0 = platform_time_seconds();
    while (platform_time_seconds() - t0 < 40 && !downloads_summary(d).failed) platform_sleep(0.1);
    CHECK(downloads_summary(d).failed == 1);
    char w50[400]; snprintf(w50, sizeof(w50), "%s/vid00000050", work);
    CHECK(!platform_file_info(w50).exists);

    downloads_destroy(d);
    platform_remove_tree(root);
}


/* Saved crash boundaries and closing during tagging both recover without a
   second network download. Every fixture is isolated from the real library. */
static void test_recovery(void) {
    const char *root = "/tmp/offbeat-dl-test-recovery";
    platform_remove_tree(root);
    char fake[256], state[256], work[256], music[256], dest[256], artist[300], script[300];
    snprintf(fake, sizeof(fake), "%s/fake", root);
    snprintf(state, sizeof(state), "%s/downloads", root);
    snprintf(work, sizeof(work), "%s/work", root);
    snprintf(music, sizeof(music), "%s/Music", root);
    snprintf(dest, sizeof(dest), "%s/Downloads", music);
    snprintf(artist, sizeof(artist), "%s/Rock/Fake Artist", music);
    platform_make_dirs(fake);
    platform_make_dirs(artist);
    setenv("FAKE_DIR", fake, 1);
    snprintf(script, sizeof(script), "%s/yt-dlp", fake); write_script(script, FAKE_YTDLP); setenv("OFFBEAT_YTDLP", script, 1);
    snprintf(script, sizeof(script), "%s/ffmpeg", fake); write_script(script, FAKE_FFMPEG); setenv("OFFBEAT_FFMPEG", script, 1);

    Dl_Job jobs[3] = {0};
    for (u32 i = 0; i < 3; i++) {
        jobs[i].uid = i + 1;
        jobs[i].state = DL_ACTIVE;
        jobs[i].metadata_ready = true;
        core_cstr_copy(jobs[i].artist, sizeof(jobs[i].artist), "Fake Artist");
    }
    core_cstr_copy(jobs[0].vid, sizeof(jobs[0].vid), "vidCACHE001");
    core_cstr_copy(jobs[0].title, sizeof(jobs[0].title), "Cached");
    snprintf(jobs[0].path, sizeof(jobs[0].path), "%s/Cached.mp3", artist);
    core_cstr_copy(jobs[1].vid, sizeof(jobs[1].vid), "vidDONE0001");
    core_cstr_copy(jobs[1].title, sizeof(jobs[1].title), "Published");
    snprintf(jobs[1].path, sizeof(jobs[1].path), "%s/Published.mp3", artist);
    CHECK(platform_file_write_all(jobs[1].path, "published", 9));
    core_cstr_copy(jobs[2].vid, sizeof(jobs[2].vid), "vidPENDING1");
    core_cstr_copy(jobs[2].title, sizeof(jobs[2].title), "Pending");
    jobs[2].metadata_ready = false;
    jobs[2].state = DL_QUEUED;
    char cache[400], audio[450], half[1100], marker[450], buf[12000];
    snprintf(cache, sizeof(cache), "%s/%s", work, jobs[0].vid);
    platform_make_dirs(cache);
    snprintf(audio, sizeof(audio), "%s/%s.mp3", cache, jobs[0].vid);
    CHECK(platform_file_write_all(audio, "complete-audio", 14));
    snprintf(half, sizeof(half), "%s.part", jobs[0].path);
    CHECK(platform_file_write_all(half, "incomplete", 10));
    snprintf(marker, sizeof(marker), "%s/tagging", cache);
    CHECK(platform_file_write_all(marker, half, strlen(half)));
    u64 n = dl_jobs_format(jobs, 3, 4, buf, sizeof(buf));
    CHECK(platform_file_write_all(state, buf, n));

    Downloads *d = downloads_create(state, work);
    CHECK(!platform_file_info(half).exists && !platform_file_info(marker).exists);
    CHECK(platform_file_info(audio).size == 14);
    platform_sleep(0.7);
    CHECK(downloads_summary(d).queued == 3 && downloads_summary(d).active == 0);
    downloads_configure(d, dest, music, 2);
    CHECK(wait_for(d, all_settled, 0, 10));
    CHECK(downloads_summary(d).done == 3 && downloads_take_finished(d) == 3);
    CHECK(platform_file_info(jobs[0].path).size > 0 && platform_file_info(jobs[1].path).size == 9);
    CHECK(!platform_file_info(cache).exists);
    char logpath[300], log[12000];
    snprintf(logpath, sizeof(logpath), "%s/ytdlp.log", fake);
    read_file(logpath, log, sizeof(log));
    CHECK(!strstr(log, jobs[0].vid) && !strstr(log, jobs[1].vid) && strstr(log, jobs[2].vid));
    char wrong[400]; snprintf(wrong, sizeof(wrong), "%s/Fake Artist", dest);
    CHECK(!platform_file_info(wrong).exists);
    downloads_clear_finished(d);

    char slow[300], started[300];
    snprintf(slow, sizeof(slow), "%s/tag_slow", fake);
    snprintf(started, sizeof(started), "%s/tag_started", fake);
    CHECK(platform_file_write_all(slow, "x", 1));
    Dl_Result pick = {0};
    core_cstr_copy(pick.vid, sizeof(pick.vid), "vidTAG00001");
    core_cstr_copy(pick.title, sizeof(pick.title), "Needs tagging");
    CHECK(downloads_enqueue(d, &pick, 1, "Fake Artist") == 1);
    CHECK(wait_for(d, file_exists, started, 10));
    Dl_Job j;
    CHECK(downloads_job(d, 0, &j) && j.metadata_ready && j.state == DL_ACTIVE);
    snprintf(half, sizeof(half), "%s.part", j.path);
    CHECK(platform_file_info(half).size > 0);
    f64 begin = platform_time_seconds();
    downloads_destroy(d);
    CHECK(platform_time_seconds() - begin < 5.0);
    CHECK(!platform_file_info(half).exists && !platform_file_info(j.path).exists);
    snprintf(audio, sizeof(audio), "%s/%s/%s.mp3", work, pick.vid, pick.vid);
    CHECK(platform_file_info(audio).size > 0);
    read_file(logpath, log, sizeof(log));
    u64 log_size = strlen(log);
    platform_file_remove(slow);
    d = downloads_create(state, work);
    downloads_configure(d, dest, music, 2);
    CHECK(wait_for(d, all_settled, 0, 10));
    CHECK(downloads_summary(d).done == 1 && downloads_summary(d).failed == 0);
    read_file(logpath, log, sizeof(log));
    CHECK(strlen(log) == log_size); /* tagging resumed offline */
    CHECK(platform_file_info(j.path).size > 0 && !platform_file_info(half).exists);
    snprintf(cache, sizeof(cache), "%s/%s", work, pick.vid);
    CHECK(!platform_file_info(cache).exists);

    /* Removing an active tagger stops it and clears both scratch and .part. */
    CHECK(platform_file_write_all(slow, "x", 1));
    platform_file_remove(started);
    core_cstr_copy(pick.vid, sizeof(pick.vid), "vidTAG00002");
    core_cstr_copy(pick.title, sizeof(pick.title), "Cancel tagging");
    CHECK(downloads_enqueue(d, &pick, 1, "Fake Artist") == 1);
    CHECK(wait_for(d, file_exists, started, 10));
    CHECK(downloads_job(d, 1, &j) && j.state == DL_ACTIVE);
    snprintf(half, sizeof(half), "%s.part", j.path);
    downloads_remove(d, j.uid);
    CHECK(wait_for(d, all_settled, 0, 5));
    CHECK(downloads_summary(d).total == 1 && !platform_file_info(j.path).exists && !platform_file_info(half).exists);
    snprintf(cache, sizeof(cache), "%s/%s", work, pick.vid);
    CHECK(!platform_file_info(cache).exists);
    /* Shutdown also reaches metadata children published after search starts. */
    snprintf(slow, sizeof(slow), "%s/enrich_slow", fake);
    snprintf(started, sizeof(started), "%s/enrich_started", fake);
    CHECK(platform_file_write_all(slow, "x", 1));
    downloads_search(d, DL_QUERY_SONG, "close during lookup");
    CHECK(wait_for(d, file_exists, started, 10));
    begin = platform_time_seconds();
    downloads_destroy(d);
    CHECK(platform_time_seconds() - begin < 5.0);
    platform_remove_tree(root);
}

int main(void) {
    test_text();
    test_parsers();
    test_enrich_parse();
    test_persistence();
    test_pipeline();
    test_library();
    test_recovery();
    if (g_fail) { printf("download_test: %d failure(s)\n", g_fail); return 1; }
    u64 heap = core_mem_stats().heap_live;
    printf("download_test: ok (heap after: %llu bytes)\n", (unsigned long long)heap);
    return 0;
}
