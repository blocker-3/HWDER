"""Execute synthetic A64 translations against an independent integer model.

No dumped instructions, guest image, emulator, or generated game code is used.
CMake invokes --emit-c, then CTest invokes --library on the compiled result.
"""
import argparse
import ctypes
from pathlib import Path
import random
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from recomp.a64 import Translator


# Public ISA encodings: register add/sub with NZCV, ADC/SBC, and variable shifts.
# x0/w0 is the destination; x1/w1 and x2/w2 are the inputs.
CASES = []
for width, sf in ((32, 0), (64, 1)):
    for name, base in (("adds", 0x2B020020), ("subs", 0x6B020020),
                       ("adc", 0x1A020020), ("sbc", 0x5A020020),
                       ("lsl", 0x1AC22020), ("lsr", 0x1AC22420),
                       ("asr", 0x1AC22820), ("ror", 0x1AC22C20)):
        CASES.append((name, width, base | (sf << 31)))


def emit(path):
    translator = Translator()
    lines = ['#include "hwder/cpu.h"',
             '#ifdef _WIN32\n#define EXPORT __declspec(dllexport)\n#else\n#define EXPORT __attribute__((visibility("default")))\n#endif']
    for index, (_, _, word) in enumerate(CASES):
        translated = translator.translate(word, 0x1000)
        if translated.kind is not None:
            raise RuntimeError("unexpected control flow in arithmetic fixture")
        # Keep the test ABI separate from Ctx's layout; use the real C header.
        lines.append(f"EXPORT void case_{index}(u64 a, u64 b, u32 nzcv, u64* out) {{\n"
                     "Ctx state = {0}; Ctx* c = &state; c->x[1]=a; c->x[2]=b;\n"
                     "c->nf=(nzcv>>3)&1; c->zf=(nzcv>>2)&1; c->cf=(nzcv>>1)&1; c->vf=nzcv&1;\n"
                     f"{translated.c}\n"
                     "out[0]=c->x[0]; out[1]=(c->nf<<3)|(c->zf<<2)|(c->cf<<1)|c->vf;\n"
                     "out[2]=c->x[1]; out[3]=c->x[2]; }")
    Path(path).write_text("\n".join(lines) + "\n", encoding="utf-8")


def expected(name, width, a, b, flags):
    mask = (1 << width) - 1
    a, b = a & mask, b & mask
    carry = (flags >> 1) & 1
    if name in ("adds", "subs", "adc", "sbc"):
        subtract = name in ("subs", "sbc")
        borrow = 1 - carry if name == "sbc" else 0
        value = a - b - borrow if subtract else a + b + (carry if name == "adc" else 0)
        result = value & mask
        if name in ("adds", "subs"):
            signed = lambda x: x - (1 << width) if x >> (width - 1) else x
            mathematical = signed(a) - signed(b) if subtract else signed(a) + signed(b)
            overflow = not -(1 << (width - 1)) <= mathematical < (1 << (width - 1))
            c = a >= b if subtract else value > mask
            flags = ((result >> (width - 1)) << 3) | ((result == 0) << 2) | (int(c) << 1) | int(overflow)
    else:
        shift = b % width
        if name == "lsl":
            result = (a << shift) & mask
        elif name == "lsr":
            result = a >> shift
        elif name == "asr":
            result = ((a - (1 << width) if a >> (width - 1) else a) >> shift) & mask
        else:
            result = ((a >> shift) | (a << ((width - shift) % width))) & mask
    return result, flags


def run(library):
    native = ctypes.CDLL(str(Path(library).resolve()))
    rng = random.Random(0xA64)
    checks = 0
    for index, (name, width, _) in enumerate(CASES):
        fn = getattr(native, f"case_{index}")
        fn.argtypes = [ctypes.c_uint64, ctypes.c_uint64, ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint64)]
        fn.restype = None
        values = [0, 1, 31, 32, 63, 64, (1 << (width - 1)) - 1, 1 << (width - 1),
                  (1 << width) - 1, 0xFFFFFFFFFFFFFFFF]
        states = [(a, b, flags) for a in values for b in values for flags in (0, 2, 15)]
        states += [(rng.getrandbits(64), rng.getrandbits(64), rng.randrange(16)) for _ in range(256)]
        for a, b, flags in states:
            out = (ctypes.c_uint64 * 4)()
            fn(a, b, flags, out)
            want = (*expected(name, width, a, b, flags), a, b)
            if tuple(out) != want:
                raise AssertionError(f"{name}{width} a={a:#x} b={b:#x} nzcv={flags:#x}: "
                                     f"got {tuple(out)}, expected {want}")
            checks += 1
    print(f"synthetic recompiler: {checks} execution comparisons across {len(CASES)} encodings passed")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--emit-c")
    mode.add_argument("--library")
    args = parser.parse_args()
    if args.emit_c:
        emit(args.emit_c)
    else:
        run(args.library)
