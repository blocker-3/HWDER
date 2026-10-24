"""Carry tools/recomp/symbols.txt and extra_entries.txt over to another build of the game's main module.

    python tools/recomp/remap_symbols.py --base-main <base exefs>/main --new-main <new exefs>/main \
        --functable <base generated>/functable.c --out <tools/recomp/<profile>>

A game update can rebuild main with almost every function moved by a small, constant amount (1.0.1: 25359 of
25366 functions identical apart from their address). Every base function is matched to the new binary by its
body with PC-relative fields and ADRP-relative offsets masked, in address order, and the call graph is checked.
A named function must be found unchanged, or the tool stops: a hook on changed code needs a person. Data symbols
are mapped through the ADRP references of matched functions. Main's own extra entries are mapped like functions;
entries in other modules are copied when those modules are byte-identical (checked by the caller's profile).
Reads only the two NSO files and the base function table; writes symbols.txt and extra_entries.txt to --out.
"""
import argparse
import bisect
import collections
import re
import struct
from pathlib import Path

import lz4.block

MAIN = 0x1000000000
HERE = Path(__file__).resolve().parent


def image(nso):
    """Mapped image of an NSO (text, rodata, data) and the end of its text."""
    flags = struct.unpack_from("<I", nso, 0xC)[0]
    segs = []
    for i in range(3):
        foff, moff, dsize = struct.unpack_from("<III", nso, 0x10 + 0x10 * i)
        csize = struct.unpack_from("<I", nso, 0x60 + 4 * i)[0]
        d = nso[foff:foff + csize]
        if flags & (1 << i):
            d = lz4.block.decompress(d, uncompressed_size=dsize)
        segs.append((moff, d))
    img = bytearray(max(m + len(d) for m, d in segs))
    for m, d in segs:
        img[m:m + len(d)] = d
    return bytes(img), segs[0][0] + len(segs[0][1])


def normalize(ws):
    """Mask PC-relative fields and the low-12 offsets that follow an ADRP (data moves with the code)."""
    out, adrp = [], set()
    for w in ws:
        if (w & 0x7C000000) == 0x14000000: w &= 0xFC000000              # B, BL
        elif (w & 0xFF000010) == 0x54000000: w &= 0xFF00001F            # B.cond
        elif (w & 0x7E000000) == 0x34000000: w &= 0xFF00001F            # CBZ/CBNZ
        elif (w & 0x7E000000) == 0x36000000: w &= 0xFFF8001F            # TBZ/TBNZ
        elif (w & 0x1F000000) == 0x10000000:                            # ADR/ADRP
            if w & 0x80000000:
                adrp.add(w & 31)
            w &= 0x9F00001F
        elif (w & 0x3B000000) == 0x18000000: w &= 0xFF00001F            # LDR literal
        elif (w & 0x7F800000) == 0x11000000 and ((w >> 5) & 31) in adrp:
            w &= 0xFFC003FF                                             # ADD imm after ADRP
        elif (w & 0x3B000000) == 0x39000000 and ((w >> 5) & 31) in adrp:
            w &= 0xFFC003FF                                             # LDR/STR unsigned imm after ADRP
        out.append(w)
    return tuple(out)


def branch_target(img, a):
    w = struct.unpack_from("<I", img, a)[0]
    if (w & 0x7C000000) != 0x14000000:
        return None
    imm = w & 0x3FFFFFF
    return a + ((imm - (1 << 26)) if imm & 0x2000000 else imm) * 4


def data_refs(img, a, n):
    """Addresses formed by ADRP + ADD / LDR / STR inside a function, in order."""
    out, regs = [], {}
    for k in range(0, n, 4):
        w = struct.unpack_from("<I", img, a + k)[0]
        if (w & 0x9F000000) == 0x90000000:
            imm = (((w >> 5) & 0x7FFFF) << 2) | ((w >> 29) & 3)
            if imm & (1 << 20):
                imm -= 1 << 21
            regs[w & 31] = ((a + k) & ~0xFFF) + (imm << 12)
        elif (w & 0x7F800000) == 0x11000000 and ((w >> 5) & 31) in regs:
            out.append(regs[(w >> 5) & 31] + ((w >> 10) & 0xFFF))
        elif (w & 0x3B000000) == 0x39000000 and ((w >> 5) & 31) in regs:
            scale = (w >> 30) & 3
            if (w & 0x04800000) == 0x04800000:  # 128-bit SIMD&FP register
                scale = 4
            out.append(regs[(w >> 5) & 31] + (((w >> 10) & 0xFFF) << scale))
    return out


def match(base, new, starts, base_end, new_end):
    """Base function start -> new start, for every function whose masked body is found unchanged."""
    funcs = [(a, (starts[i + 1] if i + 1 < len(starts) else base_end) - a) for i, a in enumerate(starts)]
    words = lambda img, a, n: normalize(struct.unpack_from(f"<{n // 4}I", img, a))
    mapping, delta = {}, 0
    for a, n in funcs:
        if n <= 0 or n % 4:
            continue
        want = words(base, a, n)
        for dd in [0] + sorted(range(-0x4000, 0x4004, 4), key=abs)[1:]:
            c = a + delta + dd
            if 0 <= c and c + n <= new_end and words(new, c, min(n, 24))[:6] == want[:6] and words(new, c, n) == want:
                mapping[a], delta = c, c - a
                break
    sizes = dict(funcs)
    # The call graph must agree: a call into a matched function lands on its new address.
    for a, c in list(mapping.items()):
        for k in range(0, sizes[a], 4):
            bt = branch_target(base, a + k)
            if bt is None:
                continue
            i = bisect.bisect_right(starts, bt) - 1
            if i >= 0 and starts[i] in mapping and branch_target(new, c + k) != mapping[starts[i]] + bt - starts[i]:
                del mapping[a]
                break
    return mapping, sizes


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--base-main", type=Path, required=True)
    ap.add_argument("--new-main", type=Path, required=True)
    ap.add_argument("--functable", type=Path, required=True)
    ap.add_argument("--symbols", type=Path, default=HERE / "symbols.txt")
    ap.add_argument("--extra", type=Path, default=HERE / "extra_entries.txt")
    ap.add_argument("--out", type=Path, required=True)
    a = ap.parse_args()
    base, base_end = image(a.base_main.read_bytes())
    new, new_end = image(a.new_main.read_bytes())
    starts = sorted({int(x, 16) - MAIN for x in re.findall(r"\{ 0x([0-9A-Fa-f]+)ULL", a.functable.read_text())
                     if MAIN <= int(x, 16) < MAIN + base_end})
    mapping, sizes = match(base, new, starts, base_end, new_end)
    print(f"{len(mapping)} of {len(starts)} main functions found unchanged")
    datamap = collections.defaultdict(collections.Counter)
    for f, c in mapping.items():
        rb, rn = data_refs(base, f, sizes[f]), data_refs(new, c, sizes[f])
        if len(rb) == len(rn):
            for x, y in zip(rb, rn):
                datamap[x][y] += 1

    def remap(addr, kind):
        off = addr - MAIN
        if not 0 <= off < len(base):
            return addr  # another module
        if kind == "data":
            seen = datamap.get(off)
            if not seen:
                # Not reached by a plain ADRP pair (an ADD, then an offset load): find its surrounding bytes,
                # which must occur exactly once near the old address.
                window = base[max(0, off - 32):off + 32]
                lo = max(0, off - 32 - 0x10000)
                hits, at = [], new.find(window, lo, off + 32 + 0x10000)
                while at != -1:
                    hits.append(at)
                    at = new.find(window, at + 1, off + 32 + 0x10000)
                if len(hits) != 1:
                    raise SystemExit(f"{addr:#x}: no matched function references this data and its bytes match "
                                     f"{len(hits)} places")
                return MAIN + hits[0] + (off - max(0, off - 32))
            (new_off, votes), = seen.most_common(1)
            if votes * 2 <= sum(seen.values()):
                raise SystemExit(f"{addr:#x}: references disagree {dict(seen)}")
            return MAIN + new_off
        i = bisect.bisect_right(starts, off) - 1
        if i < 0 or starts[i] not in mapping:
            raise SystemExit(f"{addr:#x}: its function {MAIN + starts[max(i, 0)]:#x} changed; map it by hand")
        return MAIN + mapping[starts[i]] + off - starts[i]

    a.out.mkdir(parents=True, exist_ok=True)
    lines = []
    for line in a.symbols.read_text(encoding="utf-8").splitlines():
        m = re.match(r"(\s*)0x([0-9a-fA-F]+)(\s+)(\w+)(\s+)(func|data)(.*)$", line)
        if m:
            new_addr = remap(int(m.group(2), 16), m.group(6))
            line = f"{m.group(1)}{new_addr:#x}{m.group(3)}{m.group(4)}{m.group(5)}{m.group(6)}{m.group(7)}"
        lines.append(line)
    header = f"# Remapped by tools/recomp/remap_symbols.py from {a.symbols.name} (same names and notes; addresses for this build).\n"
    (a.out / "symbols.txt").write_text(header + "\n".join(lines) + "\n", encoding="utf-8")
    extra = [f"{remap(int(x, 16), 'func'):#x}" for x in a.extra.read_text().split() if x.strip()]
    (a.out / "extra_entries.txt").write_text("\n".join(extra) + "\n")
    print(f"wrote {a.out / 'symbols.txt'} and {a.out / 'extra_entries.txt'}")


if __name__ == "__main__":
    main()
