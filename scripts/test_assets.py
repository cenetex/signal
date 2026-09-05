#!/usr/bin/env python3
"""Media pack validation and runtime layout tests use small synthetic files."""
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
import zipfile

import assets


class MediaTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.manifest = self.root / "manifest.txt"
        self.manifest.write_text("anime/episode.mpg\nmusic/song.mp3\nstations/a/portrait.png\n?stations/a/motd.json\n")
        self.source = self.root / "source"
        files = {}
        for name in ("anime/episode.mpg", "music/song.mp3", "stations/a/portrait.png"):
            target = self.source / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(name.encode())
            files[name] = {"sha256": assets.digest(target),
                           "source": "synthetic test fixture", "license": "test use"}
        self.metadata = {"schema": 1, "files": files}
        self.write_metadata()

    def write_metadata(self):
        (self.source / assets.PROVENANCE).write_text(json.dumps(self.metadata))

    def archive(self, extra=None):
        pack = self.root / "pack.zip"
        with zipfile.ZipFile(pack, "w") as output:
            for item in self.source.rglob("*"):
                if item.is_file():
                    output.write(item, item.relative_to(self.source).as_posix())
            if extra:
                output.writestr(*extra)
        return pack

    def test_complete_pack_installs_and_verifies(self):
        pack = self.archive()
        dest = self.root / "installed"
        self.assertEqual(len(assets.sync(dest, self.manifest, str(pack), assets.digest(pack))), 3)
        self.assertEqual(len(assets.verify(dest, self.manifest)), 3)

    def test_bad_pack_hash_preserves_existing_files(self):
        pack = self.archive()
        dest = self.root / "installed"
        dest.mkdir()
        keep = dest / "keep"
        keep.write_bytes(b"existing")
        with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
            assets.sync(dest, self.manifest, str(pack), "0" * 64)
        self.assertEqual(list(dest.iterdir()), [keep])
        self.assertEqual(keep.read_bytes(), b"existing")

    def test_bad_file_hash_preserves_existing_files(self):
        (self.source / "music/song.mp3").write_bytes(b"changed")
        pack = self.archive()
        dest = self.root / "installed"
        with self.assertRaisesRegex(ValueError, "hash mismatch"):
            assets.sync(dest, self.manifest, str(pack), assets.digest(pack))
        self.assertEqual(list(dest.iterdir()), [])

    def test_source_and_license_are_required(self):
        self.metadata["files"]["music/song.mp3"]["license"] = ""
        self.write_metadata()
        with self.assertRaisesRegex(ValueError, "source, and license"):
            assets.verify(self.source, self.manifest)

    def test_missing_required_file_is_an_error(self):
        (self.source / "music/song.mp3").unlink()
        with self.assertRaisesRegex(ValueError, "missing"):
            assets.verify(self.source, self.manifest)

    def test_pack_paths_stay_inside_install_root(self):
        for name in ("../escape", "/escape", "music\\escape", "C:/escape"):
            with self.subTest(name=name):
                pack = self.archive((name, "unexpected"))
                with self.assertRaisesRegex(ValueError, "Invalid media path"):
                    assets.sync(self.root / "installed", self.manifest,
                                str(pack), assets.digest(pack))

    def test_native_web_and_server_layouts(self):
        build = self.root / "build"
        build.mkdir()
        for name in ("signal", "signal_server", "signal.js", "signal.wasm", "play.html", "signal-touch-controls.js"):
            (build / name).write_bytes(name.encode())
        (build / "signal").chmod(0o755)
        for runtime in ("native", "web", "server"):
            destination = self.root / runtime
            assets.package(self.source, self.manifest, build, destination, runtime)
            self.assertTrue((destination / "SHA256SUMS").is_file())
            if runtime == "server":
                self.assertEqual(set(p.name for p in destination.iterdir()), {"signal_server", "SHA256SUMS"})
            else:
                media = destination / "assets" if runtime == "native" else destination
                self.assertEqual(len(assets.verify(media, self.manifest)), 3)
        self.assertEqual((self.root / "native/signal").stat().st_mode & 0o777, 0o755)


if __name__ == "__main__":
    unittest.main()
