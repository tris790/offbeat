# Downloading music

Offbeat can search YouTube and save songs as tagged MP3s straight into your
library. It needs [`yt-dlp`](https://github.com/yt-dlp/yt-dlp) and `ffmpeg`
(the download page says so, with install hints, when one is missing). `curl` is
optional: it only fetches the small thumbnails shown in the result lists.

## Using it

- **Ctrl+K**, type a name: below your library matches there are two extra rows,
  *Download "name"* and *Top songs by "name"*. With an empty query the palette
  offers *Download music*. **Ctrl+D** opens the page directly.
- **Song** mode lists YouTube Music songs first (studio versions with album,
  year and track number), then YouTube videos (live, remix, lyric video...).
  Click the one you want.
- **Artist** mode lists the artist's top songs (up to 100) with checkboxes and
  *Select all*, then *Download N songs*.
- Right-click any song, or the `...` of an artist, and pick *Download top 100 by
  artist*: the page opens with everything ticked.
- The dock at the bottom shows overall progress; hover (or click the arrow) to
  see every song, retry or remove one, pause, or clear the finished ones. A
  status pill at the top left of the player keeps it visible when the page is
  closed.

Closing the page, or the app, never loses anything: the queue is saved on every
change, a song that was mid-download goes back to *queued* and yt-dlp resumes
its partial file. Failed songs retry twice more (5 s, 20 s) before showing as
failed.

## Where things go

- Songs are filed **into your library**, `<music>/<Genre>/<Artist>/<Title>.mp3`:
  if the library already has a folder for that artist (whatever the genre it
  sits in), the song goes there; else, if YouTube gives a genre that matches
  one of your genre folders ("Hip-Hop/Rap" -> `Hip Hop`), a new artist folder
  is made in it; else the artist goes to the *Folder for new artists* setting
  (default: `Downloads` inside the music folder, which the library lists as its
  own group). The library rescans itself after each finished song.
  The artist tag and folder use the *primary* artist ("A feat. B", "A & B"
  become "A"), the same rule as `~/Music/normalize_music_artists.py`.
- No duplicates: the same song released twice ("Hypnotize" / "Hypnotize (2007
  Remaster)") is listed once, a song already in the artist's folder is never
  fetched again, and results the library already has show *In library*. "Same
  song" means same artist and same title once release tags (remaster, explicit,
  feat., official video, years...) are ignored; "(Live)", "(Remix)" and the
  like stay different songs. A YouTube Music hit has no artist in the search
  listing, so each one is looked up right after (a few seconds, in parallel).
- Tags: title, artist, album artist, album, year, track, genre (when YouTube
  has them), a comment, `youtube_id` and `source_url`, plus the thumbnail as
  front cover (ID3v2.3 + ID3v1).
- Queue: `<config>/downloads`. Scratch folders: `<cache>/downloads/<video id>`.
  Thumbnails: `<cache>/ytthumbs`.
- Cleanup: a finished job removes its scratch folder; giving up on one removes it
  too (and any empty artist folder it made); at startup, scratch folders of jobs
  that are gone and half-written `.part` files a crash left are deleted.
- Settings: *Folder for new artists* and *Downloads at once* (1-3) in Settings.

## Code map

| file | what |
| --- | --- |
| `src/game/download.c` | search thread, worker threads, yt-dlp/ffmpeg drivers, persistence, text helpers |
| `src/game/download_view.c` | the page, dock and status pill |
| `src/game/ytthumbs.c` | curl + stb thumbnails in a GPU atlas |
| `src/platform/platform_posix.c` | `platform_process_*` (spawn, read lines, kill), file rename/remove |
| `tests/download_test.c` | text helpers, parsers, persistence, and the whole pipeline against fake tools |

## Developing without a network

```sh
OFFBEAT_YTDLP=tools/fake_ytdlp.sh OFFBEAT_DL_FAST=1 ./build/offbeat
```

`tools/fake_ytdlp.sh` answers searches with canned results and "downloads" a
3 second sine wave plus a cover image, so the real ffmpeg tagging and the
library rescan can be watched offline (`DEMO_SPEED=<seconds>` sets how long a
download takes). `OFFBEAT_YTDLP` / `OFFBEAT_FFMPEG` point at other binaries;
`OFFBEAT_DL_FAST=1` drops the politeness delays between job starts.

Headless screenshots: see the demo tokens in `src/game/app.c` (`dl`,
`dlsong=`, `dlartist=`, `dlartistall=`, `dlgo`, `dldock`, `dlclose`, and the
generic `mouse=X:Y`, `click`, `clickat=X:Y`, `key=`, `text=`). Headless frames
run far faster than wall time, so wait a few hundred frames (`@N`) for a search
and use a scratch `OFFBEAT_MUSIC_DIR`.
