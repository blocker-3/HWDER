#!/usr/bin/env python3
"""Build the player setup zip from a clean, tagged checkout in one command.

    python tools/release_kit.py [--work build/release-kit] [--out dist] [--tools-data DIR]

Steps: the verified source kit (make_dist), the pinned private toolset (managed_tools, public
downloads checked by SHA-256), the code-free HWDER Setup.exe (build.py --setup-ui), its
bootstrap certificate, then make_setup_dist and a final content check. The same command runs
on GitHub Actions (.github/workflows/release.yml) and on a maintainer PC. It never reads a
game dump, keys, generated C or a save; Prepare builds the game on the player's own machine.
"""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import subprocess
import sys
import zipfile

TOOLS = Path(__file__).resolve().parent
ROOT = TOOLS.parent
sys.path.insert(0, str(TOOLS))
import make_dist  # noqa: E402
import make_setup_dist  # noqa: E402
import managed_tools  # noqa: E402
import pe_dependencies  # noqa: E402

# Must match make_setup_dist.assemble's certified source set.
CERTIFIED_SOURCES = ("CMakeLists.txt", "tools/build.py", "tools/setup_ui/bootstrap.cpp", "tools/setup_ui/steam_shortcut.h")
FORBIDDEN_PARTS = {"generated", "data", "save", "exefs", "romfs", "runs", "shots"}
FORBIDDEN_SUFFIXES = {".nsp", ".xci", ".nca", ".nso", ".keys", ".tik", ".bin", ".pdb", ".dll", ".png", ".jpg"}
FORBIDDEN_NAMES = {"functable.c", "module_info.c", "pipeline_warmup.bin", "zmha.bin", "zmhasd.bin", "prod.keys", "title.keys"}
MAX_MEMBER_BYTES = 32_000_000  # the largest pinned header (vulkan_structs.hpp) is ~10 MB


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def git(*args):
    return subprocess.check_output(["git", "-C", str(ROOT), *args], text=True).strip()


def certify(out, toolset):
    """Write bootstrap-certificate.json for the Setup.exe in `out` (the old runs/ certify-rN-ui.py)."""
    llvm = next(i for i in managed_tools.manifest()["artifacts"] if i["id"] == "llvm")
    lic = "llvm/" + llvm["archive_root"] + "/"
    notices = {"runtime-notices/LLVM-LICENSE.TXT": lic+"LICENSE.TXT",
               "runtime-notices/COPYING.MinGW-w64-runtime.txt": lic+"x86_64-w64-mingw32/share/mingw32/COPYING.MinGW-w64-runtime.txt"}
    maptext = (out/"setup-ui-link.map").read_text(errors="replace")
    if not ("libmingw32" in maptext and "__cxa_" in maptext and "_Unwind" in maptext):
        raise ValueError("Setup link map lacks the expected static MinGW/C++ runtime")
    if "pthread" in maptext.lower():
        raise ValueError("Setup links winpthreads; review its notice before packaging")
    # The toolset certificate holds a stamp, not per-file hashes: compare each notice with the pinned LLVM archive.
    archive = toolset.parent/"downloads"/(llvm["sha256"] + ".zip")
    if sha(archive) != llvm["sha256"]:
        raise ValueError("Cached LLVM archive does not match its pinned SHA-256")
    with zipfile.ZipFile(archive) as pinned:
        for destination, source in notices.items():
            data = managed_tools.filesystem_path(toolset/"output"/source).read_bytes()
            if data != pinned.read(source.removeprefix("llvm/")):
                raise ValueError("Toolset notice changed: " + source)
            target = out/destination
            target.parent.mkdir(exist_ok=True)
            target.write_bytes(data)
    exe = out/"HWDER Setup.exe"
    pe = pe_dependencies.inspect(exe)
    extra = [n for n in pe["imports"]+pe["delay_imports"]
             if not n.startswith(("api-ms-win-", "ext-ms-win-")) and n.removesuffix(".dll") not in pe_dependencies.OS_DLLS]
    if extra:
        raise ValueError("Setup imports non-system DLLs: " + ", ".join(extra))
    link = next(s for s in (out/"build.ninja").read_text().splitlines() if s.startswith("build HWDER$ Setup.exe:"))
    if "bootstrap.cpp.obj" not in link or any(s in link for s in ("hwder_game", "SDL", "rc_")):
        raise ValueError("Setup link input is not the code-free bootstrap alone")
    cert = {"kind": "HWDER-codefree-setup-v1", "compiler_abi": "llvm-mingw-ucrt-static-cxx-v1",
            "source_commit": git("rev-parse", "HEAD"), "exe_sha256": sha(exe),
            "source_sha256": {n: sha(ROOT/n) for n in CERTIFIED_SOURCES},
            "compiled_object_sha256": sha(out/"CMakeFiles/hwder_setup.dir/tools/setup_ui/bootstrap.cpp.obj"),
            "no_game_linkage": True, "system_imports_only": True, "pe": pe,
            "runtime_notices": {n: sha(out/n) for n in notices}, "notice_source_paths": notices,
            "link_map_sha256": sha(out/"setup-ui-link.map"), "build_ninja_sha256": sha(out/"build.ninja"),
            "toolset_certificate_sha256": sha(toolset/"complete.json"), "link_input": link,
            "linked_runtime_evidence": {"mingw": True, "llvm_cxx_abi_unwind": True, "winpthreads": False},
            "scope": "Private-compiler-built game-free setup only; no game compilation or clean-machine gameplay claim"}
    path = out/"bootstrap-certificate.json"
    path.write_text(json.dumps(cert, indent=2), encoding="utf-8")
    return path


def check_zip(target):
    """Independent gate on the finished zip: one exe, no game, key, generated or personal file."""
    with zipfile.ZipFile(target) as z:
        names = z.namelist()
        bad = []
        for name in names:
            p = PurePosixPath(name)
            parts = {x.lower() for x in p.parts}
            if (parts & FORBIDDEN_PARTS or p.suffix.lower() in FORBIDDEN_SUFFIXES
                    or p.name.lower() in FORBIDDEN_NAMES
                    or (p.name.startswith("rc_") and p.suffix == ".c") or (p.suffix.lower() == ".exe" and name != "HWDER Setup.exe")):
                bad.append(name)
            elif z.getinfo(name).file_size > MAX_MEMBER_BYTES:
                bad.append(name + " (over the size budget)")
        if bad:
            raise ValueError("Disallowed setup zip members: " + ", ".join(bad[:20]))
        manifest = json.loads(z.read("setup-manifest.json"))
    return {"entries": len(names), "baseline": manifest["baseline"],
            "exe_sha256": manifest["bootstrap"]["exe_sha256"], "source_commit": manifest["bootstrap"]["source_commit"]}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--work", type=Path, default=ROOT/"build/release-kit", help="scratch folder (recreated parts only)")
    ap.add_argument("--out", type=Path, default=ROOT/"dist")
    ap.add_argument("--tools-data", type=Path, help="managed toolset data folder (default: <work>/tools-data); reuses verified downloads")
    ap.add_argument("--name", help="zip base name (default: HWDER-setup-<git describe>)")
    ap.add_argument("--allow-untagged", action="store_true", help="package a commit that has no exact v* tag (test builds)")
    a = ap.parse_args()
    if git("status", "--porcelain", "--ignore-submodules=none"):
        ap.error("Commit coherent source before packaging (tree or a submodule is dirty)")
    version = git("describe", "--tags", "--match", "v*", "--always")
    if not a.allow_untagged and not git("tag", "--points-at", "HEAD", "--list", "v*"):
        ap.error(f"HEAD has no v* tag ({version}); tag first so the game reports the release, or pass --allow-untagged")
    name = a.name or "HWDER-setup-" + version
    work = a.work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    target = a.out.resolve()/(name + ".zip")
    if target.exists():
        ap.error(f"{target} exists; release zips are never replaced")

    print("1/5 source kit", flush=True)
    source_kit = work/"HWDER-source.zip"
    source_kit.unlink(missing_ok=True)
    make_dist.package(ROOT, work, "HWDER-source")

    print("2/5 pinned toolset (downloads are SHA-256 checked)", flush=True)
    toolset = managed_tools.install((a.tools_data or work/"tools-data").resolve(), ROOT,
                                    phase=lambda kind, **f: print("   ", kind, f.get("artifact", ""), flush=True)
                                    if kind in ("archive_check", "imports_done", "installed_ready") else None)

    print("3/5 HWDER Setup.exe", flush=True)
    setup_dir = work/"setup-ui"
    subprocess.run([sys.executable, str(TOOLS/"build.py"), "--setup-ui", "--build-dir", str(setup_dir),
                    "--managed-toolchain", str(toolset)], check=True)

    print("4/5 bootstrap certificate", flush=True)
    certificate = certify(setup_dir, toolset)

    print("5/5 setup zip", flush=True)
    make_setup_dist.assemble(source_kit, setup_dir/"HWDER Setup.exe", certificate, ROOT, target)
    info = check_zip(target)
    info.update({"zip": target.name, "zip_bytes": target.stat().st_size, "zip_sha256": sha(target), "version": version})
    (target.parent/(name + ".json")).write_text(json.dumps(info, indent=2), encoding="utf-8")
    print(json.dumps(info, indent=2))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        sys.exit(f"release_kit.py: {error}")
