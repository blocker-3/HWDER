"""Offline synthetic tests: never need/decrypt/distribute a real game or keys."""
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import first_run as setup
import local_game
import make_dist

TEST_ROOT = Path(__file__).resolve().parents[2] / "build" / "setup-tests"
TEST_ROOT.mkdir(parents=True, exist_ok=True)


def temporary_directory():
    return tempfile.TemporaryDirectory(dir=TEST_ROOT)


class Fixture(unittest.TestCase):
    def setUp(self):
        self.temp = temporary_directory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / "source [space] ü"
        self.root.mkdir()
        self.cache = Path(self.temp.name) / "private cache"
        for sub in ("runtime", "tools/recomp", "third_party"):
            (self.root / sub).mkdir(parents=True)
        for relative in ("CMakeLists.txt", "runtime/source.cpp", "tools/nso.py", "tools/nx_extract.py",
                         "tools/local_game.py", "tools/first_run.py", "tools/recomp/gen_main.py",
                         "tools/recomp/extra_entries.txt"):
            (self.root / relative).write_text("synthetic source", encoding="utf-8")
        self.modules = {name: ("NSO0fixture-" + name).encode() for name in local_game.MODULES}
        self.profile = {"id": "synthetic-only", "title_id": f"{local_game.TITLE_ID:016x}",
                        "modules_sha256": {n: hashlib.sha256(v).hexdigest() for n, v in self.modules.items()}}
        (self.root / "tools/supported_games.json").write_text(json.dumps({"profiles": [self.profile]}))
        self.nsp, self.keys = Path(self.temp.name) / "own.nsp", Path(self.temp.name) / "own.keys"
        self.nsp.write_bytes(b"synthetic NSP; no game content")
        self.keys.write_text("SYNTHETIC SECRET MUST NEVER BE COPIED")
        self.calls = []
        self.fail = None
        self.toolchain = {"lz4": "fixture", "clang": "fixture"}

    def run_stage(self, args, cwd):
        if "-m" in args:
            self.calls.append("generate")
            out = Path(args[args.index("--out") + 1])
            (out / "rc_main_000.c").write_text("synthetic translated fixture")
            if self.fail == "generate":
                raise subprocess.CalledProcessError(7, ["synthetic-generator"])
            if self.fail == "interrupt":
                raise KeyboardInterrupt()
            for n in ("sources.cmake", "funcs.h", "functable.c", "module_info.c"):
                if self.fail != "missing-generated":
                    (out / n).write_text("synthetic output")
        else:
            self.calls.append("build")
            out = Path(args[args.index("--build-dir") + 1])
            out.mkdir(parents=True, exist_ok=True)
            (out / "partial.obj").write_text("compiler progress")
            if self.fail == "build":
                raise subprocess.CalledProcessError(8, ["synthetic-build"])
            if self.fail == "noop-build":
                self.assertFalse((out / "hwder.exe").exists(), "uncertified exe must force Ninja relink")
                return
            (out / "hwder.exe").write_bytes(b"MZsynthetic-program-" + str(len(self.calls)).encode())

    def prepare(self):
        return setup.prepare(self.root, self.cache, self.nsp, self.keys, 2, self.toolchain,
                             self.run_stage, lambda nsp, keys: self.modules)

    def test_fresh_then_verified_cache_and_no_keys_copied(self):
        exe = self.prepare()
        self.assertEqual(self.calls, ["generate", "build"])
        self.assertEqual(self.prepare(), exe)
        self.assertEqual(self.calls, ["generate", "build"])
        for path in self.cache.rglob("*"):
            if path.is_file():
                self.assertNotIn(b"SYNTHETIC SECRET", path.read_bytes())
        self.assertTrue((self.cache / "last-success.json").is_file())
        self.assertFalse((self.root / "generated").exists())
        self.assertFalse((self.root / "build").exists())

    def test_runtime_change_only_rebuilds_and_preserves_play_state(self):
        exe = self.prepare()
        save = exe.parent / "save" / "player.bin"
        save.parent.mkdir()
        save.write_bytes(b"precious save")
        (exe.parent / "pipeline_cache.bin").write_bytes(b"private cache")
        (self.root / "runtime/source.cpp").write_text("new runtime")
        self.prepare()
        self.assertEqual(self.calls, ["generate", "build", "build"])
        self.assertEqual(save.read_bytes(), b"precious save")
        self.assertEqual((exe.parent / "pipeline_cache.bin").read_bytes(), b"private cache")

    def test_generator_game_and_toolchain_changes_invalidate(self):
        self.prepare()
        (self.root / "tools/recomp/gen_main.py").write_text("changed generator")
        self.prepare()
        self.assertEqual(self.calls, ["generate", "build"] * 2)
        self.nsp.write_bytes(b"different complete container, same fixture modules")
        self.prepare()
        self.assertEqual(self.calls, ["generate", "build"] * 3)
        self.toolchain["clang"] = "new compiler"
        self.prepare()
        self.assertEqual(self.calls[-1], "build")
        self.assertEqual(len(self.calls), 7)

    def test_extra_entries_invalidate_generator(self):
        self.prepare()
        (self.root / "data").mkdir()
        (self.root / "data/extra_entries.txt").write_text("0xfffe00000")
        self.prepare()
        self.assertEqual(self.calls, ["generate", "build"] * 2)

    def test_failed_build_retains_success_and_resumes_without_regeneration(self):
        exe = self.prepare()
        previous, pointer = exe.read_bytes(), (self.cache / "last-success.json").read_bytes()
        (self.root / "runtime/source.cpp").write_text("updated runtime")
        self.fail = "build"
        with self.assertRaises(subprocess.CalledProcessError):
            self.prepare()
        self.assertEqual(exe.read_bytes(), previous)
        self.assertEqual((self.cache / "last-success.json").read_bytes(), pointer)
        partial_build = next(p.parent for p in (self.cache / "builds").rglob("partial.obj")
                             if not (p.parent / "complete.json").exists())
        self.fail = None
        self.prepare()
        self.assertTrue((partial_build / "complete.json").exists())
        self.assertEqual(self.calls, ["generate", "build", "build", "build"])

    def test_partial_and_interrupted_generation_never_ready(self):
        for failure in ("generate", "interrupt", "missing-generated"):
            self.fail = failure
            with self.assertRaises((subprocess.CalledProcessError, KeyboardInterrupt, ValueError)):
                self.prepare()
            self.assertFalse((self.cache / "last-success.json").exists())
            self.assertFalse(list((self.cache / "generated").glob("*/complete.json")))
        self.fail = None
        self.prepare()
        self.assertEqual(self.calls, ["generate"] * 4 + ["build"])

    def test_corrupt_completed_generated_output_regenerates(self):
        self.prepare()
        next((self.cache / "generated").glob("*/output/rc_*.c")).write_text("damaged")
        self.prepare()
        self.assertEqual(self.calls, ["generate", "build", "generate"])

    def test_tampered_executable_rebuilt_and_failed_rebuild_not_published(self):
        exe = self.prepare()
        original = exe.read_bytes()
        cached = next((self.cache / "builds").glob("*/hwder.exe"))
        cached.write_bytes(b"MZcorrupted")
        self.fail = "build"
        with self.assertRaises(subprocess.CalledProcessError):
            self.prepare()
        self.assertEqual(exe.read_bytes(), original)
        self.assertFalse((cached.parent / "complete.json").exists())
        self.fail = None
        self.prepare()
        self.assertEqual(self.calls, ["generate", "build", "build", "build"])

    def test_noop_build_cannot_recertify_damaged_executable(self):
        exe = self.prepare()
        previous = exe.read_bytes()
        cached = next((self.cache / "builds").glob("*/hwder.exe"))
        cached.write_bytes(b"MZdamaged-newer-file")
        self.fail = "noop-build"
        with self.assertRaisesRegex(ValueError, "without a Windows executable"):
            self.prepare()
        self.assertEqual(exe.read_bytes(), previous)
        self.assertFalse(cached.exists())
        self.assertFalse((cached.parent / "complete.json").exists())

    def test_unsupported_version_rejected_before_stages(self):
        self.modules["main"] += b"different version"
        with self.assertRaisesRegex(ValueError, "Unsupported game version"):
            self.prepare()
        self.assertEqual(self.calls, [])
        self.assertFalse((self.cache / "last-success.json").exists())

    def test_source_change_during_generation_is_not_cached(self):
        runner = self.run_stage
        def mutate(args, cwd):
            runner(args, cwd)
            if "-m" in args:
                (self.root / "tools/recomp/gen_main.py").write_text("changed during generation")
        with self.assertRaisesRegex(ValueError, "Generator inputs changed"):
            setup.prepare(self.root, self.cache, self.nsp, self.keys, 2, self.toolchain,
                          mutate, lambda n, k: self.modules)
        self.assertFalse(list((self.cache / "generated").glob("*/complete.json")))

    def test_source_change_during_build_is_not_published(self):
        runner = self.run_stage
        def mutate(args, cwd):
            runner(args, cwd)
            if "--build-dir" in args:
                (self.root / "runtime/source.cpp").write_text("changed during build")
        with self.assertRaisesRegex(ValueError, "Build inputs changed"):
            setup.prepare(self.root, self.cache, self.nsp, self.keys, 2, self.toolchain,
                          mutate, lambda n, k: self.modules)
        self.assertFalse((self.cache / "last-success.json").exists())
        self.assertFalse(list((self.cache / "builds").glob("*/complete.json")))

    def test_publish_failure_preserves_last_success(self):
        exe = self.prepare()
        previous, pointer = exe.read_bytes(), (self.cache / "last-success.json").read_bytes()
        (self.root / "runtime/source.cpp").write_text("updated runtime")
        with patch.object(setup.shutil, "copyfile", side_effect=PermissionError("exe in use")):
            with self.assertRaises(PermissionError):
                self.prepare()
        self.assertEqual(exe.read_bytes(), previous)
        self.assertEqual((self.cache / "last-success.json").read_bytes(), pointer)
        self.prepare()
        self.assertEqual(self.calls, ["generate", "build", "build"])

    def test_keys_changed_during_build_prevents_publication(self):
        runner = self.run_stage
        def mutate(args, cwd):
            runner(args, cwd)
            if "--build-dir" in args:
                self.keys.write_text("changed key file during compilation")
        with self.assertRaisesRegex(ValueError, "Game or keys input changed"):
            setup.prepare(self.root, self.cache, self.nsp, self.keys, 2, self.toolchain,
                          mutate, lambda n, k: self.modules)
        self.assertFalse((self.cache / "last-success.json").exists())
        self.assertFalse((self.cache / "play/hwder.exe").exists())

    def test_cache_lock_serializes_processes_and_releases_after_error(self):
        marker = self.cache / "child-entered.txt"
        script = (f"import sys; sys.path.insert(0, {str(Path(setup.__file__).parent)!r}); "
                  "from first_run import setup_lock; from pathlib import Path\n"
                  f"with setup_lock(Path({str(self.cache)!r})):\n"
                  f"    Path({str(marker)!r}).write_text('entered')\n")
        with setup.setup_lock(self.cache):
            child = subprocess.Popen([sys.executable, "-c", script], stdout=subprocess.DEVNULL)
            try:
                with self.assertRaises(subprocess.TimeoutExpired):
                    child.wait(timeout=0.25)
                self.assertFalse(marker.exists())
            except BaseException:
                child.terminate()
                child.wait(timeout=5)
                raise
        try:
            self.assertEqual(child.wait(timeout=10), 0)
        finally:
            if child.poll() is None:
                child.terminate()
                child.wait(timeout=5)
        self.assertTrue(marker.is_file())
        with self.assertRaisesRegex(RuntimeError, "synthetic failure"):
            with setup.setup_lock(self.cache):
                raise RuntimeError("synthetic failure")
        with setup.setup_lock(self.cache):
            pass


class Parsers(unittest.TestCase):
    @staticmethod
    def pfs(name, content=b"fixture"):
        names = name.encode() + b"\0"
        return (struct.pack("<4sIII", b"PFS0", 1, len(names), 0)
                + struct.pack("<QQII", 0, len(content), 0, 0) + names + content)

    def test_bounded_pfs_accepts_regular_file(self):
        data = self.pfs("main")
        self.assertEqual(local_game.bounded_pfs(lambda o, n: data[o:o+n], len(data))["main"][1], 7)

    def test_bounded_pfs_rejects_traversal_and_malformed_bounds(self):
        for name in ("../main", "..", "C:main", "sub\\main", "sub/main"):
            data = self.pfs(name)
            with self.assertRaises(ValueError):
                local_game.bounded_pfs(lambda o, n: data[o:o+n], len(data))
        for data in (b"", b"PFS0", struct.pack("<4sIII", b"PFS0", 1000000000, 0, 0), self.pfs("main")[:-1]):
            with self.assertRaises(ValueError):
                local_game.bounded_pfs(lambda o, n: data[o:o+n], len(data))

    def test_bad_marker_types_are_incomplete(self):
        with temporary_directory() as tmp:
            p = Path(tmp) / "complete.json"
            for text in ("[]", "null", "broken", "42"):
                p.write_text(text)
                self.assertFalse(setup.valid_stage(Path(tmp), "fixture"))

    @staticmethod
    def section(files):
        names = b""
        entries, content = [], b""
        for name, value in files.items():
            entries.append(struct.pack("<QQII", len(content), len(value), len(names), 0))
            names += name.encode() + b"\0"
            content += value
        data = struct.pack("<4sIII", b"PFS0", len(files), len(names), 0) + b"".join(entries) + names + content
        return SimpleNamespace(fs_type=1, start=0, end=len(data), data_region=lambda: (0, len(data)),
                               read=lambda o, n: data[o:o+n])

    def test_program_can_contain_separate_logo_pfs_section(self):
        exefs = self.section({n: b"NSO0synthetic" for n in local_game.MODULES})
        logos = self.section({"NintendoLogo.png": b"synthetic logo", "StartupMovie.gif": b"synthetic movie"})
        program = SimpleNamespace(sections=[logos, exefs])
        selected, _, files = local_game.program_exefs(program)
        self.assertIs(selected, exefs)
        self.assertEqual(set(files), set(local_game.MODULES))

    def test_ambiguous_or_extra_module_sets_rejected(self):
        exefs = self.section({n: b"NSO0synthetic" for n in local_game.MODULES})
        other = self.section({**{n: b"synthetic" for n in local_game.MODULES}, "subsdk2": b"unsupported"})
        for sections in ([exefs, exefs], [other], []):
            with self.assertRaises(ValueError):
                local_game.program_exefs(SimpleNamespace(sections=sections))


class Packaging(unittest.TestCase):
    def test_source_kit_omits_private_artifacts_even_if_present(self):
        with temporary_directory() as tmp:
            root = Path(tmp) / "source"
            root.mkdir()
            tracked_files = list(make_dist.TOP_FILES) + ["tools/first_run.py", "runtime/source.cpp",
                "generated/rc_main.c", "build/hwder.exe", "prod.keys", "cache/shader.bin", "game.nsp",
                "save/player.bin", "pipeline_warmup.bin"]
            for name in tracked_files:
                p = root / name
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_text("synthetic project" if make_dist.safe_member(name) else "DO NOT PACKAGE")
            for sub in make_dist.SUBMODULES:
                directory = root / "third_party" / sub
                directory.mkdir(parents=True)
                for name in (".git", "LICENSE.txt", "source.c", "prebuilt.exe", "font.ttf"):
                    (directory / name).write_text("synthetic dependency")
            def listing(path):
                return tracked_files if path == root else ["LICENSE.txt", "source.c", "prebuilt.exe", "font.ttf", ".git"]
            archive = make_dist.package(root, Path(tmp) / "dist", "fixture", listing)
            with zipfile.ZipFile(archive) as z:
                self.assertFalse(any(b"DO NOT PACKAGE" in z.read(n) for n in z.namelist()))
                self.assertTrue(all(make_dist.safe_member(n) for n in z.namelist()))
                self.assertFalse(any(n.endswith(("prebuilt.exe", "font.ttf")) for n in z.namelist()))
                manifest = json.loads(z.read("fixture/source-manifest.json"))
                self.assertEqual(len(z.namelist()), len(manifest) + 1)
                for name, sha in manifest.items():
                    self.assertEqual(hashlib.sha256(z.read("fixture/" + name)).hexdigest(), sha)

    def test_packaging_rejects_private_path_inside_allowed_source_root(self):
        with temporary_directory() as tmp:
            with self.assertRaisesRegex(ValueError, "Private/disallowed"):
                make_dist.release_files(Path(tmp), lambda p: ["tools/generated/rc_game.c"])

    def test_packaging_rejects_unsafe_names(self):
        for name in ("../escape", "C:escape", "a/b", "..", ""):
            with self.assertRaises(ValueError):
                make_dist.package(Path("."), Path("."), name, lambda p: [])


if __name__ == "__main__":
    unittest.main()
