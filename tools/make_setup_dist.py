"""Assemble a private prototype kit: one certified CODE-FREE bootstrap + verified source.

The existing make_dist private-content rules remain unchanged. Never include a game-linked EXE.
The base source kit must pass its complete per-entry manifest; current committed project source
overlays it, while pinned third-party source/notices remain from that verified source kit.
"""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import subprocess
import zipfile

import make_dist


def digest(data):
    return hashlib.sha256(data).hexdigest()


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


def assemble(source_kit, bootstrap, certificate, root, target, tracked=make_dist.tracked):
    root, bootstrap, target = Path(root), Path(bootstrap), Path(target)
    cert = json.loads(Path(certificate).read_text(encoding="utf-8-sig"))
    data = bootstrap.read_bytes()
    if cert.get("kind") != "HWDER-codefree-setup-v1" or cert.get("exe_sha256") != digest(data) or not data.startswith(b"MZ"):
        raise ValueError("Only the reviewed code-free bootstrap certificate is accepted")
    expected = {"CMakeLists.txt", "tools/build.py", "tools/setup_ui/bootstrap.cpp"}
    if set(cert.get("source_sha256", {})) != expected:
        raise ValueError("Incomplete bootstrap source certification")
    for name, sha in cert["source_sha256"].items():
        if digest((root/name).read_bytes()) != sha:
            raise ValueError("Bootstrap source changed after compilation")
    if not cert.get("no_game_linkage") or not cert.get("system_imports_only"):
        raise ValueError("Bootstrap link/import review missing")
    files = verified_source(source_kit)
    # Project roots only; don't recursively sweep this checkout or its private builds/runs.
    for name in tracked(root):
        p = PurePosixPath(name)
        allowed = name in make_dist.TOP_FILES or (p.parts[0] in ("runtime","tools","docs") and p.suffix.lower() in make_dist.SOURCE_SUFFIXES)
        if not allowed:
            continue
        if not make_dist.safe_member(name) or (root/name).is_symlink():
            raise ValueError("Private/nonregular project overlay: "+name)
        files[name] = (root/name).read_bytes()
    members = {"source/"+n:b for n,b in files.items()}
    members["source/source-manifest.json"] = json.dumps({n:digest(b) for n,b in files.items()},indent=2).encode()
    members["HWDER Setup.exe"] = data  # The ONLY binary exception. Never a generic *.exe allowance.
    members["START HERE.txt"] = (
        "HWDER setup prototype\n\nExtract the complete kit into a permanent writable application folder, then open HWDER Setup.exe.\n"
        "The window opens without Python. It guides official Python/C++ Build Tools installation and checks requirements.\n"
        "Your own NSP and keys remain in place. No game, generated C, key file, Python package or compiler is bundled.\n"
        "Only choose Install setup components after reviewing its network/private-environment consent.\n"
        "Preparation can take minutes and several GB. Your data folder keeps saves/settings separately from this kit.\n"
        "This local unsigned prototype is not published or a claim of clean-machine support.\n"
    ).encode()
    manifest = {n:digest(b) for n,b in members.items()}
    members["setup-manifest.json"] = json.dumps({"schema":1,"bootstrap":cert,"files":manifest},indent=2).encode()
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
