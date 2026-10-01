"""USB sync integration tests against a temporary directory acting as a phone."""

import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


spec = importlib.util.spec_from_file_location("music_sync", Path(__file__).resolve().parents[1] / "tools/music_sync.py")
sync = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = sync
spec.loader.exec_module(sync)


class DirectoryPhone(sync.Android):
    """Use real shell/file I/O, replacing only adb's transport."""

    def __init__(self, root):
        self.root = str(root)
        self.serial = "test-phone"

    def run(self, *args, input=None):
        if args[0] == "push":
            shutil.copyfile(args[1], args[2])
            return b""
        if args[0] != "shell":
            raise AssertionError(args)
        result = subprocess.run(["sh"], input=input, capture_output=True)
        if result.returncode:
            raise sync.SyncError(result.stderr.decode())
        return result.stdout


class MusicSyncTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.root = self.base / "computer"
        self.phone_root = self.base / "phone"
        self.root.mkdir()
        self.phone_root.mkdir()
        self.phone = DirectoryPhone(self.phone_root)

    def put(self, root, name, data=b"music"):
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return path

    def plan(self, cache=None, checksum=False):
        source = sync.local_scan(self.root)
        dest = self.phone.scan()
        same, verified = sync.compare(self.root, source, dest, self.phone, cache or {}, checksum, lambda _: None)
        return source, dest, sync.make_plan(source, dest, same), verified

    def test_mirror_handles_content_changes_and_hostile_filenames(self):
        weird = "Rock/Artist's $(touch PWNED); 雪\nnew.mp3"
        self.put(self.root, weird, b"new song")
        self.put(self.root, "Rock/update.mp3", b"NEW")
        self.put(self.phone_root, "Rock/update.mp3", b"OLD")
        self.put(self.root, "Rock/same.mp3", b"same")
        self.put(self.phone_root, "Rock/same.mp3", b"same")
        self.put(self.root, "Rock/cover.jpg", b"picture")
        self.put(self.phone_root, "Old/delete.mp3")
        self.put(self.phone_root, ".thumbnails/cache.jpg")
        self.put(self.phone_root, "notes.txt", b"keep")
        self.put(self.root, "helper.py", b"not music")
        source, dest, plan, verified = self.plan()
        self.assertEqual(plan.update, ["Rock/update.mp3"])
        self.assertEqual(plan.delete, ["Old/delete.mp3"])
        self.assertEqual(plan.unchanged, 1)
        copied, final = sync.apply_plan(self.root, source, dest, plan, self.phone, False, lambda _: None)
        self.assertEqual(set(final.files), set(source.files))
        for name in source.files:
            self.assertEqual((self.root / name).read_bytes(), (self.phone_root / name).read_bytes())
        self.assertTrue((self.phone_root / ".thumbnails/cache.jpg").exists())
        self.assertTrue((self.phone_root / "notes.txt").exists())
        self.assertFalse((self.phone_root / "helper.py").exists())
        self.assertFalse((self.phone_root / "Old").exists())
        self.assertFalse((self.base / "PWNED").exists())
        self.assertFalse(list(self.phone_root.rglob(".offbeat-sync-*")))
        self.assertEqual(set(copied), set(plan.copy + plan.update))

    def test_failed_transfer_keeps_original_and_phone_only_songs(self):
        self.put(self.root, "update.mp3", b"NEW")
        self.put(self.phone_root, "update.mp3", b"OLD")
        self.put(self.phone_root, "delete.mp3")
        source, dest, plan, _ = self.plan()
        run = self.phone.run

        def corrupt_push(*args, **kwargs):
            if args[0] == "push":
                Path(args[2]).write_bytes(b"corrupted")
                return b""
            return run(*args, **kwargs)

        with patch.object(self.phone, "run", side_effect=corrupt_push):
            with self.assertRaisesRegex(sync.SyncError, "checksum mismatch"):
                sync.apply_plan(self.root, source, dest, plan, self.phone, False, lambda _: None)
        self.assertEqual((self.phone_root / "update.mp3").read_bytes(), b"OLD")
        self.assertTrue((self.phone_root / "delete.mp3").exists())
        self.assertFalse(list(self.phone_root.rglob(".offbeat-sync-*")))

    def test_source_or_phone_change_after_preview_aborts(self):
        self.put(self.root, "song.mp3")
        self.put(self.phone_root, "delete.mp3")
        source, dest, plan, _ = self.plan()
        self.put(self.root, "song.mp3", b"changed")
        with self.assertRaisesRegex(sync.SyncError, "changed after the preview"):
            sync.apply_plan(self.root, source, dest, plan, self.phone, False, lambda _: None)
        self.assertTrue((self.phone_root / "delete.mp3").exists())
        source, dest, plan, _ = self.plan()
        self.put(self.phone_root, "new.mp3")
        with self.assertRaises(sync.SyncError):
            sync.apply_plan(self.root, source, dest, plan, self.phone, False, lambda _: None)

    def test_source_change_during_transfer_blocks_deletion(self):
        song = self.put(self.root, "song.mp3")
        self.put(self.phone_root, "delete.mp3")
        source, dest, plan, _ = self.plan()
        copy = self.phone.copy

        def change_after_copy(*args):
            digest = copy(*args)
            song.unlink()
            return digest

        with patch.object(self.phone, "copy", side_effect=change_after_copy):
            with self.assertRaises(sync.SyncError):
                sync.apply_plan(self.root, source, dest, plan, self.phone, False, lambda _: None)
        self.assertTrue((self.phone_root / "delete.mp3").exists())

    def test_empty_symlink_and_collision_sources_are_rejected(self):
        with self.assertRaisesRegex(sync.SyncError, "no audio"):
            sync.local_scan(self.root)
        song = self.put(self.root, "song.mp3")
        (self.root / "link").symlink_to(song)
        with self.assertRaisesRegex(sync.SyncError, "Symlinks"):
            sync.local_scan(self.root)
        (self.root / "link").unlink()
        (self.phone_root / "song.mp3").mkdir()
        with self.assertRaisesRegex(sync.SyncError, "blocks a music file"):
            self.plan()
        (self.phone_root / "song.mp3").rmdir()
        self.put(self.root, "SONG.mp3")
        with self.assertRaisesRegex(sync.SyncError, "Case-colliding"):
            self.plan()

    def test_remote_parse_rejects_partial_traversal_and_symlink_records(self):
        for data in (b"a.mp3\0-\0", b"../a.mp3\0-\01\00\0", b"link\0l\01\00\0"):
            with self.assertRaises(sync.SyncError):
                sync.parse_remote(data)
        self.assertEqual(sync.parse_remote(b"\0d\00\00\0").files, {})

    def test_keep_extra_and_cache_avoid_repeat_hashing(self):
        self.put(self.root, "song.mp3")
        self.put(self.phone_root, "delete.mp3")
        source, dest, plan, _ = self.plan()
        sync.apply_plan(self.root, source, dest, plan, self.phone, True, lambda _: None)
        self.assertTrue((self.phone_root / "delete.mp3").exists())
        _, _, _, verified = self.plan()
        with patch.object(self.phone, "hashes", side_effect=AssertionError("should use cache")):
            _, _, plan, _ = self.plan(verified)
        self.assertEqual(plan.unchanged, 1)
        with patch.object(self.phone, "hashes", side_effect=sync.SyncError("forced recheck")):
            with self.assertRaisesRegex(sync.SyncError, "forced recheck"):
                self.plan(verified, checksum=True)

    def test_offbeat_settings_and_environment_precedence(self):
        config = self.base / "config"
        config.mkdir()
        (config / "settings").write_text("offbeat-settings 1\nmusic_dir /first\nmusic_dir /music with spaces  \n")
        with patch.dict(os.environ, {}, clear=True):
            self.assertEqual(sync.music_dir(config), Path("/music with spaces"))
            with patch.dict(os.environ, {"OFFBEAT_MUSIC_DIR": "/override"}):
                self.assertEqual(sync.music_dir(config), Path("/override"))
            with patch.dict(os.environ, {"XDG_CONFIG_HOME": "/xdg"}):
                self.assertEqual(sync.config_dir(), Path("/xdg/offbeat"))
            with patch.dict(os.environ, {"OFFBEAT_CONFIG_DIR": "/offbeat"}):
                self.assertEqual(sync.config_dir(), Path("/offbeat"))

    def test_cli_requires_confirmation_before_any_phone_changes(self):
        self.put(self.root, "new.mp3")
        self.put(self.phone_root, "delete.mp3")
        config = self.base / "config"
        output = io.StringIO()
        with (patch.object(sync, "music_dir", return_value=self.root),
              patch.object(sync, "Android", return_value=self.phone),
              patch.object(self.phone, "set_root"),
              patch("sys.stdin.isatty", return_value=False),
              contextlib.redirect_stdout(output), contextlib.redirect_stderr(io.StringIO())):
            with self.assertRaisesRegex(sync.SyncError, "interactive confirmation"):
                sync.main(["--config-dir", str(config), "--apply", "--json"])
        self.assertEqual(json.loads(output.getvalue())["delete"], ["delete.mp3"])
        self.assertFalse((self.phone_root / "new.mp3").exists())
        self.assertTrue((self.phone_root / "delete.mp3").exists())

    def test_cli_cancel_and_explicit_confirmation(self):
        self.put(self.root, "new.mp3")
        self.put(self.phone_root, "delete.mp3")
        config = self.base / "config"
        with (patch.object(sync, "music_dir", return_value=self.root),
              patch.object(sync, "Android", return_value=self.phone),
              patch.object(self.phone, "set_root"),
              patch("sys.stdin.isatty", return_value=True),
              contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO())):
            with patch("builtins.input", return_value="no"):
                self.assertEqual(sync.main(["--config-dir", str(config), "--apply"]), 0)
            self.assertFalse((self.phone_root / "new.mp3").exists())
            with patch("builtins.input", return_value="DELETE"):
                self.assertEqual(sync.main(["--config-dir", str(config), "--apply"]), 0)
        self.assertTrue((self.phone_root / "new.mp3").exists())
        self.assertFalse((self.phone_root / "delete.mp3").exists())
        self.assertEqual(json.loads((config / "music-sync/profile.json").read_text())["serial"], "test-phone")


if __name__ == "__main__":
    unittest.main()
