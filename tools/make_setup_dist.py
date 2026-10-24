"""Assemble a private prototype kit: one certified CODE-FREE bootstrap + verified source.

The existing make_dist private-content rules remain unchanged. Never include a game-linked EXE.
The base source kit must pass its complete per-entry manifest; current committed project source
overlays it, while pinned third-party source/notices remain from that verified source kit.
Project files (runtime/, tools/, docs/ and the top-level files) that the base kit carries but the
current source no longer tracks are dropped, so a removed file never ships again by accident.

The kit must track main: tools/kit_baseline.txt names the main commit whose runtime the kit is
expected to build, and packaging refuses when that commit is not an ancestor of the source HEAD.
Bump the file whenever the kit branch merges a newer main (R5 shipped a kit built from a branch
that predated main's whole-block GI blending fix, so Prepare rebuilt the jagged 120 fps flashes).
"""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import zipfile

import make_dist

GNU_NOTICES={"runtime-notices/LLVM-LICENSE.TXT","runtime-notices/COPYING.MinGW-w64-runtime.txt"}
GNU_OPTIONAL_NOTICES={"runtime-notices/COPYING.winpthreads.txt"}

def digest(data):
    return hashlib.sha256(data).hexdigest()


BASELINE = "tools/kit_baseline.txt"
PROJECT_ROOTS = ("runtime", "tools", "docs")


def baseline_commit(root):
    text = (Path(root)/BASELINE).read_text(encoding="utf-8").strip()
    if not re.fullmatch(r"[0-9a-f]{40}", text):
        raise ValueError("tools/kit_baseline.txt must hold one full 40-hex main commit")
    return text


def git_contains(root, commit):
    """True when `commit` is reachable from the source checkout's HEAD."""
    result = subprocess.run(["git", "-C", str(root), "merge-base", "--is-ancestor", commit, "HEAD"],
                            capture_output=True)
    if result.returncode not in (0, 1):
        raise ValueError("Cannot verify the kit baseline: " + result.stderr.decode(errors="replace").strip())
    return result.returncode == 0


def require_baseline(root, contains=git_contains):
    commit = baseline_commit(root)
    if not contains(root, commit):
        raise ValueError("Stale kit source: HEAD does not contain the main baseline " + commit[:12]
                         + " named in " + BASELINE + "; merge main (or update the baseline) before packaging")
    return commit


def git_text(root, *args):
    return subprocess.check_output(["git", "-C", str(root), *args], text=True).strip()


def project_file(name):
    p = PurePosixPath(name)
    # Every top-level file is ours (a dropped top file such as play-local.bat must not ship again).
    return len(p.parts) == 1 or name in make_dist.TOP_FILES or name in make_dist.PUBLIC_SCRIPTS or p.parts[0] in PROJECT_ROOTS


def verified_source(path):
    with zipfile.ZipFile(path) as z:
        names = z.namelist()
        if len(names) != len(set(names)) or z.testzip():
            raise ValueError("Duplicate/damaged source archive")
        manifests = [n for n in names if n.count("/") == 1 and n.endswith("/source-manifest.json")]
        if len(manifests) != 1:
            raise ValueError("Expected one complete source manifest")
        prefix = manifests[0].rsplit("/", 1)[0] + "/"
        manifest = json.loads(z.read(manifests[0]))
        if set(names) != {prefix+n for n in manifest} | {manifests[0]}:
            raise ValueError("Unexpected source archive file set")
        result = {}
        for name, sha in manifest.items():
            if not make_dist.safe_member(name):
                raise ValueError("Private/disallowed source archive member: " + name)
            data = z.read(prefix+name)
            if digest(data) != sha:
                raise ValueError("Source hash mismatch")
            result[name] = data
        for sub in make_dist.SUBMODULES:
            if not any(n.startswith("third_party/"+sub+"/") and PurePosixPath(n).name.lower().startswith(("license","copying")) for n in result):
                raise ValueError("Missing pinned dependency notice: "+sub)
        return result


def assemble(source_kit, bootstrap, certificate, root, target, tracked=make_dist.tracked, contains=git_contains):
    root, bootstrap, target = Path(root), Path(bootstrap), Path(target)
    baseline = require_baseline(root, contains)
    cert = json.loads(Path(certificate).read_text(encoding="utf-8-sig"))
    data = bootstrap.read_bytes()
    if cert.get("kind") != "HWDER-codefree-setup-v1" or cert.get("exe_sha256") != digest(data) or not data.startswith(b"MZ"):
        raise ValueError("Only the reviewed code-free bootstrap certificate is accepted")
    expected = {"CMakeLists.txt", "tools/build.py", "tools/setup_ui/bootstrap.cpp", "tools/setup_ui/steam_shortcut.h"}
    if set(cert.get("source_sha256", {})) != expected:
        raise ValueError("Incomplete bootstrap source certification")
    for name, sha in cert["source_sha256"].items():
        if digest((root/name).read_bytes()) != sha:
            raise ValueError("Bootstrap source changed after compilation")
    if not cert.get("no_game_linkage") or not cert.get("system_imports_only"):
        raise ValueError("Bootstrap link/import review missing")
    files = verified_source(source_kit)
    current = set(tracked(root))
    # Base-kit project files the current source no longer tracks must not ship again.
    for name in [n for n in files if project_file(n) and n not in current]:
        del files[name]
    # Project roots only; don't recursively sweep this checkout or its private builds/runs.
    for name in current:
        p = PurePosixPath(name)
        allowed = name in make_dist.TOP_FILES or name in make_dist.PUBLIC_SCRIPTS or (p.parts[0] in ("runtime","tools","docs") and p.suffix.lower() in make_dist.SOURCE_SUFFIXES)
        if not allowed:
            continue
        if not make_dist.safe_member(name) or (root/name).is_symlink():
            raise ValueError("Private/nonregular project overlay: "+name)
        files[name] = (root/name).read_bytes()
    # The kit has no .git, so tools/version.cmake reads the build's version stamp from here.
    files["version.txt"] = (git_text(root, "describe", "--tags", "--match", "v*", "--always") + "\n"
                            + git_text(root, "rev-parse", "--short=10", "HEAD") + "\n").encode()
    members = {"source/"+n:b for n,b in files.items()}
    members["source/source-manifest.json"] = json.dumps({n:digest(b) for n,b in files.items()},indent=2).encode()
    members["HWDER Setup.exe"] = data  # The ONLY binary exception. Never a generic *.exe allowance.
    if cert.get("compiler_abi")=="llvm-mingw-ucrt-static-cxx-v1":
        notices=cert.get("runtime_notices",{})
        if not GNU_NOTICES.issubset(notices) or set(notices)-GNU_NOTICES-GNU_OPTIONAL_NOTICES:
            raise ValueError("Missing or unexpected GNU bootstrap runtime notices")
        base=Path(certificate).resolve().parent
        if digest((base/"setup-ui-link.map").read_bytes())!=cert.get("link_map_sha256"):
            raise ValueError("Bootstrap link-map identity mismatch")
        for name,sha in notices.items():
            payload=(base/name).read_bytes()
            if digest(payload)!=sha:
                raise ValueError("Bootstrap runtime notice hash mismatch")
            members[name]=payload
    members["START HERE.txt"] = (
        "HWDER setup (Windows 10/11 x64)\n\nExtract the complete zip into a folder you will keep, then open HWDER Setup.exe.\n"
        "It is not code-signed yet: if Windows shows \"Windows protected your PC\", choose More info > Run anyway.\n\n"
        "One main button leads through the steps:\n"
        "  Next     pick your game .nsp and prod.keys, both from your own Switch and your own copy of the game.\n"
        "           They stay where they are; keys are never copied.\n"
        "  Install  downloads pinned build tools (checked by SHA-256) into your Data folder after you confirm.\n"
        "           No administrator rights or system installation.\n"
        "  Prepare  checks your dump and builds the game on this PC. Several minutes and several GB of disk.\n"
        "  Play     starts the game. Create desktop shortcut and Add to Steam start it later without Setup.\n\n"
        "No game, generated C, key file, Python package or compiler is bundled. Your Data folder keeps\n"
        "saves and settings separately from this kit. Keep your .nsp and prod.keys where you picked them.\n"
    ).encode()
    manifest = {n:digest(b) for n,b in members.items()}
    members["setup-manifest.json"] = json.dumps({"schema":1,"bootstrap":cert,"baseline":baseline,"files":manifest},indent=2).encode()
    target.parent.mkdir(parents=True,exist_ok=True)
    # Exclusive creation: do not replace a previous unique release artifact.
    with target.open("xb") as stream, zipfile.ZipFile(stream,"w",zipfile.ZIP_DEFLATED,compresslevel=6) as z:
        for name,data in sorted(members.items()):
            z.writestr(name,data)
    with zipfile.ZipFile(target) as z:
        if set(z.namelist()) != set(members) or z.testzip():
            raise ValueError("Setup archive verification failed")
        for name, data in members.items():
            if digest(z.read(name)) != digest(data):
                raise ValueError("Setup archive byte mismatch")
        if [n for n in z.namelist() if PurePosixPath(n).suffix.lower()==".exe"] != ["HWDER Setup.exe"]:
            raise ValueError("Unexpected executable in setup archive")
    return target


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-kit",type=Path,required=True)
    parser.add_argument("--bootstrap",type=Path,required=True)
    parser.add_argument("--certificate",type=Path,required=True)
    parser.add_argument("--out",type=Path,required=True)
    args=parser.parse_args()
    root=Path(__file__).resolve().parent.parent
    if subprocess.check_output(["git","-C",str(root),"status","--porcelain"]).strip():
        parser.error("Commit coherent source before packaging")
    print(assemble(args.source_kit,args.bootstrap,args.certificate,root,args.out))


if __name__=="__main__":
    main()
