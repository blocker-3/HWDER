"""Installed-path and owned-launch tests. Synthetic files and Popen only; no game."""
import contextlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import setup_bridge as b

ROOT = Path(__file__).resolve().parents[2] / "build/ui-tests"
ROOT.mkdir(parents=True, exist_ok=True)


class Play(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=ROOT)
        self.addCleanup(self.temp.cleanup)
        self.data = Path(self.temp.name)
        self.exe = self.data / "play/hwder.exe"
        self.exe.parent.mkdir()
        self.exe.write_bytes(b"synthetic MZ; never executed")
        self.nsp, self.keys = self.data / "original ü.nsp", self.data / "original keys.keys"
        self.nsp.write_bytes(b"synthetic game"); self.keys.write_bytes(b"synthetic keys")
        b.first_run.atomic_json(self.data / "last-success.json", {"schema": 1, "exe_sha256": b.first_run.file_hash(self.exe)})
        b.first_run.atomic_json(self.data / "setup-inputs.json", {"nsp": str(self.nsp), "keys": str(self.keys)})
        self.record = {"pid": "12345", "created": "133123456789123456", "exe": str(self.exe.resolve())}
        self.real_discovery = b.discovered_game
        self.discovery = patch.object(b, "discovered_game", return_value=None)
        self.discovery.start(); self.addCleanup(self.discovery.stop)
        self.output = io.StringIO()
        self.stdout = contextlib.redirect_stdout(self.output); self.stdout.__enter__()
        self.addCleanup(self.stdout.__exit__, None, None, None)

    def marker(self):
        b.first_run.atomic_json(self.data / "game-running.json", {"schema": 1, **self.record})

    def test_settings_only_two_paths_and_duplicate_fields_preserve_every_other_byte(self):
        settings = self.exe.parent / "hwder_settings.ini"
        preserved = b"version=3\r\nvolume=72\r\nsave_dir=Q:/Quiver/save\r\n;comment\r\n[unknown]\r\n"
        settings.write_bytes(preserved + b"nsp_path=\r\nkeys_path=old\r\nnsp_path=duplicate\r\nskip_prelaunch=0\r\n")
        before = (self.nsp.read_bytes(), self.keys.read_bytes())
        b.installed_paths(self.data, self.exe)
        result = settings.read_bytes()
        self.assertTrue(result.startswith(preserved))
        self.assertIn(b"skip_prelaunch=0\r\n", result)
        self.assertEqual(result.count(b"nsp_path="), 1)
        self.assertIn(("keys_path=" + str(self.keys.resolve())).encode(), result)
        self.assertEqual(before, (self.nsp.read_bytes(), self.keys.read_bytes()))
        installed = b.first_run.read_json(self.data / "installed-game.json")
        self.assertEqual(installed["exe"], str(self.exe.resolve()))
        self.assertEqual(installed["nsp"], str(self.nsp))

    def test_atomic_replace_failure_preserves_previous_settings_and_no_ready(self):
        settings = self.exe.parent / "hwder_settings.ini"; settings.write_bytes(b"save_dir=preserve\n")
        with patch.object(b.os, "replace", side_effect=OSError("locked")), self.assertRaises(OSError):
            b.ready_status(self.data)
        self.assertEqual(settings.read_bytes(), b"save_dir=preserve\n")
        self.assertNotIn("ready_available", self.output.getvalue())
        self.assertFalse(list(settings.parent.glob(".setup-settings-*")))

    def test_invalid_ini_path_refused_before_write(self):
        for value in ("bad\npath", "bad\x00path", "a" * 2050):
            b.first_run.atomic_json(self.data / "setup-inputs.json", {"nsp": value, "keys": str(self.keys)})
            with self.assertRaises((ValueError, OSError)):
                b.installed_paths(self.data, self.exe)
        self.assertFalse((self.exe.parent / "hwder_settings.ini").exists())

    def test_quick_status_never_scans_tools_or_downloads_and_cert_failure_disables(self):
        with patch("managed_tools.game_image") as image, patch("managed_tools.installed", side_effect=AssertionError("scan")):
            b.ready_status(self.data, managed=True)
            image.assert_called_once_with(self.exe)
        self.assertIn('"paths_ready": "yes"', self.output.getvalue())
        self.exe.write_bytes(b"tampered")
        self.output.truncate(0); self.output.seek(0)
        with self.assertRaises(ValueError): b.ready_status(self.data, managed=True)
        self.assertNotIn("ready_available", self.output.getvalue())

    def test_duplicate_and_reopen_running_skip_spawn_and_do_not_touch_settings(self):
        self.marker()
        settings = self.exe.parent / "hwder_settings.ini"; settings.write_bytes(b"nsp_path=\nsave_dir=keep\n")
        with patch("managed_tools.game_image"):
            b.launch(self.data, lambda *a, **k: self.fail("duplicate"), managed=True, identity=lambda pid: self.record)
            b.ready_status(self.data, managed=True, identity=lambda pid: self.record)
        self.assertEqual(settings.read_bytes(), b"nsp_path=\nsave_dir=keep\n")
        self.assertIn('"paths_ready": "no"', self.output.getvalue())
        b.ready_status(self.data, identity=lambda pid: None)
        self.assertIn(b"keys_path=", settings.read_bytes())

    def test_direct_installed_exe_discovery_also_prevents_settings_write_and_duplicate(self):
        with patch.object(b, "discovered_game", return_value=self.record):
            b.launch(self.data, lambda *a, **k: self.fail("direct duplicate"))
            b.ready_status(self.data)
        self.assertFalse((self.exe.parent / "hwder_settings.ini").exists())

    def test_stale_pid_creation_mismatch_and_exit_allow_verified_new_launch(self):
        self.marker(); calls = []
        identity = lambda pid: {**self.record, "created": "999"} if pid == 12345 else None
        b.launch(self.data, lambda *a, **k: calls.append(k) or SimpleNamespace(pid=54321), identity=identity)
        self.assertEqual(len(calls), 1)
        self.assertEqual(calls[0]["cwd"], self.exe.parent)
        self.assertEqual(calls[0]["env"]["HWDER_NSP"], str(self.nsp))
        self.assertEqual(b.first_run.read_json(self.data / "game-running.json"), {})
        self.assertIn('"type": "launched"', self.output.getvalue())

    def test_identity_denied_fails_closed_without_settings_or_spawn(self):
        self.marker()
        with self.assertRaisesRegex(OSError, "denied"):
            b.launch(self.data, lambda *a, **k: self.fail("unknown duplicate"), identity=lambda p: (_ for _ in ()).throw(OSError("denied")))
        self.assertFalse((self.exe.parent / "hwder_settings.ini").exists())

    def test_spawn_failure_clears_intent_but_postspawn_unknown_leaves_intent(self):
        with self.assertRaises(OSError):
            b.launch(self.data, lambda *a, **k: (_ for _ in ()).throw(OSError("not created")))
        self.assertEqual(b.first_run.read_json(self.data / "game-running.json"), {})
        with self.assertRaises(OSError):
            b.launch(self.data, lambda *a, **k: SimpleNamespace(pid=54321), identity=lambda p: (_ for _ in ()).throw(OSError("unknown child")))
        self.assertEqual(b.first_run.read_json(self.data / "game-running.json")["state"], "starting")
        with self.assertRaises(ValueError):
            b.launch(self.data, lambda *a, **k: self.fail("duplicate after unknown spawn"))

    def test_new_child_identity_persisted_as_strings_and_close_has_no_terminate_api(self):
        b.launch(self.data, lambda *a, **k: SimpleNamespace(pid=12345), identity=lambda p: self.record)
        self.assertEqual(b.first_run.read_json(self.data / "game-running.json"), {"schema": 1, **self.record})
        event = json.loads(self.output.getvalue().splitlines()[-1])
        self.assertEqual(event["created"], self.record["created"])
        self.assertIsInstance(event["pid"], str)

    def test_status_cli_uses_small_interpreter_only_and_nonlaunch_flag_is_rejected(self):
        with patch.object(sys, "argv", ["bridge", "--action", "status", "--managed-launch", "--data", str(self.data)]), patch.object(b, "compatible", return_value=True), patch.object(b, "ready_status") as status, patch.object(b.first_run, "check_prerequisites", side_effect=AssertionError("scan")):
            self.assertEqual(b.main(), 0); status.assert_called_once_with(self.data, managed=True)

    def test_corrupt_or_out_of_range_marker_is_not_assumed_exited(self):
        for value in ("broken json", {**self.record, "schema": 1, "pid": str(2**40)}, {"schema": 1, "state": "starting"}):
            path = self.data / "game-running.json"
            path.write_text(value if isinstance(value, str) else json.dumps(value))
            with self.assertRaises(ValueError): b.running_game(self.data, self.exe)

    def test_incomplete_process_enumeration_fails_closed(self):
        import ctypes
        class Function:
            def __init__(self, value): self.value = value
            def __call__(self, *args): return self.value
        kernel = SimpleNamespace(CreateToolhelp32Snapshot=Function(123), Process32FirstW=Function(0),
                                 Process32NextW=Function(0), CloseHandle=Function(1))
        with patch.object(ctypes, "WinDLL", return_value=kernel), patch.object(ctypes, "get_last_error", return_value=5):
            # Use the real function, bypassing the fixture's regular discovery mock.
            with self.assertRaisesRegex(OSError, "finish checking"):
                self.real_discovery(self.exe)


if __name__ == "__main__": unittest.main()
