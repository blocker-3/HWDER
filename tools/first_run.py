#!/usr/bin/env python3
"""User-local full build: validate -> extract -> generate -> compile -> publish.

All game-derived output stays in a private cache. No downloaded compilers or game data.
"""
import argparse
from contextlib import contextmanager
import hashlib
import importlib.metadata
import json
import os
import platform
import stat
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import time
import uuid

ROOT = Path(__file__).resolve().parent.parent
SCHEMA = 1


def log(message):
    print(f"setup: {message}", flush=True)


def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def file_hash(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as f:
        while chunk := f.read(8 * 1024 * 1024):
            h.update(chunk)
    return h.hexdigest()


def atomic_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(".tmp-" + uuid.uuid4().hex[:8])
    try:
        tmp.write_text(json.dumps(value, sort_keys=True, indent=2), encoding="utf-8")
        os.replace(tmp, path)
    finally:
        tmp.unlink(missing_ok=True)


def read_json(path):
    try:
        value = json.loads(Path(path).read_text(encoding="utf-8"))
        return value if isinstance(value, dict) else {}
    except (OSError, ValueError):
        return {}


def tree_hashes(root):
    """Exact file set: detect missing/added/tampered generated input, ignoring Python bytecode."""
    root = Path(root)
    result = {}
    for path in sorted(root.rglob("*")):
        rel = path.relative_to(root)
        if any(p in (".git", "__pycache__") for p in rel.parts) or path.suffix == ".pyc":
            continue
        if path.is_symlink():
            raise ValueError(f"Symlinks unsupported in build inputs: {rel}")
        if path.is_file():
            result[rel.as_posix()] = file_hash(path)
    return result


def valid_stage(directory, key):
    marker = read_json(Path(directory) / "complete.json")
    output = Path(directory) / "output"
    return (marker.get("schema") == SCHEMA and marker.get("key") == key
            and bool(marker.get("files")) and output.is_dir()
            and marker["files"] == tree_hashes(output))


def completed_stage(cache, category, key, produce):
    directory = cache / category / key[:24]
    if valid_stage(directory, key):
        log(f"{category}: verified cache")
        return directory / "output"
    # New attempt avoids trusting or destructively cleaning interrupted output.
    attempt = cache / category / (key[:24] + ".partial-" + uuid.uuid4().hex[:8])
    output = attempt / "output"
    output.mkdir(parents=True)
    log(f"{category}: starting (partial output is never considered complete)")
    produce(output)
    files = tree_hashes(output)
    if not files:
        raise ValueError(f"{category} produced no output")
    atomic_json(attempt / "complete.json", {"schema": SCHEMA, "key": key, "files": files})
    # A damaged previous entry remains available for inspection, without blocking recovery.
    if directory.exists():
        directory.rename(directory.with_name(key[:24] + ".invalid-" + uuid.uuid4().hex[:8]))
    attempt.rename(directory)
    return directory / "output"


@contextmanager
def setup_lock(cache):
    """Serialize coordinators sharing a cache; OS releases the lock after crash/interrupt."""
    cache.mkdir(parents=True, exist_ok=True, mode=0o700)
    with (cache / "setup.lock").open("a+b") as f:
        if not f.tell():
            f.write(b"0")
            f.flush()
        waiting = False
        while True:
            f.seek(0)
            try:
                if os.name == "nt":
                    import msvcrt
                    msvcrt.locking(f.fileno(), msvcrt.LK_NBLCK, 1)
                else:
                    import fcntl
                    fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except (OSError, BlockingIOError):
                if not waiting:
                    log("waiting for another first-run setup using this cache")
                    waiting = True
                time.sleep(1)
        try:
            yield
        finally:
            if os.name == "nt":
                f.seek(0)
                msvcrt.locking(f.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(f, fcntl.LOCK_UN)


def source_fingerprints(root):
    # Hash actual working files, including dirty source/submodule edits, not only Git HEAD.
    generator = tree_hashes(root / "tools" / "recomp")
    for name in ("nso.py", "nx_extract.py", "local_game.py", "first_run.py", "supported_games.json"):
        generator[name] = file_hash(root / "tools" / name)
    extra = root / "data" / "extra_entries.txt"
    generator["private-extra"] = file_hash(extra) if extra.exists() else None
    runtime = {"cmake": file_hash(root / "CMakeLists.txt"), "root": str(root),
               "runtime": tree_hashes(root / "runtime"), "tools": tree_hashes(root / "tools"),
               "third_party": tree_hashes(root / "third_party")}
    return digest(generator), digest(runtime)


def check_prerequisites(root, managed=None):
    if managed is not None:
        import managed_tools
        return managed_tools.prerequisites(root, managed)
    if ((os.name != "nt" and not sys.platform.startswith("linux")) or sys.maxsize <= 2**32
            or sys.version_info < (3, 10) or platform.machine().lower() not in ("amd64", "x86_64")):
        raise ValueError("Setup requires Windows or Linux x86-64 and Python 3.10+ x64")
    versions = {}
    for package in ("cryptography", "lz4"):
        try:
            versions[package] = importlib.metadata.version(package)
        except importlib.metadata.PackageNotFoundError:
            raise ValueError("Install Python dependencies: python -m pip install -r tools/requirements-setup.txt") from None
    for path in ("SDL/CMakeLists.txt", "imgui/imgui.cpp", "vulkan-headers/include/vulkan/vulkan.h",
                 "spirv-headers/include/spirv/unified1/spirv.hpp"):
        if not (root / "third_party" / path).is_file():
            raise ValueError("Missing pinned third-party sources: run git submodule update --init --recursive")
    from build import toolchain as build_toolchain
    env, cmake, ninja, cc, cxx = build_toolchain()
    tools = {"compiler": cc, "cmake": cmake, "ninja": ninja}
    if os.name == "nt":
        for variable, relative in (("INCLUDE", "Windows.h"), ("LIB", "kernel32.lib"), ("LIB", "libcmt.lib")):
            if not any((Path(p) / relative).is_file() for p in env.get(variable, "").split(";") if p):
                raise ValueError(f"Windows SDK / MSVC x64 prerequisites incomplete: missing {relative}")
        versions["target_platform"] = "windows-x86_64"
    else:
        versions["target_platform"] = "linux-x86_64"
        tools["compiler_cxx"] = cxx
        pkg_config = shutil.which("pkg-config", path=env["PATH"])
        if not pkg_config:
            raise ValueError("Linux runtime development prerequisites require pkg-config")
        for package in ("openssl >= 3.0", "libavcodec", "libavutil"):
            result = subprocess.run([pkg_config, "--modversion", package], env=env, capture_output=True, text=True)
            if result.returncode:
                raise ValueError(f"Missing Linux development dependency: {package}")
            versions[package] = result.stdout.strip()
    for name, path in tools.items():
        r = subprocess.run([path, "--version"], env=env, capture_output=True, text=True, check=True)
        versions[name] = {"path": path, "version": r.stdout.strip()}
    match = re.search(r"cmake version (\d+)\.(\d+)", versions["cmake"]["version"])
    if not match or tuple(map(int, match.groups())) < (3, 24):
        raise ValueError("CMake 3.24+ required")
    if os.name != "nt" and any("clang" not in versions[name]["version"].lower() for name in ("compiler", "compiler_cxx")):
        raise ValueError("Linux generated code requires Clang for both C and C++")
    versions.update({k: env.get(k) for k in ("VCToolsVersion", "WindowsSDKVersion", "WindowsSdkDir")})
    # Compiler/linker overrides and search paths affect output even when versions match.
    # Retain only their digest, never arbitrary environment variable contents.
    versions["compiler_environment_sha256"] = digest({
        k: env.get(k) for k in ("INCLUDE", "LIB", "LIBPATH", "CL", "_CL_", "LINK", "_LINK_",
                              "CFLAGS", "CXXFLAGS", "LDFLAGS", "CC", "CXX", "CPATH", "C_INCLUDE_PATH",
                              "CPLUS_INCLUDE_PATH", "LIBRARY_PATH", "PKG_CONFIG_PATH")})
    versions["python"] = sys.version
    log("prerequisites verified: " + versions["target_platform"] + ", Clang, CMake, Ninja, cryptography, lz4")
    return versions


def target_name(toolchain):
    target = toolchain.get("target_platform", "windows-x86_64")
    if target not in ("windows-x86_64", "linux-x86_64"):
        raise ValueError(f"Unsupported build target: {target}")
    return "hwder.exe" if target == "windows-x86_64" else "hwder"


def verify_executable(path, toolchain):
    target = toolchain.get("target_platform", "windows-x86_64")
    expected = "Windows executable" if target == "windows-x86_64" else "Linux x86-64 executable"
    if not path.is_file():
        raise ValueError(f"Build succeeded without a {expected}")
    with path.open("rb") as stream:
        header = stream.read(64)
    if target == "windows-x86_64":
        valid = header.startswith(b"MZ")
    else:
        # ELF64 little endian, current ABI version, executable/PIE, AMD64 machine.
        valid = (len(header) == 64 and header[:7] == b"\x7fELF\x02\x01\x01"
                 and struct.unpack_from("<HH", header, 16) in ((2, 62), (3, 62))
                 and struct.unpack_from("<I", header, 20)[0] == 1)
        if os.name != "nt":
            valid = valid and bool(path.stat().st_mode & stat.S_IXUSR)
    if not valid:
        raise ValueError(f"Build succeeded without a {expected}")


def executable_valid(path, toolchain):
    try:
        verify_executable(path, toolchain)
        return True
    except (OSError, ValueError):
        return False


def default_cache():
    if os.name == "nt":
        base = Path(os.environ.get("LOCALAPPDATA", str(Path.home())))
    else:
        configured = Path(os.environ.get("XDG_CACHE_HOME", str(Path.home() / ".cache")))
        base = configured if configured.is_absolute() else Path.home() / ".cache"
    return base / "hwder" / "local-build-v1"


def run_command(argv, cwd):
    subprocess.run([str(a) for a in argv], cwd=cwd, check=True)


def unpack_update(cache, update, keys):
    """The game update (.nsp or .nsz), unpacked once into the cache (tools/game_update.py): its main module and
    its RomFS patch, decrypted. Keyed by the update file's hash; the game reads it from there (update_path)."""
    import game_update
    key = digest({"schema": SCHEMA, "update": file_hash(update), "unpacker": file_hash(Path(game_update.__file__))})
    return completed_stage(cache, "update", key, lambda out: game_update.unpack(update, keys, out))


def prepare(root, cache, nsp, keys, jobs, toolchain, run=run_command, read_modules=None, before_publish=None,
            update=None):
    """Injectable stage boundary permits meaningful synthetic orchestration/failure tests."""
    from local_game import read_program, supported_profile, profile as profile_entry
    root, cache, nsp, keys = map(Path, (root, cache, nsp, keys))
    if not nsp.is_file() or nsp.suffix.lower() != ".nsp" or not keys.is_file():
        raise ValueError("Supply an existing .nsp and your own prod.keys via --nsp/--keys or HWDER_NSP/HWDER_KEYS")
    if update is not None:
        update = Path(update)
        if not update.is_file() or update.suffix.lower() not in (".nsp", ".nsz"):
            raise ValueError("Supply the game update as an existing .nsp or .nsz")
    generation_inputs, build_inputs = source_fingerprints(root)
    before = (nsp.stat().st_size, nsp.stat().st_mtime_ns)
    keys_before = (keys.stat().st_size, keys.stat().st_mtime_ns)
    log("validating NSP, own keys, title, module set and supported-version SHA-256")
    modules = (read_modules or read_program)(nsp, keys)
    update_dir = None
    if update is not None:
        log("unpacking the game update (its main module and RomFS patch)")
        update_dir = unpack_update(cache, update, keys)
        modules = {**modules, "main": (update_dir / "exefs" / "main").read_bytes()}
    profile = supported_profile(modules, root / "tools" / "supported_games.json")
    recomp = profile_entry(profile, root / "tools" / "supported_games.json").get("recomp", {})
    log(f"supported version: {profile}; hashing complete NSP for cache identity")
    game_hash = file_hash(nsp)
    if before != (nsp.stat().st_size, nsp.stat().st_mtime_ns):
        raise ValueError("Game input changed during validation; retry after the dump finishes copying")
    module_hashes = {name: hashlib.sha256(data).hexdigest() for name, data in modules.items()}
    exefs_key = digest({"schema": SCHEMA, "game": game_hash, "modules": module_hashes, "profile": profile})
    def extract(out):
        for name, data in modules.items():
            (out / name).write_bytes(data)
    exefs = completed_stage(cache, "exefs", exefs_key, extract)
    exefs_files = tree_hashes(exefs)
    gen_key = digest({"exefs": exefs_key, "generator": generation_inputs, "python": sys.version,
                      "lz4": toolchain.get("lz4")})
    def generate(out):
        args = [sys.executable, "-m", "recomp.gen_main", "--exefs", exefs, "--out", out]
        if toolchain.get("managed_directory"):
            args[1:1] = ["-B", "-X", "utf8"]
        # Explicit entries remove gen_main's implicit dependency on the working directory. Another game version
        # brings its own symbol and entry files (tools/recomp/<version>/, made by remap_symbols.py).
        args += ["--symbols", root / recomp.get("symbols", "tools/recomp/symbols.txt")]
        for extra in recomp.get("extra", ["tools/recomp/extra_entries.txt"]):
            args += ["--extra", root / extra]
        if not recomp and (root / "data" / "extra_entries.txt").is_file():  # local finds are base-game addresses
            args += ["--extra", root / "data" / "extra_entries.txt"]
        run(args, root / "tools")
        if tree_hashes(exefs) != exefs_files:
            raise ValueError("Extracted inputs changed during generation; rerun")
        for name in ("sources.cmake", "funcs.h", "functable.c", "module_info.c"):
            if not (out / name).is_file():
                raise ValueError(f"Generator did not produce {name}")
        if not list(out.glob("rc_*.c")):
            raise ValueError("Generator produced no recompiled C files")
        if source_fingerprints(root)[0] != generation_inputs:
            raise ValueError("Generator inputs changed during generation; rerun")
    generated = completed_stage(cache, "generated", gen_key, generate)
    generated_files = tree_hashes(generated)
    build_key = digest({"generated": gen_key, "generated_files": digest(generated_files),
                        "runtime": build_inputs, "toolchain": toolchain})
    bdir = cache / "builds" / build_key[:24]
    name = target_name(toolchain)
    exe = bdir / name
    marker = read_json(bdir / "complete.json")
    if (marker.get("schema") != SCHEMA or marker.get("key") != build_key or not exe.is_file()
            or marker.get("exe_sha256") != file_hash(exe) or not executable_valid(exe, toolchain)):
        log("build: compiling complete native program via tools/build.py (shared machine build lock)")
        (bdir / "complete.json").unlink(missing_ok=True)
        # Ninja can consider a newer damaged/stale executable up to date. Force its relink
        # whenever the output lacks a valid completion certificate; preserve partial objects.
        exe.unlink(missing_ok=True)
        try:
            command = [sys.executable, root / "tools" / "build.py", "--jobs", str(jobs),
                       "--build-dir", bdir, "--generated-dir", generated]
            if toolchain.get("managed_directory"):
                command[1:1] = ["-B", "-X", "utf8"]
                command += ["--managed-toolchain", toolchain["managed_directory"],
                            "--managed-temp-dir", bdir/"tmp"]
            run(command, root)
        finally:
            # A failed/interrupted compiler can still leave objects that consumed
            # changed input. Check every exit before allowing any partial reuse.
            try:
                stable = tree_hashes(generated) == generated_files and tree_hashes(exefs) == exefs_files
            except (OSError, ValueError):
                stable = False
            if not stable and bdir.exists():
                bdir.rename(bdir.with_name(bdir.name + ".invalid-" + uuid.uuid4().hex[:8]))
        if not stable:
            raise ValueError("Cached build inputs changed during compilation; rerun")
        verify_executable(exe, toolchain)
        if source_fingerprints(root) != (generation_inputs, build_inputs):
            raise ValueError("Build inputs changed during setup; rerun to build the new source version")
        if before != (nsp.stat().st_size, nsp.stat().st_mtime_ns):
            raise ValueError("Game input changed during build; rerun")
        marker = {"schema": SCHEMA, "key": build_key, "exe_sha256": file_hash(exe),
                  "game_sha256": game_hash, "profile": profile}
        atomic_json(bdir / "complete.json", marker)
    else:
        verify_executable(exe, toolchain)
        log("build: verified complete cached executable")
    if source_fingerprints(root) != (generation_inputs, build_inputs):
        raise ValueError("Source inputs changed during setup; rerun")
    if tree_hashes(exefs) != exefs_files or tree_hashes(generated) != generated_files:
        raise ValueError("Cached build inputs changed during setup; rerun")
    if (before != (nsp.stat().st_size, nsp.stat().st_mtime_ns)
            or keys_before != (keys.stat().st_size, keys.stat().st_mtime_ns)):
        raise ValueError("Game or keys input changed during setup; rerun")
    if toolchain.get("managed_directory"):
        import managed_tools
        # Verify fresh AND reused output before the stable play transaction.
        # The static GNU ABI may not borrow host/toolchain DLLs at gameplay.
        managed_tools.game_image(exe)
    # The game reads the unpacked update at run time (update_path in its settings; installed by setup_bridge).
    marker = {**marker, "update_dir": str(update_dir) if update_dir else None}
    # Stable play directory retains user settings, saves and private caches across source updates.
    if before_publish:
        before_publish()  # UI commit permission: no stable play transaction has started yet.
    play = cache / "play"
    play.mkdir(parents=True, exist_ok=True)
    target = play / name
    if not target.is_file() or file_hash(target) != marker["exe_sha256"] or not executable_valid(target, toolchain):
        tmp = play / ("hwder-" + uuid.uuid4().hex + ".tmp")
        previous = play / ("hwder-previous-" + uuid.uuid4().hex + ".tmp")
        published = False
        try:
            shutil.copyfile(exe, tmp)
            if toolchain.get("target_platform") == "linux-x86_64":
                shutil.copymode(exe, tmp)
            if file_hash(tmp) != marker["exe_sha256"]:
                raise ValueError("Executable publication checksum mismatch")
            if target.is_file():
                try:
                    os.link(target, previous)
                except OSError:
                    # A cache may live on a filesystem without hard-link support.
                    shutil.copy2(target, previous)
            os.replace(tmp, target)
            published = True
            atomic_json(cache / "last-success.json", marker)
        except BaseException:
            if published:
                if previous.exists():
                    # Keep the recovery copy if rollback itself fails (e.g. an external
                    # launcher has opened the newly published executable).
                    os.replace(previous, target)
                else:
                    target.unlink(missing_ok=True)
            raise
        else:
            previous.unlink(missing_ok=True)
        finally:
            tmp.unlink(missing_ok=True)
            if not published:
                previous.unlink(missing_ok=True)
    else:
        atomic_json(cache / "last-success.json", marker)
    log(f"ready: {target}")
    return target


def record_update_path(exe, update_dir):
    """The game reads the prepared update from update_path in its settings (on Windows Setup writes it in
    setup_bridge.installed_paths). An empty value switches a previously prepared update off. Other bytes stay."""
    settings = Path(exe).parent / "hwder_settings.ini"
    line = ("update_path=" + (str(Path(update_dir).resolve()) if update_dir else "")).encode("utf-8")
    # A new file carries the game's first-run defaults (without face_labels the game keeps buttons by position).
    original = settings.read_bytes() if settings.exists() else b"version=2\nface_labels=1\n"
    lines = original.splitlines(keepends=True)
    found = False
    for i, old in enumerate(lines):
        if old.split(b"=", 1)[0] == b"update_path":
            lines[i] = b"" if found else line + (b"\r\n" if old.endswith(b"\r\n") else b"\n")
            found = True
    if not found:
        if lines and not lines[-1].endswith(b"\n"):
            lines[-1] += b"\n"
        lines.append(line + b"\n")
    result = b"".join(lines)
    if result != original:
        temp = settings.with_name(settings.name + ".new")
        temp.write_bytes(result)
        os.replace(temp, settings)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--nsp", default=os.environ.get("HWDER_NSP"))
    ap.add_argument("--keys", default=os.environ.get("HWDER_KEYS"))
    ap.add_argument("--update", type=Path, help="your own game update (.nsp or .nsz, 1.0.1); without it the base game is built")
    ap.add_argument("--cache", type=Path, default=default_cache())
    ap.add_argument("--jobs", type=int, default=max(1, min(8, os.cpu_count() or 1)))
    ap.add_argument("--check", action="store_true", help="check installed prerequisites only")
    ap.add_argument("--launch", action="store_true", help="launch only after successful verification/build")
    a = ap.parse_args()
    if a.jobs < 1:
        ap.error("--jobs must be at least 1")
    try:
        toolchain = check_prerequisites(ROOT)
        if a.check:
            return 0
        if not a.nsp or not a.keys:
            raise ValueError("Supply --nsp and --keys, or set HWDER_NSP and HWDER_KEYS")
        nsp, keys, cache = Path(a.nsp).resolve(), Path(a.keys).resolve(), a.cache.resolve()
        with setup_lock(cache):
            exe = prepare(ROOT, cache, nsp, keys, a.jobs, toolchain,
                          **({"update": a.update.resolve()} if a.update else {}))
            record_update_path(exe, read_json(cache / "last-success.json").get("update_dir"))
            if a.launch:
                env = dict(os.environ, HWDER_NSP=str(nsp), HWDER_KEYS=str(keys))
                env.pop("HWDER_EXEFS", None)
                env.pop("HWDER_ROMFS", None)
                # Start while setup lock is held so another setup cannot replace the exe first.
                subprocess.Popen([str(exe)], cwd=exe.parent, env=env)
        return 0
    except KeyboardInterrupt:
        log("interrupted; rerun to resume verified stages and partial native compilation")
        return 130
    except (ValueError, OSError, RuntimeError, subprocess.SubprocessError, struct.error) as e:
        log(f"failed: {e}")
        log("last successful executable, saves and caches retained; fix the error and rerun")
        return 1


if __name__ == "__main__":
    sys.exit(main())
