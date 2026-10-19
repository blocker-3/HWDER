#!/usr/bin/env python3
"""Package a source/setup kit. Game-linked exe/PDB/generated C are PRIVATE outputs.

    python tools/make_dist.py [--out dist] [--name HWDER-setup-<date>]

No compilers, Python wheels, game content, keys or private caches are bundled.
"""
import argparse
import datetime
import hashlib
import json
from pathlib import Path, PurePosixPath
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parent.parent
TOP_FILES = {"CMakeLists.txt", "LICENSE", "README.md", "play-local.bat"}
SUBMODULES = ("SDL", "imgui", "spirv-headers", "vulkan-headers")
SOURCE_SUFFIXES = {".py", ".txt", ".json", ".cmake", ".c", ".cpp", ".h", ".hpp", ".inc", ".md", ".comp"}
THIRD_PARTY_SOURCE_SUFFIXES = SOURCE_SUFFIXES | {".in", ".def", ".rc", ".s", ".asm", ".m", ".mm",
                                              ".metal", ".glsl", ".vert", ".frag", ".xml", ".natvis",
                                              ".ac", ".am", ".sh", ".bat", ".ps1", ".cmakein", ".yml"}
PRIVATE_PARTS = {".git", "generated", "data", "build", "dist", "cache", "runs", "save", "exefs", "romfs",
                 "__pycache__", "legacy", "shots"}
PRIVATE_SUFFIXES = {".exe", ".dll", ".pdb", ".obj", ".lib", ".a", ".bin", ".nsp", ".xci", ".nca",
                    ".nso", ".keys", ".tik", ".pyc", ".log", ".png", ".gif", ".jpg", ".zip"}


def tracked(root):
    r = subprocess.run(["git", "-C", str(root), "ls-files", "-z"], capture_output=True, check=True)
    return [n.decode("utf-8") for n in r.stdout.split(b"\0") if n]


def safe_member(name):
    path = PurePosixPath(name)
    return (bool(path.parts) and not path.is_absolute() and ".." not in path.parts
            and not any(":" in p or "\\" in p for p in path.parts)
            and not any(p.lower() in PRIVATE_PARTS for p in path.parts)
            and path.suffix.lower() not in PRIVATE_SUFFIXES)


def release_files(root, list_tracked=tracked):
    """Explicit project roots; third-party tracked source only, with their license files."""
    root = Path(root)
    files = {}
    for name in list_tracked(root):
        p = PurePosixPath(name)
        allowed = name in TOP_FILES or (p.parts[0] in ("runtime", "tools", "docs")
                                       and p.suffix.lower() in SOURCE_SUFFIXES)
        if not allowed:
            continue
        if not safe_member(name):
            raise ValueError(f"Private/disallowed file in project release input: {name}")
        files[name] = root / name
    for name in TOP_FILES:
        if name not in files:
            raise ValueError(f"Required tracked setup file missing: {name}; commit the coherent source before packaging")
    for sub in SUBMODULES:
        directory = root / "third_party" / sub
        if not (directory / ".git").exists():
            raise ValueError(f"Initialize pinned submodule first: {directory}")
        for name in list_tracked(directory):
            relative = f"third_party/{sub}/{name}"
            # Upstream examples can contain prebuilt libraries/assets; never copy these.
            leaf = PurePosixPath(name)
            is_source = (leaf.suffix.lower() in THIRD_PARTY_SOURCE_SUFFIXES
                         or leaf.name.lower().startswith(("license", "copying", "notice"))
                         or leaf.name in ("configure", "Makefile"))
            if not is_source or not safe_member(relative):
                continue
            files[relative] = directory / name
        if not any(PurePosixPath(n).name.lower().startswith(("license", "copying"))
                   for n in files if n.startswith(f"third_party/{sub}/")):
            raise ValueError(f"Missing third-party license notice: {sub}")
    for name, path in files.items():
        if not path.is_file() or path.is_symlink():
            raise ValueError(f"Non-regular release input: {name}")
    return files


def package(root, out, name, list_tracked=tracked):
    if not name or name in (".", "..") or not all(c.isalnum() or c in "-_." for c in name):
        raise ValueError("--name must be a simple directory name")
    files = release_files(root, list_tracked)
    manifest = {n: hashlib.sha256(p.read_bytes()).hexdigest() for n, p in sorted(files.items())}
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    target = out / (name + ".zip")
    tmp = target.with_suffix(".zip.partial")
    try:
        with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
            for relative, source in sorted(files.items()):
                z.write(source, f"{name}/{relative}")
            z.writestr(f"{name}/source-manifest.json", json.dumps(manifest, indent=2, sort_keys=True))
        with zipfile.ZipFile(tmp) as z:
            expected = {f"{name}/{p}" for p in files} | {f"{name}/source-manifest.json"}
            if set(z.namelist()) != expected or any(not safe_member(n) for n in z.namelist()) or z.testzip():
                raise ValueError("Release archive verification failed")
            for relative, sha in manifest.items():
                if hashlib.sha256(z.read(f"{name}/{relative}")).hexdigest() != sha:
                    raise ValueError("Source changed while packaging; retry")
        tmp.replace(target)
    finally:
        tmp.unlink(missing_ok=True)
    return target


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", default=ROOT / "dist", type=Path)
    ap.add_argument("--name", default="HWDER-setup-" + datetime.date.today().isoformat())
    a = ap.parse_args()
    try:
        target = package(ROOT, a.out, a.name)
    except (OSError, ValueError, subprocess.SubprocessError) as e:
        sys.exit(str(e))
    print(f"Source/setup kit verified: {target} ({target.stat().st_size / 1e6:.1f} MB)")
    print("No linked game executable, generated C, keys, game data, private shader cache or toolchain bundled.")


if __name__ == "__main__":
    main()
