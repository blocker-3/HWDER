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
# Reviewed 2026-10-28: 9730d33 named the hooks (HW_HOOK/HW_GUEST, tools/recomp/symbols.txt) and dropped the
# pass-through hooks, d93c83d removed the 120 fps research hooks, df6c056 added the stage-prop LOD hook.
GUESTS=tuple("f_"+name for name in (
    "10002acb54_orig","1000387f9c","10003cf0e8","10003d45cc_orig",
    "100044660c_orig","10004555d0","100051e67c","100051e9c4_orig",
    "100051ea20","100052f6bc","100057cb78","100057cee0_orig"))


def named_symbols(root):
    from recomp.gen_main import read_symbols
    return {name:(address,is_func,hook) for address,name,is_func,hook in read_symbols(str(root/"tools/recomp/symbols.txt"))}


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def public_bindings(root):
    declared={};defined=set();symbols=None
    for path in sorted((root/"runtime/src").rglob("*.cpp")):
        source=path.read_text(encoding="utf-8")
        # game/hook.h: HW_HOOK(Name) defines f_<addr> and needs the generated f_<addr>_orig; HW_GUEST(Name) needs f_<addr>.
        for kind,name in re.findall(r'^\s*(HW_GUEST|HW_HOOK)\((\w+)\)',source,re.M):
            symbols=symbols if symbols is not None else named_symbols(root)
            if name not in symbols:
                raise ValueError("Public guest-ID allowlist changed; review required ("+name+" is not in symbols.txt)")
            guest="f_%010x"%symbols[name][0]
            if kind=="HW_HOOK":
                defined.add(guest);guest+="_orig"
            declared.setdefault(guest,set()).add(path.relative_to(root).as_posix())
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


def symbols_header():
    """generated/symbols.h as gen_main.py writes it, from the public tools/recomp/symbols.txt (names only, no code)."""
    lines=["#pragma once",'#include "hwder/cpu.h"','#ifdef __cplusplus','extern "C" {',"#endif"]
    for name,(address,is_func,hook) in named_symbols(ROOT).items():
        guest="f_%010x"%address
        lines.append("#define HW_ADDR_%s 0x%xULL"%(name,address))
        if is_func:
            lines+=["#define f_%s %s"%(name,guest),"void %s(Ctx* c);"%guest]
            if hook:
                lines+=["#define f_%s_orig %s_orig"%(name,guest),"void %s_orig(Ctx* c);"%guest]
    return "\n".join(lines+["#ifdef __cplusplus","}","#endif",""])


def payloads():
    return {"symbols.h":symbols_header(),
        "sources.cmake":
        'if(NOT HWDER_PUBLIC_RUNTIME_LINK_ONLY)\n  message(FATAL_ERROR "Public link fixture is NOT game code")\nendif()\n'
        'set(RECOMP_SOURCES "${GEN_DIR}/rc_public_link_only.c")\n',
        "rc_public_link_only.c":'#include "hwder/cpu.h"\n'+
        '\n'.join('void '+name+'(Ctx* c) { (void)c; }' for name in GUESTS)+'\n',
        "functable.c":'#include "hwder/dispatch.h"\n'
        'const HwFuncEntry hw_func_table[1]={{0,0}};\nconst unsigned hw_func_count=0;\n'
        'const unsigned long long hw_resumable[1]={0};\nconst unsigned hw_resumable_count=0;\n'
        'const HwSymbol hw_symbols[1]={{0,0}};\nconst unsigned hw_symbol_count=0;\n',
        "module_info.c":'#include "hwder/module.h"\n'
        'const HwModuleInfo hw_modules[1]={{0,0,0,0,0}};\nconst unsigned hw_module_count=0;\n'}


def create(path):
    path=directory(path)
    bindings=public_bindings(ROOT)
    path.mkdir(parents=True,exist_ok=False)
    for name,text in payloads().items():
        (path/name).write_text(text,encoding="utf-8")
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
    if set(record.get("files",{}))!={"symbols.h","sources.cmake","rc_public_link_only.c","functable.c","module_info.c"}:
        raise ValueError("Unexpected fixture source set")
    if set(p.name for p in path.iterdir())!=set(record["files"])|{"public-link-fixture.json"}:
        raise ValueError("Extra fixture files")
    from managed_tools import owned_path
    for entry in path.iterdir():owned_path(entry,path)
    if any((path/name).read_text(encoding="utf-8")!=text for name,text in payloads().items()):
        raise ValueError("Fixture payload differs from the exact public guest schema")
    expected_sources={p for files in public_bindings(ROOT).values() for p in files}
    if set(record.get("source_sha256",{}))!=expected_sources:
        raise ValueError("Unexpected fixture source identities")
    if any(sha(path/name)!=digest for name,digest in record["files"].items()) or any(sha(ROOT/name)!=digest for name,digest in record["source_sha256"].items()):
        raise ValueError("Fixture/source identity changed")
    return record


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output",required=True,type=Path)
    args=parser.parse_args()
    print(json.dumps(create(args.output),indent=2))
