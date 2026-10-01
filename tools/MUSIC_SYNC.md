# USB music sync

`music_sync.py` mirrors the computer's Offbeat music library to an Android
phone. The computer is the master; nothing is copied back from the phone.
It needs Python 3.10+ and `adb` (Android Platform Tools), with no Python packages.
Enable USB debugging, connect the phone by USB, unlock it, and authorize the
computer when Android asks. Wi-Fi adb devices are not selected.

From the repository root:

```sh
# Preview; neither music library changes.
./tools/music_sync.py

# Copy new/changed files and remove phone-only files.
./tools/music_sync.py --apply

# Copy new/changed files while keeping phone-only files.
./tools/music_sync.py --apply --keep-extra
```

Before applying a plan with deletions, the tool lists every affected path and
requires typing `DELETE` in a terminal. Cancelling changes no phone files.
Without a terminal, it refuses the operation before copying anything; use
`--keep-extra` for unattended copying. There is no automatic deletion approval.
Updates replace differing phone files with the computer's version.

## Folders and settings

The source follows Offbeat's existing settings: `OFFBEAT_MUSIC_DIR`, then
`music_dir` in the `settings` file, then `$HOME/Music`. The config folder follows
`OFFBEAT_CONFIG_DIR`, then `$XDG_CONFIG_HOME/offbeat`, then
`$HOME/.config/offbeat`. `--config-dir PATH` overrides the config location.
The tool never rewrites Offbeat's main settings file.

The default phone folder is `/sdcard/Music`. Use `--phone-dir PATH` for a
different dedicated folder in shared storage and `--serial SERIAL` if needed.
The device and phone folder are remembered after a successful apply, under
`<offbeat-config>/music-sync/profile.json`. A saved device must be connected;
the tool will not silently switch to a different phone.

Relative folder structure is preserved. Managed files are audio (`mp3`, `flac`,
`ogg`, `opus`, `m4a`, `aac`, `wav`, `aif`, `aiff`, `wma`, `alac`, `ape`), artwork
(`jpg`, `jpeg`, `png`, `webp`, `gif`, `avif`), and playlists/sidecars (`m3u`,
`m3u8`, `pls`, `cue`, `lrc`). Other files, hidden files, and hidden folders
(including Android player thumbnail caches) are left alone. Empty parents of
deleted files are removed only when truly empty. Empty source folders are
not mirrored. Audio formats listed here do not imply Offbeat can play them all.

## Comparison and interruption

Same-size files are compared using SHA-256, even if their names and timestamps
match. The first comparison of an existing library can take a few minutes.
Verified matching pairs are cached by source size/mtime and phone size/mtime
under `<offbeat-config>/music-sync/`, including during previews. Unchanged
metadata lets later runs avoid rereading audio. Use `--checksum` to recheck all
matching files if something changed contents without updating metadata.

Each copy goes to a temporary hidden directory beside its destination, is
checksum-verified, then renamed into place. Existing complete tracks survive
failed transfers. Extra files are deleted only after all copies succeed and
both folders have been rechecked for changes. An interrupted delete phase can
be finished by rerunning the tool. Abrupt termination or a disconnect can leave
a hidden `.offbeat-sync-*` temporary directory; those are excluded from sync.

An absent/empty source, failed listing, symlink, path collision, or unexpected
folder change stops the operation. Root storage volumes and Android app data
are rejected as destinations. A lock prevents overlapping runs using the same
config folder. The phone's music player may need a library rescan afterwards.

## Later Offbeat integration

```sh
./tools/music_sync.py --dry-run --json
```

This prints a versioned JSON plan containing source/destination, device serial,
relative paths to copy/update/delete, unchanged count, and transfer byte count.
Progress goes to stderr. Exit status is 0 on success or cancellation, 1 on
failure, and 130 on interruption. The planner, comparison code, and Android
transport are separate so Offbeat can initially call the CLI through its process
API, then reuse the design in C. UI approval should operate on a revalidated
plan when an integration adds a noninteractive deletion interface.

Run the tool's filesystem integration tests with:

```sh
python3 tests/music_sync_test.py
```
