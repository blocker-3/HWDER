#!/usr/bin/env python3
"""Build the Linux release, HWDER-linux-x86_64-<version>.tar.gz, in one command.

    python3 tools/linux/package_release.py [--work build/linux-release] [--out dist] [--allow-untagged]

Runs on Ubuntu 22.04 (glibc 2.35) with tools/linux/build-packages.txt installed: the GitHub workflow's runner, or a
podman/distrobox container of ubuntu:22.04. Steps: the prebuilt static libraries (build_deps.sh, sources checked by
SHA-256), the HWDER Setup window (setup_ui/), the project source (make_dist's file list), then the tarball and a
content check. The tarball holds no game code, keys, game files or saves: Setup builds the game on the player's
machine with the pinned toolchain it downloads (tools/linux/toolchain.json).
"""
import argparse
import gzip
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys
import tarfile

LINUX = Path(__file__).resolve().parent
ROOT = LINUX.parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import make_dist  # noqa: E402

PLATFORM = "linux-x86_64"
EXTRA_FILES = ("tools/linux/build_deps.sh", "tools/linux/build-packages.txt")  # .sh/.txt outside make_dist's set
FORBIDDEN_PARTS = {"generated", "data", "save", "exefs", "romfs", "runs", "shots", "build", "cache"}
FORBIDDEN_SUFFIXES = {".nsp", ".nsz", ".xci", ".nca", ".nso", ".keys", ".tik", ".bin", ".pdb", ".dll", ".exe", ".so"}
FORBIDDEN_NAMES = {"functable.c", "module_info.c", "pipeline_warmup.bin", "zmha.bin", "zmhasd.bin", "prod.keys",
                   "title.keys", "hwder", "hwder_settings.ini"}
EXECUTABLES = {"hwder-setup", "setup/hwder-setup-ui"}
MAX_GLIBC = (2, 35)
ALLOWED_NEEDED = {"libc.so.6", "libm.so.6", "libpthread.so.0", "libdl.so.2", "librt.so.1", "ld-linux-x86-64.so.2"}


def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def git(*args):
    return subprocess.check_output(["git", "-C", str(ROOT), *args], text=True).strip()


def run(command, **kwargs):
    print("   $", " ".join(map(str, command)), flush=True)
    subprocess.run([str(c) for c in command], check=True, **kwargs)


def build_deps(work):
    """Reuses an earlier deps build only when its script and SDL source are unchanged."""
    deps = work / "linux-deps"
    stamp = {"script": sha(LINUX / "build_deps.sh"), "sdl": git("-C", "third_party/SDL", "rev-parse", "HEAD")}
    if read_json(deps / ".stamp.json") == stamp and (deps / "BUILD-INFO.json").is_file():
        print("   reusing", deps)
        return deps
    run(["sh", LINUX / "build_deps.sh", deps, work / "downloads"])
    (deps / ".stamp.json").write_text(json.dumps(stamp), encoding="utf-8")
    return deps


def read_json(path):
    try:
        return json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def build_setup_ui(work, deps):
    out = work / "setup-ui"
    run(["cmake", "-S", LINUX / "setup_ui", "-B", out, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
         f"-DHWDER_LINUX_DEPS={deps}"])
    run(["cmake", "--build", out])
    exe = out / "hwder-setup-ui"
    run(["strip", exe])
    check_portable(exe)
    return exe


def check_portable(exe):
    """Only glibc libraries at load time, no symbol newer than glibc 2.35."""
    dynamic = subprocess.check_output(["readelf", "-d", str(exe)], text=True)
    needed = set(re.findall(r"\(NEEDED\)\s+Shared library: \[([^\]]+)\]", dynamic))
    if needed - ALLOWED_NEEDED:
        raise ValueError(f"{exe.name} needs non-glibc libraries: {sorted(needed - ALLOWED_NEEDED)}")
    symbols = subprocess.check_output(["objdump", "-T", str(exe)], text=True)
    versions = {tuple(map(int, v.split("."))) for v in re.findall(r"GLIBC_(\d+(?:\.\d+)+)", symbols)}
    newest = max(versions) if versions else (0,)
    if newest > MAX_GLIBC:
        raise ValueError(f"{exe.name} needs glibc {'.'.join(map(str, newest))}, newer than 2.35")
    return {"needed": sorted(needed), "glibc": ".".join(map(str, newest))}


def collect(deps, setup_ui, version, commit):
    """Published path -> (source path or bytes, executable)."""
    files = {name: (path, False) for name, path in make_dist.release_files(ROOT).items()}
    for name in EXTRA_FILES:
        files[name] = (ROOT / name, name.endswith(".sh"))
    files["hwder-setup"] = (LINUX / "hwder-setup", True)
    files["setup/hwder-setup-ui"] = (setup_ui, True)
    files["README-LINUX.txt"] = (LINUX / "README-LINUX.txt", False)
    files["version.txt"] = (f"{version}\n{commit}\n".encode(), False)
    for path in sorted(deps.rglob("*")):
        rel = path.relative_to(deps).as_posix()
        if path.is_file() and not rel.startswith("."):
            files["linux-deps/" + rel] = (path, False)
    return files


def check_members(names):
    bad = []
    for name in names:
        p = PurePosixPath(name)
        parts = {x.lower() for x in p.parts[1:]}  # the top folder is the release name
        if (parts & FORBIDDEN_PARTS or p.suffix.lower() in FORBIDDEN_SUFFIXES or p.name.lower() in FORBIDDEN_NAMES
                or (p.name.startswith("rc_") and p.suffix == ".c")):
            bad.append(name)
    if bad:
        raise ValueError("Disallowed release members: " + ", ".join(bad[:20]))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--work", type=Path, default=ROOT / "build" / "linux-release")
    ap.add_argument("--out", type=Path, default=ROOT / "dist")
    ap.add_argument("--allow-untagged", action="store_true", help="package a commit without an exact v* tag (tests)")
    ap.add_argument("--allow-dirty", action="store_true", help="package uncommitted changes (local tests only)")
    a = ap.parse_args()
    if not sys.platform.startswith("linux"):
        ap.error("run this on Linux (Ubuntu 22.04: the GitHub runner or a container)")
    if not a.allow_dirty and git("status", "--porcelain", "--ignore-submodules=none"):
        ap.error("Commit coherent source before packaging (tree or a submodule is dirty)")
    version = git("describe", "--tags", "--match", "v*", "--always") + ("-dirty" if a.allow_dirty and git(
        "status", "--porcelain") else "")
    if not a.allow_untagged and not git("tag", "--points-at", "HEAD", "--list", "v*"):
        ap.error(f"HEAD has no v* tag ({version}); tag first, or pass --allow-untagged")
    commit = git("rev-parse", "--short=10", "HEAD")
    top = f"HWDER-{PLATFORM}-{version}"
    work, out = a.work.resolve(), a.out.resolve()
    work.mkdir(parents=True, exist_ok=True)
    out.mkdir(parents=True, exist_ok=True)
    target = out / (top + ".tar.gz")
    if target.exists():
        ap.error(f"{target} exists; releases are never replaced")

    print("1/4 prebuilt libraries (SDL3, FFmpeg H.264, libcrypto)", flush=True)
    deps = build_deps(work)
    print("2/4 HWDER Setup window", flush=True)
    setup_ui = build_setup_ui(work, deps)
    print("3/4 release files", flush=True)
    files = collect(deps, setup_ui, version, commit)
    check_members(f"{top}/{n}" for n in files)
    print("4/4 tarball", flush=True)
    manifest = {}
    for name, (source, _) in files.items():
        manifest[name] = hashlib.sha256(source).hexdigest() if isinstance(source, bytes) else sha(source)
    release = {"schema": 1, "name": top, "version": version, "commit": commit, "platform": PLATFORM,
               "files": dict(sorted(manifest.items()))}
    files["release.json"] = (json.dumps(release, indent=1).encode(), False)
    partial = target.with_name(target.name + ".partial")
    try:
        with open(partial, "wb") as raw, gzip.GzipFile(fileobj=raw, mode="wb", mtime=0, filename="") as gz, \
                tarfile.open(fileobj=gz, mode="w", format=tarfile.PAX_FORMAT) as tar:
            for name, (source, executable) in sorted(files.items()):
                data = source if isinstance(source, bytes) else Path(source).read_bytes()
                info = tarfile.TarInfo(f"{top}/{name}")
                info.size, info.mode, info.mtime = len(data), 0o755 if (executable or name in EXECUTABLES) else 0o644, 0
                tar.addfile(info, io.BytesIO(data))
        with tarfile.open(partial) as tar:
            names = tar.getnames()
            check_members(names)
            if set(names) != {f"{top}/{n}" for n in files}:
                raise ValueError("Tarball member list does not match")
            for name, digest in manifest.items():
                if hashlib.sha256(tar.extractfile(f"{top}/{name}").read()).hexdigest() != digest:
                    raise ValueError(f"{name} changed while packaging; retry")
        os.replace(partial, target)
    finally:
        if partial.exists():
            partial.unlink()
    info = {"tarball": target.name, "bytes": target.stat().st_size, "sha256": sha(target), "version": version,
            "commit": commit, "files": len(files), "setup_ui": check_portable(setup_ui),
            "deps": read_json(deps / "BUILD-INFO.json")}
    (out / (top + ".sha256")).write_text(f"{info['sha256']}  {target.name}\n", encoding="ascii")
    (out / (top + ".json")).write_text(json.dumps(info, indent=2), encoding="utf-8")
    print(json.dumps(info, indent=2))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        sys.exit(f"package_release.py: {error}")
