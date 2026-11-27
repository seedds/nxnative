"""Offset jump tables proved by dataflow over a function's control flow.

A dispatch is `br xD` where xD = C + (entry << s), the entry loaded from a table (ldrsw / ldrb / ldrh / ldrsb / ldrsh [xB, xI ...]) and C a constant (the table base itself, or an
adr). One forward analysis over the function proves, for every such dispatch at once, which values the index can take at the load and which constant the base holds, on every
path that reaches it. The tables' entries then become edges of the control flow, and the analysis runs again until no table changes.

The index domain is relational where dispatches need it. A register holds a symbolic value: a root (an unknown value, created where a register was written or where paths with
different values join) and a chain of operations applied to it (a 32-bit truncation, an and with a mask, a complement, an add). Each root carries its range, the set of its
possible low 12 bits, and any narrowed ranges on masked projections of it (such as its low 32 bits or a 16-bit mask). A guard (cmp / b.cond, tst / b.cond, cbz, tbz) refines the root, so every register derived from it narrows at once: the bound reaches the index whether the
test comes before the mask (ULTRA STREET FIGHTER II: tbnz w17,#3; and x18,x17,#0xf), after it (Manual Samuel: and w13,w17,#7; tbnz w17,#2), on a copy made earlier (DRAGON QUEST
XI S: mov w0,w18 ... cmp w18,#4; b.hi), on a masked copy made in another block (FINAL FANTASY CRYSTAL CHRONICLES: and x6,x17,#0x7ff there; and w18,w17,#0x7ff; cmp w18,#3; b.hi
here), or before a complement (Return of the Obra Dinn: tst w17,#7; b.eq; mvn w17,w17; and x18,x17,#7). An output that depends only on its root's low 12 bits (any chain with an
and of at most 0xfff) is computed exactly.

Control enters a function at its start, at the entries the caller names (direct branches from other functions, targets of tables elsewhere), all with every register
unknown, and at its C++ exception landing pads (from the LSDA), with what the calls they catch left: the callee-saved registers as they were. An edge carrying an unchecked
index into a guarded run makes the bound fail by itself. Calls and system calls clobber x0-x18 and x30. Provably local fixed stack slots inside the function's allocated frame
survive calls and reloads as long as no stack pointer escapes to a call, memory, or an unhandled operation and no overlapping store writes the slot. A dispatch through any register counts: its targets are edges (a byte
or halfword table whose index stays unbounded lands on every pad it can). A br that reads no table (an indirect tail call) leaves the function, and code that nothing above
reaches is dead to the analysis, as both are to the rewriter's own sites. A number (mov) inside page zero is never a table base: Horizon never maps page zero, so no path of a
working game carries it to a load (Cuphead 1.3.7 main+0x59dc90 interleaves four dispatch loops that share x12 as a base and as the constant 1).

There are two readings (solve). The first knows every number the code writes and gives the control flow. The second reads the br x18 tables again on that control flow with
those numbers unknown, so that each table's size is what its guards allow however control reaches it.

solve() returns {br address: targets} for every dispatch it proves and {br address: reason} for the others."""
import struct
from functools import lru_cache
from typing import Any
from capstone import CS_GRP_JUMP, CS_GRP_CALL, CS_GRP_RET, CS_GRP_INT
from capstone.arm64 import ARM64_OP_IMM, ARM64_OP_REG, ARM64_OP_MEM, ARM64_EXT_UXTW, ARM64_EXT_SXTW, ARM64_EXT_INVALID, ARM64_EXT_UXTX, ARM64_EXT_SXTX, ARM64_SFT_LSL

M32, M64 = (1 << 32) - 1, (1 << 64) - 1
LOW = 0xfff                 # roots track their low 12 bits as a set
FULL = (1 << 4096) - 1
MAXC = 8                    # constants a register may hold before it is unknown
MAXN = 1 << 16              # the most entries a table may have
MAXS = 64                   # tracked local stack slots per state before evicting the oldest
ENUM = 1 << 12              # a root range this small is enumerated exactly
WIDEN = 6                   # visits of an entry point before growing ranges and constant sets widen to unknown
OUTER = 16                  # rounds of table edges before giving up
STEPS = 3_000_000           # instructions interpreted per round before giving up

def _mask(w): return M32 if w == 32 else M64

def _pattern(lo, hi):
    """The low-12-bit set of the values lo..hi"""
    if hi < lo: return 0
    if hi - lo >= LOW: return FULL
    a, b = lo & LOW, hi & LOW
    if a <= b: return ((1 << (b - a + 1)) - 1) << a
    return ((1 << (b + 1)) - 1) | (((1 << (4096 - a)) - 1) << a)

def _bits(bs):
    """The members of a 4096-bit set, ascending"""
    return [i for i, c in enumerate(reversed(bin(bs)[2:])) if c == '1'] if bs else []

def _apply(chain, v):
    for op in chain:
        if op[0] == 'w': v &= M32
        elif op[0] == 'and': v &= op[1]
        elif op[0] == 'not': v = ~v & _mask(op[1])
        else: v = (v + op[1]) & _mask(op[2])
    return v

def _masked(chain): return any(op[0] == 'and' and op[1] <= LOW for op in chain)

def _top(chain, w):
    """An upper bound of a chain's output for a root of width w"""
    hi = _mask(w)
    for op in chain:
        if op[0] == 'w': hi = min(hi, M32)
        elif op[0] == 'and': hi = min(hi, op[1])
        else: hi = _mask(op[-1])
    return hi

def _mask_prefix(chain, root):
    """(count of leading w/and ops, combined mask of those ops and root's width)"""
    m = M64 if len(root) == 1 else _mask(root[-1])
    p = 0
    while p < len(chain) and chain[p][0] in ('w', 'and'):
        m &= M32 if chain[p][0] == 'w' else chain[p][1]
        p += 1
    return p, m

class Abs:
    """{x : lo <= x <= hi and x & 0xfff in bits}"""
    __slots__ = ('lo', 'hi', 'bits')
    def __init__(self, lo, hi, bits): self.lo, self.hi, self.bits = lo, hi, bits & _pattern(lo, hi)
    @staticmethod
    def of(vals):
        vals = list(vals)
        if not vals: return Abs(1, 0, 0)
        bs = 0
        for v in vals: bs |= 1 << (v & LOW)
        return Abs(min(vals), max(vals), bs)
    def empty(self): return self.lo > self.hi or not self.bits
    def members(self):
        """The values, when few enough to list"""
        if self.empty(): return []
        if self.hi - self.lo < ENUM: return [v for v in range(self.lo, self.hi + 1) if self.bits >> (v & LOW) & 1]
        return None
    def union(self, o):
        if self.empty(): return o
        if o.empty(): return self
        return Abs(min(self.lo, o.lo), max(self.hi, o.hi), self.bits | o.bits)
    def con(self): return _mk(self.lo, self.hi, self.bits)

def _mk(lo, hi, bs, masks=()):
    """A root's constraint: its range, the set of its low 12 bits, and sorted (mask, lo, hi) bounds on (root & mask) (e.g. low 32 bits or a 16-bit mask)"""
    out = {}
    for m, mlo, mhi in masks:
        mlo, mhi = max(mlo, 0), min(mhi, m)
        if m in out: mlo, mhi = max(out[m][0], mlo), min(out[m][1], mhi)
        if mlo > mhi: return (1, 0, 0, ())
        out[m] = (mlo, mhi)
    kept = []
    for m, (mlo, mhi) in sorted(out.items(), reverse=True):
        if not m & (m + 1) and hi <= m:
            lo, hi = max(lo, mlo), min(hi, mhi)
            continue
        if (m & LOW) == LOW: bs &= _pattern(mlo, mhi)
        if mlo > 0 or mhi < m: kept.append((m, mlo, mhi))
    bs &= _pattern(lo, hi)
    if lo > hi or not bs: return (1, 0, 0, ())
    return (lo, hi, bs, tuple(reversed(kept)))

def _mrange(con, m):
    """(lo, hi) of (root & m) under root constraint con"""
    lo, hi, _, masks = con
    mlo, mhi = (lo, hi) if hi <= m and not m & (m + 1) else (0, min(hi, m))
    for km, klo, khi in masks:
        if km == m: mlo, mhi = max(mlo, klo), min(mhi, khi)
        elif not m & ~km:   # m is a submask of km: (x & m) <= (x & km), and equal when every extra bit of km lies above khi
            diff = km & ~m
            if khi < (diff & -diff): mlo, mhi = max(mlo, klo), min(mhi, khi)
            else: mhi = min(mhi, khi)
    return mlo, mhi

def _cunion(a, b):
    ms = []
    if a[3] or b[3]:
        for m in {x[0] for x in a[3]} | {x[0] for x in b[3]}:
            la, ha = _mrange(a, m); lb, hb = _mrange(b, m)
            lo, hi = min(la, lb), max(ha, hb)
            if lo > 0 or hi < m: ms.append((m, lo, hi))
    return _mk(min(a[0], b[0]), max(a[1], b[1]), a[2] | b[2], ms)

ZERO = ('c', frozenset((0,)), frozenset((0,)))

def _addresses(v):
    """The constants of v that can be a table's base: a number inside page zero cannot (Horizon never maps page zero, so no path of a working game carries it to a
    table load: Cuphead 1.3.7 main+0x59dc90 uses x12 as one loop's base and as another's constant 1)"""
    return frozenset(x for x in v[1] if x not in v[2] or x >= 0x1000)

def _default(root): return (0, M64 if len(root) == 1 else _mask(root[-1]), FULL, ())

def _widen(old, new, root):
    """new, with each part that grew since old (the range, the low 12 bits, each masked range) made unknown and the others kept: a guard on the low 32 bits
    holds while the full range keeps growing (ASTRAL CHAIN main+0x6db1a4: cmp w18,#3 on w12 - 2, joined from four blocks inside a loop)"""
    if new == old: return new
    d = _default(root)
    lo, hi = new[:2] if new[:2] == old[:2] else d[:2]
    ms = [(m, mlo, mhi) for m, mlo, mhi in new[3] if _mrange(old, m) == (mlo, mhi)]
    return _mk(lo, hi, new[2] if new[2] == old[2] else FULL, ms)

class State:
    """regs: 31 values; cons: root → (lo, hi, bits, masks) where narrower than its width; flags: (kind, value, imm, width, register) of the last compare, or None;
    frame: bytes allocated below entry sp; slots: {offset: (size, value)} of provably local fixed stack slots inside [0, frame); escaped: whether any stack pointer may have escaped.
    A value is ('c', values, numbers) constants, numbers being those of them a mov wrote rather than an adrp / adr (addresses), ('s', root, chain) symbolic (a root ('e', k, ...) is the entry the table load at instruction k gave, a number like any other), ('t', k, constants, shift) a table target, or ('sp', offset) a stack pointer sp + offset"""
    __slots__ = ('regs', 'cons', 'flags', 'frame', 'slots', 'escaped')
    def __init__(self, regs, cons, flags, frame=0, slots=None, escaped=False):
        self.regs, self.cons, self.flags = regs, cons, flags
        self.frame, self.slots, self.escaped = frame, {} if slots is None else slots, escaped
    def copy(self): return State(list(self.regs), dict(self.cons), self.flags, self.frame, dict(self.slots), self.escaped)
    def con(self, root): return self.cons.get(root) or _default(root)
    def key(self): return (tuple(self.regs), tuple(sorted(self.cons.items())), self.flags, self.frame, tuple(sorted(self.slots.items())), self.escaped)
    def tidy(self):
        live = {v[1] for v in self.regs if v[0] == 's'}
        if self.slots:
            for _, v in self.slots.values():
                if v[0] == 's': live.add(v[1])
        if self.flags and self.flags[1][0] == 's': live.add(self.flags[1][1])
        self.cons = {r: c for r, c in self.cons.items() if r in live and c != _default(r)}

def _image(bs, op):
    """The low-12-bit set of op's outputs for inputs with low bits in bs"""
    if op[0] in ('w', 'not', 'add') and bs == FULL: return FULL
    if op[0] == 'and' and bs == FULL: return Abs.of(b & op[1] for b in range(4096)).bits
    return Abs.of(_apply((op,), b) & LOW for b in _bits(bs)).bits

@lru_cache(maxsize=1 << 14)
def _masked_image(chain, bs):
    """(lo, hi, bits) of a mask-determined chain's outputs for roots whose low 12 bits lie in bs"""
    a = Abs.of(_apply(chain, b) for b in _bits(bs))
    return a.lo, a.hi, a.bits

def _eval(st, v):
    """The values v can hold, as an Abs, or None when it is not a number"""
    if v[0] == 'c': return Abs.of(v[1])
    if v[0] != 's': return None
    root, chain = v[1], v[2]
    con = st.con(root); lo, hi, bs, _ = con
    if _masked(chain): return Abs(*_masked_image(chain, bs))
    if hi - lo < ENUM: return Abs.of(_apply(chain, x) for x in range(lo, hi + 1) if bs >> (x & LOW) & 1)
    p, pm = _mask_prefix(chain, root)
    if p:
        lo, hi = _mrange(con, pm)
        ebs = bs if (pm & LOW) == LOW else _image(bs, ('and', pm))
        if hi - lo < ENUM:
            return Abs.of(_apply(chain[p:], x) for x in range(lo, hi + 1) if not (x & ~pm) and ebs >> (x & LOW) & 1)
        bs = ebs
    for op in chain[p:]:
        if op[0] == 'w': lo, hi = (lo, hi) if hi <= M32 else (0, M32)
        elif op[0] == 'and': lo, hi = 0, min(hi, op[1])
        elif op[0] == 'not':
            m = _mask(op[1]); lo, hi = (m - hi, m - lo) if hi <= m else (0, m)
        else:
            m = _mask(op[2]); lo, hi = (lo + op[1], hi + op[1]) if 0 <= lo + op[1] and hi + op[1] <= m else (0, m)
        bs = _image(bs, op)
    return Abs(lo, hi, bs)

@lru_cache(maxsize=1 << 12)
def _pred(key):
    """The test a guard's edge makes of a value, from a hashable key: ('cmp', cond, imm, width, taken), ('tst', mask, zero), ('bit', bit, zero), ('zero', zero)"""
    kind = key[0]
    if kind == 'cmp':
        c = _cond(*key[1:]); assert c is not None   # flag_edge refines only on a condition _cond reads
        return c[0]
    if kind == 'tst': return (lambda x: not x & key[1]) if key[2] else (lambda x: bool(x & key[1]))
    if kind == 'bit': return (lambda x: not x >> key[1] & 1) if key[2] else (lambda x: bool(x >> key[1] & 1))
    return (lambda x: x == 0) if key[1] else (lambda x: x != 0)

@lru_cache(maxsize=1 << 14)
def _filter(chain, bs, key, low):
    """The low-12-bit patterns of bs whose output through chain passes the test key (low: of the output's low 12 bits only)"""
    pred = _pred(key)
    return sum(1 << b for b in _bits(bs) if pred(_apply(chain, b) & LOW if low else _apply(chain, b)))

def _refine(st, v, reg, key, interval=None, low=False):
    """Narrow v (held by register reg) to the values that pass the test key (_pred); interval: the same as a range of v's values; low: the test reads only v's low
    12 bits. False when no value is left: the edge cannot be taken"""
    pred = _pred(key)
    if v[0] == 'c':
        keep = frozenset(x for x in v[1] if pred(x))
        if not keep: return False
        if reg is not None and st.regs[reg] == v: st.regs[reg] = ('c', keep, v[2] & keep)
        return True
    if v[0] != 's': return True
    root, chain = v[1], v[2]
    con = st.con(root); lo, hi, bs, masks_t = con
    masks = {m: (a, b) for m, a, b in masks_t}
    p, pm = _mask_prefix(chain, root)
    if _masked(chain): bs = _filter(chain, bs, key, False)
    elif hi - lo < ENUM:
        keep = [x for x in range(lo, hi + 1) if bs >> (x & LOW) & 1 and pred(_apply(chain, x))]
        if not keep: return False
        a = Abs.of(keep); lo, hi, bs = a.lo, a.hi, a.bits
    elif p and (mr := _mrange(con, pm))[1] - mr[0] < ENUM:
        ebs = bs if (pm & LOW) == LOW else _image(bs, ('and', pm))
        keep = [x for x in range(mr[0], mr[1] + 1) if not (x & ~pm) and ebs >> (x & LOW) & 1 and pred(_apply(chain[p:], x))]
        if not keep: return False
        a = Abs.of(keep); masks[pm] = (a.lo, a.hi)
        if (pm & LOW) == LOW: bs &= a.bits
    else:
        if low: bs = _filter(chain, bs, key, True)
        if interval is not None:
            plo, phi = interval; ok = True
            for i in range(len(chain) - 1, -1, -1):   # the root values whose output lies in plo..phi, undoing each step on the interval
                if i < p:   # chain[:p] is a mask prefix (w / and): record the narrowed range on (root & pm)
                    mlo, mhi = _mrange(con, pm)
                    mlo, mhi = max(mlo, plo), min(mhi, phi)
                    if mlo > mhi: return False
                    masks[pm] = (mlo, mhi); plo, phi = 0, M64; break
                op = chain[i]
                if op[0] == 'w':
                    if _top(chain[:i], root[-1]) > M32: ok = False; break
                elif op[0] == 'not':
                    m = _mask(op[1]); plo, phi = m - phi, m - plo
                elif op[0] == 'add':
                    m = _mask(op[2])
                    if plo - op[1] < 0 or phi - op[1] > m: ok = False; break
                    plo, phi = plo - op[1], phi - op[1]
                else: ok = False; break
            if ok: lo, hi = max(lo, plo), min(hi, phi)
            else:   # the chain cannot be undone: the registers holding exactly this value get it as a root of its own, narrowed
                a = _eval(st, v)
                if a is None: return True
                lo2, hi2 = max(a.lo, interval[0]), min(a.hi, interval[1])
                if lo2 > hi2 or not a.bits & _pattern(lo2, hi2): return False
                # A guard on a wrapping W-register expression may need a new root. Loop widening must still preserve the zeroed upper half.
                own = ('r', root, chain, 32 if _top(chain, root[-1]) <= M32 else 64)
                st.regs = [('s', own, ()) if r == v else r for r in st.regs]
                if st.slots: st.slots = {off: (sz, ('s', own, ()) if sv == v else sv) for off, (sz, sv) in st.slots.items()}
                st.cons[own] = _mk(lo2, hi2, a.bits)
                if st.flags and st.flags[1] == v: st.flags = st.flags[:1] + (('s', own, ()),) + st.flags[2:]
                return True
    c = _mk(lo, hi, bs, [(m, a, b) for m, (a, b) in masks.items()])
    if c[0] > c[1] or not c[2]: return False
    st.cons[root] = c
    return True

def _reg(p, op):
    """(number, width) of a register operand, 'zr' for wzr / xzr, None for anything else (sp, vector registers)"""
    if op.type != ARM64_OP_REG: return None
    return _name(p.reg_name(op.reg))

def _name(n):
    if n in ('xzr', 'wzr'): return 'zr'
    n = {'lr': 'x30', 'fp': 'x29'}.get(n, n)
    if n and n[0] in 'xw' and n[1:].isdigit(): return (int(n[1:]), 64 if n[0] == 'x' else 32)
    return None

def _imm(op):
    v = op.imm
    if op.shift.type == ARM64_SFT_LSL and op.shift.value: v <<= op.shift.value
    return v

TRAPS = ('brk', 'udf', 'hlt', 'eret', 'drps', 'eretaa', 'eretab')
RETS = ('ret', 'retaa', 'retab')                               # with pointer authentication (the Switch 2 SDK's sdk module: retab)
BLRS = ('blr', 'blraa', 'blrab', 'blraaz', 'blrabz')
BRS = ('braa', 'brab', 'braaz', 'brabz')
CONDS = ('eq', 'ne', 'hs', 'cs', 'lo', 'cc', 'mi', 'pl', 'vs', 'vc', 'hi', 'ls', 'ge', 'lt', 'gt', 'le', 'al', 'nv')
TABLE_LOADS = {'ldrsw': (4, True), 'ldrb': (1, False), 'ldrsb': (1, True), 'ldrh': (2, False), 'ldrsh': (2, True)}
FMT = {(4, True): '<i', (1, False): '<B', (1, True): '<b', (2, False): '<H', (2, True): '<h'}
PURE_LOADS = frozenset((
    'ldr', 'ldur', 'ldrb', 'ldurb', 'ldrh', 'ldurh', 'ldrsb', 'ldursb', 'ldrsh', 'ldursh', 'ldrsw', 'ldursw',
    'ldp', 'ldnp', 'ldpsw',
    'ldar', 'ldarb', 'ldarh', 'ldaxr', 'ldaxrb', 'ldaxrh', 'ldaxp', 'ldxr', 'ldxrb', 'ldxrh', 'ldxp',
    'ldapr', 'ldaprb', 'ldaprh', 'ldtr', 'ldtrb', 'ldtrh', 'ldtrsb', 'ldtrsh', 'ldtrsw',
    'ld1', 'ld2', 'ld3', 'ld4', 'ld1r', 'ld2r', 'ld3r', 'ld4r',
    'prfm', 'prfum',
))

def _is_sp(p, op): return op.type == ARM64_OP_REG and p.reg_name(op.reg) in ('sp', 'wsp')

def _reg_size(name):
    if name in ('xzr', 'fp', 'lr'): return 8
    if name == 'wzr': return 4
    if name and name[1:].isdigit(): return {'x': 8, 'd': 8, 'w': 4, 's': 4, 'q': 16, 'h': 2, 'b': 1}.get(name[0])
    return None

def _store_size(p, m, ops):
    if m in ('strb', 'sturb', 'stlrb', 'sttrb', 'stxrb', 'stlxrb'): return 1
    if m in ('strh', 'sturh', 'stlrh', 'sttrh', 'stxrh', 'stlxrh'): return 2
    if m in ('str', 'stur', 'stlr', 'sttr') and ops and ops[0].type == ARM64_OP_REG: return _reg_size(p.reg_name(ops[0].reg))
    if m in ('stxr', 'stlxr') and len(ops) >= 2 and ops[1].type == ARM64_OP_REG: return _reg_size(p.reg_name(ops[1].reg))
    if m in ('stp', 'stnp') and len(ops) >= 2 and ops[0].type == ARM64_OP_REG and ops[1].type == ARM64_OP_REG:
        s0, s1 = _reg_size(p.reg_name(ops[0].reg)), _reg_size(p.reg_name(ops[1].reg))
        return 2 * s0 if s0 and s0 == s1 else None
    if m in ('stxp', 'stlxp') and len(ops) >= 3 and ops[1].type == ARM64_OP_REG and ops[2].type == ARM64_OP_REG:
        s0, s1 = _reg_size(p.reg_name(ops[1].reg)), _reg_size(p.reg_name(ops[2].reg))
        return 2 * s0 if s0 and s0 == s1 else None
    return None

def _cond(cond: str, imm: int, w: int, taken: bool):
    """(pred, interval) of the compared value when b.cond after cmp value,#imm is taken (or not); None when the condition bounds nothing usable"""
    m = _mask(w); imm &= m; sign = 1 << (w - 1)
    def s(x): return x - (1 << w) if x & sign else x
    si = s(imm)
    cond = {'cs': 'hs', 'cc': 'lo'}.get(cond) or cond
    if not taken: cond = {'eq': 'ne', 'ne': 'eq', 'hi': 'ls', 'ls': 'hi', 'hs': 'lo', 'lo': 'hs', 'gt': 'le', 'le': 'gt', 'ge': 'lt', 'lt': 'ge'}.get(cond, '')
    return {'eq': (lambda x: x == imm, (imm, imm)), 'ne': (lambda x: x != imm, None),
            'hi': (lambda x: x > imm, (imm + 1, m)), 'ls': (lambda x: x <= imm, (0, imm)),
            'hs': (lambda x: x >= imm, (imm, m)), 'lo': (lambda x: x < imm, (0, imm - 1)),
            'gt': (lambda x: s(x) > si, None), 'le': (lambda x: s(x) <= si, None),
            'ge': (lambda x: s(x) >= si, None), 'lt': (lambda x: s(x) < si, None)}.get(cond)

class Unsupported(Exception): pass

class Ins:
    """An instruction with what the analysis reads of it decoded once: Capstone builds its operand objects again at every access"""
    __slots__ = ('p', 'address', 'mnemonic', 'op_str', 'operands', 'update_flags', 'writeback', '_writes', '_reads', '_control')
    def __init__(self, p):
        self.p, self.address, self.mnemonic, self.op_str = p, p.address, p.mnemonic, p.op_str
        self.operands, self.update_flags, self.writeback = p.operands, p.update_flags, p.writeback
        self._writes = self._reads = self._control = None
    def reg_name(self, r): return self.p.reg_name(r)
    def _access(self):
        if self._writes is None:
            r, w = self.p.regs_access()
            self._reads = [self.p.reg_name(x) for x in r]
            self._writes = [self.p.reg_name(x) for x in w]
    def writes(self):
        self._access()
        assert self._writes is not None
        return self._writes
    def reads(self):
        self._access()
        assert self._reads is not None
        return self._reads
    def control(self):
        if self._control is None: self._control = any(self.p.group(g) for g in (CS_GRP_JUMP, CS_GRP_CALL, CS_GRP_RET, CS_GRP_INT))
        return self._control

class Function:
    def __init__(self, insl, img, code_ok, consts):
        self.insl, self.img, self.code_ok, self.consts = insl, img, code_ok, consts
        self.at = {p.address: k for k, p in enumerate(insl)}
        self.leaders = set(); self.calls = None; self.landing = {}

    # Values
    def detach(self, st, root, n):
        """root is about to name a new value in register n: other registers and stack slots still holding it (or a value narrowed from it) from an earlier pass (a loop)
        keep that value as a root of their own"""
        def stale(r): return r == root or (r[0] == 'r' and r[1] == root)
        for i, v in enumerate(st.regs):
            if v[0] == 's' and stale(v[1]) and (i != n or v[1] != root):
                w = 64 if len(v[1]) == 1 else v[1][-1]
                old = ('x', v[1], i, w); st.regs[i] = ('s', old, v[2])
                if v[1] in st.cons: st.cons[old] = st.cons[v[1]]
        if st.slots:
            for off, (sz, v) in list(st.slots.items()):
                if v[0] == 's' and stale(v[1]):
                    w = 64 if len(v[1]) == 1 else v[1][-1]
                    old = ('xs', v[1], off, w); st.slots[off] = (sz, ('s', old, v[2]))
                    if v[1] in st.cons: st.cons[old] = st.cons[v[1]]
        if st.flags and st.flags[1][0] == 's' and stale(st.flags[1][1]): st.flags = None
        for r in [r for r in st.cons if stale(r)]: del st.cons[r]

    def fresh(self, st, k, n, w):
        """The value instruction k writes into register n when nothing better is known"""
        root = ('d', k, n, w); self.detach(st, root, n)
        return ('s', root, ())

    def trunc(self, v):
        if v[0] == 'c': return ('c', frozenset(x & M32 for x in v[1]), frozenset(x & M32 for x in v[2]))
        if v[0] == 's': return v if _top(v[2], v[1][-1]) <= M32 else ('s', v[1], v[2] + (('w',),))
        return None

    def val(self, st, r):
        """The value register operand r reads"""
        if r == 'zr': return ZERO if self.consts else ('s', ('z',), ())
        v = st.regs[r[0]]
        return (self.trunc(v) or ('?',)) if r[1] == 32 else v

    def linked(self, st, k, r):
        """The value r reads, for a value derived from it: a register holding a few small constants (a loop's state, an index) gets a root of its own holding
        exactly them, so that a later test of the register narrows what was derived from it (Manual Samuel: mov w17,#4 ... and w13,w17,#7; tbnz w17,#2)"""
        if r != 'zr' and (v := st.regs[r[0]])[0] == 'c' and v[1] == v[2] and max(v[1]) < ENUM:
            root = ('k', k, r[0], 64)
            self.detach(st, root, r[0])
            st.regs[r[0]] = ('s', root, ()); st.cons[root] = Abs.of(v[1]).con()
            if st.flags and st.flags[4] == r[0] and st.flags[1] == v: st.flags = None
        return self.val(st, r)

    def put(self, st, k, d, v):
        n, w = d
        if w == 32 and v[0] in ('c', 's'): v = self.trunc(v)   # a table entry (ldrb / ldrh into wN) is already the value the register holds
        if st.flags and st.flags[4] == n and st.flags[1][0] != 's': st.flags = None
        st.regs[n] = v

    def op(self, st, k, d, src, op):
        """d = op(src)"""
        if src[0] == 'c':
            return self.put(st, k, d, ('c', frozenset(_apply((op,), x) for x in src[1]), frozenset(_apply((op,), x) for x in src[2])))
        if src[0] != 's': return self.put(st, k, d, self.fresh(st, k, d[0], d[1]))
        self.put(st, k, d, ('s', src[1], src[2] + (op,)))

    def unknown(self, st, k):
        """Every general-purpose register instruction k writes becomes unknown"""
        p = self.insl[k]
        for name in p.writes():
            r = _name(name)
            if r not in (None, 'zr'): self.put(st, k, r, self.fresh(st, k, r[0], r[1]))

    def _escape(self, st, k):
        """A stack pointer may have escaped: drop all tracked local slots and stop tracking stack pointers."""
        st.escaped = True; st.frame = 0; st.slots.clear()
        if st.flags and st.flags[1][0] == 'sp': st.flags = None
        for i, v in enumerate(st.regs):
            if v[0] == 'sp':
                root = ('es', k, i, 64); self.detach(st, root, i)
                st.regs[i] = ('s', root, ())

    def _sp_shift(self, st, k, delta):
        """sp_new = sp_old + delta (delta < 0 allocates -delta bytes; delta > 0 frees delta bytes)."""
        if st.escaped or delta == 0: return
        if delta < 0:
            st.frame += -delta
            if st.slots: st.slots = {off - delta: sv for off, sv in st.slots.items()}
        else:
            st.frame = max(0, st.frame - delta)
            if st.slots: st.slots = {off - delta: (sz, v) for off, (sz, v) in st.slots.items() if off >= delta and off - delta + sz <= st.frame}
        for i, v in enumerate(st.regs):
            if v[0] == 'sp':
                off = v[1] - delta
                if delta > 0 and off < 0: self._escape(st, k); return
                st.regs[i] = ('sp', off)

    def _sp_reset(self, st, k):
        """sp was written to an unknown value (dynamic alloca or unknown base)."""
        st.frame = 0; st.slots.clear()
        if any(v[0] == 'sp' for v in st.regs): self._escape(st, k)

    def _slot_kill(self, st, off, sz):
        if not st.slots: return
        end = off + sz
        for so in [so for so, (ssz, _) in st.slots.items() if so < end and off < so + ssz]: del st.slots[so]

    def _slot_put(self, st, off, sz, v):
        if not st.escaped and 0 <= off and off + sz <= st.frame and v[0] in ('c', 's'):
            st.slots.pop(off, None); st.slots[off] = (sz, v)
            if len(st.slots) > MAXS: del st.slots[next(iter(st.slots))]

    def _slot_get(self, st, off, dw):
        slot = st.slots.get(off)
        if slot is None: return None
        sz = dw // 8; ssz, sv = slot
        return sv if ssz == sz or (sz == 4 and ssz == 8) else None

    def _store_val(self, st, p, op):
        if op.type != ARM64_OP_REG: return None
        rn = p.reg_name(op.reg); r = _name(rn)
        if r is None: return None
        if r == 'zr':
            w = 64 if rn == 'xzr' else 32
            v = self.val(st, 'zr')
            if w == 32: v = self.trunc(v)
            return (w // 8, v) if v is not None and v[0] in ('c', 's') else None
        v = self.val(st, r)
        return (r[1] // 8, v) if v[0] in ('c', 's') else None

    def _reads_sp(self, st, p):
        if st.escaped: return False
        has_sp_reg = any(v[0] == 'sp' for v in st.regs)
        if not has_sp_reg and 'sp' not in p.op_str: return False
        for name in p.reads():
            if name in ('sp', 'wsp'): return True
            if has_sp_reg:
                r = _name(name)
                if r not in (None, 'zr') and st.regs[r[0]][0] == 'sp': return True
        if has_sp_reg:
            for i, op in enumerate(p.operands):
                if op.type == ARM64_OP_REG and (i > 0 or p.mnemonic in ('dc', 'ic', 'at', 'tlbi')):
                    r = _name(p.reg_name(op.reg))
                    if r not in (None, 'zr') and st.regs[r[0]][0] == 'sp': return True
        return False

    def clobber(self, st, k, syscall=False):
        if not st.escaped and (syscall or any(st.regs[n][0] == 'sp' for n in (*range(19), 30))):
            self._escape(st, k)
        for n in list(range(19)) + [30]: st.regs[n] = self.fresh(st, k, n, 64)
        st.flags = None

    # Instructions
    def step(self, st, k, edges, loads):
        """Run instruction k on st: [(successor instruction, its state)]"""
        p = self.insl[k]; m = p.mnemonic; ops = p.operands
        nxt = k + 1 if k + 1 < len(self.insl) and self.insl[k + 1].address == p.address + 4 else None
        fall = [(nxt, st)] if nxt is not None else []
        if m in TRAPS or m in RETS: return []
        if m == 'b': return [(self.at[ops[0].imm], st)] if ops[0].imm in self.at else []
        if m.startswith('b.') and m[2:] in CONDS:
            out = []
            for taken, dest in ((True, self.at.get(ops[0].imm)), (False, nxt)):
                if dest is None: continue
                s2 = st.copy()
                if s2.flags and not self.flag_edge(s2, m[2:], taken): continue
                out.append((dest, s2))
            return out
        if m in ('cbz', 'cbnz', 'tbz', 'tbnz'):
            r = _reg(p, ops[0]); v = self.val(st, r) if r else ('?',); out = []
            for taken, dest in ((True, self.at.get(ops[-1].imm)), (False, nxt)):
                if dest is None: continue
                s2 = st.copy(); zero = (m in ('cbz', 'tbz')) == taken   # the tested value (or bit) is zero on this edge
                n = r[0] if r not in (None, 'zr') else None
                if m in ('cbz', 'cbnz'): ok = _refine(s2, v, n, ('zero', zero), (0, 0) if zero else None)
                else:
                    b = ops[1].imm; w = r[1] if r not in (None, 'zr') else 64
                    ok = _refine(s2, v, n, ('bit', b, zero), (0, (1 << b) - 1) if zero and b == w - 1 else None, b < 12)
                if ok: out.append((dest, s2))
            return out
        if m in ('bl', 'svc', 'hvc', 'smc') + BLRS:
            self.clobber(st, k, m in ('svc', 'hvc', 'smc'))
            if (m == 'bl' or m in BLRS) and self.calls is not None: self.calls.append((p.address, st.copy()))
            return fall
        if m in BRS: return []   # an authenticated indirect branch (braa / brab ...): leaves the function, as a br that reads no table does
        if m == 'br': return [(self.at[t], st.copy()) for t in edges.get(k, ()) if t in self.at]
        if p.control():
            raise Unsupported(f'{m} at {p.address:#x}')
        self.data(st, k, loads)
        return fall

    def flag_edge(self, st, cond, taken):
        kind, v, imm, w, n = st.flags
        if kind == 'tst':
            if cond not in ('eq', 'ne'): return True
            zero = (cond == 'eq') == taken
            return _refine(st, v, n, ('tst', imm, zero), None, imm <= LOW)
        c = _cond(cond, imm, w, taken)
        if c is None: return True
        if c[1] is not None and c[1][0] > c[1][1]: return False
        return _refine(st, v, n, ('cmp', cond, imm, w, taken), c[1])

    def _mem(self, st, k, p, m, ops, mem_idx, loads):
        """Handle memory loads and stores, tracking provably local fixed stack slots and rejecting aliases/overlaps."""
        mo = ops[mem_idx]; base_name = p.reg_name(mo.mem.base); has_idx = bool(mo.mem.index); disp = mo.mem.disp
        base_is_sp = base_name in ('sp', 'wsp')
        base_r = None if base_is_sp else _name(base_name)
        base_sp_off = 0 if base_is_sp else (st.regs[base_r[0]][1] if not st.escaped and base_r not in (None, 'zr') and st.regs[base_r[0]][0] == 'sp' else None)
        idx_r = _name(p.reg_name(mo.mem.index)) if has_idx else None
        if not st.escaped and idx_r not in (None, 'zr') and st.regs[idx_r[0]][0] == 'sp': self._escape(st, k); base_sp_off = None
        post_imm = p.writeback and mem_idx < len(ops) - 1 and ops[-1].type == ARM64_OP_IMM
        pre_wb = p.writeback and mem_idx == len(ops) - 1
        if m in PURE_LOADS:
            dst = _reg(p, ops[0])
            if m in TABLE_LOADS and len(ops) == 2 and dst not in (None, 'zr') and has_idx and disp == 0 and not p.writeback:
                size, _ = TABLE_LOADS[m]; scale = size.bit_length() - 1
                if base_r not in (None, 'zr') and idx_r not in (None, 'zr') and mo.shift.value == scale and mo.ext in (ARM64_EXT_INVALID, ARM64_EXT_UXTW, ARM64_EXT_SXTW, ARM64_EXT_UXTX, ARM64_EXT_SXTX):
                    bv, iv = st.regs[base_r[0]], self.val(st, idx_r)
                    ia = _eval(st, iv) if iv[0] in ('c', 's') else None
                    if ia is not None and mo.ext in (ARM64_EXT_SXTW, ARM64_EXT_SXTX) and ia.hi > (0x7fffffff if mo.ext == ARM64_EXT_SXTW else M64 >> 1): ia = None
                    loads.setdefault(k, []).append((_addresses(bv) if bv[0] == 'c' else None, ia))
                    root = ('e', k, dst[0], dst[1]); self.detach(st, root, dst[0])   # a number as well: an index loaded from a byte table (ldrb w10,[x9,x10]; cmp w10,#0xa) is bounded by its guard
                    if not TABLE_LOADS[m][1]: st.cons[root] = _mk(0, (1 << 8 * size) - 1, FULL)
                    return self.put(st, k, dst, ('s', root, ()))
            if p.writeback:
                if base_is_sp and pre_wb:
                    self._sp_shift(st, k, disp); self.unknown(st, k); return
                if base_is_sp and not post_imm:
                    self._sp_reset(st, k); self.unknown(st, k); return
                if not base_is_sp and base_sp_off is not None:
                    self._escape(st, k); self.unknown(st, k); return
            reloaded = False
            if not st.escaped and base_sp_off is not None and not has_idx:
                off = 0 if post_imm else base_sp_off + disp
                if m in ('ldr', 'ldur', 'ldar') and dst not in (None, 'zr'):
                    sv = self._slot_get(st, off, dst[1])
                    if sv is not None: self.put(st, k, dst, sv); reloaded = True
                elif m in ('ldp', 'ldnp') and len(ops) >= 3:
                    d1, d2 = dst, _reg(p, ops[1])
                    if d1 not in (None, 'zr') and d2 not in (None, 'zr') and d1[1] == d2[1]:
                        sz = d1[1] // 8
                        v1, v2 = self._slot_get(st, off, d1[1]), self._slot_get(st, off + sz, d2[1])
                        if v1 is not None or v2 is not None:
                            self.put(st, k, d1, v1 if v1 is not None else self.fresh(st, k, d1[0], d1[1]))
                            self.put(st, k, d2, v2 if v2 is not None else self.fresh(st, k, d2[0], d2[1]))
                            reloaded = True
            if not reloaded: self.unknown(st, k)
            if post_imm and base_is_sp: self._sp_shift(st, k, _imm(ops[-1]))
            if p.update_flags: st.flags = None
            return
        # Memory write (or atomic RMW): any stack pointer stored to memory escapes the stack frame.
        data_ops = ops[1:mem_idx] if m in ('stxr', 'stxrb', 'stxrh', 'stlxr', 'stlxrb', 'stlxrh', 'stxp', 'stlxp') else ops[:mem_idx]
        if not st.escaped:
            for op in data_ops:
                if _is_sp(p, op): self._escape(st, k); break
                r = _reg(p, op)
                if r not in (None, 'zr') and st.regs[r[0]][0] == 'sp': self._escape(st, k); break
        if p.writeback:
            if base_is_sp and pre_wb:
                self._sp_shift(st, k, disp)
            elif base_is_sp and not post_imm:
                self._sp_reset(st, k)
            elif not base_is_sp and base_sp_off is not None:
                self._escape(st, k)
        if not st.escaped and base_sp_off is not None:
            if has_idx:
                st.slots.clear()
            else:
                wsz = _store_size(p, m, ops)
                if wsz is None:
                    st.slots.clear()
                else:
                    off = 0 if (base_is_sp and p.writeback) else base_sp_off + disp
                    self._slot_kill(st, off, wsz)
                    if m in ('str', 'stur', 'stlr') and not post_imm:
                        sv = self._store_val(st, p, ops[0])
                        if sv is not None: self._slot_put(st, off, sv[0], sv[1])
                    elif m in ('stp', 'stnp') and not post_imm:
                        sz = wsz // 2
                        v0, v1 = self._store_val(st, p, ops[0]), self._store_val(st, p, ops[1])
                        if v0 is not None and v0[0] == sz: self._slot_put(st, off, sz, v0[1])
                        if v1 is not None and v1[0] == sz: self._slot_put(st, off + sz, sz, v1[1])
        if post_imm and base_is_sp: self._sp_shift(st, k, _imm(ops[-1]))
        self.unknown(st, k)
        if p.update_flags: st.flags = None

    def data(self, st, k, loads):
        p = self.insl[k]; m = p.mnemonic; ops = p.operands
        if not ops: return self.unknown(st, k)
        mem_idx = next((i for i, op in enumerate(ops) if op.type == ARM64_OP_MEM), None)
        if mem_idx is not None: return self._mem(st, k, p, m, ops, mem_idx, loads)
        sets = p.update_flags
        # Stack pointer updates (prologue / epilogue / dynamic alloca)
        if _is_sp(p, ops[0]):
            if not sets and m in ('add', 'sub') and len(ops) == 3 and ops[2].type == ARM64_OP_IMM:
                if _is_sp(p, ops[1]):
                    self._sp_shift(st, k, _imm(ops[2]) if m == 'add' else -_imm(ops[2])); return
                sr = _reg(p, ops[1])
                if not st.escaped and sr not in (None, 'zr') and sr[1] == 64 and st.regs[sr[0]][0] == 'sp':
                    self._sp_shift(st, k, st.regs[sr[0]][1] + (_imm(ops[2]) if m == 'add' else -_imm(ops[2]))); return
            elif not sets and m == 'mov' and len(ops) == 2:
                if _is_sp(p, ops[1]): return
                sr = _reg(p, ops[1])
                if not st.escaped and sr not in (None, 'zr') and sr[1] == 64 and st.regs[sr[0]][0] == 'sp':
                    self._sp_shift(st, k, st.regs[sr[0]][1]); return
            self._sp_reset(st, k); self.unknown(st, k)
            if sets: st.flags = None
            return
        dst = _reg(p, ops[0])
        if m in ('cmp', 'cmn', 'tst', 'subs', 'adds', 'ands'):
            if m in ('subs', 'adds', 'ands') and self._reads_sp(st, p): self._escape(st, k)
            flags = None
            a = ops[0] if m in ('cmp', 'cmn', 'tst') else ops[1]
            r = _reg(p, a) if len(ops) >= 2 else None
            if r not in (None, 'zr') and ops[-1].type == ARM64_OP_IMM and len(ops) == (2 if m in ('cmp', 'cmn', 'tst') else 3):
                v = self.linked(st, k, r) if m in ('subs', 'adds', 'ands') else self.val(st, r); imm = _imm(ops[-1])
                if m in ('cmp', 'subs'): flags = ('cmp', v, imm, r[1], r[0])
                elif m in ('cmn', 'adds'): flags = ('cmp', v, -imm & _mask(r[1]), r[1], r[0])
                else: flags = ('tst', v, imm, r[1], r[0])
                if m in ('subs', 'adds', 'ands') and dst not in (None, 'zr'):
                    self.op(st, k, dst, v, ('and', imm) if m == 'ands' else ('add', imm if m == 'adds' else -imm, dst[1]))
            elif m in ('subs', 'adds', 'ands'): self.unknown(st, k)
            st.flags = flags
            return
        # Deriving a tracked stack pointer ('sp', off) into a 64-bit GPR (e.g. frame pointer x29 or non-escaping local address)
        if not st.escaped and not sets and dst not in (None, 'zr') and dst[1] == 64:
            if m == 'mov' and len(ops) == 2 and _is_sp(p, ops[1]):
                return self.put(st, k, dst, ('sp', 0))
            if m in ('add', 'sub') and len(ops) == 3 and _is_sp(p, ops[1]) and ops[2].type == ARM64_OP_IMM:
                return self.put(st, k, dst, ('sp', _imm(ops[2]) if m == 'add' else -_imm(ops[2])))
            if len(ops) >= 2 and (sr := _reg(p, ops[1])) not in (None, 'zr') and sr[1] == 64 and st.regs[sr[0]][0] == 'sp':
                if m == 'mov' and len(ops) == 2 and ops[1].shift.value == 0:
                    return self.put(st, k, dst, st.regs[sr[0]])
                if m in ('add', 'sub') and len(ops) == 3 and ops[2].type == ARM64_OP_IMM:
                    return self.put(st, k, dst, ('sp', st.regs[sr[0]][1] + (_imm(ops[2]) if m == 'add' else -_imm(ops[2]))))
        if self._reads_sp(st, p): self._escape(st, k)
        if m in ('cset', 'csetm') and dst not in (None, 'zr'):
            x = frozenset((0, 1 if m == 'cset' else _mask(dst[1])))
            if not self.consts: nv = self.fresh(st, k, dst[0], dst[1]); st.cons[nv[1]] = Abs.of(x).con(); return self.put(st, k, dst, nv)
            return self.put(st, k, dst, ('c', x, x))
        if dst in (None, 'zr'):
            self.unknown(st, k)
            if sets: st.flags = None
            return
        if m in ('adrp', 'adr'): return self.put(st, k, dst, ('c', frozenset((ops[1].imm,)), frozenset()))
        if m == 'mov' and len(ops) == 2:
            if ops[1].type == ARM64_OP_IMM or _reg(p, ops[1]) == 'zr':   # a number, not an address: only kept with consts
                if not self.consts: return self.put(st, k, dst, self.fresh(st, k, dst[0], dst[1]))
                x = frozenset(((ops[1].imm if ops[1].type == ARM64_OP_IMM else 0) & _mask(dst[1]),))
                return self.put(st, k, dst, ('c', x, x))
            r = _reg(p, ops[1])
            if r is not None and ops[1].shift.value == 0:
                v = self.linked(st, k, r)
                return self.put(st, k, dst, v if v[0] != '?' else self.fresh(st, k, dst[0], dst[1]))
        src = _reg(p, ops[1]) if len(ops) > 1 else None
        if src is not None and not sets:
            v: Any = self.linked(st, k, src)
            if v[0] != '?':
                if m in ('add', 'sub') and len(ops) == 3 and ops[2].type == ARM64_OP_IMM:
                    return self.op(st, k, dst, v, ('add', _imm(ops[2]) * (1 if m == 'add' else -1), dst[1]))
                if m == 'and' and len(ops) == 3 and ops[2].type == ARM64_OP_IMM:
                    imm = _imm(ops[2])
                    return self.op(st, k, dst, v, ('w',) if imm == M32 else ('and', imm))
                if m == 'mvn' and len(ops) == 2 and ops[1].shift.value == 0: return self.op(st, k, dst, v, ('not', dst[1]))
                if m in ('uxtb', 'uxth') and len(ops) == 2: return self.op(st, k, dst, v, ('and', 0xff if m == 'uxtb' else 0xffff))
                if m == 'ubfx' and len(ops) == 4 and ops[2].imm == 0: return self.op(st, k, dst, v, ('and', (1 << ops[3].imm) - 1))
                if (m == 'lsr' and len(ops) == 3 and ops[2].type == ARM64_OP_IMM) or (m == 'ubfx' and len(ops) == 4):   # a new value, bounded by the shift (ASTRAL CHAIN main+0x6db104: lsr x17,x11,#32; cmp w17,#5)
                    s = ops[2].imm; top = _mask(dst[1]) >> s if m == 'lsr' else (1 << ops[3].imm) - 1
                    a = _eval(st, v); nv = self.fresh(st, k, dst[0], dst[1])
                    lo, hi = (a.lo >> s, min(a.hi >> s, top)) if a is not None and m == 'lsr' else (0, top)
                    st.cons[nv[1]] = _mk(lo, hi, _pattern(lo, hi))
                    return self.put(st, k, dst, nv)
                if m == 'sxtw' and len(ops) == 2:
                    a = _eval(st, v)
                    return self.put(st, k, dst, v if a is not None and a.hi <= 0x7fffffff else self.fresh(st, k, dst[0], dst[1]))
                if m in ('cinc', 'csinc') and len(ops) in (2, 3) and (rb := _reg(p, ops[2]) if len(ops) == 3 else src) is not None:   # cinc wD,wN: N or N + 1 (the condition is no operand)
                    vb = self.linked(st, k, rb); w_ = dst[1]
                    a, b = _eval(st, v), _eval(st, vb)
                    nv = self.fresh(st, k, dst[0], w_)
                    if a is not None and b is not None and b.hi < _mask(w_):
                        u = a.union(Abs(b.lo + 1, b.hi + 1, _image(b.bits, ('add', 1, w_)))); st.cons[nv[1]] = u.con()
                    return self.put(st, k, dst, nv)
                if m == 'csel' and len(ops) == 3 and (rb := _reg(p, ops[2])) is not None:
                    vb: Any = self.val(st, rb)
                    if v == vb: return self.put(st, k, dst, v)
                    if v[0] == 'c' and vb[0] == 'c' and len(v[1] | vb[1]) <= MAXC: return self.put(st, k, dst, ('c', v[1] | vb[1], v[2] | vb[2]))
                    a, b = _eval(st, v), _eval(st, vb)
                    nv = self.fresh(st, k, dst[0], dst[1])
                    if a is not None and b is not None: st.cons[nv[1]] = a.union(b).con()
                    return self.put(st, k, dst, nv)
        if m == 'add' and len(ops) == 3 and dst[1] == 64 and (ra := _reg(p, ops[1])) not in (None, 'zr') and (rb := _reg(p, ops[2])) not in (None, 'zr') \
           and ra[1] == 64 and rb[1] == 64 and ops[2].ext in (ARM64_EXT_INVALID, ARM64_EXT_UXTX, ARM64_EXT_SXTX) and ops[2].shift.type in (0, ARM64_SFT_LSL):
            va, vb, sh = st.regs[ra[0]], st.regs[rb[0]], ops[2].shift.value
            def entry(v): return v[0] == 's' and v[1][0] == 'e' and not v[2]
            if entry(vb) and va[0] == 'c': return self.put(st, k, dst, ('t', vb[1][1], _addresses(va), sh))
            if entry(va) and vb[0] == 'c' and sh == 0: return self.put(st, k, dst, ('t', va[1][1], _addresses(vb), 0))
        self.unknown(st, k)
        if sets: st.flags = None

    # The fixpoint
    def _join_val(self, a, b, x, y, k, tag, key_id, widen, escaped, cons, phis, pairs, assigned):
        if x == y and not (escaped and x[0] == 'sp'):
            if x[0] == 's' and x[1] not in cons:
                r = x[1]; ca = a.con(r); c = _cunion(ca, b.con(r))
                cons[r] = _widen(ca, c, r) if widen else c
            return x
        if x[0] == 'c' and y[0] == 'c' and len(x[1] | y[1]) <= MAXC and not (widen and not y[1] <= x[1]):
            return ('c', x[1] | y[1], x[2] | y[2])
        if x[0] == 's' and y[0] == 's' and x[2] == y[2]:   # one root on each side: the join of the roots, the chain kept
            key = (x[1], y[1])
            if key not in pairs:
                root = (tag, k, key_id, max(_mask(r[-1]) if len(r) > 1 else M64 for r in key).bit_length())
                pairs[key] = root; phis.add(root)
                c = _cunion(a.con(x[1]), b.con(y[1]))
                if widen and x[1] == root: c = _widen(a.con(root), c, root)
                cons[root] = c
            assigned.add((tag, key_id))
            return ('s', pairs[key], x[2])
        ea, eb = _eval(a, x), _eval(b, y)
        root = (tag, k, key_id, 32 if ea is not None and eb is not None and max(ea.hi, eb.hi) <= M32 else 64)   # 32 bits wide when both values are: a copy into wN is then the same value
        phis.add(root); assigned.add((tag, key_id))
        if ea is not None and eb is not None:
            c = ea.union(eb).con()
            if widen and x == ('s', root, ()): c = _widen(a.con(root), c, root)
            cons[root] = c
        return ('s', root, ())

    def join(self, a, b, k, visits):
        """The state at instruction k: a (stored) joined with b (incoming). Registers and stack slots that hold the same chain of one root on each side keep their
        chains of one shared phi root, so a copy or a mask stays tied to its source through a loop (Mario + Rabbids Kingdom Battle subsdk0+0x7340b8: x18 copied from
        w17 at the loop's head, w17 checked again in every case block)"""
        regs, cons, widen = [], {}, visits > WIDEN
        phis, pairs, assigned = set(), {}, set()
        escaped = a.escaped or b.escaped or any((x[0] == 'sp' or y[0] == 'sp') and x != y for x, y in zip(a.regs, b.regs))
        frame = 0 if escaped else min(a.frame, b.frame)
        if widen and frame < a.frame: frame = 0
        for i, (x, y) in enumerate(zip(a.regs, b.regs)):
            regs.append(self._join_val(a, b, x, y, k, 'p', i, widen, escaped, cons, phis, pairs, assigned))
        slots = {}
        if not escaped and a.slots and b.slots:
            for off, (sz_a, vx) in a.slots.items():
                sb = b.slots.get(off)
                if sb is not None and sb[0] == sz_a and 0 <= off and off + sz_a <= frame:
                    slots[off] = (sz_a, self._join_val(a, b, vx, sb[1], k, 'ps', off, widen, escaped, cons, phis, pairs, assigned))
        for i, v in enumerate(regs):   # a register still holding a phi of this point from an earlier pass keeps it as a root of its own
            if v[0] == 's' and v[1] in phis and ('p', i) not in assigned:
                w = 64 if len(v[1]) == 1 else v[1][-1]
                old = ('x', k, v[1], i, w); regs[i] = ('s', old, v[2])
                if v[1] in cons: cons[old] = cons[v[1]]
        for off, (sz, v) in list(slots.items()):
            if v[0] == 's' and v[1] in phis and ('ps', off) not in assigned:
                w = 64 if len(v[1]) == 1 else v[1][-1]
                old = ('xs', k, v[1], off, w); slots[off] = (sz, ('s', old, v[2]))
                if v[1] in cons: cons[old] = cons[v[1]]
        flags = a.flags if a.flags == b.flags and not (a.flags and a.flags[1][0] == 's' and a.flags[1][1] in phis) else None
        st = State(regs, cons, flags, frame, slots, escaped); st.tidy()
        return st

    def run(self, seeds, edges):
        """The fixpoint from the seeds (every register unknown) and, once the paths they start are done, from the landing pads (self.landing: {pad: call ranges}),
        each with what the calls it catches left: the unwinder restores the callee-saved registers to their values at the call"""
        states, visits, loads, work, steps = {}, {}, {}, [], 0
        self.calls = []
        for k in seeds:
            states[k] = State([('s', ('seed', k, n, 64), ()) for n in range(31)], {}, None); visits[k] = 1; work.append(k)
        reached = set(); done = 0
        while True:
          if not work:
            if done == len(self.calls): break
            done = len(self.calls)
            for k, ranges in self.landing.items():
                after = [c for a, c in self.calls if any(lo <= a < hi for lo, hi in ranges)]
                if not after: continue
                land = after[0]
                for c in after[1:]: land = self.join(land, c, -1, 1)
                land.tidy(); old = states.get(k)
                new = land if old is None else self.join(old, land, k, visits.get(k, 0) + 1)
                if old is None or new.key() != old.key(): states[k] = new; visits[k] = visits.get(k, 0) + 1; work.append(k)
            if not work: break
          while work:
              k = work.pop(); st = states[k].copy()
              while True:
                  steps += 1; reached.add(k)
                  if steps > STEPS: raise Unsupported('no fixpoint')
                  succ = self.step(st, k, edges, loads)
                  if len(succ) == 1 and succ[0][0] == k + 1 and k + 1 not in self.leaders and k + 1 not in states:
                      k += 1; st = succ[0][1]; continue
                  for dest, s2 in succ:
                      s2.tidy(); old = states.get(dest)
                      if old is None: states[dest] = s2; visits[dest] = 1; work.append(dest); continue
                      visits[dest] += 1
                      new = self.join(old, s2, dest, visits[dest])
                      if new.key() != old.key(): states[dest] = new; work.append(dest)
                  break
        self.reached = reached
        return states, loads

    def starts(self):
        """In text without unwind entries: code no known path reaches, after a b, br, ret or trap and the padding that aligns a function (udf #0, or words that do
        not decode), is a function's start, reached by an indirect call (DRAGON QUEST XI S main+0x3edcbf0, a virtual function in its 70 MB without unwind entries).
        A case block of a table not yet proved follows a b too, but without padding"""
        out = set()
        for k, p in enumerate(self.insl):
            if k in self.reached or k == 0 or p.mnemonic in ('nop', 'udf'): continue
            j = k - 1
            while j >= 0 and self.insl[j].mnemonic == 'udf' and self.insl[j + 1].address == self.insl[j].address + 4: j -= 1
            padded = j < k - 1 or self.insl[k - 1].address + 4 != p.address
            if padded and (j < 0 or self.insl[j].mnemonic in ('b', 'br') + RETS + BRS + TRAPS or self.insl[j].address + 4 != self.insl[j + 1].address): out.add(k)
        return out

    def solve(self, entries, outside, fixed=None, landing=None, unwound=True):
        """fixed: {br address: targets} every round starts from (the other reading's tables); landing: {landing pad: [call ranges]};
        unwound: False for text without unwind entries, whose unreached function starts are entries too (starts)"""
        insl = self.insl
        self.landing = {self.at[a]: r for a, r in (landing or {}).items() if a in self.at}
        direct = {self.at[p.operands[-1].imm] for p in insl if (p.mnemonic in ('b', 'cbz', 'cbnz', 'tbz', 'tbnz') or p.mnemonic.startswith('b.')) and p.operands[-1].imm in self.at}
        # Entries with every register unknown: the start, the entries named, and the targets of calls inside the range (a piece of text with no unwind entries holds
        # many functions, entered by bl from one another: DRAGON QUEST XI S main, 70 MB of it)
        calls = {self.at[p.operands[0].imm] for p in insl if p.mnemonic == 'bl' and p.operands[0].imm in self.at}
        seeds = {0} | calls | {self.at[a] for a in set(entries) | set(outside) if a in self.at}
        edges = {self.at[a]: sorted(ts) for a, ts in (fixed or {}).items() if a in self.at}
        self.edges = {}
        for _ in range(OUTER):
            self.leaders = direct | seeds | set(self.landing) | {self.at[t] for ts in edges.values() for t in ts if t in self.at}
            states, loads = self.run(seeds, edges)
            found, failed, spread = self.read(states, loads)
            new = dict(edges)
            for a, ts in list(found.items()) + list(spread.items()): new[self.at[a]] = sorted(set(new.get(self.at[a], ())) | set(ts))
            if new == edges:
                more = set() if unwound else self.starts() - seeds
                if not more: self.edges = {insl[k].address: ts for k, ts in edges.items()}; return found, failed
                seeds |= more; continue
            edges = new   # edges only grow, so the rounds end; a table read with more edges than it needs stays sound
        self.edges = {insl[k].address: ts for k, ts in edges.items()}
        return {}, {p.address: 'tables kept changing' for p in insl if p.mnemonic == 'br'}

    def read(self, states, loads):
        """Each br's value, from a last walk of every block from its fixpoint state, and the tables they read"""
        values = {}
        for k0, st0 in states.items():
            st = st0.copy(); k = k0
            while True:
                p = self.insl[k]
                if p.mnemonic == 'br':
                    r = _reg(p, p.operands[0]); values.setdefault(k, []).append(st.regs[r[0]] if r not in (None, 'zr') else ('?',)); break
                succ = self.step(st, k, {}, {})
                if len(succ) == 1 and succ[0][0] == k + 1 and k + 1 not in self.leaders and k + 1 not in states:
                    k += 1; st = succ[0][1]; continue
                break
        found, failed, spread = {}, {}, {}
        for k, vs in values.items():
            a = self.insl[k].address
            if not all(v[0] == 't' for v in vs):
                failed[a] = 'a table target joined with another value' if any(v[0] == 't' for v in vs) else 'not a table'
                continue
            if len({(v[1], v[3]) for v in vs}) != 1: failed[a] = 'more than one table load reaches the br'; continue
            kl, cs, sh = vs[0][1], frozenset().union(*(v[2] for v in vs)), vs[0][3]
            targets, why = self.table(kl, cs, sh, loads.get(kl, []))
            if targets is not None: found[a] = targets; continue
            failed[a] = why
            size, signed = TABLE_LOADS[self.insl[kl].mnemonic]
            if size <= 2 and not signed and sh == 2 and len(cs) == 1:   # an unproved byte or halfword table still lands only on its pads
                c = next(iter(cs)); spread[a] = [c + 4 * e for e in range(min(1 << 8 * size, (self.insl[-1].address + 4 - c) // 4 + 1)) if c + 4 * e in self.at]
        return found, failed, spread

    def table(self, kl, cs, sh, recs):
        if not recs: return None, 'the table load is not reached'
        size, signed = TABLE_LOADS[self.insl[kl].mnemonic]
        bases, idx = set(), None
        for b, a in recs:
            if b is None: return None, 'the table base is not a constant'
            if a is None: return None, 'the index is not bounded'
            bases |= b; idx = a if idx is None else idx.union(a)
        if idx is None: return None, 'the table load is not reached'
        n = idx.hi + 1
        if idx.empty(): return None, 'no index reaches the load'
        if n > MAXN: return None, f'the index reaches {idx.hi:#x}'
        bases = {b for b in bases if 0 <= b and b + size * n <= len(self.img)}
        if len(bases) != 1 or len(cs) != 1: return None, f'{len(bases)} table bases and {len(cs)} target bases'
        tb, c = next(iter(bases)), next(iter(cs))
        def ok(t): return t in self.at   # a decoded instruction of this function: a word read past the table's end is almost never one
        targets = [c + (struct.unpack_from(FMT[(size, signed)], self.img, tb + size * i)[0] << sh) for i in range(n)]
        if not all(ok(t) for t in targets):
            reach = idx.members()   # the words between reachable entries may be no entries at all
            if reach is None: return None, 'a target is not an instruction of the function'
            targets = [c + (struct.unpack_from(FMT[(size, signed)], self.img, tb + size * i)[0] << sh) for i in reach]
            if not all(ok(t) for t in targets): return None, 'a target is not an instruction of the function'
        return targets, None

def solve(insl, img, code_ok, entries=(), outside=(), landing=None, unwound=True):
    """{br address: targets} and {br address: reason} for the function insl (decoded with details). entries: direct branches into it from elsewhere;
    outside: targets in it of tables elsewhere. code_ok(address): whether an address is code of the module. landing: {landing pad: [(start, end) of the calls it catches]}
    (nso_code.landing_pads). Code no path, table or landing pad reaches is dead to the analysis, as it is to the rewriter's own sites, except in text without unwind
    entries (unwound False: a piece of it, or the code before the first FDE), where such code after a terminator starts a function an indirect call reaches.
    Two readings. The first knows every number the code writes (mov #imm, cset) and gives the function's control flow: every dispatch's targets, through any
    register (Manual Samuel's x17 tables, a byte table whose index is a csel of constants). The second reads the rewriter's br x18 tables again on that control
    flow with those numbers unknown, so that each table's size is what its guards allow, however control reaches it: a path the analysis cannot see then still
    finds its entry (Manual Samuel main+0x2e8174: cmp w18,#6 allows 7 entries where the constants on the known paths reach 2). Only a br x18 that no guard bounds
    keeps the first reading's table (a base that is the constant 1 on another path, Cuphead)"""
    if not insl: return {}, {}
    insl = [Ins(p) for p in insl]
    x18 = {p.address for p in insl if p.mnemonic == 'br' and p.op_str == 'x18'}
    edges: dict = {}
    for _ in range(OUTER):   # each reading starts from every edge either has found: the guards' tables reach case blocks the constants rule out, and back
        try:
            full = Function(insl, img, code_ok, True); found, failed = full.solve(entries, outside, edges, landing, unwound)
            used = dict(edges)
            for a, ts in full.edges.items(): used[a] = sorted(set(used.get(a, ())) | set(ts))
            guard = Function(insl, img, code_ok, False); guarded, _ = guard.solve(entries, outside, used, landing, unwound)
        except Unsupported as e: return {}, {p.address: str(e) for p in insl if p.mnemonic == 'br'}
        new = dict(used)
        for a, ts in guarded.items(): new[a] = sorted(set(new.get(a, ())) | set(ts))   # the guards add the tables they prove, at the size the guards allow
        if new == used: break   # nothing the constants reading did not already run with
        edges = new
    for a in x18:
        if a in guarded: found[a] = guarded[a]; failed.pop(a, None)
    return found, failed
