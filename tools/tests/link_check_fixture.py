"""Link-check fixture: a stand-in generated/ directory with no game code in it.

Writes symbols.h from tools/recomp/symbols.txt plus empty stubs for every guest function the runtime names,
so the complete host runtime can be compiled and linked on a machine without a game dump (the Linux port's
compile gate). The executable this produces has no modules and must never be run or distributed.

    python3 tools/tests/link_check_fixture.py --output build/link-check-generated
    python3 tools/build.py --generated-dir build/link-check-generated --build-dir build/link-check
"""
import argparse
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]


def symbols(path):
    out = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.split("#")[0].strip()
        if not line:
            continue
        parts = line.split()
        attrs = set(parts[2:])
        out.append((int(parts[0], 16), parts[1], "data" not in attrs, "hook" in attrs))
    return out


def fn_name(addr):
    return f"f_{addr:x}"


def referenced_guests():
    """f_<addr> / f_<addr>_orig names the runtime declares without defining (direct guest calls, PROBE hooks)."""
    declared, defined = set(), set()
    for path in sorted((ROOT / "runtime/src").rglob("*.cpp")):
        source = path.read_text(encoding="utf-8")
        for name, end in re.findall(r'extern\s+"C"\s+void\s+(f_[0-9a-f]{10}(?:_orig)?)\s*\(Ctx\*\s*\w*\)\s*([;{])', source):
            (defined if end == "{" else declared).add(name)
        for address in re.findall(r"^PROBE\(([0-9a-f]{10}),", source, re.M):
            defined.add("f_" + address)
            declared.add("f_" + address + "_orig")
    return declared - defined


def create(out):
    out.mkdir(parents=True, exist_ok=True)
    syms = symbols(ROOT / "tools/recomp/symbols.txt")
    sym_h = ["#pragma once", '#include "hwder/cpu.h"', "// Link-check fixture: NOT game code.",
             "#ifdef __cplusplus", 'extern "C" {', "#endif"]
    stubs = referenced_guests()
    for addr, name, is_func, hook in syms:
        sym_h.append(f"#define HW_ADDR_{name} {addr:#x}ULL")
        if not is_func:
            continue
        sym_h.append(f"#define f_{name} {fn_name(addr)}")
        sym_h.append(f"void {fn_name(addr)}(Ctx* c);")
        if hook:
            sym_h.append(f"#define f_{name}_orig {fn_name(addr)}_orig")
            sym_h.append(f"void {fn_name(addr)}_orig(Ctx* c);")
            stubs.add(fn_name(addr) + "_orig")  # the runtime defines the hook itself
        else:
            stubs.add(fn_name(addr))
    sym_h += ["#ifdef __cplusplus", "}", "#endif", ""]
    (out / "symbols.h").write_text("\n".join(sym_h), encoding="utf-8")
    (out / "rc_link_check.c").write_text('#include "hwder/cpu.h"\n' + "".join(
        f"void {name}(Ctx* c) {{ (void)c; }}\n" for name in sorted(stubs)), encoding="utf-8")
    (out / "functable.c").write_text(
        '#include "hwder/dispatch.h"\n'
        "const HwFuncEntry hw_func_table[1] = {{0, 0}};\nconst unsigned hw_func_count = 0;\n"
        "const unsigned long long hw_resumable[1] = {0};\nconst unsigned hw_resumable_count = 0;\n"
        "const HwSymbol hw_symbols[1] = {{0, 0}};\nconst unsigned hw_symbol_count = 0;\n", encoding="utf-8")
    (out / "module_info.c").write_text(
        '#include "hwder/module.h"\nconst HwModuleInfo hw_modules[1];\nconst unsigned hw_module_count = 0;\n',
        encoding="utf-8")
    (out / "sources.cmake").write_text('set(RECOMP_SOURCES "${GEN_DIR}/rc_link_check.c")\n', encoding="utf-8")
    (out / "funcs.h").write_text('#pragma once\n#include "hwder/cpu.h"\n#include "hwder/dispatch.h"\n', encoding="utf-8")
    print(f"link-check fixture: {len(stubs)} stub guest functions in {out}")


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--output", required=True, type=Path)
    create(ap.parse_args().output)
