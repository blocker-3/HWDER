"""Run only public synthetic suites; never invoke the game, generator, or packager."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import build


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--python-only", action="store_true")
    args = parser.parse_args()
    suite = unittest.defaultTestLoader.discover(str(ROOT / "tools/tests"), pattern="test_build_lock.py")
    if not suite.countTestCases():
        raise RuntimeError("Build-lock suite discovered zero tests")
    # The integrated first-run suite is optional for older source checkouts. Never
    # silently pass an empty discovery or fetch another branch's implementation.
    feature = [ROOT / "tools" / p for p in
               ("first_run.py", "local_game.py", "tests/test_first_run.py")]
    if any(p.exists() for p in feature):
        if not all(p.is_file() for p in feature):
            raise RuntimeError("Partial first-run integration: coordinator, local_game and tests are required together")
        first_run_suite = unittest.defaultTestLoader.discover(str(ROOT / "tools/tests"), pattern="test_first_run.py")
        if not first_run_suite.countTestCases():
            raise RuntimeError("First-run suite discovered zero tests")
        suite.addTests(first_run_suite)
    else:
        print("SKIP first-run/source-package exclusion suite: separate candidate is not integrated in this checkout", flush=True)
        if os.environ.get("GITHUB_ACTIONS"):
            print("::warning::Source-package exclusion coverage awaits explicit first-run candidate integration", flush=True)
    # README debug-switch table must match runtime/src/debug_flags.cpp (tools/gen_debug_flags_md.py).
    flags_suite = unittest.defaultTestLoader.discover(str(ROOT / "tools/tests"), pattern="test_debug_flags_md.py")
    if not flags_suite.countTestCases():
        raise RuntimeError("Debug-flags README suite discovered zero tests")
    suite.addTests(flags_suite)
    if not unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful():
        return 1
    if args.python_only:
        return 0
    directory = ROOT / "build/synthetic"
    subprocess.run([sys.executable, str(ROOT / "tools/build.py"), "--synthetic-tests",
                    "--target", "synthetic_tests", "--jobs", str(args.jobs),
                    "--build-dir", str(directory)], cwd=ROOT, check=True)
    env, cmake, _, _, _ = build.toolchain()
    ctest = Path(cmake).with_name("ctest.exe" if os.name == "nt" else "ctest")
    subprocess.run([str(ctest), "--test-dir", str(directory), "--output-on-failure",
                    "--no-tests=error", "--verbose"], env=env, cwd=ROOT, check=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
