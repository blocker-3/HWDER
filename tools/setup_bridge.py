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
import uuid

sys.dont_write_bytecode = True
sys.path.insert(0,str(Path(__file__).resolve().parent))
import first_run

PINS = {"cryptography": "47.0.0", "lz4": "4.4.5", "cffi": "2.0.0", "pycparser": "3.0", "zstandard": "0.25.0"}
OWNER = "hwder-setup-components-v1"
NONCE = ""


def emit(kind, message="", **fields):
    print(("HWDER_UI:" + NONCE + ":" if NONCE else "") +
          json.dumps({"type": kind, "message": message, **fields}, ensure_ascii=False), flush=True)


def compatible():
    return (sys.implementation.name == "cpython" and (3, 11) <= sys.version_info[:2] <= (3, 14)
            and struct.calcsize("P") == 8 and platform.machine().lower() in ("amd64", "x86_64")
            and not bool(__import__("sysconfig").get_config_var("Py_GIL_DISABLED")))


def require_avx2(present=None, build=None):
    """The game is compiled for AVX2; without it hwder.exe would crash at start with no message."""
    if present is None:
        import ctypes
        present = ctypes.windll.kernel32.IsProcessorFeaturePresent
    build = sys.getwindowsversion().build if build is None else build
    # PF_AVX2_INSTRUCTIONS_AVAILABLE (40) is only reported from Windows 10 2004 (build 19041); trust a "no" from there on.
    if build >= 19041 and not present(40):
        raise ValueError("CPU lacks AVX2")


def components(data, create=None, run=subprocess.run):
    """Explicit consent is owned by the native UI. Injectable boundaries never install in tests."""
    with first_run.setup_lock(Path(data).resolve()):
        return _components_locked(data, create, run)


def _components_locked(data, create, run):
    # Legacy installed-Python action only. The managed embeddable interpreter
    # intentionally has no venv/pip and never takes this path.
    if create is None:
        import venv
        create=lambda p:venv.EnvBuilder(with_pip=True).create(p)
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
    create(directory)
    python = directory / "Scripts/python.exe"
    emit("stage", "Installing the four pinned setup components from PyPI", stage="components")
    run([str(python), "-m", "pip", "--isolated", "install", "--require-virtualenv",
         "--index-url", "https://pypi.org/simple", "--only-binary=:all:", "--no-deps",
         "--disable-pip-version-check", "--no-input", "-r", str(Path(__file__).with_name("requirements-ui.txt"))], check=True)
    emit("components_ready", "Setup components installed. Choose Check requirements next.")


STEPS = (("prerequisites", "Checking requirements"), ("validating", "Checking your game"),
         ("exefs:", "Preparing game files"), ("generated:", "Preparing game code"),
         ("build:", "Building the game"), ("ready:", "Finishing setup"))


def stage_for(message):
    """Numbered setup step for a backend log line, or None for plain output."""
    if "hashing" in message:
        message = "validating"
    for number, (prefix, title) in enumerate(STEPS, 1):
        if prefix in message:
            return number, title
    return None


def stage_log(message):
    if message.startswith("private-tools: "):
        emit("stage",message.removeprefix("private-tools: "),stage="tools")
        return
    found = stage_for(message)
    if found:
        number, title = found
        emit("stage", f"Step {number} of {len(STEPS)}: {title}", stage=title, step=number, steps=len(STEPS))
    emit("log", message)


RECOVERY = (
    # (substrings of the backend detail, user sentence, next step for the window)
    (("CPU lacks AVX2",),
     "This PC's processor lacks AVX2, which HWDER needs (most Intel and AMD processors from 2015 on have it).", "close"),
    (("updates/bundles unsupported", "Expected exactly one base-game program"),
     "This NSP has an update or is not the base game. Use the base game only, then Prepare again.", "inputs"),
    (("Unsupported ExeFS module set", "not a supported", "unsupported version", "supported version"),
     "This dump is not the supported game version. Use the unmodified base game.", "inputs"),
    (("Cannot decrypt", "decryption key is missing", "Keys file is malformed", "check dump and keys", "check your own dump and keys"),
     "Your prod.keys cannot open this dump. Use the keys from the same console.", "inputs"),
    (("game or key file moved", "Supply an existing .nsp", "Choose your NSP and prod.keys"),
     "Your game or key file moved. Pick it again, then Prepare.", "inputs"),
    (("Private tools are missing", "Private tool certificate changed", "Private tools are not ready",
      "Private Python package identity", "Private tools need verification", "Private toolchain"),
     "Private tools are not installed for this kit yet. Choose Install (about 260 MB, no administrator access).", "install"),
    (("Your game is running", "Close it before"),
     "Your game is running. Close it, then try again.", "close"),
    (("No verified prepared game",),
     "No prepared game yet. Choose Prepare.", "prepare"),
    (("build.py", "Build succeeded without", "compil", "ninja", "cmake", "CMake"),
     "The build did not finish. Open Advanced, Details for the error, then Retry.", "prepare"),
    (("cancelled", "Cancelled"),
     "Cancelled. Nothing was changed; choose Prepare to continue.", "prepare"),
)


def recovery(detail):
    """One plain sentence and the window step it belongs to."""
    for needles, message, step in RECOVERY:
        if any(needle in detail for needle in needles):
            return message, step
    line = detail.strip().splitlines()[0][:160] if detail.strip() else "unknown error"
    return "Setup did not finish: " + line.rstrip(".") + ". Fix that, then Retry.", "prepare"


def checked_ready(data):
    data = Path(data).resolve()
    marker = first_run.read_json(data / "last-success.json")
    exe = data / "play/hwder.exe"
    if marker.get("schema") != first_run.SCHEMA or not exe.is_file() or first_run.file_hash(exe) != marker.get("exe_sha256"):
        raise ValueError("No verified prepared game. Choose Prepare first.")
    config = first_run.read_json(data / "setup-inputs.json")
    nsp, keys = Path(config.get("nsp", "")), Path(config.get("keys", ""))
    if not nsp.is_file() or not keys.is_file():
        raise ValueError("Your game or key file moved. Pick it again, then Prepare.")
    update = marker.get("update_dir")
    if update and not (Path(update) / "update.json").is_file():
        raise ValueError("Your prepared game update is missing. Choose Prepare again.")
    return exe, nsp, keys


def process_identity(pid):
    """Read-only process identity. None means exited; inaccessible is not 'exited'."""
    import ctypes
    from ctypes import wintypes as w
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.argtypes = (w.DWORD, w.BOOL, w.DWORD)
    kernel.OpenProcess.restype = w.HANDLE
    kernel.CloseHandle.argtypes = (w.HANDLE,)
    kernel.WaitForSingleObject.argtypes = (w.HANDLE, w.DWORD)
    kernel.QueryFullProcessImageNameW.argtypes = (w.HANDLE, w.DWORD, w.LPWSTR, ctypes.POINTER(w.DWORD))
    kernel.GetProcessTimes.argtypes = (w.HANDLE,) + (ctypes.POINTER(w.FILETIME),) * 4
    handle = kernel.OpenProcess(0x100000 | 0x1000, False, int(pid))
    if not handle:
        if ctypes.get_last_error() == 87:  # ERROR_INVALID_PARAMETER: PID no longer exists.
            return None
        raise OSError("Cannot verify the previously launched game. Close it before retrying.")
    try:
        if kernel.WaitForSingleObject(handle, 0) == 0:
            return None
        name = ctypes.create_unicode_buffer(32768)
        length = w.DWORD(len(name))
        times = [w.FILETIME() for _ in range(4)]
        if not kernel.QueryFullProcessImageNameW(handle, 0, name, ctypes.byref(length)) or not kernel.GetProcessTimes(handle, *(ctypes.byref(t) for t in times)):
            raise OSError("Cannot verify the running game identity. Close it before retrying.")
        created = (times[0].dwHighDateTime << 32) | times[0].dwLowDateTime
        return {"pid": str(pid), "created": str(created), "exe": str(Path(name.value).resolve())}
    finally:
        kernel.CloseHandle(handle)


def running_game(data, exe, identity=process_identity):
    path = Path(data) / "game-running.json"
    try:
        marker = json.loads(path.read_text(encoding="utf-8")) if path.exists() else {}
    except (OSError, ValueError):
        raise ValueError("The game launch record could not be read. Close the game before repairing it.")
    if not marker:
        return None
    if not isinstance(marker, dict) or marker.get("schema") != 1 or marker.get("exe") != str(exe.resolve()) or not str(marker.get("pid", "")).isdigit() or not str(marker.get("created", "")).isdigit() or not 0 < int(marker["pid"]) < 2**32 or not 0 < int(marker["created"]) < 2**64:
        raise ValueError("The game launch record is invalid. Close the game, then retry verification.")
    actual = identity(int(marker["pid"]))
    if actual is not None and actual == {k: marker[k] for k in ("pid", "created", "exe")}:
        return actual
    return None  # Exited, or a reused PID belonging to a different process.


def discovered_game(exe):
    """Also recognize direct installed-EXE/shortcut launches. Read-only, exact image match."""
    if os.name != "nt":
        return None
    import ctypes
    from ctypes import wintypes as w
    class Entry(ctypes.Structure):
        _fields_ = [("size", w.DWORD), ("usage", w.DWORD), ("pid", w.DWORD), ("heap", ctypes.c_size_t),
                    ("module", w.DWORD), ("threads", w.DWORD), ("parent", w.DWORD), ("priority", w.LONG),
                    ("flags", w.DWORD), ("name", w.WCHAR * 260)]
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateToolhelp32Snapshot.argtypes = (w.DWORD, w.DWORD)
    kernel.CreateToolhelp32Snapshot.restype = w.HANDLE
    kernel.Process32FirstW.argtypes = kernel.Process32NextW.argtypes = (w.HANDLE, ctypes.POINTER(Entry))
    kernel.CloseHandle.argtypes = (w.HANDLE,)
    snapshot = kernel.CreateToolhelp32Snapshot(2, 0)
    if snapshot == ctypes.c_void_p(-1).value:
        raise OSError("Cannot check whether your installed game is already running.")
    try:
        entry = Entry(); entry.size = ctypes.sizeof(entry)
        ctypes.set_last_error(0)
        more = kernel.Process32FirstW(snapshot, ctypes.byref(entry))
        while more:
            if entry.name.casefold() == exe.name.casefold():
                actual = process_identity(entry.pid)  # Access denied is deliberately not 'not running'.
                if actual and os.path.normcase(actual["exe"]) == os.path.normcase(str(exe.resolve())):
                    return actual
            ctypes.set_last_error(0)
            more = kernel.Process32NextW(snapshot, ctypes.byref(entry))
        if ctypes.get_last_error() != 18:  # ERROR_NO_MORE_FILES is the only successful end.
            raise OSError("Could not finish checking whether the installed game is running.")
    finally:
        kernel.CloseHandle(snapshot)
    return None


def ready_status(data, managed=None, identity=process_identity):
    with first_run.setup_lock(Path(data)):
        exe, _, _ = checked_ready(data)
        if managed is not None:
            import managed_tools
            managed_tools.game_image(exe)
        running = running_game(data, exe, identity) or discovered_game(exe)
        if not running:
            installed_paths(data, exe)
    emit("ready_available", "Ready to play. Use Play now, or create a desktop shortcut or add it to Steam for next time.", exe=str(exe), paths_ready="no" if running else "yes")
    if running:
        emit("launched", "Game is running. Play will be available when it closes.", **running)


def installed_paths(data, exe):
    """Persist paths, never inputs, so the installed game works without the setup kit."""
    config = first_run.read_json(Path(data) / "setup-inputs.json")
    update = first_run.read_json(Path(data) / "last-success.json").get("update_dir") or ""
    # An empty update_path switches a previously prepared update off.
    values = {"nsp_path": str(Path(config["nsp"]).resolve()), "keys_path": str(Path(config["keys"]).resolve()),
              "update_path": str(Path(update).resolve()) if update else ""}
    for key, value in values.items():
        if any(c in value for c in "\r\n\0") or len((key + "=" + value).encode("utf-8")) >= 2046:
            raise ValueError("The selected path cannot be stored in the game's settings. Choose a shorter path.")
    settings = exe.parent / "hwder_settings.ini"
    # A new file carries the game's first-run defaults: without a face_labels line the game treats the file
    # as an older install and keeps face buttons by position.
    original = settings.read_bytes() if settings.exists() else b"version=2\nface_labels=1\n"
    # Runtime reads flat key=value records. Preserve all unrelated bytes, including line endings.
    result = []
    seen = set()
    for line in original.splitlines(keepends=True):
        key = line.split(b"=", 1)[0].decode("utf-8", errors="replace")
        if key in values:
            if key not in seen:
                result.append((key + "=" + values[key]).encode("utf-8") + (b"\r\n" if line.endswith(b"\r\n") else b"\n"))
                seen.add(key)
        else:
            result.append(line)
    if result and not result[-1].endswith((b"\r", b"\n")):
        result.append(b"\n")
    result.extend((key + "=" + value + "\n").encode("utf-8") for key, value in values.items() if key not in seen)
    content = b"".join(result)
    if content != original or not settings.exists():
        temporary = settings.with_name(".setup-settings-" + uuid.uuid4().hex + ".tmp")
        try:
            temporary.write_bytes(content)
            os.replace(temporary, settings)
        finally:
            temporary.unlink(missing_ok=True)
    first_run.atomic_json(Path(data) / "installed-game.json", {"schema": 1, "exe": str(exe.resolve()),
                         "game_folder": str(exe.parent.resolve()), **config})


def commit_permission():
    emit("publish_request", "Finishing setup. Cancellation will wait while your playable version is committed.")
    if sys.stdin.readline().strip() != "commit":
        raise ValueError("Cancelled before updating the playable version.")


def prepare(data, nsp, keys, backend=first_run, before_publish=None, managed=None, update=None):
    data = Path(data).resolve()
    nsp, keys = Path(nsp).resolve(), Path(keys).resolve()
    update = Path(update).resolve() if update else None
    toolchain = backend.check_prerequisites(first_run.ROOT, managed) if managed else backend.check_prerequisites(first_run.ROOT)
    with backend.setup_lock(data):
        existing = data / "play/hwder.exe"
        if existing.is_file() and (running_game(data, existing) or discovered_game(existing)):
            raise ValueError("Your game is running. Close it, then try again.")
        exe = backend.prepare(first_run.ROOT, data, nsp, keys, max(1, min(8, os.cpu_count() or 1)),
                              toolchain, before_publish=before_publish, **({"update": update} if update else {}))
        # The original backend owns all extraction/build/publication/certification and the host lock.
        if first_run.file_hash(exe) != backend.read_json(data / "last-success.json").get("exe_sha256"):
            raise ValueError("Prepared executable certificate did not match.")
        backend.atomic_json(data / "setup-inputs.json", {"nsp": str(nsp), "keys": str(keys),
                            **({"update": str(update)} if update else {})})
        installed_paths(data, exe)
    emit("ready", "Ready to play. Your saves and settings stay in " + str(data / "play"), exe=str(exe), paths_ready="yes")


def launch(data, popen=subprocess.Popen, managed=None, identity=process_identity):
    # A separate, explicit UI action; never assigned to the cancellable preparation job.
    with first_run.setup_lock(Path(data)):
        exe, nsp, keys = checked_ready(data)
        if managed is not None:
            import managed_tools
            managed_tools.game_image(exe)
        running = running_game(data, exe, identity) or discovered_game(exe)
        if running:
            emit("launched", "Game is already running. Play will be available when it closes.", paths_ready="no", **running)
            return
        installed_paths(data, exe)
        env = dict(os.environ)
        for name in list(env):
            if name.startswith("HWDER_"):
                del env[name]
        # The paths are in hwder_settings.ini (installed_paths). HWDER_NSP/HWDER_KEYS would make the startup
        # screen warn that they override the picked files.
        if managed is not None:
            windows=Path(os.environ.get("SystemRoot",r"C:\Windows"))
            env["PATH"]=os.pathsep.join(map(str,(windows/"System32",windows)))
        # A persisted intent fails closed if the launcher dies after Popen but before identity publication.
        marker = Path(data) / "game-running.json"
        first_run.atomic_json(marker, {"schema": 1, "exe": str(exe.resolve()), "state": "starting"})
        try:
            child = popen([str(exe)], cwd=exe.parent, env=env, close_fds=True,
                  stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except OSError:
            first_run.atomic_json(marker, {})  # Definitive Popen failure; no child was created.
            raise
        running = identity(child.pid) if child is not None else None  # injectable no-process test boundary
        if running:
            first_run.atomic_json(marker, {"schema": 1, **running})
        else:
            first_run.atomic_json(marker, {})
    emit("launched", "Game started. You can close Setup; the game stays open.", exe=str(exe), paths_ready="yes",
         **({k: running[k] for k in ("pid", "created")} if running else {}))


def demo(action, fail=False, lifecycle=False, data=None, race=False):
    if lifecycle:
        # Explicit native lifecycle fixture: standard Python sleep only, never the game or user inputs.
        exe = Path(sys.executable).resolve()
        if race and action == "status" and not (Path(data) / "preview-race-once").exists():
            (Path(data) / "preview-race-once").write_text("fixed immediate-exit fixture")
            child = subprocess.Popen([str(exe), "-I", "-S", "-c", "import time;time.sleep(0.15)"], close_fds=True,
                                     stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            identity = process_identity(child.pid)
            emit("ready_available", "Synthetic running old install; path handoff deferred.", exe=str(exe), paths_ready="no")
            if identity: emit("launched", "Synthetic child exits while helper is still busy.", **identity)
            time.sleep(0.8)
            return
        if action == "launch":
            child = subprocess.Popen([str(exe), "-I", "-S", "-c", "import time;time.sleep(4)"],
                close_fds=True, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            identity = process_identity(child.pid)
            if identity:
                first_run.atomic_json(Path(data) / "preview-child.json", identity)
                emit("launched", "Preview child running (no game).", **identity)
                return
        emit("ready_available", "Ready to play (synthetic lifecycle fixture; no game).", exe=str(exe), paths_ready="yes")
        return
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
    parser.add_argument("--action", choices=("check", "components", "prepare", "launch", "status"), required=True)
    parser.add_argument("--data", type=Path, required=True)
    parser.add_argument("--nsp")
    parser.add_argument("--keys")
    parser.add_argument("--update")
    parser.add_argument("--demo", action="store_true")
    parser.add_argument("--demo-fail", action="store_true")
    parser.add_argument("--demo-lifecycle", action="store_true")
    parser.add_argument("--demo-lifecycle-race", action="store_true")
    parser.add_argument("--ui-protocol", action="store_true")
    parser.add_argument("--nonce", default="")
    parser.add_argument("--managed-toolchain",type=Path)
    parser.add_argument("--managed-launch",action="store_true")
    args = parser.parse_args()
    if args.managed_launch and args.action not in ("launch", "status"):
        parser.error("--managed-launch is valid only for launch/status")
    global NONCE
    if args.nonce and not re.fullmatch("[0-9a-f]{32}", args.nonce):
        parser.error("Invalid UI session identifier")
    NONCE = args.nonce
    first_run.log = stage_log
    try:
        if args.demo:
            demo(args.action, args.demo_fail, args.demo_lifecycle, args.data, args.demo_lifecycle_race)
        elif not compatible():
            raise ValueError("Install standard Windows x64 CPython 3.11–3.14 (not free-threaded), then recheck.")
        elif args.action == "components":
            components(args.data)
        elif args.action == "check":
            require_avx2()
            first_run.check_prerequisites(first_run.ROOT,args.managed_toolchain)
            emit("checked", "Requirements ready. Private tools and this kit check out.")
            try:
                exe, _, _ = checked_ready(args.data)
                emit("ready_available", "Your verified game is ready to play.", exe=str(exe))
            except ValueError:
                pass
        elif args.action == "prepare":
            require_avx2()
            if not args.nsp or not args.keys:
                raise ValueError("Choose your NSP and prod.keys files.")
            prepare(args.data, args.nsp, args.keys, before_publish=commit_permission if args.ui_protocol else None,
                    managed=args.managed_toolchain, update=args.update or None)
        elif args.action == "launch":
            launch(args.data,managed=True if args.managed_launch else args.managed_toolchain)
        elif args.action == "status":
            ready_status(args.data,managed=True if args.managed_launch else args.managed_toolchain)
        return 0
    except (ValueError, OSError, RuntimeError, subprocess.SubprocessError, struct.error) as error:
        detail = str(error)
        if "Install Python dependencies" in detail:
            message, step = "Choose Install setup components, then Check requirements again.", "install"
        elif "pinned third-party" in detail:
            message, step = "Extract the complete release kit, including its source folder, then Retry.", "prepare"
        elif not args.managed_toolchain and any(word in detail for word in ("Visual Studio", "Windows SDK", "CMake", "Ninja", "MSVC", "clang-cl")):
            message, step = "Choose C++ Build Tools for the required components, then Check requirements again.", "install"
        else:
            message, step = recovery(detail)
        emit("log", detail)
        emit("failed", message, next=step)
        return 1


if __name__ == "__main__":
    sys.exit(main())
