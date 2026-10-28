#!/usr/bin/env python3
"""HWDER Setup for Linux: the interactive terminal setup, and the backend the HWDER Setup window drives.

    hwder-setup --cli                       interactive setup in a terminal (what the window does, as questions)
    python3 tools/linux_setup.py status     what is installed (add --json for the window's JSON lines)
    python3 tools/linux_setup.py prepare --nsp GAME.nsp --keys prod.keys [--update UPDATE.nsp]
    python3 tools/linux_setup.py steam | desktop [--desktop-icon] | play | uninstall

Everything lives in $XDG_DATA_HOME/HWDER (else ~/.local/share/HWDER):
    app/        this release, copied there on first use (the menu entry "HWDER Setup" starts it from there)
    toolchain/  the pinned build tools (Zig/Clang, CMake, Ninja, Python + wheels), downloaded once and checked by
                SHA-256 (tools/linux/toolchain.json)
    build/      the private build cache; build/play is the game: hwder, its settings and shader caches
    save/       your saves (the game's default save folder)
    mods/       game mods (the game's default mods folder; Setup creates it)
    setup.json  the files you chose
Your game, keys and update files are read where they are and never copied. The game is built on this machine from
your own dump; no game code is downloaded or shipped. Runs on the system Python 3.8+ (standard library only); the
build itself runs on the pinned Python.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.error
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "tools"
GAME_NAME = "Hyrule Warriors: Definitive Edition"
SCHEMA = 1
DISK_NEEDED = 8 * 1024 ** 3  # toolchain ~0.7 GB, generated C + objects ~4 GB at most, with headroom


# ---- Output: plain text for the terminal, JSON lines for the setup window -------------------------------------

class Report:
    def __init__(self, as_json=False):
        self.json = as_json

    def emit(self, kind, **fields):
        if self.json:
            print(json.dumps({"type": kind, **fields}), flush=True)

    def stage(self, ident, text):
        if self.json:
            self.emit("stage", id=ident, text=text)
        else:
            print(f"\n== {text}", flush=True)

    def progress(self, fraction, text=""):
        if self.json:
            self.emit("progress", fraction=round(max(0.0, min(1.0, fraction)), 4), text=text)
        elif text:
            print(f"\r   {text:<70}", end="", flush=True)

    def end_progress(self):
        if not self.json:
            print(flush=True)

    def log(self, text):
        if self.json:
            self.emit("log", text=text)
        else:
            print(f"   {text}", flush=True)

    def result(self, ok, message, **data):
        if self.json:
            self.emit("result", ok=ok, message=message, data=data)
        else:
            print(("\n" if not ok else "") + message, flush=True)
        return 0 if ok else 1


class SetupError(Exception):
    pass


# ---- Places ----------------------------------------------------------------------------------------------------

def data_dir():
    configured = os.environ.get("XDG_DATA_HOME", "")
    base = Path(configured) if configured and Path(configured).is_absolute() else Path.home() / ".local" / "share"
    return base / "HWDER"


def app_dir():
    return data_dir() / "app"


def cache_dir():
    return data_dir() / "build"


def play_exe():
    return cache_dir() / "play" / "hwder"


def read_json(path):
    try:
        value = json.loads(Path(path).read_text(encoding="utf-8"))
        return value if isinstance(value, dict) else {}
    except (OSError, ValueError):
        return {}


def write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name("." + path.name + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True), encoding="utf-8")
    os.replace(temporary, path)


def release_info(root=None):
    """release.json is written into the release by tools/linux/package_release.py; a git checkout has none."""
    root = root or ROOT
    info = read_json(Path(root) / "release.json")
    if not info:
        try:
            version = subprocess.run(["git", "-C", str(root), "describe", "--tags", "--match", "v*", "--always",
                                      "--dirty"], capture_output=True, text=True, timeout=10).stdout.strip()
        except (OSError, subprocess.SubprocessError):
            version = ""
        info = {"version": (version or "unknown") + " (source checkout)", "files": None}
    return info


def config():
    return read_json(data_dir() / "setup.json")


def save_config(**changes):
    value = config()
    value.update(changes)
    value["schema"] = SCHEMA
    write_json(data_dir() / "setup.json", value)
    return value


def deps_dir(root=None):
    return Path(root or ROOT) / "linux-deps"


# ---- The release copy in the data folder ------------------------------------------------------------------------

def file_sha256(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def install_app(report):
    """Copies this release into data/app (verified against its release.json), unless it already runs from there or
    from a source checkout. Returns the folder the build uses."""
    info = release_info()
    if not info.get("files"):
        return ROOT  # developer checkout: build from it in place
    target = app_dir()
    try:
        if target.resolve() == ROOT:
            return ROOT
    except OSError:
        pass
    installed = read_json(target / "release.json")
    if installed.get("files") == info["files"] and all((target / n).is_file() for n in info["files"]):
        return target
    report.stage("install", f"Installing HWDER {info['version']} into {target}")
    staging = target.with_name("app.new")
    shutil.rmtree(staging, ignore_errors=True)
    names = sorted(info["files"])
    for i, name in enumerate(names):
        source, destination = ROOT / name, staging / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
        if file_sha256(destination) != info["files"][name]:
            shutil.rmtree(staging, ignore_errors=True)
            raise SetupError(f"The release is damaged ({name} does not match). Download and unpack it again.")
        if i % 200 == 0:
            report.progress(i / len(names), f"copying {i}/{len(names)} files")
    shutil.copy2(ROOT / "release.json", staging / "release.json")
    report.end_progress()
    old = target.with_name("app.old")
    shutil.rmtree(old, ignore_errors=True)
    if target.exists():
        os.replace(target, old)
    os.replace(staging, target)
    shutil.rmtree(old, ignore_errors=True)
    return target


# ---- Pinned toolchain --------------------------------------------------------------------------------------------

def manifest(root=ROOT):
    return json.loads((Path(root) / "tools" / "linux" / "toolchain.json").read_text(encoding="utf-8"))


def toolchain_dir(root=ROOT):
    identity = hashlib.sha256((Path(root) / "tools" / "linux" / "toolchain.json").read_bytes()).hexdigest()[:16]
    return data_dir() / "toolchain" / identity


def tool_paths(tc):
    tc = Path(tc)
    return {"python": tc / "python" / "bin" / "python3", "cmake": tc / "cmake" / "bin" / "cmake",
            "ninja": tc / "ninja" / "ninja", "zig": tc / "zig" / "zig", "cc": tc / "bin" / "zig-cc",
            "cxx": tc / "bin" / "zig-c++", "ar": tc / "bin" / "zig-ar", "ranlib": tc / "bin" / "zig-ranlib"}


def toolchain_ready(root=ROOT):
    tc = toolchain_dir(root)
    marker = read_json(tc / "complete.json")
    return (marker.get("manifest") == file_sha256(Path(root) / "tools" / "linux" / "toolchain.json")
            and all(p.is_file() for p in tool_paths(tc).values()))


def download(url, target, expected_sha256, size, report, label):
    """Streams `url` into `target`, checking size and SHA-256; retries a few times."""
    last_error = None
    for attempt in range(4):
        if attempt:
            time.sleep(2 ** attempt)
        h, done = hashlib.sha256(), 0
        try:
            request = urllib.request.Request(url, headers={"User-Agent": "HWDER-Setup"})
            with urllib.request.urlopen(request, timeout=60) as reply, open(target, "wb") as out:
                while True:
                    chunk = reply.read(1 << 20)
                    if not chunk:
                        break
                    done += len(chunk)
                    if done > size:
                        raise SetupError(f"{label}: the download is larger than expected")
                    h.update(chunk)
                    out.write(chunk)
                    report.progress(done / size, f"{label}: {done / 1e6:.0f} of {size / 1e6:.0f} MB")
            if done != size or h.hexdigest() != expected_sha256:
                raise SetupError(f"{label}: the download does not match its pinned SHA-256; it was not used")
            report.end_progress()
            return
        except (OSError, urllib.error.URLError) as e:
            last_error = e
    report.end_progress()
    raise SetupError(f"Could not download {label} ({last_error}). Check the internet connection and try again.")


def safe_extract_tar(archive, destination):
    with tarfile.open(archive) as tar:
        if hasattr(tarfile, "data_filter"):
            tar.extractall(destination, filter="data")
            return
        base = Path(destination).resolve()
        for member in tar.getmembers():
            target = (base / member.name).resolve()
            if base not in target.parents and target != base:
                raise SetupError(f"Unsafe path in {Path(archive).name}: {member.name}")
            if member.issym() or member.islnk():
                link = (target.parent / member.linkname).resolve() if member.issym() else (base / member.linkname).resolve()
                if base not in link.parents:
                    raise SetupError(f"Unsafe link in {Path(archive).name}: {member.name}")
            elif not (member.isfile() or member.isdir()):
                raise SetupError(f"Unsupported entry in {Path(archive).name}: {member.name}")
        tar.extractall(destination)


def safe_extract_zip(archive, destination):
    base = Path(destination).resolve()
    with zipfile.ZipFile(archive) as z:
        for info in z.infolist():
            target = (base / info.filename).resolve()
            if base not in target.parents and target != base:
                raise SetupError(f"Unsafe path in {Path(archive).name}: {info.filename}")
            z.extract(info, base)
            mode = (info.external_attr >> 16) & 0o777
            if mode and not info.is_dir():
                os.chmod(target, mode | 0o600)


def write_wrappers(tc, final=None):
    """Zig's clang, archiver and ranlib as plain commands for CMake: C/C++ target glibc 2.35 so the game runs on
    any distribution from Ubuntu 22.04 / SteamOS 3 on. `final` is where the toolchain folder ends up (the wrappers
    are written while it is still being assembled)."""
    paths = tool_paths(tc)
    zig = shlex.quote(str(tool_paths(final or tc)["zig"]))
    (Path(tc) / "bin").mkdir(exist_ok=True)
    scripts = {
        "cc": f'if [ "$1" = --version ]; then exec {zig} cc --version; fi\nexec {zig} cc -target x86_64-linux-gnu.2.35 "$@"\n',
        "cxx": f'if [ "$1" = --version ]; then exec {zig} c++ --version; fi\nexec {zig} c++ -target x86_64-linux-gnu.2.35 "$@"\n',
        "ar": f'exec {zig} ar "$@"\n',
        "ranlib": f'exec {zig} ranlib "$@"\n',
    }
    for name, body in scripts.items():
        path = paths[name]
        path.write_text("#!/bin/sh\n# Written by HWDER Setup (tools/linux_setup.py).\n" + body, encoding="utf-8")
        os.chmod(path, 0o755)


def install_toolchain(report, root=ROOT):
    """Downloads, checks and unpacks the pinned tools once; later runs only check the completion marker."""
    if toolchain_ready(root):
        write_wrappers(toolchain_dir(root))  # cheap; repairs wrappers of a moved data folder
        return toolchain_dir(root)
    value = manifest(root)
    tc = toolchain_dir(root)
    total = sum(a["size"] for a in value["artifacts"])
    report.stage("tools", f"Downloading the build tools once ({total / 1e6:.0f} MB, each checked by SHA-256)")
    free = shutil.disk_usage(data_dir()).free
    if free < DISK_NEEDED:
        raise SetupError(f"Not enough free space in {data_dir()}: {free / 1e9:.1f} GB free, about "
                         f"{DISK_NEEDED / 1e9:.0f} GB needed.")
    staging = tc.with_name(tc.name + ".partial")
    shutil.rmtree(staging, ignore_errors=True)
    staging.mkdir(parents=True)
    downloads = data_dir() / "downloads"
    downloads.mkdir(parents=True, exist_ok=True)
    try:
        wheels = []
        for artifact in value["artifacts"]:
            name = artifact["url"].rsplit("/", 1)[1].replace("%2B", "+")
            archive = downloads / name
            if not (archive.is_file() and archive.stat().st_size == artifact["size"]
                    and file_sha256(archive) == artifact["sha256"]):
                download(artifact["url"], archive, artifact["sha256"], artifact["size"], report,
                         f"{artifact['id']} {artifact['version']}")
            report.log(f"{artifact['id']} {artifact['version']}: SHA-256 verified")
            if artifact.get("wheel"):
                wheels.append((artifact, archive))
                continue
            unpack = staging / (".unpack-" + artifact["id"])
            unpack.mkdir()
            if name.endswith(".zip"):
                safe_extract_zip(archive, unpack)
            else:
                safe_extract_tar(archive, unpack)
            source = unpack / artifact["archive_root"] if artifact["archive_root"] else unpack
            os.replace(source, staging / artifact["id"])
            shutil.rmtree(unpack, ignore_errors=True)
            for required in artifact["required"]:
                if not (staging / artifact["id"] / required).exists():
                    raise SetupError(f"{artifact['id']} is missing {required} after unpacking")
        site = sorted((staging / "python" / "lib").glob("python3.*/site-packages"))
        if not site:
            raise SetupError("The pinned Python has no site-packages folder")
        for artifact, archive in wheels:
            safe_extract_zip(archive, site[0])
            for required in artifact["required"]:
                if not (site[0] / required).exists():
                    raise SetupError(f"{artifact['id']} is missing {required} after unpacking")
        write_wrappers(staging, tc)
        check = subprocess.run([str(tool_paths(staging)["python"]), "-I", "-c",
                                "import cryptography.hazmat.primitives.ciphers, lz4.block, zstandard; print('ok')"],
                               capture_output=True, text=True)
        if check.stdout.strip() != "ok":
            raise SetupError("The pinned Python could not load its modules: " + (check.stderr.strip()[-300:] or "?"))
        write_json(staging / "complete.json", {"manifest": file_sha256(Path(root) / "tools" / "linux" / "toolchain.json"),
                                               "target": value["target"]})
        if tc.exists():
            shutil.rmtree(tc)
        os.replace(staging, tc)
    finally:
        shutil.rmtree(staging, ignore_errors=True)
    # Older toolchains (from an earlier release) and the downloads are no longer needed.
    for other in tc.parent.iterdir():
        if other != tc and other.is_dir():
            shutil.rmtree(other, ignore_errors=True)
    shutil.rmtree(downloads, ignore_errors=True)
    report.log("build tools ready")
    return tc


def build_environment(tc, app):
    paths = tool_paths(tc)
    env = {k: v for k, v in os.environ.items()
           if k not in ("LD_PRELOAD", "LD_LIBRARY_PATH", "PYTHONPATH", "PYTHONHOME", "CFLAGS", "CXXFLAGS", "LDFLAGS",
                        "CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "LIBRARY_PATH", "PKG_CONFIG_PATH", "CC", "CXX",
                        "HWDER_NSP", "HWDER_KEYS", "HWDER_EXEFS", "HWDER_ROMFS")}
    env.update({
        "PATH": os.pathsep.join([str(Path(tc) / "bin"), str(paths["cmake"].parent), str(paths["ninja"].parent),
                                 str(paths["python"].parent), "/usr/bin", "/bin"]),
        "CC": str(paths["cc"]), "CXX": str(paths["cxx"]), "HWDER_AR": str(paths["ar"]),
        "HWDER_RANLIB": str(paths["ranlib"]), "HWDER_LINUX_DEPS": str(deps_dir(app)),
        "ZIG_GLOBAL_CACHE_DIR": str(Path(tc) / "zig-cache"), "ZIG_LOCAL_CACHE_DIR": str(Path(tc) / "zig-cache"),
        "PYTHONNOUSERSITE": "1", "PYTHONUTF8": "1",
    })
    return env


# ---- Checking the player's files --------------------------------------------------------------------------------

def clean_path(text):
    """Paths typed, pasted or dropped into a terminal: quotes, file:// URIs, ~ and surrounding spaces."""
    text = (text or "").strip()
    if len(text) >= 2 and text[0] == text[-1] and text[0] in "'\"":
        text = text[1:-1]
    if text.startswith("file://"):
        text = urllib.request.url2pathname(text[7:])
    elif "\\ " in text:
        text = text.replace("\\ ", " ")
    return str(Path(os.path.expanduser(text)).absolute()) if text else ""


def check_file(kind, path):
    """Quick check of a chosen file; returns (ok, message). The build checks the game's title and version first
    thing (within seconds) with the keys."""
    if not path:
        return (kind == "update"), ("not used" if kind == "update" else "not chosen yet")
    p = Path(path)
    if not p.is_file():
        return False, "file not found"
    try:
        with p.open("rb") as f:
            head = f.read(4096)
    except OSError as e:
        return False, f"cannot be read ({e.strerror})"
    if kind in ("nsp", "update"):
        suffixes = (".nsp",) if kind == "nsp" else (".nsp", ".nsz")
        if p.suffix.lower() not in suffixes:
            return False, "choose your " + ("game .nsp file" if kind == "nsp" else "update .nsp or .nsz file")
        if head[:4] != b"PFS0":
            return False, "not an NSP (no PFS0 header)"
        size = p.stat().st_size
        if kind == "nsp" and size < 1024 ** 3:
            return False, f"too small for the game ({size / 1e6:.0f} MB)"
        return True, f"{size / 1e9:.1f} GB" if size >= 1e9 else f"{size / 1e6:.0f} MB"
    try:
        text = p.read_text(encoding="utf-8", errors="replace")
    except OSError as e:
        return False, f"cannot be read ({e.strerror})"
    names = {line.split("=", 1)[0].strip().lower() for line in text.splitlines() if "=" in line}
    if "header_key" not in names or not any(n.startswith("key_area_key_application_") for n in names):
        return False, "not a prod.keys file (header_key and key_area_key_application_* are needed)"
    return True, f"{len(names)} keys"


# ---- Prepare: build the game ------------------------------------------------------------------------------------

NINJA_STEP = re.compile(r"^\[(\d+)/(\d+)\]")


def default_jobs():
    cpus = os.cpu_count() or 1
    try:
        memory_gb = os.sysconf("SC_PAGE_SIZE") * os.sysconf("SC_PHYS_PAGES") / 1024 ** 3
    except (ValueError, OSError, AttributeError):
        memory_gb = 8
    return max(1, min(cpus, int(memory_gb // 2), 8))


def run_build(report, tc, app, nsp, keys, update, jobs):
    """Runs tools/first_run.py on the pinned Python; maps its output to stages and a progress fraction."""
    command = [str(tool_paths(tc)["python"]), "-u", str(Path(app) / "tools" / "first_run.py"), "--nsp", nsp,
               "--keys", keys, "--cache", str(cache_dir()), "--jobs", str(jobs), "--no-steam"]
    if update:
        command += ["--update", update]
    report.stage("build", "Preparing the game from your files (about 15 minutes on a ROG Ally; the first time only)")
    process = subprocess.Popen(command, cwd=str(app), env=build_environment(tc, app), stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True, errors="replace", bufsize=1,
                               start_new_session=True)
    failure, phase, last_tick = None, 0.02, 0.0
    phases = (("validating NSP", 0.02, "Checking your game and keys"), ("unpacking the game update", 0.03,
              "Unpacking the game update"), ("exefs:", 0.05, "Reading the game's code"), ("generated:", 0.07,
              "Translating the game's code (a few minutes)"), ("build: compiling", 0.12, "Compiling"))
    try:
        for line in process.stdout:
            line = line.rstrip("\n")
            step = NINJA_STEP.match(line)
            if step:
                done, total = int(step.group(1)), int(step.group(2))
                now = time.monotonic()
                if now - last_tick > 0.5 or done == total:
                    last_tick = now
                    report.progress(0.12 + 0.86 * done / max(total, 1), f"Compiling {done} of {total}")
                continue
            if not line.strip() or line.startswith(("-- ", "[", "ninja:")):
                continue
            if line.startswith("setup: failed:"):
                failure = line[len("setup: failed:"):].strip()
            for marker, fraction, text in phases:
                if marker in line and fraction > phase:
                    phase = fraction
                    report.end_progress()
                    report.progress(fraction, text)
            if line.startswith("setup: ") or "error" in line.lower():
                report.end_progress()
                report.log(line[len("setup: "):] if line.startswith("setup: ") else line)
        code = process.wait()
    except KeyboardInterrupt:
        os.killpg(process.pid, signal.SIGINT)
        process.wait()
        raise
    report.end_progress()
    if code:
        raise SetupError(failure or f"The build stopped (exit {code}). Your saves and the last working game are kept.")
    if not play_exe().is_file():
        raise SetupError("The build finished without the game")


def import_app_tools(app):
    sys.path.insert(0, str(Path(app) / "tools"))
    import steam_shortcut  # noqa: E402  (stdlib only; imports first_run, also stdlib only)
    return steam_shortcut


def prepare(report, nsp, keys, update=None, jobs=None):
    for kind, path in (("nsp", nsp), ("keys", keys), ("update", update)):
        ok, message = check_file(kind, path)
        if not ok:
            raise SetupError({"nsp": "Game", "keys": "prod.keys", "update": "Update"}[kind] + ": " + message)
    data_dir().mkdir(parents=True, exist_ok=True)
    os.chmod(data_dir(), 0o700)
    (data_dir() / "mods").mkdir(exist_ok=True)
    app = install_app(report)
    tc = install_toolchain(report, app)
    save_config(nsp=nsp, keys=keys, update=update or "")
    run_build(report, tc, app, nsp, keys, update, jobs or default_jobs())
    report.stage("finish", "Finishing")
    steam_shortcut = import_app_tools(app)
    exe = play_exe()
    # Steam, the menu entry and the desktop icon start the game without HWDER_NSP/HWDER_KEYS: its settings name them.
    steam_shortcut.store_game_paths(cache_dir(), exe, nsp, keys)
    steam_shortcut.write_launcher(exe, "")
    save_config(built=release_info(app).get("version"), built_at=time.strftime("%Y-%m-%d %H:%M"))
    write_desktop_entries(app, report, desktop_icon=False)
    return exe


# ---- Menu entries, Steam, Play ----------------------------------------------------------------------------------

NINTENDO_ICON = ("https://assets.nintendo.com/image/upload/c_fill,g_auto,w_256,h_256/f_png/store/software/switch/"
                 "70010000006023/4b035b357b10e59dce73609dd071e61d131d90b2e477c29088f26fbe50b44d1b")


def desktop_quote(value):
    """Exec= argument quoting from the Desktop Entry spec."""
    value = str(value)
    if not re.search(r'[\s"\'\\><~|&;$*?#()`]', value):
        return value
    return '"' + re.sub(r'(["`$\\])', r"\\\1", value) + '"'


def user_desktop():
    try:
        out = subprocess.run(["xdg-user-dir", "DESKTOP"], capture_output=True, text=True, timeout=5).stdout.strip()
        if out and Path(out).is_dir() and Path(out) != Path.home():
            return Path(out)
    except (OSError, subprocess.SubprocessError):
        pass
    fallback = Path.home() / "Desktop"
    return fallback if fallback.is_dir() else None


def game_icon():
    icon = data_dir() / "hwder.png"
    if not icon.is_file():
        try:
            request = urllib.request.Request(NINTENDO_ICON, headers={"User-Agent": "HWDER-Setup"})
            with urllib.request.urlopen(request, timeout=20) as reply:
                picture = reply.read(4 << 20)
            if picture.startswith(b"\x89PNG"):
                icon.write_bytes(picture)
        except (OSError, urllib.error.URLError, ValueError):
            pass
    return str(icon) if icon.is_file() else "applications-games"


def entry_text(name, comment, exec_path, folder, icon, categories):
    return ("[Desktop Entry]\nType=Application\nVersion=1.0\n"
            f"Name={name}\nComment={comment}\nExec={desktop_quote(exec_path)}\nPath={folder}\nIcon={icon}\n"
            f"Terminal=false\nCategories={categories}\nStartupNotify=false\n")


def write_entry(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name("." + path.name + ".tmp")
    temporary.write_text(text, encoding="utf-8")
    os.chmod(temporary, 0o755)  # Plasma and GNOME only start desktop-folder launchers that are executable
    os.replace(temporary, path)


def menu_folder():
    configured = os.environ.get("XDG_DATA_HOME", "")
    base = Path(configured) if configured and Path(configured).is_absolute() else Path.home() / ".local" / "share"
    return base / "applications"


def write_desktop_entries(app, report, desktop_icon=False):
    """Menu entries for the game and for HWDER Setup; with desktop_icon also a game icon on the desktop."""
    exe = play_exe()
    icon = game_icon()
    written = []
    setup = entry_text("HWDER Setup", "Set up, update or rebuild Hyrule Warriors: Definitive Edition (HWDER)",
                       Path(app) / "hwder-setup", Path(app), icon, "Game;Settings;")
    write_entry(menu_folder() / "hwder-setup.desktop", setup)
    written.append(menu_folder() / "hwder-setup.desktop")
    if exe.is_file():
        game = entry_text(GAME_NAME, "Play HWDER (native PC version, built from your own game)",
                          exe.parent / "hwder-steam.sh", exe.parent, icon, "Game;ActionGame;")
        write_entry(menu_folder() / "hwder.desktop", game)
        written.append(menu_folder() / "hwder.desktop")
        if desktop_icon:
            folder = user_desktop()
            if folder is None:
                report.log("no desktop folder found; the game is in the application menu")
            else:
                write_entry(folder / "hwder.desktop", game)
                written.append(folder / "hwder.desktop")
    if shutil.which("update-desktop-database"):
        subprocess.run(["update-desktop-database", str(menu_folder())], capture_output=True)
    for path in written:
        report.log(f"launcher: {path}")
    return written


def desktop_state():
    folder = user_desktop()
    return {"menu": (menu_folder() / "hwder.desktop").is_file(),
            "desktop": bool(folder and (folder / "hwder.desktop").is_file())}


def add_to_steam(report, assume_yes):
    exe = play_exe()
    if not exe.is_file():
        raise SetupError("Prepare the game first")
    app = install_app(report)
    steam_shortcut = import_app_tools(app)
    current = config()
    if current.get("nsp") and current.get("keys"):
        steam_shortcut.store_game_paths(cache_dir(), exe, current["nsp"], current["keys"])
    if not report.json:
        return steam_shortcut.add_to_steam(exe, assume_yes=assume_yes)
    # The window asked already; Steam's own messages become log lines.
    original = steam_shortcut.say
    steam_shortcut.say = lambda message: report.log(message)
    try:
        return steam_shortcut.add_to_steam(exe, assume_yes=True)
    finally:
        steam_shortcut.say = original


def steam_state(app):
    try:
        steam_shortcut = import_app_tools(app)
        roots = steam_shortcut.steam_roots()
        running = bool(steam_shortcut.steam_processes())
        present = bool(roots) and play_exe().is_file() and steam_shortcut.entry_present(play_exe())
        game_mode = running and steam_shortcut.game_mode(steam_shortcut.steam_processes())
        return {"installed": bool(roots), "running": running, "game_mode": bool(game_mode), "added": present}
    except (OSError, ValueError, ImportError):
        return {"installed": False, "running": False, "game_mode": False, "added": False}


def game_running():
    for pid in os.listdir("/proc"):
        if pid.isdigit():
            try:
                if Path("/proc", pid, "comm").read_text().strip() == "hwder":
                    return True
            except OSError:
                continue
    return False


def play(report):
    exe = play_exe()
    launcher = exe.parent / "hwder-steam.sh"
    if not exe.is_file() or not launcher.is_file():
        raise SetupError("Prepare the game first")
    if game_running():
        raise SetupError("HWDER is already running")
    env = {k: v for k, v in os.environ.items() if k not in ("HWDER_NSP", "HWDER_KEYS", "LD_LIBRARY_PATH")}
    subprocess.Popen([str(launcher)], cwd=str(exe.parent), env=env, stdin=subprocess.DEVNULL,
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
    report.log("HWDER is starting")


def update_needed(info, built, game_present):
    """A release whose version differs from the one that built the game (newer, older or another branch) offers
    "Update to <version>"; a source checkout (no release.json file list) is not checked."""
    return bool(game_present and info.get("files") and built != info.get("version"))


def status():
    current = config()
    info = release_info()
    app = app_dir() if (app_dir() / "release.json").is_file() else ROOT
    exe = play_exe()
    last = read_json(cache_dir() / "last-success.json")
    files = {}
    for kind in ("nsp", "keys", "update"):
        path = current.get(kind, "")
        ok, message = check_file(kind, path)
        files[kind] = {"path": path, "ok": ok, "message": message}
    try:
        free = shutil.disk_usage(data_dir() if data_dir().exists() else Path.home()).free
    except OSError:
        free = 0
    built = current.get("built")
    return {"version": info.get("version"), "data_dir": str(data_dir()), "release_dir": str(ROOT),
            "mods_dir": str(data_dir() / "mods"),
            "installed_version": read_json(app_dir() / "release.json").get("version"),
            "toolchain_ready": toolchain_ready(app), "files": files,
            "game_ready": exe.is_file() and bool(last), "game_path": str(exe), "built_version": built,
            "rebuild_needed": update_needed(info, built, exe.is_file()),
            "update_in_build": bool(last.get("update_dir")), "profile": last.get("profile"),
            "steam": steam_state(app), "launchers": desktop_state(), "game_running": game_running(),
            "free_bytes": free, "jobs": default_jobs()}


def uninstall(report):
    """Removes the toolchain, the build (the game and its caches), the release copy and the menu entries. Saves
    (data/save) and setup.json stay; the Steam entry stays until removed in Steam."""
    if game_running():
        raise SetupError("Close HWDER first")
    for path in (data_dir() / "toolchain", cache_dir(), data_dir() / "downloads", app_dir(), data_dir() / "hwder.png"):
        if path.is_dir():
            shutil.rmtree(path, ignore_errors=True)
        elif path.exists():
            path.unlink()
    folder = user_desktop()
    for entry in (menu_folder() / "hwder.desktop", menu_folder() / "hwder-setup.desktop",
                  folder / "hwder.desktop" if folder else None):
        if entry and entry.is_file():
            entry.unlink()
    report.log(f"kept your saves in {data_dir() / 'save'}")


# ---- Interactive terminal setup ---------------------------------------------------------------------------------

def ask_path(kind, label, current, optional=False):
    try:
        import readline  # noqa: F401  (Tab completes paths)
        readline.set_completer_delims(" \t\n;")
        readline.parse_and_bind("tab: complete")
        def complete(text, state):
            base = os.path.expanduser(text)
            matches = sorted(p + ("/" if os.path.isdir(p) else "") for p in __import__("glob").glob(base + "*"))
            return matches[state] if state < len(matches) else None
        readline.set_completer(complete)
    except ImportError:
        pass
    while True:
        hint = f" [{current}]" if current else (" [Enter: none]" if optional else "")
        extra = " ('-' removes it)" if optional and current else ""
        try:
            answer = input(f"{label}{hint}{extra}: ")
        except EOFError:
            raise SetupError("cancelled")
        if not answer.strip():
            if current or optional:
                path = current
            else:
                continue
        elif optional and answer.strip() == "-":
            path = ""
        else:
            path = clean_path(answer)
        ok, message = check_file(kind, path)
        if ok:
            if path:
                print(f"   ok: {path} ({message})")
            return path
        print(f"   {message}. Drag the file into this window or type its path (Tab completes).")


def yes(question, default=True):
    try:
        answer = input(question + (" [Y/n] " if default else " [y/N] ")).strip().lower()
    except EOFError:
        return default
    return answer in ("y", "yes") if answer else default


def interactive(report):
    current = config()
    info = release_info()
    print(f"HWDER Setup {info.get('version')} - Hyrule Warriors: Definitive Edition for Linux\n"
          f"Builds the game on this PC from your own dump. Your files are read where they are, never copied.\n"
          f"Everything goes to {data_dir()} (saves in {data_dir() / 'save'}).")
    if game_running():
        print("\nHWDER is running. Close it before preparing the game again.")
    print("\n== 1/3  Your files")
    nsp = ask_path("nsp", "Game (.nsp)", current.get("nsp", ""))
    keys = ask_path("keys", "Keys (prod.keys)", current.get("keys", ""))
    print("Game update 1.0.1 is optional (your own update .nsp or .nsz).")
    update = ask_path("update", "Update", current.get("update", ""), optional=True)
    status_now = status()
    ready = (status_now["game_ready"] and not status_now["rebuild_needed"] and current.get("nsp") == nsp
             and current.get("keys") == keys and current.get("update", "") == (update or ""))
    print("\n== 2/3  Prepare")
    if status_now["rebuild_needed"]:
        print(f"Installed: {status_now['built_version'] or 'an earlier release'}. This release is {status_now['version']}.")
        if not yes(f"Update to {status_now['version']} now? It takes about 15 minutes; saves and settings are kept."):
            return report.result(False, "Nothing changed. Run HWDER Setup again to update.")
        prepare(report, nsp, keys, update)
        print("\nThe game is updated.")
    elif ready and not yes("The game is already prepared with these files. Prepare it again?", default=False):
        pass
    else:
        first = "" if toolchain_ready() else " The build tools are downloaded first (about 200 MB, once)."
        if not yes(f"Prepare the game now? It takes about 15 minutes.{first}"):
            return report.result(False, "Nothing changed. Run HWDER Setup again when you are ready.")
        prepare(report, nsp, keys, update)
        print("\nThe game is ready.")
    print("\n== 3/3  Launchers")
    write_desktop_entries(app_dir() if (app_dir() / "release.json").is_file() else ROOT, report,
                          desktop_icon=yes("Put a game icon on the desktop too? (It is in the application menu.)",
                                           default=False))
    steam = steam_state(app_dir() if (app_dir() / "release.json").is_file() else ROOT)
    if steam["installed"] and not steam["added"]:
        if yes("Add HWDER to Steam (so it starts from your library and Game Mode)?"):
            add_to_steam(report, assume_yes=None)
    elif steam["added"]:
        print("HWDER is in your Steam library.")
    if yes("Play now?", default=False):
        play(report)
    return report.result(True, "Done. Start the game from the application menu"
                         + (" or Steam" if steam_state(ROOT)["added"] else "") + "; run HWDER Setup again to "
                         f"update, change files or rebuild.\nSaves: {data_dir() / 'save'}   Mods: {data_dir() / 'mods'}")


# ---- Command line -----------------------------------------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--json", action="store_true", help="JSON lines for the HWDER Setup window")
    sub = ap.add_subparsers(dest="command")
    sub.add_parser("status", help="what is installed and chosen")
    check = sub.add_parser("check", help="check chosen files without building")
    for p in (check,):
        p.add_argument("--nsp", default="")
        p.add_argument("--keys", default="")
        p.add_argument("--update", default="")
    p = sub.add_parser("prepare", help="download the build tools if needed and build the game")
    p.add_argument("--nsp", required=True)
    p.add_argument("--keys", required=True)
    p.add_argument("--update", default="", help="your own game update 1.0.1 (.nsp or .nsz); omit for the base game")
    p.add_argument("--jobs", type=int, default=0)
    s = sub.add_parser("steam", help="add the game to Steam (closes and restarts a running Steam)")
    s.add_argument("--yes", action="store_true", help="close and restart a running Steam without asking")
    d = sub.add_parser("desktop", help="write the menu entries (and a desktop icon with --desktop-icon)")
    d.add_argument("--desktop-icon", action="store_true")
    sub.add_parser("play", help="start the game")
    sub.add_parser("toolchain", help="download and check the build tools only")
    u = sub.add_parser("uninstall", help="remove the game build, tools and launchers; keeps saves")
    u.add_argument("--yes", action="store_true")
    a = ap.parse_args(argv)
    report = Report(a.json)
    if sys.version_info < (3, 8) or not sys.platform.startswith("linux") or platform.machine() != "x86_64":
        return report.result(False, "HWDER Setup needs Linux on x86-64 and Python 3.8 or newer.")
    try:
        if a.command in (None, "interactive"):
            if not sys.stdin.isatty():
                return report.result(False, "Run hwder-setup from a terminal or open the HWDER Setup window.")
            return interactive(report)
        if a.command == "status":
            value = status()
            if a.json:
                report.emit("status", **value)
            else:
                print(json.dumps(value, indent=2))
            return 0
        if a.command == "check":
            results = {k: dict(zip(("ok", "message"), check_file(k, clean_path(getattr(a, k)))))
                       for k in ("nsp", "keys", "update")}
            ok = all(r["ok"] for r in results.values())
            return report.result(ok, "files look right" if ok else "check the files", files=results)
        if a.command == "prepare":
            exe = prepare(report, clean_path(a.nsp), clean_path(a.keys), clean_path(a.update) or None, a.jobs or None)
            return report.result(True, "The game is ready.", game=str(exe))
        if a.command == "toolchain":
            install_toolchain(report, install_app(report))
            return report.result(True, "Build tools ready.")
        if a.command == "steam":
            ok = add_to_steam(report, True if a.yes else None)
            return report.result(ok, "HWDER is in your Steam library." if ok else "Not added to Steam.")
        if a.command == "desktop":
            write_desktop_entries(install_app(report), report, a.desktop_icon)
            return report.result(True, "Launchers written.")
        if a.command == "play":
            play(report)
            return report.result(True, "Starting HWDER.")
        if a.command == "uninstall":
            if not a.yes and not (sys.stdin.isatty() and yes(f"Remove the game build, tools and launchers from "
                                                            f"{data_dir()}? Saves stay.", default=False)):
                return report.result(False, "Nothing removed.")
            uninstall(report)
            return report.result(True, "HWDER removed (saves kept). Remove the Steam entry in Steam if you added it.")
    except KeyboardInterrupt:
        report.end_progress()
        return report.result(False, "Stopped. Run Setup again to continue where it left off.")
    except SetupError as e:
        report.end_progress()
        return report.result(False, str(e))
    except (OSError, ValueError, subprocess.SubprocessError) as e:
        report.end_progress()
        return report.result(False, f"Setup failed: {e}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
