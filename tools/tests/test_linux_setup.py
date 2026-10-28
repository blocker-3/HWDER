"""Offline tests of the Linux setup backend (tools/linux_setup.py): no game, keys, network or toolchain needed."""
import hashlib
import io
import json
import os
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import linux_setup  # noqa: E402

TEST_ROOT = Path(__file__).resolve().parents[2] / "build" / "setup-tests"
TEST_ROOT.mkdir(parents=True, exist_ok=True)


class Quiet(linux_setup.Report):
    def __init__(self):
        super().__init__(True)
        self.events = []

    def emit(self, kind, **fields):
        self.events.append({"type": kind, **fields})


class LinuxSetupTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=TEST_ROOT)
        self.addCleanup(self.temp.cleanup)
        self.dir = Path(self.temp.name)
        patcher = patch.dict(os.environ, {"XDG_DATA_HOME": str(self.dir / "data")})
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_data_folder_follows_xdg_and_ignores_relative_values(self):
        self.assertEqual(linux_setup.data_dir(), self.dir / "data" / "HWDER")
        with patch.dict(os.environ, {"XDG_DATA_HOME": "relative/path"}):
            self.assertEqual(linux_setup.data_dir(), Path.home() / ".local" / "share" / "HWDER")
        self.assertEqual(linux_setup.play_exe(), self.dir / "data" / "HWDER" / "build" / "play" / "hwder")

    def test_check_file_accepts_real_shapes_and_explains_problems(self):
        nsp = self.dir / "game.nsp"
        with nsp.open("wb") as f:
            f.write(b"PFS0" + b"\0" * 60)
            f.truncate(1024 ** 3 + 10)  # sparse: the size check only reads the length
        self.assertTrue(linux_setup.check_file("nsp", str(nsp))[0])
        small = self.dir / "small.nsp"
        small.write_bytes(b"PFS0" + b"\0" * 100)
        self.assertIn("too small", linux_setup.check_file("nsp", str(small))[1])
        self.assertTrue(linux_setup.check_file("update", str(small))[0])
        wrong = self.dir / "game.xci"
        wrong.write_bytes(b"HEAD")
        self.assertIn(".nsp", linux_setup.check_file("nsp", str(wrong))[1])
        fake = self.dir / "fake.nsp"
        fake.write_bytes(b"NOPE" * 10)
        self.assertIn("PFS0", linux_setup.check_file("update", str(fake))[1])
        keys = self.dir / "prod.keys"
        keys.write_text("header_key = 00\nkey_area_key_application_00 = 11\n")
        self.assertEqual(linux_setup.check_file("keys", str(keys)), (True, "2 keys"))
        keys.write_text("title_kek_00 = 00\n")
        self.assertFalse(linux_setup.check_file("keys", str(keys))[0])
        self.assertEqual(linux_setup.check_file("update", ""), (True, "not used"))
        self.assertEqual(linux_setup.check_file("keys", ""), (False, "not chosen yet"))
        self.assertEqual(linux_setup.check_file("keys", str(self.dir / "missing")), (False, "file not found"))

    @unittest.skipIf(os.name == "nt", "POSIX path forms")
    def test_clean_path_handles_drops_quotes_and_home(self):
        self.assertEqual(linux_setup.clean_path("'/a b/c.nsp' "), "/a b/c.nsp")
        self.assertEqual(linux_setup.clean_path("file:///a%20b/c.nsp"), "/a b/c.nsp")
        self.assertEqual(linux_setup.clean_path("/a\\ b/c.nsp"), "/a b/c.nsp")
        self.assertEqual(linux_setup.clean_path("~/x.keys"), str(Path.home() / "x.keys"))
        self.assertEqual(linux_setup.clean_path("  "), "")

    def test_desktop_entries_quote_exec_and_name_the_game(self):
        self.assertEqual(linux_setup.desktop_quote("/home/a/play/hwder-steam.sh"), "/home/a/play/hwder-steam.sh")
        self.assertEqual(linux_setup.desktop_quote('/x y/$a"b'), '"/x y/\\$a\\"b"')
        text = linux_setup.entry_text("N", "C", "/x y/run.sh", "/x y", "icon", "Game;")
        self.assertIn('Exec="/x y/run.sh"\n', text)
        self.assertIn("Terminal=false\n", text)

    @unittest.skipIf(os.name == "nt", "executable bits and /bin/sh")
    def test_desktop_entries_written_for_menu_and_desktop(self):
        play = linux_setup.play_exe()
        play.parent.mkdir(parents=True)
        play.write_bytes(b"\x7fELF")
        desktop = self.dir / "Desktop"
        desktop.mkdir()
        with patch.object(linux_setup, "game_icon", return_value="icon"), \
                patch.object(linux_setup, "user_desktop", return_value=desktop):
            written = linux_setup.write_desktop_entries(self.dir / "app", Quiet(), desktop_icon=True)
        self.assertEqual({p.name for p in written}, {"hwder.desktop", "hwder-setup.desktop"})
        game = (self.dir / "data" / "applications" / "hwder.desktop").read_text()
        self.assertIn(f"Exec={play.parent / 'hwder-steam.sh'}", game)
        self.assertTrue((desktop / "hwder.desktop").stat().st_mode & 0o100)

    def test_download_checks_size_and_hash(self):
        source = self.dir / "artifact.bin"
        source.write_bytes(b"pinned bytes")
        digest = hashlib.sha256(b"pinned bytes").hexdigest()
        target = self.dir / "out.bin"
        linux_setup.download(source.as_uri(), target, digest, 12, Quiet(), "artifact")
        self.assertEqual(target.read_bytes(), b"pinned bytes")
        with patch.object(linux_setup.time, "sleep"):
            with self.assertRaisesRegex(linux_setup.SetupError, "SHA-256"):
                linux_setup.download(source.as_uri(), target, "0" * 64, 12, Quiet(), "artifact")
            with self.assertRaisesRegex(linux_setup.SetupError, "larger"):
                linux_setup.download(source.as_uri(), target, digest, 5, Quiet(), "artifact")

    def test_archives_cannot_write_outside_their_folder(self):
        bad_tar = self.dir / "bad.tar.gz"
        with tarfile.open(bad_tar, "w:gz") as tar:
            info = tarfile.TarInfo("../escape.txt")
            info.size = 1
            tar.addfile(info, io.BytesIO(b"x"))
        out = self.dir / "unpack"
        out.mkdir()
        with self.assertRaises((linux_setup.SetupError, tarfile.TarError)):
            linux_setup.safe_extract_tar(bad_tar, out)
        self.assertFalse((self.dir / "escape.txt").exists())
        bad_zip = self.dir / "bad.zip"
        with zipfile.ZipFile(bad_zip, "w") as z:
            z.writestr("../escape.txt", "x")
        with self.assertRaises(linux_setup.SetupError):
            linux_setup.safe_extract_zip(bad_zip, out)
        self.assertFalse((self.dir / "escape.txt").exists())

    def test_toolchain_manifest_is_pinned_and_complete(self):
        value = linux_setup.manifest()
        ids = {a["id"] for a in value["artifacts"]}
        self.assertTrue({"zig", "cmake", "ninja", "python", "cryptography", "cffi", "pycparser", "lz4",
                         "zstandard"} <= ids)
        for artifact in value["artifacts"]:
            self.assertRegex(artifact["sha256"], "^[0-9a-f]{64}$")
            self.assertTrue(artifact["url"].startswith("https://"))
            self.assertGreater(artifact["size"], 0)
            self.assertTrue(artifact["required"])
            self.assertIn("license", artifact)

    def test_build_environment_uses_only_the_pinned_tools_and_release_libraries(self):
        tc, app = self.dir / "tc", self.dir / "app"
        with patch.dict(os.environ, {"CFLAGS": "-O0", "LD_PRELOAD": "/overlay.so", "HWDER_KEYS": "/k",
                                     "PYTHONPATH": "/x"}):
            env = linux_setup.build_environment(tc, app)
        for name in ("CFLAGS", "LD_PRELOAD", "HWDER_KEYS", "PYTHONPATH"):
            self.assertNotIn(name, env)
        self.assertEqual(env["CC"], str(tc / "bin" / "zig-cc"))
        self.assertEqual(env["HWDER_LINUX_DEPS"], str(app / "linux-deps"))
        self.assertTrue(env["PATH"].startswith(str(tc / "bin")))

    @unittest.skipIf(os.name == "nt", "shell wrappers")
    def test_wrappers_target_glibc_235(self):
        tc = self.dir / "tc"
        tc.mkdir()
        linux_setup.write_wrappers(tc, self.dir / "final")
        cc = (tc / "bin" / "zig-cc").read_text()
        self.assertIn("cc -target x86_64-linux-gnu.2.35", cc)
        self.assertIn(str(self.dir / "final" / "zig" / "zig"), cc)  # where the folder ends up, not the staging one
        self.assertTrue((tc / "bin" / "zig-ar").stat().st_mode & 0o100)

    def test_install_app_copies_verified_release_and_skips_checkouts(self):
        report = Quiet()
        self.assertEqual(linux_setup.install_app(report), linux_setup.ROOT)  # a git checkout builds in place
        release = self.dir / "release"
        (release / "tools").mkdir(parents=True)
        (release / "tools" / "a.py").write_text("print(1)")
        files = {"tools/a.py": hashlib.sha256(b"print(1)").hexdigest()}
        (release / "release.json").write_text(json.dumps({"version": "v9", "files": files}))
        with patch.object(linux_setup, "ROOT", release):
            target = linux_setup.install_app(report)
            self.assertEqual(target, linux_setup.app_dir())
            self.assertEqual((target / "tools" / "a.py").read_text(), "print(1)")
            self.assertEqual(linux_setup.install_app(report), target)  # unchanged: no second copy
            (release / "tools" / "a.py").write_text("tampered")
            files2 = dict(files)
            (release / "release.json").write_text(json.dumps({"version": "v10", "files": files2 | {"x": "0"}}))
            with self.assertRaises((linux_setup.SetupError, OSError)):
                linux_setup.install_app(report)
            self.assertEqual((target / "tools" / "a.py").read_text(), "print(1)")  # old copy kept

    def test_update_offered_when_the_release_differs_from_the_build(self):
        release = {"version": "v0.2.3-1-gabc", "files": {"x": "0"}}
        self.assertFalse(linux_setup.update_needed(release, "v0.2.3-1-gabc", True))
        self.assertTrue(linux_setup.update_needed(release, "v0.2.2", True))      # older build
        self.assertTrue(linux_setup.update_needed(release, "v0.2.4", True))      # newer build (another branch)
        self.assertTrue(linux_setup.update_needed(release, None, True))          # never recorded
        self.assertFalse(linux_setup.update_needed(release, "v0.2.2", False))    # nothing built yet: plain Prepare
        self.assertFalse(linux_setup.update_needed({"version": "v0.2.3 (source checkout)", "files": None}, "x", True))

    def test_json_report_lines(self):
        out = io.StringIO()
        with patch("sys.stdout", out):
            report = linux_setup.Report(True)
            report.stage("tools", "Downloading")
            report.progress(1.5, "x")
            self.assertEqual(report.result(False, "no"), 1)
        lines = [json.loads(line) for line in out.getvalue().splitlines()]
        self.assertEqual([l["type"] for l in lines], ["stage", "progress", "result"])
        self.assertEqual(lines[1]["fraction"], 1.0)


if __name__ == "__main__":
    unittest.main()
