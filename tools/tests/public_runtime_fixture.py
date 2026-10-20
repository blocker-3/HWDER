"""Public link-only guest fixture. No real modules, game input, or ready certificate.

Only these source-declared guest entry points are replaced. All host runtime/SDL
symbols must resolve normally; new guest dependencies require explicit review.
The resulting executable must never be run or distributed.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/"tools"))
OWNER="HWDER-public-runtime-link-NOT-A-GAME-v1"
GUESTS=tuple("f_"+name for name in (
    "10001763bc_orig","1000176490_orig","10001766a4_orig","100017672c_orig",
    "10001767e8_orig","10001dee94_orig","10002acb54_orig","10002d96b4_orig",
    "10002d9878_orig","10003878d0_orig","1000387f9c","10003d0a74_orig",
    "10003d2da4_orig","10003d45cc_orig","10003d7028_orig","100045de0c_orig",
    "100051e67c","100051e9c4_orig","100051ea20_orig","100052ab10_orig",
    "100052f6bc_orig","100057cb78","100057cee0_orig"))


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def public_bindings(root):
    declared={};defined=set()
    for path in sorted((root/"runtime/src").rglob("*.cpp")):
        source=path.read_text(encoding="utf-8")
        for name,end in re.findall(r'extern\s+"C"\s+void\s+(f_[0-9a-f]{10}(?:_orig)?)\s*\(Ctx\*\s*\w+\)\s*([;{])',source):
            if end=="{":defined.add(name)
            else:declared.setdefault(name,set()).add(path.relative_to(root).as_posix())
        for address in re.findall(r'^PROBE\(([0-9a-f]{10}),',source,re.M):
            defined.add("f_"+address)
            declared.setdefault("f_"+address+"_orig",set()).add(path.relative_to(root).as_posix())
    observed={name:sorted(files) for name,files in declared.items() if name not in defined}
    if set(observed)!=set(GUESTS):
        raise ValueError("Public guest-ID allowlist changed; review required")
    return observed


def directory(path):
    from managed_tools import owned_path
    return owned_path(path,ROOT/"build/private-toolchain-data/probes")


def create(path):
    path=directory(path)
    bindings=public_bindings(ROOT)
    path.mkdir(parents=True,exist_ok=False)
    (path/"sources.cmake").write_text(
        'if(NOT HWDER_PUBLIC_RUNTIME_LINK_ONLY)\n  message(FATAL_ERROR "Public link fixture is NOT game code")\nendif()\n'
        'set(RECOMP_SOURCES "${GEN_DIR}/rc_public_link_only.c")\n',encoding="utf-8")
    (path/"rc_public_link_only.c").write_text('#include "hwder/cpu.h"\n'+
        '\n'.join('void '+name+'(Ctx* c) { (void)c; }' for name in GUESTS)+'\n',encoding="utf-8")
    (path/"functable.c").write_text('#include "hwder/dispatch.h"\n'
        'const HwFuncEntry hw_func_table[1]={{0,0}};\nconst unsigned hw_func_count=0;\n'
        'const unsigned long long hw_resumable[1]={0};\nconst unsigned hw_resumable_count=0;\n',encoding="utf-8")
    (path/"module_info.c").write_text('#include "hwder/module.h"\n'
        'const HwModuleInfo hw_modules[1]={{0,0,0,0,0}};\nconst unsigned hw_module_count=0;\n',encoding="utf-8")
    record={"owner":OWNER,"guest_bindings":bindings,"never_execute":True,
            "source_sha256":{name:sha(ROOT/name) for name in sorted({p for files in bindings.values() for p in files})},
            "files":{p.name:sha(p) for p in path.iterdir()}}
    (path/"public-link-fixture.json").write_text(json.dumps(record,indent=2),encoding="utf-8")
    return record


def verify(path):
    path=directory(path)
    record=json.loads((path/"public-link-fixture.json").read_text())
    if record.get("owner")!=OWNER or record.get("never_execute") is not True or record.get("guest_bindings")!=public_bindings(ROOT):
        raise ValueError("Not the reviewed public link fixture")
    if set(record.get("files",{}))!={"sources.cmake","rc_public_link_only.c","functable.c","module_info.c"}:
        raise ValueError("Unexpected fixture source set")
    if set(p.name for p in path.iterdir())!=set(record["files"])|{"public-link-fixture.json"}:
        raise ValueError("Extra fixture files")
    if any(sha(path/name)!=digest for name,digest in record["files"].items()) or any(sha(ROOT/name)!=digest for name,digest in record["source_sha256"].items()):
        raise ValueError("Fixture/source identity changed")
    return record


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output",required=True,type=Path)
    args=parser.parse_args()
    print(json.dumps(create(args.output),indent=2))
