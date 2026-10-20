"""No network, installation, real keys/game or compiler. Injectable owned boundaries."""
import contextlib
import hashlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import setup_bridge as bridge

ROOT = Path(__file__).resolve().parents[2] / "build/ui-tests"
ROOT.mkdir(parents=True, exist_ok=True)


class Bridge(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=ROOT)
        self.addCleanup(self.temp.cleanup)
        self.data = Path(self.temp.name)

    def fixture(self):
        exe = self.data / "play/hwder.exe"
        exe.parent.mkdir()
        exe.write_bytes(b"MZsynthetic executable; never run")
        nsp, keys = self.data / "synthetic.nsp", self.data / "synthetic.keys"
        nsp.write_text("synthetic game")
        keys.write_text("synthetic secret")
        bridge.first_run.atomic_json(self.data / "last-success.json",
            {"schema": 1, "exe_sha256": hashlib.sha256(exe.read_bytes()).hexdigest()})
        bridge.first_run.atomic_json(self.data / "setup-inputs.json", {"nsp": str(nsp), "keys": str(keys)})
        return exe

    def test_owned_component_install_exact_pins_and_retry_without_global_install(self):
        calls = []
        def create(directory):
            python = directory / "Scripts/python.exe"
            python.parent.mkdir(exist_ok=True)
            python.write_bytes(b"fake installer")
        def run(args, **kwargs):
            calls.append((args, kwargs))
        with contextlib.redirect_stdout(io.StringIO()):
            bridge.components(self.data, create, run)
            bridge.components(self.data, create, run)
        args = calls[0][0]
        self.assertEqual(Path(args[0]), self.data / "setup-python/Scripts/python.exe")
        for flag in ("--isolated", "--require-virtualenv", "--no-deps", "--no-input", "--only-binary=:all:"):
            self.assertIn(flag, args)
        self.assertIn("https://pypi.org/simple", args)
        self.assertEqual(len(calls), 2)
        pins = Path(bridge.__file__).with_name("requirements-ui.txt").read_text()
        for name, version in bridge.PINS.items():
            self.assertIn(name + "==" + version, pins)

    def test_unknown_existing_environment_refused_before_installer(self):
        (self.data / "setup-python").mkdir()
        with self.assertRaisesRegex(ValueError, "not owned"):
            bridge.components(self.data, lambda p:self.fail("must not create"), lambda *a,**k:self.fail("must not install"))

    def test_component_ownership_and_installer_are_inside_existing_setup_lock(self):
        held = []
        @contextlib.contextmanager
        def locked(path):
            held.append(Path(path))
            try:
                yield
            finally:
                held.clear()
        def check(*args, **kwargs):
            self.assertEqual(held, [self.data.resolve()])
        with patch.object(bridge.first_run, "setup_lock", locked), contextlib.redirect_stdout(io.StringIO()):
            bridge.components(self.data, check, check)
        self.assertEqual(held, [])

    def test_failed_install_retains_owned_retry_marker(self):
        def fail(*args, **kwargs):
            raise subprocess.CalledProcessError(1, "fake-pip")
        with contextlib.redirect_stdout(io.StringIO()), self.assertRaises(subprocess.CalledProcessError):
            bridge.components(self.data, lambda p:None, fail)
        self.assertEqual(json.loads((self.data / "setup-python/hwder-owner.json").read_text())["owner"], bridge.OWNER)

    def test_ready_certificate_and_paths_required_no_stale_launch(self):
        exe = self.fixture()
        self.assertEqual(bridge.checked_ready(self.data)[0], exe)
        calls = []
        bridge.launch(self.data, lambda *a,**k:calls.append((a,k)))
        self.assertEqual(len(calls), 1)
        self.assertFalse(any("synthetic secret" in str(value) for value in calls))
        exe.write_bytes(b"tampered")
        with self.assertRaises(ValueError):
            bridge.launch(self.data, lambda *a,**k:self.fail("uncertified launch"))

    def test_missing_original_input_prevents_launch(self):
        self.fixture()
        (self.data / "synthetic.keys").unlink()
        with self.assertRaisesRegex(ValueError, "moved"):
            bridge.launch(self.data, lambda *a,**k:self.fail("missing-input launch"))

    def test_commit_permission_requires_exact_ack_eof_and_cancel_refuse(self):
        for answer in ("", "cancel\n", "not-commit\n"):
            with patch.object(bridge.sys, "stdin", io.StringIO(answer)), contextlib.redirect_stdout(io.StringIO()), self.assertRaises(ValueError):
                bridge.commit_permission()
        with patch.object(bridge.sys, "stdin", io.StringIO("commit\n")), contextlib.redirect_stdout(io.StringIO()):
            bridge.commit_permission()

    def test_control_events_use_session_nonce(self):
        output = io.StringIO()
        with patch.object(bridge, "NONCE", "a"*32), contextlib.redirect_stdout(output):
            bridge.emit("publish_request", "safe")
        self.assertTrue(output.getvalue().startswith("HWDER_UI:"+"a"*32+":"))


if __name__ == "__main__":
    unittest.main()
