"""Offline synthetic tests: never need/decrypt/distribute a real game or keys."""
import hashlib
import json
import os
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
import build
import build_lock

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
        self.failure = None
        self.toolchain = {"lz4": "fixture", "clang": "fixture"}

    def run_stage(self, args, cwd):
        if "-m" in args:
            self.calls.append("generate")
            out = Path(args[args.index("--out") + 1])
            (out / "rc_main_000.c").write_text("synthetic translated fixture")
            if self.failure == "generate":
                raise subprocess.CalledProcessError(7, ["synthetic-generator"])
            if self.failure == "interrupt":
                raise KeyboardInterrupt()
            for n in ("sources.cmake", "funcs.h", "functable.c", "module_info.c"):
                if self.failure != "missing-generated":
                    (out / n).write_text("synthetic output")
        else:
            self.calls.append("build")
            out = Path(args[args.index("--build-dir") + 1])
            out.mkdir(parents=True, exist_ok=True)
            (out / "partial.obj").write_text("compiler progress")
            if self.failure == "build":
                raise subprocess.CalledProcessError(8, ["synthetic-build"])
            if self.failure == "noop-build":
                self.assertFalse((out / "hwder.exe").exists(), "uncertified exe must force Ninja relink")
                return
            (out / "hwder.exe").write_bytes(b"MZsynthetic-program-" + str(len(self.calls)).encode())

    def prepare(self):
        return setup.prepare(self.root, self.cache, self.nsp, self.keys, 2, self.toolchain,
                             self.run_stage, lambda nsp, keys: self.modules)

    def linux_stage(self, args, cwd):
        self.run_stage(args, cwd)
        if "-m" not in args:
            out = Path(args[args.index("--build-dir") + 1])
            windows = out / "hwder.exe"
            if windows.exists():
                windows.unlink()
            header = bytearray(64)
            header[:7] = b"\x7fELF\x02\x01\x01"
            struct.pack_into("<HHI", header, 16, 3, 62, 1)
            if self.failure == "wrong-linux-arch":
                struct.pack_into("<H", header, 18, 183)  # AArch64 is not the Ally target.
            (out / "hwder").write_bytes(header + b"synthetic ELF; never executed")
            (out / "hwder").chmod(0o755)

    def linux_prepare(self):
        self.toolchain["target_platform"] = "linux-x86_64"
        with setup.setup_lock(self.cache):
            return setup.prepare(self.root, self.cache, self.nsp, self.keys, 2, self.toolchain,
                                 self.linux_stage, lambda nsp, keys: self.modules)

    def test_linux_artifact_verified_cache_and_mode(self):
        executable = self.linux_prepare()
        self.assertEqual(executable.name, "hwder")
        self.assertFalse((executable.parent / "hwder.exe").exists())
        self.assertEqual(self.linux_prepare(), executable)
        self.assertEqual(self.calls, ["generate", "build"])
        setup.verify_executable(executable, self.toolchain)
        if os.name != "nt":
            self.assertTrue(executable.stat().st_mode & 0o100)
            self.assertEqual(self.cache.stat().st_mode & 0o077, 0)

    def test_linux_wrong_architecture_retains_last_success_and_saves(self):
        executable = self.linux_prepare()
        original = executable.read_bytes()
        save = executable.parent / "save" / "player.bin"
        save.parent.mkdir(); save.write_bytes(b"precious save")
        pointer = (self.cache / "last-success.json").read_bytes()
        (self.root / "runtime/source.cpp").write_text("new source")
        self.failure = "wrong-linux-arch"
        with self.assertRaisesRegex(ValueError, "Linux x86-64 executable"):
            self.linux_prepare()
        self.assertEqual(executable.read_bytes(), original)
        self.assertEqual(save.read_bytes(), b"precious save")
        self.assertEqual((self.cache / "last-success.json").read_bytes(), pointer)

    def test_linux_failed_pointer_after_copy_backup_restores_executable(self):
        executable = self.linux_prepare()
        original = executable.read_bytes()
        pointer = (self.cache / "last-success.json").read_bytes()
        (self.root / "runtime/source.cpp").write_text("updated runtime")
        write_marker = setup.atomic_json
        def fail_pointer(path, value):
            if Path(path).name == "last-success.json":
                raise OSError("synthetic pointer failure")
            return write_marker(path, value)
        with patch.object(setup.os, "link", side_effect=OSError("no hardlinks")), \
             patch.object(setup, "atomic_json", side_effect=fail_pointer):
            with self.assertRaisesRegex(OSError, "synthetic pointer failure"):
                self.linux_prepare()
        self.assertEqual(executable.read_bytes(), original)
        self.assertEqual((self.cache / "last-success.json").read_bytes(), pointer)
        if os.name != "nt":
            self.assertTrue(executable.stat().st_mode & 0o100)
        self.assertFalse(list(executable.parent.glob("*.tmp")))

    def test_linux_rejects_mz_and_wrong_elf_classes(self):
        self.toolchain["target_platform"] = "linux-x86_64"
        path = Path(self.temp.name) / "artifact"
        for content in (b"MZwrong host", b"\x7fELF", b"\x7fELF\x01\x01\x01" + bytes(57)):
            path.write_bytes(content); path.chmod(0o755)
            self.assertFalse(setup.executable_valid(path, self.toolchain))

    @unittest.skipIf(os.name == "nt", "POSIX executable mode contract")
    def test_linux_repairs_cached_and_published_execute_modes(self):
        executable = self.linux_prepare()
        cached = next((self.cache / "builds").glob("*/hwder"))
        cached.chmod(0o644)
        self.linux_prepare()
        self.assertEqual(self.calls, ["generate", "build", "build"])
        executable.chmod(0o644)
        self.linux_prepare()
        self.assertTrue(executable.stat().st_mode & 0o100)
        self.assertEqual(self.calls, ["generate", "build", "build"])

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

    def test_managed_args_cache_separation_and_image_gate_before_publish(self):
        old=self.prepare().read_bytes()
        self.toolchain.update(kind="llvm-mingw-ucrt-static-cxx-v1",managed_directory="private toolset")
        commands=[]
        def run(args,cwd):
            commands.append(list(args));self.run_stage(args,cwd)
        with patch("managed_tools.game_image",side_effect=ValueError("bad dependency")):
            with self.assertRaisesRegex(ValueError,"bad dependency"):
                setup.prepare(self.root,self.cache,self.nsp,self.keys,2,self.toolchain,run,
                              lambda n,k:self.modules,before_publish=lambda:self.fail("must not publish"))
        self.assertEqual((self.cache/"play/hwder.exe").read_bytes(),old)
        self.assertEqual(commands[0][1:4],["-B","-X","utf8"])
        self.assertEqual(len(commands),1)  # Existing generator stage is reused.
        self.assertIn("--managed-toolchain",commands[0]);self.assertIn("--managed-temp-dir",commands[0])
        self.assertEqual(self.calls,["generate","build","build"])
        # Generation cache is ABI-independent; complete program cache is not.
        with patch("managed_tools.game_image") as image:
            setup.prepare(self.root,self.cache,self.nsp,self.keys,2,self.toolchain,run,lambda n,k:self.modules)
            image.assert_called_once()
        with patch("managed_tools.game_image") as image:
            setup.prepare(self.root,self.cache,self.nsp,self.keys,2,self.toolchain,run,lambda n,k:self.modules)
            image.assert_called_once()  # Reused image is still gated.
        commands.clear()
        with patch("managed_tools.game_image"):
            setup.prepare(self.root,self.cache/"fresh",self.nsp,self.keys,2,self.toolchain,run,lambda n,k:self.modules)
        self.assertEqual(commands[0][1:4],["-B","-X","utf8"])
        self.assertIn("-m",commands[0])

    def test_update_replaces_main_selects_its_profile_and_is_recorded(self):
        updated_main = b"NSO0fixture-updated-main"
        (self.root / "tools/recomp/v9").mkdir()
        for name in ("symbols.txt", "extra_entries.txt"):
            (self.root / "tools/recomp/v9" / name).write_text("synthetic remapped " + name)
        update_profile = {"id": "synthetic-update", "title_id": f"{local_game.TITLE_ID:016x}",
                          "modules_sha256": {**self.profile["modules_sha256"],
                                             "main": hashlib.sha256(updated_main).hexdigest()},
                          "recomp": {"symbols": "tools/recomp/v9/symbols.txt",
                                     "extra": ["tools/recomp/v9/extra_entries.txt"]}}
        (self.root / "tools/supported_games.json").write_text(json.dumps({"profiles": [self.profile, update_profile]}))
        update = Path(self.temp.name) / "own-update.nsz"
        update.write_bytes(b"synthetic update; no game content")
        def unpack(path, keys, out):
            self.assertEqual((Path(path), Path(keys)), (update, self.keys))
            (out / "exefs").mkdir()
            (out / "exefs/main").write_bytes(updated_main)
            (out / "update.json").write_text("{}")
        commands = []
        def run(args, cwd):
            commands.append([str(a) for a in args]); self.run_stage(args, cwd)
        with patch("game_update.unpack", side_effect=unpack):
            exe = setup.prepare(self.root, self.cache, self.nsp, self.keys, 2, self.toolchain, run,
                                lambda nsp, keys: self.modules, update=update)
        generate = next(c for c in commands if "-m" in c)
        self.assertIn(str(self.root / "tools/recomp/v9/symbols.txt"), generate)
        self.assertIn(str(self.root / "tools/recomp/v9/extra_entries.txt"), generate)
        self.assertNotIn(str(self.root / "tools/recomp/extra_entries.txt"), generate)
        marker = json.loads((self.cache / "last-success.json").read_text())
        self.assertEqual(Path(marker["update_dir"], "exefs/main").read_bytes(), updated_main)
        self.assertTrue(exe.is_file())
        with self.assertRaisesRegex(ValueError, "update"):
            setup.prepare(self.root, self.cache, self.nsp, self.keys, 2, self.toolchain, run,
                          lambda nsp, keys: self.modules, update=Path(self.temp.name) / "missing.nsz")

    def test_default_prerequisites_remain_opt_in_managed(self):
        with patch("managed_tools.prerequisites",return_value={"kind":"private"}) as selected:
            self.assertEqual(setup.check_prerequisites(self.root,"private"),{"kind":"private"})
            selected.assert_called_once_with(self.root,"private")

    def test_ui_cancel_before_commit_retains_previous_play_certificate_and_save(self):
        exe = self.prepare()
        old, marker = exe.read_bytes(), (self.cache / "last-success.json").read_bytes()
        save = exe.parent / "save.bin"
        save.write_bytes(b"precious save")
        (self.root / "runtime/source.cpp").write_text("new version")
        calls = []
        def refuse():
            calls.append("permission")
            raise ValueError("cancel before ACK")
        with self.assertRaisesRegex(ValueError, "cancel before ACK"):
            setup.prepare(self.root, self.cache, self.nsp, self.keys, 2, self.toolchain,
                          self.run_stage, lambda n,k:self.modules, before_publish=refuse)
        self.assertEqual(calls, ["permission"])
        self.assertEqual(exe.read_bytes(), old)
        self.assertEqual((self.cache / "last-success.json").read_bytes(), marker)
        self.assertEqual(save.read_bytes(), b"precious save")

    def test_ui_ack_then_pointer_failure_rolls_back_existing_play(self):
        exe = self.prepare()
        old, marker = exe.read_bytes(), (self.cache / "last-success.json").read_bytes()
        (self.root / "runtime/source.cpp").write_text("new version")
        original = setup.atomic_json
        acknowledged = []
        def fail(path, value):
            if Path(path) == self.cache / "last-success.json":
                self.assertEqual(acknowledged, ["commit"])
                raise OSError("commit failure")
            original(path, value)
        with patch.object(setup, "atomic_json", side_effect=fail), self.assertRaisesRegex(OSError, "commit failure"):
            setup.prepare(self.root, self.cache, self.nsp, self.keys, 2, self.toolchain,
                          self.run_stage, lambda n,k:self.modules, before_publish=lambda:acknowledged.append("commit"))
        self.assertEqual(exe.read_bytes(), old)
        self.assertEqual((self.cache / "last-success.json").read_bytes(), marker)

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
        self.failure = "build"
        with self.assertRaises(subprocess.CalledProcessError):
            self.prepare()
        self.assertEqual(exe.read_bytes(), previous)
        self.assertEqual((self.cache / "last-success.json").read_bytes(), pointer)
        partial_build = next(p.parent for p in (self.cache / "builds").rglob("partial.obj")
                             if not (p.parent / "complete.json").exists())
        self.failure = None
        self.prepare()
        self.assertTrue((partial_build / "complete.json").exists())
        self.assertEqual(self.calls, ["generate", "build", "build", "build"])

    def test_partial_and_interrupted_generation_never_ready(self):
        for failure in ("generate", "interrupt", "missing-generated"):
            self.failure = failure
            with self.assertRaises((subprocess.CalledProcessError, KeyboardInterrupt, ValueError)):
                self.prepare()
            self.assertFalse((self.cache / "last-success.json").exists())
            self.assertFalse(list((self.cache / "generated").glob("*/complete.json")))
        self.failure = None
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
        self.failure = "build"
        with self.assertRaises(subprocess.CalledProcessError):
            self.prepare()
        self.assertEqual(exe.read_bytes(), original)
        self.assertFalse((cached.parent / "complete.json").exists())
        self.failure = None
        self.prepare()
        self.assertEqual(self.calls, ["generate", "build", "build", "build"])

    def test_noop_build_cannot_recertify_damaged_executable(self):
        exe = self.prepare()
        previous = exe.read_bytes()
        cached = next((self.cache / "builds").glob("*/hwder.exe"))
        cached.write_bytes(b"MZdamaged-newer-file")
        self.failure = "noop-build"
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

    def test_extracted_input_change_during_generation_is_not_certified(self):
        runner = self.run_stage
        def mutate(args, cwd):
            runner(args, cwd)
            if "-m" in args:
                exefs = Path(args[args.index("--exefs") + 1])
                (exefs / "main").write_bytes(b"NSO0changed during generation")
        with self.assertRaisesRegex(ValueError, "Extracted inputs changed"):
            setup.prepare(self.root, self.cache, self.nsp, self.keys, 2, self.toolchain,
                          mutate, lambda n, k: self.modules)
        self.assertFalse(list((self.cache / "generated").glob("*/complete.json")))
        self.assertNotIn("build", self.calls)
        self.prepare()
        self.assertTrue((self.cache / "last-success.json").exists())

    def test_generated_input_change_during_build_quarantines_partial_objects(self):
        exe = self.prepare()
        previous, pointer = exe.read_bytes(), (self.cache / "last-success.json").read_bytes()
        (self.root / "runtime/source.cpp").write_text("updated runtime")
        runner = self.run_stage
        damaged_build = []
        def mutate(args, cwd):
            runner(args, cwd)
            if "--build-dir" in args:
                damaged_build.append(Path(args[args.index("--build-dir") + 1]))
                generated = Path(args[args.index("--generated-dir") + 1])
                (generated / "rc_main_000.c").write_text("changed during compilation")
        with self.assertRaisesRegex(ValueError, "Cached build inputs changed during compilation"):
            setup.prepare(self.root, self.cache, self.nsp, self.keys, 2, self.toolchain,
                          mutate, lambda n, k: self.modules)
        self.assertEqual(exe.read_bytes(), previous)
        self.assertEqual((self.cache / "last-success.json").read_bytes(), pointer)
        self.assertFalse(damaged_build[0].exists())
        invalid = list((self.cache / "builds").glob("*.invalid-*"))
        self.assertEqual(len(invalid), 1)
        self.assertTrue((invalid[0] / "partial.obj").exists())
        self.assertFalse((invalid[0] / "complete.json").exists())
        self.prepare()
        self.assertEqual(self.calls, ["generate", "build", "build", "generate", "build"])

    def test_generated_content_identity_controls_native_cache(self):
        self.prepare()
        generated = next((self.cache / "generated").glob("*/output"))
        (generated / "rc_main_000.c").write_text("a different completed generator result")
        marker = setup.read_json(generated.parent / "complete.json")
        marker["files"] = setup.tree_hashes(generated)
        setup.atomic_json(generated.parent / "complete.json", marker)
        self.prepare()
        self.assertEqual(self.calls, ["generate", "build", "build"])

    def check_failed_consumer_quarantine(self, failure):
        exe = self.prepare()
        previous, pointer = exe.read_bytes(), (self.cache / "last-success.json").read_bytes()
        (self.root / "runtime/source.cpp").write_text("updated runtime")
        runner = self.run_stage
        attempted = []
        def mutate(args, cwd):
            runner(args, cwd)
            if "--build-dir" in args:
                bdir = Path(args[args.index("--build-dir") + 1])
                attempted.append(bdir)
                generated = Path(args[args.index("--generated-dir") + 1])
                if failure == "missing-input":
                    (generated / "rc_main_000.c").unlink()
                else:
                    (generated / "rc_main_000.c").write_text("changed during compiler work")
                if failure == "interrupt":
                    raise KeyboardInterrupt()
                if failure == "no-executable":
                    (bdir / "hwder.exe").unlink()
                else:
                    raise subprocess.CalledProcessError(8, ["synthetic failed compiler"])
        with self.assertRaises((subprocess.CalledProcessError, KeyboardInterrupt, ValueError)):
            setup.prepare(self.root, self.cache, self.nsp, self.keys, 2, self.toolchain,
                          mutate, lambda n, k: self.modules)
        self.assertFalse(attempted[0].exists(), "all suspect native objects must leave the reusable build path")
        invalid = list((self.cache / "builds").glob("*.invalid-*"))
        self.assertEqual(len(invalid), 1)
        self.assertTrue((invalid[0] / "partial.obj").exists())
        self.assertEqual(exe.read_bytes(), previous)
        self.assertEqual((self.cache / "last-success.json").read_bytes(), pointer)
        self.prepare()
        self.assertEqual(self.calls, ["generate", "build", "build", "generate", "build"])

    def test_changed_cached_input_with_failed_compile_quarantines_objects(self):
        self.check_failed_consumer_quarantine("failed")

    def test_changed_cached_input_with_interrupted_compile_quarantines_objects(self):
        self.check_failed_consumer_quarantine("interrupt")

    def test_changed_cached_input_without_executable_quarantines_objects(self):
        self.check_failed_consumer_quarantine("no-executable")

    def test_deleted_cached_input_with_failed_compile_quarantines_objects(self):
        self.check_failed_consumer_quarantine("missing-input")

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

    def test_success_pointer_failure_rolls_back_executable_and_preserves_user_state(self):
        exe = self.prepare()
        previous, pointer = exe.read_bytes(), (self.cache / "last-success.json").read_bytes()
        save = exe.parent / "save.bin"
        save.write_bytes(b"precious save and private state")
        (self.root / "runtime/source.cpp").write_text("updated runtime")
        write_json = setup.atomic_json
        def fail_pointer(path, value):
            if Path(path) == self.cache / "last-success.json":
                raise OSError("synthetic last-success replacement failure")
            return write_json(path, value)
        with patch.object(setup, "atomic_json", side_effect=fail_pointer):
            with self.assertRaisesRegex(OSError, "synthetic last-success"):
                self.prepare()
        self.assertEqual(exe.read_bytes(), previous)
        self.assertEqual((self.cache / "last-success.json").read_bytes(), pointer)
        self.assertEqual(save.read_bytes(), b"precious save and private state")
        self.assertEqual(list(exe.parent.glob("*.tmp")), [])
        self.prepare()
        self.assertEqual(self.calls, ["generate", "build", "build"])

    def test_first_publication_pointer_failure_leaves_no_uncertified_playable(self):
        write_json = setup.atomic_json
        def fail_pointer(path, value):
            if Path(path) == self.cache / "last-success.json":
                raise OSError("synthetic pointer failure")
            return write_json(path, value)
        with patch.object(setup, "atomic_json", side_effect=fail_pointer):
            with self.assertRaisesRegex(OSError, "synthetic pointer failure"):
                self.prepare()
        self.assertFalse((self.cache / "play/hwder.exe").exists())
        self.assertFalse((self.cache / "last-success.json").exists())

    def test_success_pointer_failure_rolls_back_without_hardlink_support(self):
        with patch.object(setup.os, "link", side_effect=OSError("hardlinks unavailable")):
            self.test_success_pointer_failure_rolls_back_executable_and_preserves_user_state()

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


class BuildLock(unittest.TestCase):
    @unittest.skipUnless(os.name == "nt", "host known-folder contract is Windows-specific")
    def test_redirected_localappdata_cannot_split_machine_lock(self):
        expected = build_lock.lock_path()
        with patch.dict(os.environ, {"LOCALAPPDATA": str(TEST_ROOT / "redirected-profile")}):
            self.assertEqual(build_lock.lock_path(), expected)
        self.assertEqual(expected.name, "build.lock")
        self.assertEqual(expected.parent.name, "hwder")

    @unittest.skipUnless(os.name == "nt", "host profile contract is Windows-specific")
    def test_known_folder_is_actual_host_profile_not_package_redirection(self):
        import winreg
        registry_path = r"Software\Microsoft\Windows\CurrentVersion\Explorer\User Shell Folders"
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, registry_path) as key:
            local_appdata = os.path.expandvars(winreg.QueryValueEx(key, "Local AppData")[0])
        self.assertEqual(build_lock.lock_path(), Path(local_appdata) / "hwder/build.lock")

    def test_single_file_build_is_inside_shared_lock(self):
        events = []
        from contextlib import contextmanager
        @contextmanager
        def synthetic_lock():
            events.append("lock")
            try:
                yield
            finally:
                events.append("unlock")
        with patch.object(build, "build_lock", synthetic_lock), \
             patch.object(build, "build", side_effect=lambda a: events.append(a.only_file)), \
             patch.object(sys, "argv", ["build.py", "--only-file", "synthetic.cpp"]):
            build.main()
        self.assertEqual(events, ["lock", "synthetic.cpp", "unlock"])

    def test_empty_legacy_lock_serializes_and_recovers_after_process_exit(self):
        with temporary_directory() as tmp:
            path, marker = Path(tmp) / "build.lock", Path(tmp) / "entered.txt"
            script = (f"import sys; sys.path.insert(0, {str(Path(build_lock.__file__).parent)!r}); "
                      "import build_lock; from pathlib import Path\n"
                      f"build_lock.lock_path = lambda: Path({str(path)!r})\n"
                      "with build_lock.build_lock():\n"
                      f"    Path({str(marker)!r}).write_text('entered')\n")
            with path.open("a+b") as legacy:
                if os.name == "nt":
                    import msvcrt
                    msvcrt.locking(legacy.fileno(), msvcrt.LK_NBLCK, 1)
                else:
                    import fcntl
                    fcntl.flock(legacy, fcntl.LOCK_EX | fcntl.LOCK_NB)
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
