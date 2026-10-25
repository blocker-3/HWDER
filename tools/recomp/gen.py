"""HWDER static recompiler: AArch64 module -> C source.

    python -m recomp.gen [--module main] [--out ../generated] [--files N]

Pipeline:
  1. load + relocate all modules (nso.Process), pick the module to recompile
  2. discover functions: .eh_frame FDEs, entry/init/fini, exported symbols, BL targets,
     tail-call targets, absolute code pointers in relocated data, ADRP+ADD code refs
  3. per function: find intra-function branch targets, recover jump tables
  4. emit one C function per guest function; PLT stubs become direct HLE calls
  5. emit function table, import table and the relocated memory image
"""
import argparse
import bisect
import collections
import json
import os
import re
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from nso import Process, R_AARCH64_RELATIVE, R_AARCH64_GLOB_DAT, R_AARCH64_JUMP_SLOT, R_AARCH64_ABS64  # noqa
from recomp.a64 import Translator, Unimplemented, hx, bits, bit, sext  # noqa: E402


def fn_name(addr):
    return f"f_{addr:x}"


# ------------------------------------------------------------------ eh_frame

def read_uleb(b, o):
    r = s = 0
    while True:
        x = b[o]
        o += 1
        r |= (x & 0x7F) << s
        s += 7
        if not x & 0x80:
            return r, o


def read_sleb(b, o):
    r = s = 0
    while True:
        x = b[o]
        o += 1
        r |= (x & 0x7F) << s
        s += 7
        if not x & 0x80:
            if x & 0x40:
                r -= 1 << s
            return r, o


def read_encoded(b, o, enc):
    """Returns (value, new_offset). Only the encodings clang emits are supported."""
    fmt = enc & 0x0F
    app = enc & 0x70
    start = o
    if fmt == 0x0B:
        v = struct.unpack_from("<i", b, o)[0]
        o += 4
    elif fmt == 0x03:
        v = struct.unpack_from("<I", b, o)[0]
        o += 4
    elif fmt == 0x0C:
        v = struct.unpack_from("<q", b, o)[0]
        o += 8
    elif fmt == 0x04 or fmt == 0x00:
        v = struct.unpack_from("<Q", b, o)[0]
        o += 8
    else:
        raise NotImplementedError(f"eh encoding {enc:#x}")
    if app == 0x10:
        v += start
    return v, o


def parse_fdes(m):
    """Return list of (start, end) image offsets for every FDE in the module's .eh_frame."""
    img = m.image
    hdr = m.eh_frame_hdr[0]
    eh = struct.unpack_from("<i", img, hdr + 4)[0] + hdr + 4
    end = len(img)
    cies = {}
    out = []
    o = eh
    while o < end:
        length = struct.unpack_from("<I", img, o)[0]
        if length == 0:
            break
        body = o + 4
        nxt = body + length
        cie_ptr = struct.unpack_from("<I", img, body)[0]
        if cie_ptr == 0:  # CIE
            p = body + 5
            aug_end = img.index(b"\0", p)
            aug = img[p:aug_end].decode()
            p = aug_end + 1
            _, p = read_uleb(img, p)  # code align
            _, p = read_sleb(img, p)  # data align
            p += 1  # return reg
            enc = 0
            if aug.startswith("z"):
                _, p = read_uleb(img, p)
                for ch in aug[1:]:
                    if ch == "R":
                        enc = img[p]
                        p += 1
                    elif ch == "P":
                        penc = img[p]
                        p += 1
                        _, p = read_encoded(img, p, penc)
                    elif ch == "L":
                        p += 1
            cies[o] = enc
        else:
            cie = body - cie_ptr
            enc = cies.get(cie, 0x1B)
            start, p = read_encoded(img, body + 4, enc)
            rng, p = read_encoded(img, p, enc & 0x0F)
            out.append((start, start + rng))
        o = nxt
    return out


# ------------------------------------------------------------------ module analysis

class Module:
    def __init__(self, proc, name):
        self.proc = proc
        self.m = m = [x for x in proc.modules if x.name == name][0]
        self.base = m.base
        self.text_lo = m.base + m.text_off
        self.text_hi = m.base + m.text_off + m.text_size
        self.tr = Translator()
        self._cache = {}

    def in_text(self, a):
        return self.text_lo <= a < self.text_hi and (a & 3) == 0

    def word(self, a):
        return struct.unpack_from("<I", self.m.image, a - self.base)[0]

    def u64(self, a):
        return struct.unpack_from("<Q", self.m.image, a - self.base)[0]

    def s32(self, a):
        return struct.unpack_from("<i", self.m.image, a - self.base)[0]

    def insn(self, a):
        t = self._cache.get(a)
        if t is None:
            w = self.word(a)
            try:
                t = self.tr.translate(w, a)
            except Unimplemented as e:
                t = None
                self._cache[a] = ("unimpl", w, str(e))
                return self._cache[a]
            self._cache[a] = t
        return t


# ------------------------------------------------------------------ PLT / imports

def find_plt_stubs(mod):
    """PLT stub: adrp x16, P; ldr x17, [x16, #o]; add x16, x16, #o; br x17 -> GOT slot P+o."""
    m = mod.m
    jump_slots = {}
    for r_off, r_type, r_sym, r_add in m.relas:
        if r_type == R_AARCH64_JUMP_SLOT:
            jump_slots[m.base + r_off] = m.symbols[r_sym].name
    stubs = {}
    a = mod.text_lo
    while a + 16 <= mod.text_hi:
        w0 = mod.word(a)
        if w0 & 0x9F00001F == 0x90000010:  # adrp x16
            w1, w2, w3 = mod.word(a + 4), mod.word(a + 8), mod.word(a + 12)
            if (w1 & 0xFFC003FF == 0xF9400211 and w2 & 0xFFC003FF == 0x91000210 and w3 == 0xD61F0220):
                imm = sext((bits(w0, 23, 5) << 2) | bits(w0, 30, 29), 21)
                page = (a & ~0xFFF) + (imm << 12)
                slot = page + bits(w1, 21, 10) * 8
                if slot in jump_slots:
                    stubs[a] = jump_slots[slot]
                    a += 16
                    continue
        a += 4
    return stubs, jump_slots


# ------------------------------------------------------------------ jump tables

def decode_reg_fields(w):
    return bits(w, 4, 0), bits(w, 9, 5), bits(w, 20, 16)


def recover_jump_table(mod, br_addr, fstart, fend):
    """Recognise clang's `ldrs{w,h,b} xE, [xT, xI, lsl #s]; add xD, xT, xE (lsl #k); br xD`
    (and the adr-based compact variant). Returns list of target addresses or None."""
    w_br = mod.word(br_addr)
    rd_br = bits(w_br, 9, 5)
    window = []
    a = br_addr - 4
    while a >= fstart and len(window) < 16:
        window.append(a)
        a -= 4
    # find the add producing the branch register
    add_a = None
    for a in window:
        w = mod.word(a)
        if w & 0xFF200000 == 0x8B000000 and bits(w, 4, 0) == rd_br:  # add xD, xN, xM{, lsl #k}
            add_a = a
            break
        if writes_reg(w, rd_br):
            return None
    if add_a is None:
        return None
    w_add = mod.word(add_a)
    rn, rm, shift_amt = bits(w_add, 9, 5), bits(w_add, 20, 16), bits(w_add, 15, 10)
    if bits(w_add, 23, 22) != 0:
        return None
    # find the GPR register-offset load of rn or rm (skipping unrelated instructions)
    load = None
    for a in window:
        if a >= add_a:
            continue
        w = mod.word(a)
        if w & 0x3B200C00 == 0x38200800 and not bit(w, 26) and bits(w, 4, 0) in (rn, rm):
            size, opc = bits(w, 31, 30), bits(w, 23, 22)
            if opc == 0:
                return None
            load = (a, bits(w, 4, 0), size, opc, bits(w, 9, 5), bits(w, 20, 16), bit(w, 12))
            break
        if writes_reg(w, rn) or writes_reg(w, rm):
            return None
    if load is None:
        return None
    la, rt, size, opc, tbl_reg, idx_reg, scaled = load
    entry_size = 1 << size
    signed = opc in (2, 3)
    base_reg = rm if rt == rn else rn
    df = const_dataflow(mod, fstart, fend)
    st_load = df.get(la, {})
    st_add = df.get(add_a, {})
    if tbl_reg not in st_load or base_reg not in st_add:
        return None
    table = st_load[tbl_reg]
    base = st_add[base_reg]
    # bound: the guarding `b.hi/b.hs/b.gt default` before the load, and the compare that
    # feeds it (the compiler may compare a copy of the index register).
    count = None
    for a in range(la - 4, max(fstart, la - 4 * 24) - 4, -4):
        w = mod.word(a)
        if w & 0xFF000010 == 0x54000000 and bits(w, 3, 0) in (8, 2, 12):
            cond = bits(w, 3, 0)
            for b in range(a - 4, max(fstart, a - 4 * 8) - 4, -4):
                wb = mod.word(b)
                if wb & 0x7F80001F == 0x7100001F:  # cmp Wn/Xn, #imm
                    k = bits(wb, 21, 10) << (12 if bit(wb, 22) else 0)
                    count = k if cond == 2 else k + 1
                    break
                if wb & 0x7FE00C10 == 0x7A400800:  # ccmp Wn, #imm5, #nzcv, cond
                    k = bits(wb, 20, 16)
                    count = k if cond == 2 else k + 1
                    break
                if (wb >> 29) & 1 and bits(wb, 28, 24) in (0b10001, 0b01011, 0b01010, 0b11010):
                    break  # another flag-setting instruction: give up
            break
        if w & 0x7C000000 == 0x14000000 or w & 0xFE000000 == 0xD6000000:
            break
    bounded = count is not None
    if count is not None and (count <= 0 or count > 4096):
        return None
    if count is None:
        # Bound not provable locally: read entries while they land inside the function.
        # Extra entries only add unused switch cases, so over-reading is harmless.
        count = 1024
    targets = []
    for i in range(count):
        ea = table + i * entry_size
        off = ea - mod.base
        if off < 0 or off + entry_size > len(mod.m.image):
            return None
        raw = int.from_bytes(mod.m.image[off:off + entry_size], "little", signed=signed)
        tgt = (base + (raw << shift_amt)) & (2 ** 64 - 1)
        if not bounded and not (fstart <= tgt < fend and tgt & 3 == 0):
            break
        if not mod.in_text(tgt):
            return None
        targets.append(tgt)
    return targets or None


def writes_reg(w, r):
    """Does instruction w write general-purpose register r (x0-x30)?"""
    if r == 31:
        return False
    rt = bits(w, 4, 0)
    op0 = bits(w, 28, 25)
    if op0 & 0b0101 == 0b0100:  # loads and stores
        rn = bits(w, 9, 5)
        if w & 0x3A000000 == 0x28000000 and bits(w, 24, 23) in (1, 3) and rn == r:
            return True
        if w & 0x3B200000 == 0x38000000 and bits(w, 11, 10) in (1, 3) and rn == r:
            return True
        if w & 0xBF800000 in (0x0C800000, 0x0D800000) and rn == r:
            return True
        if w & 0x3F000000 == 0x08000000:
            if not bit(w, 22) and not bit(w, 23):
                return bits(w, 20, 16) == r
            if bit(w, 22):
                return rt == r or (bit(w, 21) and bits(w, 14, 10) == r)
            return False
        if bit(w, 26):
            return False
        if w & 0x3B000000 == 0x18000000:
            return rt == r and bits(w, 31, 30) != 3
        if w & 0x3A000000 == 0x28000000:
            return bool(bit(w, 22)) and (rt == r or bits(w, 14, 10) == r)
        if w & 0x3B000000 in (0x39000000, 0x38000000):
            return rt == r and bits(w, 23, 22) != 0 and not (bits(w, 31, 30) == 3 and bits(w, 23, 22) == 2)
        return False
    if op0 & 0b1110 == 0b1010:
        if w & 0x7C000000 == 0x14000000 and bit(w, 31):
            return r == 30
        if w & 0xFFFFFC1F == 0xD63F0000:
            return r == 30
        if w & 0xFFF00000 == 0xD5300000:
            return rt == r
        return False
    if op0 & 0b0111 == 0b0111:
        if rt != r:
            return False
        if w & 0x5F200000 == 0x1E200000 and bits(w, 15, 10) == 0:
            return bits(w, 18, 16) in (0, 1, 4, 5, 6)
        if w & 0x5F200000 == 0x1E000000:
            return bits(w, 18, 17) == 0
        if w & 0xBFE08400 == 0x0E000400 and bits(w, 14, 11) in (5, 7):
            return True
        return False
    return rt == r


_CONST_CACHE = {}


def const_dataflow(mod, fstart, fend):
    """Forward constant propagation of ADRP/ADR/ADD-imm/MOV-reg values over the function CFG.
    Returns {addr: {reg: value}} holding the state *before* each instruction."""
    key = (fstart, fend)
    if key in _CONST_CACHE:
        return _CONST_CACHE[key]
    succ = {}
    leaders = {fstart}
    for a in range(fstart, fend, 4):
        t = mod.insn(a)
        if isinstance(t, tuple):
            continue
        if t.kind in ("b", "bcond") and fstart <= t.target < fend:
            leaders.add(t.target)
            leaders.add(a + 4)
        elif t.kind in ("br", "ret", "brk", "udf", "b"):
            leaders.add(a + 4)
    state_in = {fstart: {}}
    work = [fstart]
    result = {}
    visits = collections.Counter()
    pending_leaders = sorted(leaders)
    weak = set()  # blocks seeded without known predecessors: never weaken real facts
    while work or pending_leaders:
        if not work:
            # blocks only reachable through indirect branches: start them with no knowledge
            lb = pending_leaders.pop()
            if lb in state_in or not (fstart <= lb < fend):
                continue
            state_in[lb] = {}
            weak.add(lb)
            work.append(lb)
            continue
        blk = work.pop()
        visits[blk] += 1
        if visits[blk] > 50:
            continue
        st = dict(state_in[blk])
        a = blk
        while a < fend:
            result[a] = st
            w = mod.word(a)
            t = mod.insn(a)
            st = dict(st)
            succs = []
            term = False
            if not isinstance(t, tuple):
                if t.kind in ("b", "bcond") and fstart <= t.target < fend:
                    succs.append(t.target)
                if t.kind in ("b", "br", "ret", "brk", "udf"):
                    term = True
                if t.kind in ("bl", "blr"):
                    for r in list(st):
                        if r <= 18 or r == 30:
                            del st[r]
            if w & 0x9F000000 == 0x90000000:
                imm = sext((bits(w, 23, 5) << 2) | bits(w, 30, 29), 21)
                st[bits(w, 4, 0)] = (a & ~0xFFF) + (imm << 12)
            elif w & 0x9F000000 == 0x10000000:
                st[bits(w, 4, 0)] = a + sext((bits(w, 23, 5) << 2) | bits(w, 30, 29), 21)
            elif w & 0xFF800000 == 0x91000000:  # add xd, xn, #imm
                d, n = bits(w, 4, 0), bits(w, 9, 5)
                if n in st and d != 31:
                    st[d] = st[n] + (bits(w, 21, 10) << (12 if bit(w, 22) else 0))
                else:
                    st.pop(d, None)
            elif w & 0xFFE0FFE0 == 0xAA0003E0:  # mov xd, xm
                d, m_ = bits(w, 4, 0), bits(w, 20, 16)
                if m_ in st:
                    st[d] = st[m_]
                else:
                    st.pop(d, None)
            else:
                for r in list(st):
                    if writes_reg(w, r):
                        del st[r]
            nxt = a + 4
            if not term and nxt < fend and nxt not in leaders:
                a = nxt
                continue
            if not term and nxt < fend:
                succs.append(nxt)
            for sb in succs:
                old = state_in.get(sb)
                if old is None:
                    state_in[sb] = dict(st)
                    if blk in weak:
                        weak.add(sb)
                    work.append(sb)
                elif blk in weak and sb not in weak:
                    pass
                else:
                    if sb in weak and blk not in weak:
                        weak.discard(sb)
                        state_in[sb] = dict(st)
                        work.append(sb)
                        continue
                    merged = {k: v for k, v in old.items() if st.get(k) == v}
                    if merged != old:
                        state_in[sb] = merged
                        work.append(sb)
            break
    _CONST_CACHE[key] = result
    return result


def is_sp_restore(w):
    """LDP/LDR (load) with SP as base register."""
    if bits(w, 9, 5) != 31:
        return False
    if w & 0x3A000000 == 0x28000000:
        return bool(bit(w, 22))
    if w & 0x3B000000 in (0x39000000, 0x38000000):
        return bits(w, 23, 22) == 1
    return False


def function_constant(mod, r, fstart, fend):
    """If register r is only ever written in [fstart, fend) by `adrp r` (+ `add r, r, #imm`)
    or `adr r`, return that constant (loop-invariant table base hoisted by the compiler)."""
    val = None
    defs = 0
    page = None
    for a in range(fstart, fend, 4):
        w = mod.word(a)
        if not writes_reg(w, r):
            continue
        if is_sp_restore(w):
            continue  # callee-saved register restore on an exit path
        if w & 0x9F000000 == 0x90000000:
            imm = sext((bits(w, 23, 5) << 2) | bits(w, 30, 29), 21)
            page = (a & ~0xFFF) + (imm << 12)
            cand = page
        elif w & 0x9F000000 == 0x10000000:
            cand = a + sext((bits(w, 23, 5) << 2) | bits(w, 30, 29), 21)
        elif w & 0xFF800000 == 0x91000000 and bits(w, 9, 5) == r and page is not None:
            cand = page + (bits(w, 21, 10) << (12 if bit(w, 22) else 0))
            if val == page:
                val = cand
                continue
            return None
        else:
            return None
        if val is not None and val != cand:
            return None
        val = cand
        defs += 1
    return val


# ------------------------------------------------------------------ discovery

def discover(mod, plt_stubs, log, extra=()):
    m = mod.m
    fdes = [(mod.base + s, mod.base + e) for s, e in parse_fdes(m)]
    fde_by_start = {}
    for s, e in fdes:
        if mod.in_text(s):
            fde_by_start.setdefault(s, e)
    fde_sorted = sorted(fde_by_start.items())
    fde_starts = [s for s, _ in fde_sorted]
    seeds = set(fde_by_start)
    seeds.add(mod.text_lo)
    for a in m.init_array + m.fini_array:
        seeds.add(mod.u64(mod.base + a))
    for s in m.symbols:
        if s.defined and s.type == 2 and s.value:
            seeds.add(mod.base + s.value)
    # absolute code pointers in relocated data (vtables, callback tables)
    data_ptrs = 0
    for r_off, r_type, r_sym, r_add in m.relas:
        if r_type == R_AARCH64_RELATIVE:
            tgt = mod.base + r_add
            if mod.in_text(tgt):
                seeds.add(tgt)
                data_ptrs += 1
    # PLT0: the unrelocated JUMP_SLOT entries all point at the lazy-binding resolver stub
    plt0 = {mod.base + v for v in m.jump_slot_init.values() if mod.in_text(mod.base + v)}
    seeds |= plt0
    seeds.update(plt_stubs)
    seeds.update(extra)  # runtime-discovered entry points (extra_entries.txt)
    seeds = {s for s in seeds if mod.in_text(s)}
    log(f"  seeds: {len(fde_by_start)} FDEs, {data_ptrs} data code-pointers, {len(plt_stubs)} PLT stubs, "
        f"PLT0 {', '.join(hex(x) for x in sorted(plt0)) or 'none'}")

    def containing_fde(a):
        i = bisect.bisect_right(fde_starts, a) - 1
        if i >= 0 and fde_sorted[i][0] <= a < fde_sorted[i][1]:
            return fde_sorted[i]
        return None

    funcs = {}
    pending = sorted(seeds)
    known = set(seeds)
    sorted_known = sorted(known)

    def extent(start):
        fde = containing_fde(start)
        if fde:
            return fde[1]
        i = bisect.bisect_right(sorted_known, start)
        nxt = sorted_known[i] if i < len(sorted_known) else mod.text_hi
        # also stop at the next FDE start
        j = bisect.bisect_right(fde_starts, start)
        if j < len(fde_starts):
            nxt = min(nxt, fde_starts[j])
        return nxt

    rounds = 0
    while pending:
        rounds += 1
        new = set()
        for start in pending:
            if start in plt_stubs:
                funcs[start] = start + 16
                continue
            end = extent(start)
            funcs[start] = end
            adrp_regs = {}
            for a in range(start, end, 4):
                t = mod.insn(a)
                if isinstance(t, tuple):
                    continue
                w = mod.word(a)
                if t.kind == "bl" and mod.in_text(t.target):
                    new.add(t.target)
                elif t.kind in ("b", "bcond") and mod.in_text(t.target) and not (start <= t.target < end):
                    new.add(t.target)
                # ADRP/ADR + ADD code references (function pointers passed as arguments)
                if w & 0x9F000000 == 0x90000000:
                    imm = sext((bits(w, 23, 5) << 2) | bits(w, 30, 29), 21)
                    adrp_regs[bits(w, 4, 0)] = (a & ~0xFFF) + (imm << 12)
                elif w & 0xFF800000 == 0x91000000 and bits(w, 9, 5) in adrp_regs:
                    tgt = adrp_regs[bits(w, 9, 5)] + (bits(w, 21, 10) << (12 if bit(w, 22) else 0))
                    if mod.in_text(tgt) and not (start <= tgt < end):
                        new.add(tgt)
                elif w & 0x9F000000 == 0x10000000:
                    tgt = a + sext((bits(w, 23, 5) << 2) | bits(w, 30, 29), 21)
                    if mod.in_text(tgt) and not (start <= tgt < end):
                        new.add(tgt)
        new -= known
        if new:
            known |= new
            sorted_known = sorted(known)
            # non-FDE functions may have shrunk: recompute every function lacking an FDE
            pending = sorted(new | {s for s in funcs if s not in fde_by_start and s not in plt_stubs})
        else:
            pending = []
    log(f"  discovery converged after {rounds} rounds: {len(funcs)} functions")
    return funcs, fde_by_start


# ------------------------------------------------------------------ emission

# Segment markers in a function's instruction list (Emitter.emit_function).
EXIT = object()    # control leaves the function here: Ctx must hold every register
CALL = object()    # a call follows: Ctx must hold every register
RELOAD = object()  # the call returned: registers may have changed in Ctx

_REG_USE = re.compile(r"c->x\[(\d+)\]|c->sp\b")
_REG_WRITE = re.compile(r"(?:c->x\[(\d+)\]|c->(sp)) = (?!=)|&c->x\[(\d+)\]")


# x9-x15: a callee never reads them (AAPCS64 temporaries), so a BL/BLR need not write them back first.
_CALL_SCRATCH = sum(1 << r for r in range(9, 16))


def _reg_bit(m):
    """Bit of the register a _REG_USE/_REG_WRITE match names: x0-x30 by number, SP as bit 31."""
    n = next((g for g in m.groups() if g is not None), "sp")
    return 1 << (31 if n == "sp" else int(n))


def _reg_name(b):
    return "sp_" if b == 31 else f"x{b}_"


def _reg_field(b):
    return "c->sp" if b == 31 else f"c->x[{b}]"


def plain_emit(insns, labels, segs, succ, head, tail):
    """Registers live in Ctx: every segment's text as is, one line per instruction."""
    out = ["  " + h for h in head]
    for (a, _), s in zip(insns, segs):
        if a in labels:
            out.append(f"L_{a:x}: ;")
        text = " ".join(x for x in s if isinstance(x, str))
        if text:
            out.append("  " + text)
    text = " ".join(x for x in tail if isinstance(x, str))
    if text:
        out.append("  " + text)
    return "".join(x + "\n" for x in out)


def _reads_writes(text):
    """(registers read, registers written) by a segment's C text. A register written and then read within the
    same text counts as read too, which only costs an unneeded load."""
    w, wpos = 0, set()
    for m in _REG_WRITE.finditer(text):
        w |= _reg_bit(m)
        wpos.add(m.start())
    r = 0
    for m in _REG_USE.finditer(text):
        if m.start() not in wpos:
            r |= _reg_bit(m)
    return r, w


def regcache_emit(insns, labels, segs, succ, head, tail):
    """Registers live in C locals (x<n>_, sp_), so the compiler keeps them in host registers: guest stores, which
    may alias Ctx, no longer force them to be reloaded. Ctx is brought up to date only where the function is
    left or calls out (EXIT, CALL), with the registers that may have changed since the last such point (a forward
    may-dataflow over the instructions); after a call (RELOAD) the locals read later are loaded again (a backward
    liveness pass), as are those read before any write at entry."""
    n = len(segs)
    rw = [[_reads_writes(x) if isinstance(x, str) else (0, 0) for x in s] for s in segs]
    used = 0
    for s in rw:
        for r, w in s:
            used |= r | w
    if not used:
        return plain_emit(insns, labels, segs, succ, head, tail)

    # Forward: registers possibly changed since the last sync, before each instruction.
    def forward(i, dirty):
        for j, x in enumerate(segs[i]):
            if x is RELOAD:
                dirty = 0
            elif isinstance(x, str):
                dirty |= rw[i][j][1]
        return dirty

    # Every instruction starts the walk: code that only a resume (dynamic jump into the function) or nothing
    # reaches still passes its writes on to the instructions after it.
    din = [0] * n
    work = list(range(n - 1, -1, -1))
    while work:
        i = work.pop()
        d = forward(i, din[i])
        for k in succ[i]:
            if din[k] | d != din[k]:
                din[k] |= d
                work.append(k)
    last = forward(n - 1, din[n - 1])

    # Backward: registers whose local value is read later (a sync stores its whole dirty set, so it reads it).
    # Branches inside an instruction: a conditional exit ("if (cond) { <exit> tail }") and a jump table's switch,
    # which come before the instruction's EXIT, continue at its successors.
    branchy = {i for i, (a, t) in enumerate(insns) if not isinstance(t, tuple) and t.kind in ("bcond", "br")}
    # What a CALL writes back: everything for an SVC (the kernel takes any register), no temporaries for BL/BLR.
    call_mask = [~_CALL_SCRATCH if not isinstance(t, tuple) and t.kind in ("bl", "blr") else ~0 for a, t in insns]

    def backward(i, out_live):
        dirty_at = []
        d = din[i]
        for j, x in enumerate(segs[i]):
            dirty_at.append(d)
            if x is RELOAD:
                d = 0
            elif isinstance(x, str):
                d |= rw[i][j][1]
        live, reload_live = out_live, {}
        for j in range(len(segs[i]) - 1, -1, -1):
            x = segs[i][j]
            if x is EXIT:
                live = dirty_at[j]  # an exit does not continue here
            elif x is CALL:
                live = dirty_at[j] & call_mask[i]  # after a call, what is read again is reloaded
            elif x is RELOAD:
                reload_live[j] = live
                live = 0
            else:
                r, w = rw[i][j]
                live = (live & ~w) | r
        if i in branchy:
            live |= out_live  # their segments before the EXIT write nothing
        return live, reload_live

    def live_out(i):
        v = 0
        for k in succ[i]:
            v |= lin[k]
        if i == n - 1 and tail:
            v |= last
        return v

    lin = [0] * n
    changed = True
    while changed:
        changed = False
        for i in range(n - 1, -1, -1):
            live, _ = backward(i, live_out(i))
            if live != lin[i]:
                lin[i] = live
                changed = True

    rename = lambda text: _REG_USE.sub(lambda m: _reg_name(31 if m.group(0) == "c->sp" else int(m.group(1))), text)
    regs = [b for b in range(32) if used >> b & 1]
    store = lambda d: " ".join(f"{_reg_field(b)} = {_reg_name(b)};" for b in regs if d >> b & 1)
    load = lambda d: " ".join(f"{_reg_name(b)} = {_reg_field(b)};" for b in regs if d >> b & 1)
    # Entry: what the first instruction reads before writing; resumable functions may start at any label.
    entry = lin[0] if not head else used
    out = ["  u64 " + ", ".join(_reg_name(b) + (f" = {_reg_field(b)}" if entry >> b & 1 else "") for b in regs) + ";"]
    out += ["  " + h for h in head]
    for i, ((a, _), s) in enumerate(zip(insns, segs)):
        if a in labels:
            out.append(f"L_{a:x}: ;")
        _, reload_live = backward(i, live_out(i))
        parts, dirty = [], din[i]
        for j, x in enumerate(s):
            if x is EXIT or x is CALL:
                if dirty & (call_mask[i] if x is CALL else ~0):
                    parts.append(store(dirty & (call_mask[i] if x is CALL else ~0)))
            elif x is RELOAD:
                dirty = 0
                parts.append(load(reload_live[j]))
            else:
                parts.append(rename(x))
                dirty |= rw[i][j][1]
        text = " ".join(p for p in parts if p)
        if text:
            out.append("  " + text)
    if tail:
        parts = [store(last) if x is EXIT else rename(x) for x in tail]
        out.append("  " + " ".join(p for p in parts if p))
    return "".join(x + "\n" for x in out)


class Emitter:
    def __init__(self, mod, funcs, plt_stubs, log, regcache=True):
        self.mod = mod
        self.funcs = funcs
        self.plt = plt_stubs
        self.log = log
        self.regcache = regcache
        self.stats = collections.Counter()
        self.imports_used = set()

    def call_expr(self, target):
        if target in self.plt:
            name = self.plt[target]
            self.imports_used.add(name)
            return f"hle_{name}(c);"
        if target in self.funcs:
            return f"{fn_name(target)}(c);"
        self.stats["call_unknown"] += 1
        return f"hw_call(c, {hx(target)});"

    def tail_expr(self, target):
        if target in self.plt:
            name = self.plt[target]
            self.imports_used.add(name)
            return f"HW_MUSTTAIL return hle_{name}(c);"
        if target in self.funcs:
            return f"HW_MUSTTAIL return {fn_name(target)}(c);"
        self.stats["tail_unknown"] += 1
        return f"c->pc = {hx(target)}; HW_MUSTTAIL return hw_dispatch(c);"

    def emit_function(self, start, end):
        mod = self.mod
        if start in self.plt:
            name = self.plt[start]
            self.imports_used.add(name)
            return f"void {fn_name(start)}(Ctx* restrict c) {{ HW_MUSTTAIL return hle_{name}(c); }}\n"
        labels = set()
        jtables = {}
        insns = []
        for a in range(start, end, 4):
            t = mod.insn(a)
            insns.append((a, t))
            if isinstance(t, tuple):
                continue
            if t.kind in ("b", "bcond") and start <= t.target < end:
                labels.add(t.target)
            elif t.kind == "br":
                tg = recover_jump_table(mod, a, start, end)
                if tg is not None and all(start <= x < end for x in tg):
                    jtables[a] = tg
                    labels.update(tg)
                    self.stats["jumptable"] += 1
                elif tg is not None:
                    self.stats["jumptable_external"] += 1
        # A function with an indirect branch we could not resolve may be entered at any
        # instruction at runtime (a missed jump table). Give it a resume prologue: the dispatcher
        # sets c->pc = target | 1 and calls the function, which jumps straight to the label.
        dynamic_br = [a for a, t in insns if not isinstance(t, tuple) and t.kind == "br" and a not in jtables]
        if dynamic_br:
            labels = {a for a, _ in insns}
            self.__dict__.setdefault("resumable", []).append(start)
            self.stats["resumable"] += 1
        # Each instruction becomes segments: C text, EXIT (write back the registers changed since the last sync,
        # on a path that leaves the function) or CALL (write back, call, reload: the callee may use and change
        # any register). succ lists the instructions control may continue at.
        idx = {a: i for i, (a, _) in enumerate(insns)}
        segs, succ = [], []
        for i, (a, t) in enumerate(insns):
            nxt = [i + 1] if i + 1 < len(insns) else []
            if isinstance(t, tuple):
                _, w, why = t
                if w == 0 or (w >> 16) == 0:
                    s = [EXIT, f"hw_brk(c, {hx(a)}, 0xFFFF); return;"]
                else:
                    s = [EXIT, f"hw_unimpl(c, {hx(a)}, 0x{w:08X}u); return;"]
                    self.stats["unimpl"] += 1
                segs.append(s)
                succ.append([])
                continue
            k = t.kind
            if k is None:
                s = [t.c] if t.c else []
            elif k == "b":
                if start <= t.target < end:
                    s, nxt = [f"goto L_{t.target:x};"], [idx[t.target]]
                else:
                    s, nxt = [EXIT, self.tail_expr(t.target)], []
            elif k == "bcond":
                if start <= t.target < end:
                    s = [f"if ({t.cond}) goto L_{t.target:x};"]
                    nxt = nxt + [idx[t.target]]
                else:
                    s = [f"if ({t.cond}) {{", EXIT, f"{self.tail_expr(t.target)} }}"]
            # After a call, x30 holds the address the callee returned to. If that is not our
            # continuation (PIC "bl to get PC" tricks, longjmp), unwind: return to C frames above
            # until the frame whose call site matches.
            elif k == "bl":
                s = [f"c->x[30] = {hx(a + 4)};", CALL, self.call_expr(t.target), RELOAD,
                     f"if (c->x[30] != {hx(a + 4)}) return;"]
            elif k == "blr":
                reg = f"c->x[{t.reg}]" if t.reg != 31 else "0"
                s = [f"{{ u64 t_ = {reg}; c->x[30] = {hx(a + 4)};", CALL, "hw_call(c, t_); }", RELOAD,
                     f"if (c->x[30] != {hx(a + 4)}) return;"]
            elif k == "br":
                reg = f"c->x[{t.reg}]" if t.reg != 31 else "0"
                s = []
                if a in jtables:
                    cases = " ".join(f"case {hx(x)}: goto L_{x:x};" for x in sorted(set(jtables[a])))
                    s.append(f"switch ({reg}) {{ {cases} default: break; }}")
                    nxt = sorted({idx[x] for x in jtables[a]})
                else:
                    nxt = []
                s += [f"c->pc = {reg};", EXIT, "HW_MUSTTAIL return hw_dispatch(c);"]
                self.stats["br_dynamic" if a not in jtables else "br_table"] += 1
            elif k == "ret":
                nxt = []
                if t.reg == 30:
                    s = [EXIT, "return;"]
                else:
                    s = [f"c->pc = c->x[{t.reg}];", EXIT, "HW_MUSTTAIL return hw_dispatch(c);"]
            elif k == "svc":
                s = [CALL, t.c, RELOAD]
            elif k in ("brk", "udf"):
                s, nxt = [EXIT, f"{t.c or f'hw_brk(c, {hx(a)}, 0);'} return;"], []
            else:
                raise RuntimeError(k)
            segs.append(s)
            succ.append(nxt)
        # fallthrough off the end of the function
        last_t = insns[-1][1] if insns else None
        terminal = (not isinstance(last_t, tuple) and last_t is not None and
                    last_t.kind in ("b", "br", "ret", "brk", "udf"))
        tail = []
        if not terminal:
            tail = [EXIT, self.tail_expr(end)]
            self.stats["fallthrough"] += 1
        head = []
        if dynamic_br:
            cases = " ".join(f"case {hx(x)}: goto L_{x:x};" for x in sorted(labels))
            head.append(f"if (c->pc & 1) {{ u64 p_ = c->pc & ~1ull; c->pc = 0; switch (p_) {{ {cases} default: break; }} }}")
        body = (regcache_emit if self.regcache else plain_emit)(insns, labels, segs, succ, head, tail)
        return f"void {fn_name(start)}(Ctx* restrict c) {{\n" + body + "}\n"


