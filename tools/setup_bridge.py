"""Private setup UI bridge. No game/keys contents in events or configuration."""
import argparse
import json
import os
from pathlib import Path
import platform
import re
import struct
import subprocess
import sys
import time
import venv

import first_run

PINS = {"cryptography": "47.0.0", "lz4": "4.4.5", "cffi": "2.0.0", "pycparser": "3.0"}
OWNER = "hwder-setup-components-v1"
NONCE = ""


def emit(kind, message="", **fields):
    print(("HWDER_UI:" + NONCE + ":" if NONCE else "") +
          json.dumps({"type": kind, "message": message, **fields}, ensure_ascii=False), flush=True)


def compatible():
    return (sys.implementation.name == "cpython" and (3, 11) <= sys.version_info[:2] <= (3, 14)
            and struct.calcsize("P") == 8 and platform.machine().lower() in ("amd64", "x86_64")
            and not bool(__import__("sysconfig").get_config_var("Py_GIL_DISABLED")))


def components(data, create=None, run=subprocess.run):
    """Explicit consent is owned by the native UI. Injectable boundaries never install in tests."""
    with first_run.setup_lock(Path(data).resolve()):
        return _components_locked(data, create, run)


def _components_locked(data, create, run):
    data = Path(data).resolve()
    directory = data / "setup-python"
    if directory.resolve() != directory:
        raise ValueError("Setup environment must not be a redirected directory.")
    marker = directory / "hwder-owner.json"
    if directory.exists():
        try:
            if json.loads(marker.read_text()) != {"owner": OWNER, "pins": PINS}:
                raise ValueError("This Python environment belongs to another setup.")
        except (OSError, ValueError):
            raise ValueError("Existing setup-python is not owned by this setup; choose another data folder.")
    else:
        directory.mkdir(parents=True)
        first_run.atomic_json(marker, {"owner": OWNER, "pins": PINS})
    emit("stage", "Creating your private setup environment", stage="components")
    (create or (lambda p: venv.EnvBuilder(with_pip=True).create(p)))(directory)
    python = directory / "Scripts/python.exe"
    emit("stage", "Installing the four pinned setup components from PyPI", stage="components")
    run([str(python), "-m", "pip", "--isolated", "install", "--require-virtualenv",
         "--index-url", "https://pypi.org/simple", "--only-binary=:all:", "--no-deps",
         "--disable-pip-version-check", "--no-input", "-r", str(Path(__file__).with_name("requirements-ui.txt"))], check=True)
    emit("components_ready", "Setup components installed. Choose Check requirements next.")


def stage_log(message):
    mapping = (("validating", "Checking your game"), ("hashing", "Checking your game"),
               ("exefs:", "Preparing game files"), ("generated:", "Preparing game code"),
               ("build:", "Building the game"), ("ready:", "Finishing setup"),
               ("prerequisites", "Checking requirements"))
    for prefix, title in mapping:
        if prefix in message:
            emit("stage", title, stage=title)
            break
    emit("log", message)


def checked_ready(data):
    data = Path(data).resolve()
    marker = first_run.read_json(data / "last-success.json")
    exe = data / "play/hwder.exe"
    if marker.get("schema") != first_run.SCHEMA or not exe.is_file() or first_run.file_hash(exe) != marker.get("exe_sha256"):
        raise ValueError("No verified prepared game. Choose Prepare game first.")
    config = first_run.read_json(data / "setup-inputs.json")
    nsp, keys = Path(config.get("nsp", "")), Path(config.get("keys", ""))
    if not nsp.is_file() or not keys.is_file():
        raise ValueError("Your original game or key file moved. Browse to it and prepare again.")
    return exe, nsp, keys


def commit_permission():
    emit("publish_request", "Finishing setup. Cancellation will wait while your playable version is committed.")
    if sys.stdin.readline().strip() != "commit":
        raise ValueError("Cancelled before updating the playable version.")


def prepare(data, nsp, keys, backend=first_run, before_publish=None):
    data = Path(data).resolve()
    nsp, keys = Path(nsp).resolve(), Path(keys).resolve()
    toolchain = backend.check_prerequisites(first_run.ROOT)
    with backend.setup_lock(data):
        exe = backend.prepare(first_run.ROOT, data, nsp, keys, max(1, min(8, os.cpu_count() or 1)),
                              toolchain, before_publish=before_publish)
        # The original backend owns all extraction/build/publication/certification and the host lock.
        if first_run.file_hash(exe) != backend.read_json(data / "last-success.json").get("exe_sha256"):
            raise ValueError("Prepared executable certificate did not match.")
        backend.atomic_json(data / "setup-inputs.json", {"nsp": str(nsp), "keys": str(keys)})
    emit("ready", "Ready to play. Your saves and settings stay in " + str(data / "play"), exe=str(exe))


def launch(data, popen=subprocess.Popen):
    # A separate, explicit UI action; never assigned to the cancellable preparation job.
    with first_run.setup_lock(Path(data)):
        exe, nsp, keys = checked_ready(data)
        env = dict(os.environ)
        for name in list(env):
            if name.startswith("HWDER_"):
                del env[name]
        env.update(HWDER_NSP=str(nsp), HWDER_KEYS=str(keys))
        popen([str(exe)], cwd=exe.parent, env=env, close_fds=True,
              stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    emit("launched", "Game started.")


def demo(action, fail=False):
    # No paths, input inspection, venv, package installation, compiler or game process.
    print('{"type":"publish_request","message":"UNTRUSTED ordinary log, not a control"}', flush=True)
    print('HWDER_UI:00000000000000000000000000000000:{"type":"ready"}', flush=True)
    for title in ("Checking requirements", "Checking your game", "Preparing game code", "Building the game"):
        emit("stage", title + " (preview)", stage=title)
        time.sleep(0.35)
    if fail and action == "prepare":
        raise ValueError("Preview failure: fix the selected input and choose Retry. Existing game retained.")
    if action == "prepare":
        commit_permission()
        time.sleep(1.2)  # deterministic synthetic safe-commit interval for native UI tests.
    if action == "check":
        emit("checked", "Preview requirements ready. No packages or toolchain were installed.")
    elif action == "prepare":
        emit("ready", "Preview ready. No game was prepared; Play and shortcuts stay disabled.")
    else:
        emit("demo_ready", "Preview complete. No game, packages or toolchain were installed.")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--action", choices=("check", "components", "prepare", "launch"), required=True)
    parser.add_argument("--data", type=Path, required=True)
    parser.add_argument("--nsp")
    parser.add_argument("--keys")
    parser.add_argument("--demo", action="store_true")
    parser.add_argument("--demo-fail", action="store_true")
    parser.add_argument("--ui-protocol", action="store_true")
    parser.add_argument("--nonce", default="")
    args = parser.parse_args()
    global NONCE
    if args.nonce and not re.fullmatch("[0-9a-f]{32}", args.nonce):
        parser.error("Invalid UI session identifier")
    NONCE = args.nonce
    first_run.log = stage_log
    try:
        if args.demo:
            demo(args.action, args.demo_fail)
        elif not compatible():
            raise ValueError("Install standard Windows x64 CPython 3.11–3.14 (not free-threaded), then recheck.")
        elif args.action == "components":
            components(args.data)
        elif args.action == "check":
            first_run.check_prerequisites(first_run.ROOT)
            emit("checked", "Requirements ready. Choose your game and key, then Prepare game.")
            try:
                exe, _, _ = checked_ready(args.data)
                emit("ready_available", "Your verified game is ready to play.", exe=str(exe))
            except ValueError:
                pass
        elif args.action == "prepare":
            if not args.nsp or not args.keys:
                raise ValueError("Choose your NSP and prod.keys files.")
            prepare(args.data, args.nsp, args.keys, before_publish=commit_permission if args.ui_protocol else None)
        elif args.action == "launch":
            launch(args.data)
        return 0
    except (ValueError, OSError, RuntimeError, subprocess.SubprocessError, struct.error) as error:
        detail = str(error)
        if "Install Python dependencies" in detail:
            recovery = "Choose Install setup components, then Check requirements again."
        elif "pinned third-party" in detail:
            recovery = "Extract the complete release kit, including its source folder, then retry."
        elif any(word in detail for word in ("Visual Studio", "Windows SDK", "CMake", "Ninja", "MSVC", "clang-cl")):
            recovery = "Choose C++ Build Tools for the required components, then Check requirements again."
        else:
            recovery = "Setup could not finish. Check the details, correct the problem and retry."
        emit("log", detail)
        emit("failed", recovery + " Saves/settings are retained; retry to verify the playable version.")
        return 1


if __name__ == "__main__":
    sys.exit(main())
