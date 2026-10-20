"""Reviewed bounded private compiler probe. Public inputs only; never starts HWDER."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import time
from types import SimpleNamespace

sys.dont_write_bytecode=True
sys.path.insert(0,str(Path(__file__).resolve().parent))
import build
from build_lock import build_lock
import first_run
import make_setup_dist
import managed_tools
import pe_dependencies

ROOT=Path(__file__).resolve().parent.parent
BASE_KIT_SHA="23122ec45e862d2ddf7aa4236bdc3f768a5452c2fff888f9ce9489a1db2109cc"


def stage_public_sources(archive):
    if first_run.file_hash(archive)!=BASE_KIT_SHA:
        raise ValueError("Unreviewed public base source ZIP")
    files=make_setup_dist.verified_source(archive)
    selected={name:data for name,data in files.items() if name.startswith("third_party/")}
    for name,data in selected.items():
        path=managed_tools.owned_path(ROOT/name,ROOT/"third_party")
        if path.exists() and path.read_bytes()!=data:
            raise ValueError("Refusing to replace differing public dependency source: "+name)
    for name,data in selected.items():
        path=ROOT/name
        path.parent.mkdir(parents=True,exist_ok=True)
        if not path.exists():
            path.write_bytes(data)
    return {name:hashlib.sha256(data).hexdigest() for name,data in selected.items()}


def tool_imports(toolset):
    paths=managed_tools.paths(toolset)
    python=paths["python"].parent
    llvm=paths["cc"].parent
    roots=[paths["cc"],paths["cxx"],llvm/"clang.exe",llvm/"clang-23.exe",llvm/"ld.lld.exe"]
    records=pe_dependencies.closure(roots,llvm)
    python_images=[paths["python"],*python.rglob("*.pyd")]
    records.update(pe_dependencies.closure(python_images,python))
    for name in ("cmake","ninja"):
        records.update(pe_dependencies.closure([paths[name]],paths[name].parent))
    failures={name:record["unresolved"] for name,record in records.items() if record["unresolved"]}
    if failures:
        raise ValueError("Non-OS dependencies missing app-locally: "+json.dumps(failures))
    # Every private PE visited must be one of the byte-certified extracted files.
    certificate=first_run.read_json(Path(toolset)/"complete.json")
    output=(Path(toolset)/"output").resolve()
    for name in records:
        path=Path(name)
        relative=path.relative_to(output).as_posix()
        sha=first_run.file_hash(path)
        if certificate["files"].get(relative)!=sha:
            raise ValueError("Private PE changed after extraction certification")
        records[name]["sha256"]=sha
    return records


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--toolchain",required=True,type=Path)
    parser.add_argument("--source-kit",required=True,type=Path)
    parser.add_argument("--output",required=True,type=Path)
    parser.add_argument("--inventory-only",action="store_true")
    args=parser.parse_args()
    toolset=args.toolchain.resolve()
    # Exact local test invocation supplies a toolset under this private Data root.
    data=(ROOT/"build/private-toolchain-data").resolve()
    managed_tools.owned_path(toolset,data)
    output=managed_tools.owned_path(args.output,data)
    output.mkdir(parents=True,exist_ok=True)
    report={"started":time.time(),"game":False,"compiler_executed":False,
            "toolset":str(toolset),"output":str(output),"source_root":str(ROOT)}
    report_path=output/"probe-result.json"
    try:
        report["pe_import_closure"]=tool_imports(toolset)
        if args.inventory_only:
            report["status"]="IMPORT_INVENTORY_PASS"
            return 0
        with build_lock():
            report["public_source_hashes"]=stage_public_sources(args.source_kit)
            report["compiler_executed"]=True
            config=SimpleNamespace(jobs=2,only_file=None,target=None,build_dir=str(output/"cmake"),
                                   generated_dir=None,synthetic_tests=False,host_tests=False,
                                   game_only=False,setup_ui=False,managed_probe=True,
                                   managed_toolchain=toolset)
            result=build.build(config)
            report["build_exit"]=result
            if result:
                report["status"]="COMPILE_OR_LINK_FAILED"
                return result
            selected=managed_tools.paths(toolset)
            ctest=selected["cmake"].with_name("ctest.exe")
            env=managed_tools.environment(toolset)
            result=subprocess.run([str(ctest),"--test-dir",str(output/"cmake"),
                                   "--output-on-failure","--no-tests=error","--verbose"],env=env).returncode
            report["test_exit"]=result
            report["synthetic_pe"]=pe_dependencies.closure([output/"cmake/managed_runtime_probe.exe"],
                                                           output/"cmake")
            if any(v["unresolved"] for v in report["synthetic_pe"].values()):
                raise ValueError("Synthetic output requires a non-system DLL")
            metadata=pe_dependencies.inspect(output/"cmake/managed_runtime_probe.exe")
            if metadata["stack_reserve"]!=16777216 or metadata["dynamic_base"]:
                raise ValueError("Synthetic stack/ASLR contract differs from production")
            report["status"]="PASS" if result==0 else "SYNTHETIC_CONTRACT_FAILED"
            return result
    except BaseException as error:
        report["status"]="ERROR"
        report["error"]=str(error)
        raise
    finally:
        report["finished"]=time.time()
        first_run.atomic_json(report_path,report)
        print("managed-probe: "+report["status"],flush=True)


if __name__=="__main__":
    sys.exit(main())
