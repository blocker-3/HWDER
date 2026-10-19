"""Public process/host-path regressions; never acquire the real build lock."""
from contextlib import contextmanager
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import build
import build_lock


class BuildLock(unittest.TestCase):
    @unittest.skipUnless(os.name == "nt", "host known-folder contract is Windows-specific")
    def test_redirected_localappdata_cannot_split_machine_lock(self):
        expected = build_lock.lock_path()
        with tempfile.TemporaryDirectory() as directory:
            with patch.dict(os.environ, {"LOCALAPPDATA": directory}):
                self.assertEqual(build_lock.lock_path(), expected)
        self.assertEqual(expected.name, "build.lock")
        self.assertEqual(expected.parent.name, "hwder")

    @unittest.skipUnless(os.name == "nt", "host profile contract is Windows-specific")
    def test_known_folder_is_actual_host_profile_not_package_redirection(self):
        # Registry is an independent host-profile oracle; comparing two calls to
        # the same known-folder API would miss package redirection of both calls.
        import winreg
        registry_path = r"Software\Microsoft\Windows\CurrentVersion\Explorer\User Shell Folders"
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, registry_path) as key:
            local_appdata = os.path.expandvars(winreg.QueryValueEx(key, "Local AppData")[0])
        self.assertEqual(build_lock.lock_path(), Path(local_appdata) / "hwder/build.lock")

    def test_single_file_compile_is_inside_shared_lock(self):
        events = []
        @contextmanager
        def synthetic_lock():
            events.append("lock")
            try:
                yield
            finally:
                events.append("unlock")
        with tempfile.TemporaryDirectory() as directory:
            argv = ["build.py", "--only-file", "synthetic.cpp", "--build-dir", directory]
            with patch.object(build, "build_lock", synthetic_lock), \
                 patch.object(build, "toolchain", return_value=({}, "cmake", "ninja", "clang", "clang++")), \
                 patch.object(build.subprocess, "run", side_effect=lambda *a, **k: (events.append("compile"),
                              subprocess.CompletedProcess(a[0], 0))[1]), \
                 patch.object(sys, "argv", argv):
                self.assertEqual(build.main(), 0)
        self.assertEqual(events, ["lock", "compile", "unlock"])

    def test_empty_legacy_lock_serializes_and_releases_after_error(self):
        with tempfile.TemporaryDirectory() as directory:
            path, marker = Path(directory) / "build.lock", Path(directory) / "entered.txt"
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
            with patch.object(build_lock, "lock_path", return_value=path):
                with self.assertRaisesRegex(RuntimeError, "synthetic failure"):
                    with build_lock.build_lock():
                        raise RuntimeError("synthetic failure")
                # A different process must reacquire after the exception.
                subprocess.run([sys.executable, "-c", script], timeout=10, check=True,
                               stdout=subprocess.DEVNULL)


class BuildModes(unittest.TestCase):
    def test_default_synthetic_build_includes_excluded_codec_suites(self):
        # The codec executables are EXCLUDE_FROM_ALL: Ninja's `all` target is
        # insufficient even though configure and an A64-only build can succeed.
        with tempfile.TemporaryDirectory() as directory:
            argv = ["build.py", "--synthetic-tests", "--build-dir", directory]
            with patch.object(build, "build_lock"), \
                 patch.object(build, "toolchain", return_value=({}, "cmake", "ninja", "clang", "clang++")), \
                 patch.object(build.subprocess, "run", return_value=subprocess.CompletedProcess([], 0)) as run, \
                 patch.object(sys, "argv", argv):
                self.assertEqual(build.main(), 0)
            self.assertEqual(run.call_args_list[-1].args[0][-1], "synthetic_tests")

    def test_absent_host_candidate_fails_before_lock_or_toolchain(self):
        with tempfile.TemporaryDirectory() as directory:
            with patch.object(build, "ROOT", directory), \
                 patch.object(build, "build_lock") as lock, \
                 patch.object(build, "toolchain") as toolchain, \
                 patch.object(sys, "argv", ["build.py", "--host-tests"]), \
                 patch.object(sys, "stderr", new_callable=io.StringIO) as stderr:
                with self.assertRaises(SystemExit) as error:
                    build.main()
                self.assertEqual(error.exception.code, 2)
                self.assertIn("separate, unmerged Linux host-test candidate", stderr.getvalue())
                lock.assert_not_called()
                toolchain.assert_not_called()


if __name__ == "__main__":
    unittest.main()
