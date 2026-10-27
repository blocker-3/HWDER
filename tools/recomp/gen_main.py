"""Driver: recompile every ExeFS module into one program.

    python -m recomp.gen_main [--modules rtld,main,subsdk0,subsdk1,sdk]

Outputs (in generated/):
  rc_NNNN.c       recompiled functions (~12k lines per file)
  funcs.h         prototypes
  functable.c     guest address -> host function table (all modules)
  module_info.c   module layout (the runtime loads the NSOs itself from the user's ExeFS)
  symbols.h       named aliases for the functions listed in tools/recomp/symbols.txt
  sources.cmake   file list for CMake
"""
import argparse
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from nso import Process  # noqa: E402
from recomp.gen import Emitter, Module, discover, fn_name  # noqa: E402
from recomp.a64 import hx  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))

# The SDK's C library routines, by their exported name, run as host library calls instead of translated
# AArch64 (the guest memcpy's byte-merging loops cost ~8% of MainThread at the busy spot). Guest memory is
# identity mapped; x0 (the returned destination) is left as is, and nothing else a caller may rely on
# changes. memcpy uses memmove: a guest call with overlapping ranges then behaves like the original.
LIBC_INTRINSICS = {
    "memcpy": "memmove((void*)HW_PTR(c->x[0]), (const void*)HW_PTR(c->x[1]), (size_t)c->x[2]);",
    "memmove": "memmove((void*)HW_PTR(c->x[0]), (const void*)HW_PTR(c->x[1]), (size_t)c->x[2]);",
    "memset": "memset((void*)HW_PTR(c->x[0]), (int)(u8)c->x[1], (size_t)c->x[2]);",
    "bzero": "memset((void*)HW_PTR(c->x[0]), 0, (size_t)c->x[1]);",
    "strlen": "c->x[0] = (u64)strlen((const char*)HW_PTR(c->x[0]));",
}


def libc_intrinsics(mod):
    """{guest address: C body} for the LIBC_INTRINSICS functions mod exports."""
    if mod.m.name != "sdk":
        return {}
    return {mod.base + s.value: LIBC_INTRINSICS[s.name] for s in mod.m.symbols
            if s.defined and s.type == 2 and s.value and s.name in LIBC_INTRINSICS}


def read_symbols(path):
    """tools/recomp/symbols.txt: `<addr> <Name> [func|data] [hook]  # note` per line -> list of
    (addr, name, is_func, is_hook). Names must be C identifiers and unique; so must addresses."""
    out, names, addrs = [], set(), set()
    if not os.path.exists(path):
        return out
    for n, ln in enumerate(open(path), 1):
        ln = ln.split("#")[0].strip()
        if not ln:
            continue
        parts = ln.split()
        if len(parts) < 2 or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", parts[1]):
            raise SystemExit(f"{path}:{n}: expected `<addr> <Name> [func|data] [hook]`")
        addr, name, attrs = int(parts[0], 16), parts[1], set(parts[2:])
        if attrs - {"func", "data", "hook"}:
            raise SystemExit(f"{path}:{n}: unknown attribute in {sorted(attrs)}")
        is_func = "data" not in attrs
        if "hook" in attrs and not is_func:
            raise SystemExit(f"{path}:{n}: a data symbol cannot be a hook")
        if name in names or addr in addrs:
            raise SystemExit(f"{path}:{n}: duplicate symbol {name} / {addr:#x}")
        names.add(name)
        addrs.add(addr)
        out.append((addr, name, is_func, "hook" in attrs))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--modules", default="rtld,main,subsdk0,subsdk1,sdk")
    ap.add_argument("--exefs", default=os.path.join(HERE, "..", "..", "data", "exefs"))
    ap.add_argument("--out", default=os.path.join(HERE, "..", "..", "generated"))
    ap.add_argument("--lines-per-file", type=int, default=12000)
    ap.add_argument("--extra", action="append",
                    help="extra entry points (hex, one per line) discovered at runtime; repeatable "
                         "(default: tools/recomp/extra_entries.txt plus data/extra_entries.txt if present)")
    ap.add_argument("--hooks", default=os.path.join(HERE, "hooks.txt"),
                    help="guest functions the runtime overrides (hex, one per line, # comments)")
    ap.add_argument("--symbols", default=os.path.join(HERE, "symbols.txt"),
                    help="named guest symbols; those marked `hook` are overridden like --hooks entries")
    ap.add_argument("--no-regcache", action="store_true",
                    help="keep guest registers in Ctx throughout (gen.regcache_emit; A/B tests)")
    ap.add_argument("--no-libc-intrinsics", action="store_true",
                    help="translate the SDK's memcpy/memset/... like any other function (A/B tests)")
    a = ap.parse_args()
    t0 = time.time()

    def log(s):
        print(f"[{time.time() - t0:6.1f}s] {s}", flush=True)

    proc = Process(a.exefs)
    extra = set()
    for path in a.extra or [os.path.join(HERE, "extra_entries.txt"),
                            os.path.join(HERE, "..", "..", "data", "extra_entries.txt")]:
        if os.path.exists(path):
            ents = {int(x, 16) for x in open(path).read().split() if x.strip()}
            log(f"{len(ents)} extra entry points from {path}")
            extra |= ents
    hooks = set()
    if os.path.exists(a.hooks):
        hooks = {int(ln.split("#")[0], 16) for ln in open(a.hooks) if ln.split("#")[0].strip()}
        log(f"{len(hooks)} hooked functions from {a.hooks}")
    symbols = read_symbols(a.symbols)
    sym_hooks = {addr for addr, _, _, hook in symbols if hook}
    if sym_hooks - hooks:
        log(f"{len(sym_hooks - hooks)} hooked functions from {a.symbols}")
    hooks |= sym_hooks
    sym_name = {addr: name for addr, name, _, _ in symbols}
    os.makedirs(a.out, exist_ok=True)

    all_starts, files, mod_infos, resumable = [], [], [], []
    for name in a.modules.split(","):
        if not any(m.name == name for m in proc.modules):
            continue
        mod = Module(proc, name)
        log(f"module {name}: base={mod.base:#x} text={mod.text_lo:#x}-{mod.text_hi:#x}")
        funcs, _ = discover(mod, {}, log, {x for x in extra if mod.in_text(x)})
        em = Emitter(mod, funcs, {}, log, regcache=not a.no_regcache)
        intrinsics = {} if a.no_libc_intrinsics else libc_intrinsics(mod)
        intrinsics = {st: b for st, b in intrinsics.items() if st in funcs and st not in hooks}
        if intrinsics:
            log(f"  {len(intrinsics)} C library functions as host calls")

        def emit(st):
            if st in intrinsics:  # the translated body stays as f_<addr>_guest (HWDER_GUEST_LIBC=1 selects it)
                guest = em.emit_function(st, funcs[st]).replace(
                    f"void {fn_name(st)}(Ctx* restrict c) {{", f"void {fn_name(st)}_guest(Ctx* restrict c) {{", 1)
                return (f"void {fn_name(st)}_guest(Ctx* restrict c);\n"
                        f"void {fn_name(st)}(Ctx* restrict c) {{\n"
                        f"  if (hw_guest_libc) {{ HW_MUSTTAIL return {fn_name(st)}_guest(c); }}\n"
                        f"  {intrinsics[st]}\n}}\n" + guest)
            return em.emit_function(st, funcs[st])

        starts = sorted(funcs)
        # One file per 64 KiB of guest code: adding an entry point only changes (and rebuilds) one file.
        buckets = {}
        for st in starts:
            buckets.setdefault((st - mod.text_lo) >> 16, []).append(st)
        for bucket, sts in sorted(buckets.items()):
            fname = f"rc_{name}_{bucket:03x}.c"
            body = "".join((f"// {sym_name[st]}\n" if st in sym_name else "") + emit(st) for st in sts)
            # Hooked functions keep their body as f_<addr>_orig; the runtime defines f_<addr>, so
            # direct calls, tail calls and the dispatch table all reach the hook.
            for st in sts:
                if st in hooks:
                    body = body.replace(f"void {fn_name(st)}(Ctx* restrict c) {{",
                                        f"void {fn_name(st)}_orig(Ctx* restrict c) {{", 1)
                    # A runtime without this hook (a worktree on an older branch building against this
                    # generated dir) still links: f_<addr> falls back to the original body.
                    body += (f"void {fn_name(st)}_default(Ctx* restrict c) {{ HW_MUSTTAIL return {fn_name(st)}_orig(c); }}\n"
                             f'#pragma comment(linker, "/alternatename:{fn_name(st)}={fn_name(st)}_default")\n')
            # Per-file prototypes (not a global header) so unrelated changes don't rebuild everything.
            protos = sorted(set(re.findall(r"\bf_[0-9a-f]+\b", body)))
            text = ('#include "hwder/cpu.h"\n#include "hwder/dispatch.h"\n'
                    + "".join(f"void {p}(Ctx* restrict c);\n" for p in protos) + "\n" + body)
            path = os.path.join(a.out, fname)
            old = open(path).read() if os.path.exists(path) else None
            if old != text:  # keep timestamps of unchanged files for incremental builds
                with open(path, "w") as f:
                    f.write(text)
            files.append(fname)
        log(f"  {len(starts)} functions; stats {dict(em.stats)}")
        all_starts += starts
        resumable += getattr(em, "resumable", [])
        with open(os.path.join(a.exefs, name), "rb") as nso:
            build_id = nso.read(0x60)[0x40:0x50].hex()
        mod_infos.append((name, mod.base, mod.m.image_size, mod.text_lo, mod.text_hi, build_id))

    for old in os.listdir(a.out):  # drop files from an older layout
        if old.startswith("rc_") and old.endswith(".c") and old not in files:
            os.remove(os.path.join(a.out, old))

    def write_if_changed(fname, text):
        path = os.path.join(a.out, fname)
        if not os.path.exists(path) or open(path).read() != text:
            with open(path, "w") as f:
                f.write(text)

    with open(os.path.join(a.out, "sources.cmake"), "w") as f:
        f.write("set(RECOMP_SOURCES\n" + "".join(f"  ${{GEN_DIR}}/{x}\n" for x in files) + ")\n")
    with open(os.path.join(a.out, "funcs.h"), "w") as f:
        f.write('#pragma once\n#include "hwder/cpu.h"\n#include "hwder/dispatch.h"\n')
        for st in all_starts:
            f.write(f"void {fn_name(st)}(Ctx* restrict c);\n")
    with open(os.path.join(a.out, "functable.c"), "w") as f:
        f.write('#include "funcs.h"\n\nconst HwFuncEntry hw_func_table[] = {\n')
        for st in all_starts:
            f.write(f"  {{ {hx(st)}, {fn_name(st)} }},\n")
        f.write("};\nconst unsigned hw_func_count = sizeof(hw_func_table) / sizeof(hw_func_table[0]);\n")
        # Functions that accept entry at any instruction (unresolved indirect branches), sorted.
        f.write("const unsigned long long hw_resumable[] = {" + ",".join(hx(x) for x in sorted(resumable)) + ",0};\n")
        f.write(f"const unsigned hw_resumable_count = {len(resumable)};\n")
        # Named guest symbols (tools/recomp/symbols.txt), sorted by address, for backtraces and logs.
        f.write("const HwSymbol hw_symbols[] = {\n")
        for addr, name, _, _ in sorted(symbols):
            f.write(f'  {{ {hx(addr)}, "{name}" }},\n')
        f.write("  { 0, 0 },\n};\n")
        f.write(f"const unsigned hw_symbol_count = {len(symbols)};\n")
    starts_set = set(all_starts)
    sym_h = ["#pragma once", '#include "hwder/cpu.h"', "// Generated by tools/recomp/gen_main.py from tools/recomp/symbols.txt.",
             "// f_<Name> is the recompiled function; f_<Name>_orig the generated body of a hooked one; HW_ADDR_<Name> the guest address.",
             "#ifdef __cplusplus", 'extern "C" {', "#endif"]
    for addr, name, is_func, hook in symbols:
        sym_h.append(f"#define HW_ADDR_{name} {hx(addr)}")
        if not is_func:
            continue
        if addr not in starts_set:
            raise SystemExit(f"symbols.txt: {name} {addr:#x} is not a recompiled function start")
        sym_h.append(f"#define f_{name} {fn_name(addr)}")
        sym_h.append(f"void {fn_name(addr)}(Ctx* c);")
        if hook:
            sym_h.append(f"#define f_{name}_orig {fn_name(addr)}_orig")
            sym_h.append(f"void {fn_name(addr)}_orig(Ctx* c);")
    sym_h += ["#ifdef __cplusplus", "}", "#endif", ""]
    write_if_changed("symbols.h", "\n".join(sym_h))
    with open(os.path.join(a.out, "module_info.c"), "w") as f:
        f.write('#include "hwder/module.h"\n\nconst HwModuleInfo hw_modules[] = {\n')
        for name, base, size, tlo, thi, build_id in mod_infos:
            f.write(f'  {{ "{name}", {hx(base)}, {hx(size)}, {hx(tlo)}, {hx(thi)}\n'
                    f'#ifdef HW_MODULE_HAS_BUILD_ID\n    , "{build_id}"\n#endif\n  }},\n')
        f.write("};\nconst unsigned hw_module_count = sizeof(hw_modules) / sizeof(hw_modules[0]);\n")
    log(f"done: {len(all_starts)} functions in {len(files)} files")


if __name__ == "__main__":
    main()
