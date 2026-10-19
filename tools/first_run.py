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
    cache.mkdir(parents=True, exist_ok=True)
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


def check_prerequisites(root):
    if os.name != "nt" or sys.maxsize <= 2**32 or sys.version_info < (3, 10):
        raise ValueError("Setup requires Windows x64 and Python 3.10+ x64")
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
    import msvc
    from build import vs_tool
    env = msvc.vs_env()
    tools = {"clang-cl": shutil.which("clang-cl.exe", path=env["PATH"]),
             "cmake": vs_tool(os.path.join("CMake", "bin"), "cmake.exe", env),
             "ninja": vs_tool("Ninja", "ninja.exe", env)}
    if not tools["clang-cl"]:
        raise ValueError("Install Visual Studio's C++ Clang tools for Windows component (clang-cl)")
    for variable, relative in (("INCLUDE", "Windows.h"), ("LIB", "kernel32.lib"), ("LIB", "libcmt.lib")):
        if not any((Path(p) / relative).is_file() for p in env.get(variable, "").split(";") if p):
            raise ValueError(f"Windows SDK / MSVC x64 prerequisites incomplete: missing {relative}")
    for name, path in tools.items():
        r = subprocess.run([path, "--version"], env=env, capture_output=True, text=True, check=True)
        versions[name] = {"path": path, "version": r.stdout.strip()}
    match = re.search(r"cmake version (\d+)\.(\d+)", versions["cmake"]["version"])
    if not match or tuple(map(int, match.groups())) < (3, 24):
        raise ValueError("CMake 3.24+ required")
    versions.update({k: env.get(k) for k in ("VCToolsVersion", "WindowsSDKVersion", "WindowsSdkDir")})
    # Compiler/linker overrides and search paths affect output even when versions match.
    # Retain only their digest, never arbitrary environment variable contents.
    versions["compiler_environment_sha256"] = digest({
        k: env.get(k) for k in ("INCLUDE", "LIB", "LIBPATH", "CL", "_CL_", "LINK", "_LINK_",
                              "CFLAGS", "CXXFLAGS", "LDFLAGS")})
    versions["python"] = sys.version
    log("prerequisites verified: clang-cl, MSVC x64 libraries, Windows SDK, CMake, Ninja, cryptography, lz4")
    return versions


def run_command(argv, cwd):
    subprocess.run([str(a) for a in argv], cwd=cwd, check=True)


def prepare(root, cache, nsp, keys, jobs, toolchain, run=run_command, read_modules=None):
    """Injectable stage boundary permits meaningful synthetic orchestration/failure tests."""
    from local_game import read_program, supported_profile
    root, cache, nsp, keys = map(Path, (root, cache, nsp, keys))
    if not nsp.is_file() or nsp.suffix.lower() != ".nsp" or not keys.is_file():
        raise ValueError("Supply an existing .nsp and your own prod.keys via --nsp/--keys or HWDER_NSP/HWDER_KEYS")
    generation_inputs, build_inputs = source_fingerprints(root)
    before = (nsp.stat().st_size, nsp.stat().st_mtime_ns)
    keys_before = (keys.stat().st_size, keys.stat().st_mtime_ns)
    log("validating NSP, own keys, title, module set and supported-version SHA-256")
    modules = (read_modules or read_program)(nsp, keys)
    profile = supported_profile(modules, root / "tools" / "supported_games.json")
    log(f"supported version: {profile}; hashing complete NSP for cache identity")
    game_hash = file_hash(nsp)
    if before != (nsp.stat().st_size, nsp.stat().st_mtime_ns):
        raise ValueError("Game input changed during validation; retry after the dump finishes copying")
    module_hashes = {name: hashlib.sha256(data).hexdigest() for name, data in modules.items()}
    exefs_key = digest({"schema": SCHEMA, "game": game_hash, "modules": module_hashes})
    def extract(out):
        for name, data in modules.items():
            (out / name).write_bytes(data)
    exefs = completed_stage(cache, "exefs", exefs_key, extract)
    exefs_files = tree_hashes(exefs)
    gen_key = digest({"exefs": exefs_key, "generator": generation_inputs, "python": sys.version,
                      "lz4": toolchain.get("lz4")})
    def generate(out):
        args = [sys.executable, "-m", "recomp.gen_main", "--exefs", exefs, "--out", out]
        # Explicit entries remove gen_main's implicit dependency on the working directory.
        args += ["--extra", root / "tools" / "recomp" / "extra_entries.txt"]
        if (root / "data" / "extra_entries.txt").is_file():
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
    exe = bdir / "hwder.exe"
    marker = read_json(bdir / "complete.json")
    if (marker.get("schema") != SCHEMA or marker.get("key") != build_key or not exe.is_file()
            or marker.get("exe_sha256") != file_hash(exe)):
        log("build: compiling complete native program via tools/build.py (shared machine build lock)")
        (bdir / "complete.json").unlink(missing_ok=True)
        # Ninja can consider a newer damaged/stale executable up to date. Force its relink
        # whenever the output lacks a valid completion certificate; preserve partial objects.
        exe.unlink(missing_ok=True)
        try:
            run([sys.executable, root / "tools" / "build.py", "--jobs", str(jobs),
                 "--build-dir", bdir, "--generated-dir", generated], root)
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
        if not exe.is_file():
            raise ValueError("Build succeeded without a Windows executable")
        with exe.open("rb") as f:
            magic = f.read(2)
        if magic != b"MZ":
            raise ValueError("Build succeeded without a Windows executable")
        if source_fingerprints(root) != (generation_inputs, build_inputs):
            raise ValueError("Build inputs changed during setup; rerun to build the new source version")
        if before != (nsp.stat().st_size, nsp.stat().st_mtime_ns):
            raise ValueError("Game input changed during build; rerun")
        marker = {"schema": SCHEMA, "key": build_key, "exe_sha256": file_hash(exe),
                  "game_sha256": game_hash, "profile": profile}
        atomic_json(bdir / "complete.json", marker)
    else:
        log("build: verified complete cached executable")
    if source_fingerprints(root) != (generation_inputs, build_inputs):
        raise ValueError("Source inputs changed during setup; rerun")
    if tree_hashes(exefs) != exefs_files or tree_hashes(generated) != generated_files:
        raise ValueError("Cached build inputs changed during setup; rerun")
    if (before != (nsp.stat().st_size, nsp.stat().st_mtime_ns)
            or keys_before != (keys.stat().st_size, keys.stat().st_mtime_ns)):
        raise ValueError("Game or keys input changed during setup; rerun")
    # Stable play directory retains user settings, saves and private caches across source updates.
    play = cache / "play"
    play.mkdir(parents=True, exist_ok=True)
    target = play / "hwder.exe"
    if not target.is_file() or file_hash(target) != marker["exe_sha256"]:
        tmp = play / ("hwder-" + uuid.uuid4().hex + ".tmp")
        previous = play / ("hwder-previous-" + uuid.uuid4().hex + ".tmp")
        published = False
        try:
            shutil.copyfile(exe, tmp)
            if file_hash(tmp) != marker["exe_sha256"]:
                raise ValueError("Executable publication checksum mismatch")
            if target.is_file():
                try:
                    os.link(target, previous)
                except OSError:
                    # A cache may live on a filesystem without hard-link support.
                    shutil.copyfile(target, previous)
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


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--nsp", default=os.environ.get("HWDER_NSP"))
    ap.add_argument("--keys", default=os.environ.get("HWDER_KEYS"))
    ap.add_argument("--cache", type=Path, default=Path(os.environ.get("LOCALAPPDATA", str(Path.home()))) / "hwder" / "local-build-v1")
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
            exe = prepare(ROOT, cache, nsp, keys, a.jobs, toolchain)
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
