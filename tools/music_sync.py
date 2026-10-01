#!/usr/bin/env python3
"""Mirror Offbeat's music folder to Android over USB; see MUSIC_SYNC.md."""

import argparse
from dataclasses import asdict, dataclass
from decimal import Decimal
import fcntl
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shlex
import stat
import subprocess
import sys
import uuid


AUDIO = frozenset(".mp3 .flac .ogg .opus .m4a .aac .wav .aif .aiff .wma .alac .ape".split())
MANAGED = AUDIO | frozenset(".jpg .jpeg .png .webp .gif .avif .m3u .m3u8 .pls .cue .lrc".split())


class SyncError(Exception):
    pass


@dataclass(frozen=True)
class Entry:
    size: int
    mtime_ns: int

    def signature(self):
        return [self.size, self.mtime_ns]


@dataclass
class Snapshot:
    files: dict
    dirs: set
    other: set


@dataclass
class Plan:
    copy: list
    update: list
    delete: list
    unchanged: int
    bytes: int


def managed(path):
    p = PurePosixPath(path)
    return not any(part.startswith(".") for part in p.parts) and p.suffix.lower() in MANAGED


def relative_path(path):
    p = PurePosixPath(path)
    if not path or p.is_absolute() or ".." in p.parts or str(p) != path:
        raise SyncError(f"Invalid relative path: {path!r}")
    return path


def config_dir():
    if os.environ.get("OFFBEAT_CONFIG_DIR"):
        return Path(os.environ["OFFBEAT_CONFIG_DIR"])
    return Path(os.environ.get("XDG_CONFIG_HOME") or Path.home() / ".config") / "offbeat"


def music_dir(config):
    # Match src/game/app.c and settings_parse(), including last key winning.
    value = ""
    try:
        text = (config / "settings").read_text()
    except FileNotFoundError:
        text = ""
    for line in text.splitlines():
        key, sep, val = line.rstrip("\r ").partition(" ")
        if sep and key == "music_dir":
            value = val
    return Path(os.environ.get("OFFBEAT_MUSIC_DIR") or value or Path.home() / "Music")


def read_json(path):
    try:
        return json.loads(path.read_text())
    except (FileNotFoundError, ValueError):
        return {}


def write_json(path, value):
    temp = path.with_name(path.name + ".tmp")
    temp.write_text(json.dumps(value, ensure_ascii=True, indent=2) + "\n")
    temp.replace(path)


def local_scan(root):
    files, dirs, other = {}, set(), set()
    if not root.is_dir():
        raise SyncError(f"Music folder is missing: {root}")

    def visit(directory):
        with os.scandir(directory) as entries:
            for item in entries:
                if item.name.startswith("."):
                    continue
                path = Path(item.path).relative_to(root).as_posix()
                info = item.stat(follow_symlinks=False)
                if stat.S_ISLNK(info.st_mode):
                    raise SyncError(f"Symlinks are not supported: {item.path}")
                if stat.S_ISDIR(info.st_mode):
                    dirs.add(path)
                    visit(item.path)
                elif stat.S_ISREG(info.st_mode) and managed(path):
                    files[path] = Entry(info.st_size, info.st_mtime_ns)
                else:
                    other.add(path)

    visit(root)
    if not any(PurePosixPath(p).suffix.lower() in AUDIO for p in files):
        raise SyncError("Music folder contains no audio files; refusing to mirror an empty library.")
    return Snapshot(files, dirs, other)


def file_hash(path, expected):
    info = path.stat()
    if Entry(info.st_size, info.st_mtime_ns) != expected or path.is_symlink():
        raise SyncError(f"Source changed during sync: {path}")
    digest = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            digest.update(block)
    info = path.stat()
    if Entry(info.st_size, info.st_mtime_ns) != expected:
        raise SyncError(f"Source changed while reading: {path}")
    return digest.hexdigest()


class Android:
    def __init__(self, adb, serial=None):
        self.adb = adb
        result = self.run("devices", "-l")
        devices = []
        for line in result.decode().splitlines()[1:]:
            fields = line.split()
            if len(fields) >= 2 and fields[1] == "device" and any(x.startswith("usb:") for x in fields[2:]):
                devices.append(fields[0])
        if serial:
            if serial not in devices:
                raise SyncError(f"USB device {serial!r} is not connected and authorized. Check adb devices -l.")
        elif len(devices) == 1:
            serial = devices[0]
        else:
            raise SyncError("Connect and unlock one Android USB device, authorize USB debugging, or use --serial.")
        self.serial = serial
        self.root = None

    def run(self, *args, input=None):
        cmd = [self.adb]
        if getattr(self, "serial", None):
            cmd += ["-s", self.serial]
        result = subprocess.run(cmd + list(args), input=input, capture_output=True)
        if result.returncode:
            detail = (result.stderr or result.stdout).decode(errors="replace").strip()
            raise SyncError(f"adb failed: {detail}")
        return result.stdout

    def shell(self, script):
        return self.run("shell", "sh -s", input=script.encode())

    def set_root(self, root):
        path = PurePosixPath(root)
        if not path.is_absolute() or ".." in path.parts:
            raise SyncError("Phone folder must be an absolute path without '..'.")
        resolved = self.shell(f"readlink -f {shlex.quote(str(path))}\n").decode().strip()
        p = PurePosixPath(resolved)
        parts = p.parts
        # Only a subfolder of Android shared storage, never a whole volume.
        valid = (len(parts) >= 5 and parts[1:3] == ("storage", "emulated")
                 or len(parts) >= 4 and parts[1] == "storage" and parts[2] != "emulated")
        if not valid or any(x.startswith(".") for x in parts[1:]) or "Android" in parts:
            raise SyncError(f"Phone folder must be a dedicated folder inside shared storage: {resolved!r}")
        self.root = str(p)

    def path(self, relative):
        return self.root + "/" + relative_path(relative)

    def scan(self):
        root = shlex.quote(self.root)
        data = self.shell(
            f"if [ -e {root} ]; then\n"
            f"  [ -d {root} ] && [ ! -L {root} ] || exit 1\n"
            # NUL records support spaces, quotes, Unicode, and newlines.
            f"  find {root} -mindepth 1 -name '.*' -prune -o "
            "-printf '%P\\0%M\\0%s\\0%T@\\0'\n"
            "fi\n")
        return parse_remote(data)

    def hashes(self, paths):
        result = {}
        for offset in range(0, len(paths), 64):
            batch = paths[offset:offset + 64]
            quoted = " ".join(shlex.quote(self.path(p)) for p in batch)
            output = self.shell(f'for p in {quoted}; do sha256sum < "$p" || exit 1; done\n')
            hashes = [line.split()[0].decode() for line in output.splitlines()]
            if len(hashes) != len(batch) or any(len(h) != 64 or any(c not in "0123456789abcdef" for c in h) for h in hashes):
                raise SyncError("Invalid checksum response from phone.")
            result.update(zip(batch, hashes))
        return result

    def copy(self, root, path, entry):
        digest = file_hash(root / path, entry)
        dest = self.path(path)
        parent = str(PurePosixPath(dest).parent)
        # Private temporary directory beside the destination; interrupted files
        # never become music tracks and existing complete files remain intact.
        temp_dir = parent + "/.offbeat-sync-" + uuid.uuid4().hex
        temp = temp_dir + "/data"
        self.shell(f"mkdir -p {shlex.quote(parent)} && mkdir {shlex.quote(temp_dir)}\n")
        try:
            self.run("push", str(root / path), temp)
            remote_hash = self.shell(f"sha256sum < {shlex.quote(temp)}\n").split()[0].decode()
            if remote_hash != digest:
                raise SyncError(f"Transfer checksum mismatch: {path!r}")
            info = (root / path).stat()
            if Entry(info.st_size, info.st_mtime_ns) != entry:
                raise SyncError(f"Source changed during transfer: {path!r}")
            self.shell(f"mv -f {shlex.quote(temp)} {shlex.quote(dest)}\n")
        finally:
            self.shell(f"rm -f {shlex.quote(temp)} && rmdir {shlex.quote(temp_dir)}\n")
        return digest

    def delete(self, path):
        self.shell(f"rm {shlex.quote(self.path(path))}\n")

    def prune_empty_parents(self, paths):
        parents = set()
        for path in paths:
            parent = PurePosixPath(path).parent
            while str(parent) != ".":
                parents.add(str(parent))
                parent = parent.parent
        for parent in sorted(parents, key=lambda p: len(PurePosixPath(p).parts), reverse=True):
            # rmdir cannot remove player caches or other unmanaged files.
            self.shell(f"rmdir {shlex.quote(self.path(parent))} 2>/dev/null || true\n")


def parse_remote(data):
    files, dirs, other = {}, set(), set()
    if not data:
        return Snapshot(files, dirs, other)
    fields = data.split(b"\0")
    if fields[-1] != b"" or (len(fields) - 1) % 4:
        raise SyncError("Incomplete file listing from phone; refusing to sync.")
    for i in range(0, len(fields) - 1, 4):
        mode = fields[i + 1][:1]
        # Some Toybox versions still print the root with -mindepth 1.
        if i == 0 and not fields[i] and mode == b"d":
            continue
        path = relative_path(os.fsdecode(fields[i]))
        if mode == b"l":
            raise SyncError(f"Symlinks are not supported on phone: {path!r}")
        if mode == b"d":
            dirs.add(path)
        elif mode == b"-" and managed(path):
            try:
                files[path] = Entry(int(fields[i + 2]), int(Decimal(fields[i + 3].decode()) * 1_000_000_000))
            except (ValueError, ArithmeticError) as e:
                raise SyncError(f"Invalid file metadata for {path!r}") from e
        else:
            other.add(path)
    return Snapshot(files, dirs, other)


def check_collisions(source, dest):
    for path in source.files:
        if path in dest.dirs or path in dest.other:
            raise SyncError(f"Phone path blocks a music file: {path!r}")
        parent = PurePosixPath(path).parent
        while str(parent) != ".":
            if str(parent) in dest.files or str(parent) in dest.other:
                raise SyncError(f"Phone file blocks a music folder: {str(parent)!r}")
            parent = parent.parent
    # Android shared storage is commonly case-insensitive.
    case_paths = {}
    for path in sorted(set(source.files) | source.dirs | set(dest.files) | dest.dirs | dest.other):
        key = path.casefold()
        if key in case_paths and case_paths[key] != path:
            raise SyncError(f"Case-colliding paths on Android: {case_paths[key]!r}, {path!r}")
        case_paths[key] = path


def make_plan(source, dest, same):
    check_collisions(source, dest)
    copy = sorted(source.files.keys() - dest.files.keys())
    update = sorted(p for p in source.files.keys() & dest.files.keys() if p not in same)
    delete = sorted(dest.files.keys() - source.files.keys())
    return Plan(copy, update, delete, len(same), sum(source.files[p].size for p in copy + update))


def compare(root, source, dest, android, cache, checksum, progress):
    same, verified, candidates = set(), {}, []
    for path in sorted(source.files.keys() & dest.files.keys()):
        left, right = source.files[path], dest.files[path]
        if left.size != right.size:
            continue
        record = cache.get(path)
        if (not checksum and isinstance(record, dict) and record.get("local") == left.signature()
                and record.get("phone") == right.signature()):
            same.add(path)
            verified[path] = record
        else:
            candidates.append(path)
    for offset in range(0, len(candidates), 64):
        batch = candidates[offset:offset + 64]
        progress(f"Checking contents: {min(offset + 64, len(candidates))}/{len(candidates)}")
        hashes = android.hashes(batch)
        for path in batch:
            digest = file_hash(root / path, source.files[path])
            if digest == hashes[path]:
                same.add(path)
                verified[path] = {"local": source.files[path].signature(), "phone": dest.files[path].signature(), "sha256": digest}
    return same, verified


def apply_plan(root, source, dest, plan, android, keep_extra, progress):
    if local_scan(root) != source or android.scan() != dest:
        raise SyncError("Folders changed after the preview; run sync again.")
    copied = {}
    for i, path in enumerate(plan.copy + plan.update, 1):
        progress(f"Copying {i}/{len(plan.copy) + len(plan.update)}: {path!r}")
        copied[path] = android.copy(root, path, source.files[path])
    # No deletions occur until all transfers have succeeded and the source is
    # still present and unchanged. Recheck the phone for concurrent changes.
    if local_scan(root) != source:
        raise SyncError("Source changed during sync; no extra songs were deleted. Run again.")
    final = android.scan()
    expected = dest.files.keys() | source.files.keys()
    if final.files.keys() != expected or final.other != dest.other:
        raise SyncError("Phone contents changed during sync; no extra songs were deleted. Run again.")
    for path, entry in dest.files.items():
        if path not in copied and final.files.get(path) != entry:
            raise SyncError(f"Phone file changed during sync: {path!r}; no extra songs were deleted.")
    if not keep_extra:
        for i, path in enumerate(plan.delete, 1):
            progress(f"Deleting {i}/{len(plan.delete)}: {path!r}")
            android.delete(path)
        android.prune_empty_parents(plan.delete)
    return copied, android.scan()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    action = parser.add_mutually_exclusive_group()
    action.add_argument("--dry-run", action="store_true", help="preview only (the default)")
    action.add_argument("--apply", action="store_true", help="apply changes; prompt before deleting phone-only files")
    parser.add_argument("--keep-extra", action="store_true", help="copy and update, keeping phone-only files")
    parser.add_argument("--serial", help="USB device serial; otherwise use saved device or the only connected USB device")
    parser.add_argument("--phone-dir", help="phone music folder; default /sdcard/Music or saved profile")
    parser.add_argument("--config-dir", type=Path, help="Offbeat config folder (settings and sync profile)")
    parser.add_argument("--checksum", action="store_true", help="recheck file contents even when comparison metadata is cached")
    parser.add_argument("--json", action="store_true", help="emit the preview as JSON; progress goes to stderr")
    parser.add_argument("--adb", default="adb", help="adb executable")
    args = parser.parse_args(argv)
    config = args.config_dir or config_dir()
    state_dir = config / "music-sync"
    state_dir.mkdir(parents=True, exist_ok=True)
    with (state_dir / "lock").open("w") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as e:
            raise SyncError("Another music sync is already running.") from e
        profile = read_json(state_dir / "profile.json")
        if not isinstance(profile, dict):
            raise SyncError("Invalid sync profile.json.")
        root = music_dir(config).resolve(strict=True)
        source = local_scan(root)
        android = Android(args.adb, args.serial or profile.get("serial"))
        android.set_root(args.phone_dir or profile.get("phone_dir") or "/sdcard/Music")
        progress = lambda message: print(message, file=sys.stderr, flush=True)
        progress(f"Computer: {root}\nPhone: {android.serial}:{android.root}")
        dest = android.scan()
        check_collisions(source, dest)
        key = hashlib.sha256(f"{root}\0{android.serial}\0{android.root}".encode()).hexdigest()[:24]
        cache_path = state_dir / f"checksums-{key}.json"
        cache = read_json(cache_path)
        if not isinstance(cache, dict):
            cache = {}
        same, verified = compare(root, source, dest, android, cache, args.checksum, progress)
        # Comparison cache is only an optimization, not a sync baseline. A
        # preview can cache verified pairs without changing either library.
        if local_scan(root) != source or android.scan() != dest:
            raise SyncError("Folders changed during comparison; run sync again.")
        write_json(cache_path, verified)
        plan = make_plan(source, dest, same)
        summary = {"version": 1, "source": str(root), "serial": android.serial, "destination": android.root,
                   "keep_extra": args.keep_extra, **asdict(plan)}
        if args.json:
            print(json.dumps(summary, ensure_ascii=True, indent=2), flush=True)
        else:
            for label, paths in (("COPY", plan.copy), ("UPDATE", plan.update), ("KEEP" if args.keep_extra else "DELETE", plan.delete)):
                for path in paths:
                    print(f"{label:6} {path!r}")
            print(f"{len(plan.copy)} new, {len(plan.update)} updated, {len(plan.delete)} "
                  f"{'kept' if args.keep_extra else 'deleted'}, {plan.unchanged} unchanged; {plan.bytes / 1024**2:.1f} MiB to copy.", flush=True)
        if not args.apply:
            progress("Preview only. Run with --apply to sync.")
            return 0
        if plan.delete and not args.keep_extra:
            if not sys.stdin.isatty():
                raise SyncError("Deletion requires interactive confirmation. Use a terminal, or --keep-extra to copy without deleting.")
            progress(f"Delete the {len(plan.delete)} phone-only files listed above from {android.root}? Type DELETE to confirm:")
            if input().strip() != "DELETE":
                progress("Cancelled; no phone files changed.")
                return 0
        copied, final = apply_plan(root, source, dest, plan, android, args.keep_extra, progress)
        for path, digest in copied.items():
            verified[path] = {"local": source.files[path].signature(), "phone": final.files[path].signature(), "sha256": digest}
        write_json(cache_path, verified)
        write_json(state_dir / "profile.json", {"version": 1, "serial": android.serial, "phone_dir": android.root})
        progress("Sync complete." if not args.keep_extra else "Copies complete; phone-only files were kept.")
        return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (SyncError, OSError, ValueError) as e:
        print(f"music-sync: {e}", file=sys.stderr)
        sys.exit(1)
    except (KeyboardInterrupt, EOFError):
        print("music-sync: cancelled", file=sys.stderr)
        sys.exit(130)
