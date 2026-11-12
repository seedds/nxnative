# A small AArch64 interpreter, for running single functions of a game's module or a rewriter stub with their callees stubbed: static analysis and tests, with no host and
# no game process. Emu(image, relocations) reads the decompressed image (dm/tools/nso.py) where nothing was written, and keeps writes in pages of its own; call(address,
# arguments...) runs to the return, step() one instruction. hooks[address] = f(emu) runs instead of the code there, default_call = f(emu, target, site) takes every bl / blr
# whose target is not in enter. Integer instructions and the floating-point and SIMD moves compilers use to pass constants; one it does not know raises Unknown with its
# disassembly. dm/tools/tests/test_bayonetta3_render_scale.py runs a profile's render scale stubs in it
import struct
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from capstone.arm64 import (ARM64_OP_REG, ARM64_OP_IMM, ARM64_OP_FP, ARM64_SFT_LSL, ARM64_SFT_LSR, ARM64_SFT_ASR, ARM64_SFT_ROR, ARM64_SFT_MSL,
                            ARM64_EXT_UXTB, ARM64_EXT_UXTH, ARM64_EXT_UXTW, ARM64_EXT_UXTX, ARM64_EXT_SXTB, ARM64_EXT_SXTH, ARM64_EXT_SXTW, ARM64_EXT_SXTX, ARM64_EXT_INVALID)

M64 = (1 << 64) - 1


class Unknown(Exception): pass
class Stop(Exception): pass


def sx(v, bits): v &= (1 << bits) - 1; return v - (1 << bits) if v >> (bits - 1) else v


class Emu:
    STACK_TOP = 0x7000_0000_0000
    RET = 0x7fff_ffff_0000   # the return address of the outermost call

    def __init__(self, img, relocs=None):
        self.img = bytearray(img)
        for off, add in (relocs or {}).items(): struct.pack_into('<Q', self.img, off, add & M64)
        self.mem = {}                     # sparse bytes outside the image / written over it: page -> bytearray
        self.md = Cs(CS_ARCH_ARM64, CS_MODE_ARM); self.md.detail = True
        self.x = [0] * 32                 # x0..x30, [31] = sp
        self.v = [0] * 32                 # 128-bit values
        self.n = self.z = self.c = self.vf = 0
        self.pc = 0
        self.cache = {}
        self.hooks = {}                   # address -> f(emu): called instead of running the code there (then returns to x30)
        self.default_call = None          # f(emu, target, site) for a bl/blr whose target is not in self.enter; None = run it
        self.enter = set()                # call targets to run (when default_call is set)
        self.steps = 0
        self.x[31] = self.STACK_TOP

    # ---- memory ----
    def _page(self, a):
        p = a >> 12
        if p not in self.mem:
            b = bytearray(0x1000); o = p << 12
            if 0 <= o < len(self.img): n = min(0x1000, len(self.img) - o); b[:n] = self.img[o:o + n]
            self.mem[p] = b
        return self.mem[p]

    def rd(self, a, n):
        a &= M64; out = bytearray()
        while n:
            pg = self._page(a); o = a & 0xfff; k = min(n, 0x1000 - o); out += pg[o:o + k]; a += k; n -= k
        return bytes(out)

    def wr(self, a, b):
        a &= M64; i = 0
        while i < len(b):
            pg = self._page(a); o = a & 0xfff; k = min(len(b) - i, 0x1000 - o); pg[o:o + k] = b[i:i + k]; a += k; i += k

    def r32(self, a): return struct.unpack('<I', self.rd(a, 4))[0]
    def r64(self, a): return struct.unpack('<Q', self.rd(a, 8))[0]
    def w32(self, a, v): self.wr(a, struct.pack('<I', v & 0xffffffff))
    def w64(self, a, v): self.wr(a, struct.pack('<Q', v & M64))

    # ---- registers ----
    def reg(self, name):
        """(kind, index, bits): kind 'x' general (index 31 = zero register), 'sp', 'v' vector / fp"""
        if name == 'sp': return 'sp', 31, 64
        if name == 'wsp': return 'sp', 31, 32
        if name == 'xzr': return 'x', 31, 64
        if name == 'wzr': return 'x', 31, 32
        if name in ('x29', 'fp'): return 'x', 29, 64
        if name in ('x30', 'lr'): return 'x', 30, 64
        k, i = name[0], int(name[1:])
        if k == 'x': return 'x', i, 64
        if k == 'w': return 'x', i, 32
        return 'v', i, {'b': 8, 'h': 16, 's': 32, 'd': 64, 'q': 128, 'v': 128}[k]

    def get(self, name):
        k, i, bits = self.reg(name)
        if k == 'sp': return self.x[31] & ((1 << bits) - 1)
        if k == 'x': return 0 if i == 31 else self.x[i] & ((1 << bits) - 1)
        return self.v[i] & ((1 << bits) - 1)

    def set(self, name, val):
        k, i, bits = self.reg(name)
        if k == 'sp': self.x[31] = val & M64
        elif k == 'x':
            if i != 31: self.x[i] = val & ((1 << bits) - 1)
        else: self.v[i] = val & ((1 << bits) - 1)

    def bits(self, name): return self.reg(name)[2]

    # ---- operands ----
    def shifted(self, v, op, bits):
        mask = (1 << bits) - 1
        if op.ext != ARM64_EXT_INVALID:
            e = op.ext
            v = {ARM64_EXT_UXTB: v & 0xff, ARM64_EXT_UXTH: v & 0xffff, ARM64_EXT_UXTW: v & 0xffffffff, ARM64_EXT_UXTX: v & M64,
                 ARM64_EXT_SXTB: sx(v, 8), ARM64_EXT_SXTH: sx(v, 16), ARM64_EXT_SXTW: sx(v, 32), ARM64_EXT_SXTX: sx(v, 64)}[e]
            return (v << op.shift.value) & mask
        t, n = op.shift.type, op.shift.value
        if not t: return v & mask
        if t == ARM64_SFT_LSL: return (v << n) & mask
        if t == ARM64_SFT_LSR: return (v & mask) >> n
        if t == ARM64_SFT_ASR: return (sx(v, bits) >> n) & mask
        if t == ARM64_SFT_ROR: v &= mask; return ((v >> n) | (v << (bits - n))) & mask
        raise Unknown(f'shift {t}')

    def val(self, ins, op, bits):
        if op.type == ARM64_OP_IMM: return self.shifted(op.imm, op, 64) & ((1 << bits) - 1) if op.shift.type else op.imm & ((1 << bits) - 1)
        if op.type == ARM64_OP_REG: return self.shifted(self.get(ins.reg_name(op.reg)), op, bits)
        raise Unknown('operand')

    def addr(self, ins, op, size):
        """(address to access, write-back or None)"""
        base = ins.reg_name(op.mem.base); a = self.get(base)
        if op.mem.index:
            idx = self.get(ins.reg_name(op.mem.index))
            ib = self.bits(ins.reg_name(op.mem.index))
            if op.ext != ARM64_EXT_INVALID:
                idx = {ARM64_EXT_UXTW: idx & 0xffffffff, ARM64_EXT_SXTW: sx(idx, 32), ARM64_EXT_SXTX: sx(idx, 64), ARM64_EXT_UXTX: idx}[op.ext]
            elif ib == 32: idx &= 0xffffffff
            if op.shift.value: idx <<= op.shift.value
            return (a + idx) & M64, None
        d = op.mem.disp
        if ins.writeback:
            if len(ins.operands) > (3 if ins.mnemonic in ('stp', 'ldp', 'ldpsw') else 2):   # post-index: [base], #imm
                return a, (base, (a + ins.operands[-1].imm) & M64)
            return (a + d) & M64, (base, (a + d) & M64)                                       # pre-index
        return (a + d) & M64, None

    def flags_add(self, a, b, carry, bits):
        mask = (1 << bits) - 1; r = (a & mask) + (b & mask) + carry
        self.c = r >> bits & 1; r &= mask; self.n = r >> (bits - 1); self.z = int(r == 0)
        self.vf = int((sx(a, bits) + sx(b, bits) + carry) != sx(r, bits))
        return r

    def cond(self, cc):
        n, z, c, v = self.n, self.z, self.c, self.vf
        return {'eq': z, 'ne': not z, 'hs': c, 'cs': c, 'lo': not c, 'cc': not c, 'mi': n, 'pl': not n, 'vs': v, 'vc': not v, 'hi': c and not z, 'ls': not c or z,
                'ge': n == v, 'lt': n != v, 'gt': not z and n == v, 'le': z or n != v, 'al': 1}[cc]

    # ---- run ----
    def decode(self, pc):
        if pc not in self.cache:
            ins = next(self.md.disasm(self.rd(pc, 4), pc), None)
            if ins is None: raise Unknown(f'undecodable word {self.r32(pc):08x} at {pc:#x}')
            self.cache[pc] = ins
        return self.cache[pc]

    def call(self, target, *args, max_steps=5_000_000):
        for i, a in enumerate(args): self.x[i] = a & M64
        self.x[30] = self.RET; self.pc = target; n0 = self.steps
        while self.pc != self.RET:
            if self.steps - n0 > max_steps: raise Stop(f'more than {max_steps} steps')
            self.step()
        return self.x[0]

    def do_call(self, target, site):
        self.x[30] = site + 4
        if target in self.hooks: self.hooks[target](self); self.pc = site + 4
        elif self.default_call and target not in self.enter: self.default_call(self, target, site); self.pc = site + 4
        else: self.pc = target

    def step(self):
        pc = self.pc; ins = self.decode(pc); self.steps += 1
        m = ins.mnemonic; ops = ins.operands; nxt = pc + 4
        R = lambda i: ins.reg_name(ops[i].reg)
        G = lambda i: self.get(R(i))
        if m.startswith('b.'):
            self.pc = ops[0].imm if self.cond(m[2:]) else nxt; return
        if m == 'b': self.pc = ops[0].imm; return
        if m == 'bl': self.do_call(ops[0].imm, pc); return
        if m == 'blr': self.do_call(G(0), pc); return
        if m == 'br': self.pc = G(0); return
        if m == 'ret': self.pc = self.x[30]; return
        if m in ('cbz', 'cbnz'):
            self.pc = ops[1].imm if (G(0) == 0) == (m == 'cbz') else nxt; return
        if m in ('tbz', 'tbnz'):
            self.pc = ops[2].imm if (G(0) >> ops[1].imm & 1) == (m == 'tbnz') else nxt; return
        self.pc = nxt
        if m == 'nop': return
        d = R(0) if ops and ops[0].type == ARM64_OP_REG else None
        bits = self.bits(d) if d else 64
        mask = (1 << bits) - 1
        isv = d is not None and self.reg(d)[0] == 'v'
        if m in ('mov', 'movz') and not isv:
            self.set(d, self.val(ins, ops[1], bits)); return
        if m == 'movn': self.set(d, ~self.val(ins, ops[1], bits)); return
        if m == 'movk':
            sh = ops[1].shift.value; self.set(d, self.get(d) & ~(0xffff << sh) | (ops[1].imm & 0xffff) << sh); return
        if m == 'mvn': self.set(d, ~self.val(ins, ops[1], bits)); return
        if m in ('adrp', 'adr'): self.set(d, ops[1].imm); return
        if m in ('add', 'sub', 'adds', 'subs') and not isv:
            a = G(1); b = self.val(ins, ops[2], bits)
            if m[0] == 'a': r = (a + b) & mask if m == 'add' else self.flags_add(a, b, 0, bits)
            else: r = (a - b) & mask if m == 'sub' else self.flags_add(a, ~b & mask, 1, bits)
            self.set(d, r); return
        if m == 'cmp': self.flags_add(G(0), ~self.val(ins, ops[1], bits) & mask, 1, bits); return
        if m == 'cmn': self.flags_add(G(0), self.val(ins, ops[1], bits), 0, bits); return
        if m == 'neg': self.set(d, -self.val(ins, ops[1], bits)); return
        if m == 'negs': self.set(d, self.flags_add(0, ~self.val(ins, ops[1], bits) & mask, 1, bits)); return
        if m in ('ccmp', 'ccmn'):
            if self.cond(ins.op_str.split(',')[-1].strip()):
                b = self.val(ins, ops[1], bits); self.flags_add(G(0), ~b & mask if m == 'ccmp' else b, int(m == 'ccmp'), bits)
            else: f = ops[2].imm; self.n, self.z, self.c, self.vf = f >> 3 & 1, f >> 2 & 1, f >> 1 & 1, f & 1
            return
        if m in ('and', 'orr', 'eor', 'ands', 'bic', 'orn', 'eon', 'bics') and not isv:
            a = G(1); b = self.val(ins, ops[2], bits)
            if m in ('bic', 'orn', 'eon', 'bics'): b = ~b & mask
            r = {'a': a & b, 'b': a & b, 'o': a | b, 'e': a ^ b}[m[0]] & mask
            if m in ('ands', 'bics'): self.n = r >> (bits - 1); self.z = int(r == 0); self.c = self.vf = 0
            self.set(d, r); return
        if m == 'tst':
            r = G(0) & self.val(ins, ops[1], bits); self.n = r >> (bits - 1) & 1; self.z = int(r == 0); self.c = self.vf = 0; return
        if m in ('lsl', 'lsr', 'asr', 'ror'):
            a = G(1); n = self.val(ins, ops[2], bits) % bits
            self.set(d, {'lsl': a << n, 'lsr': a >> n, 'asr': sx(a, bits) >> n, 'ror': (a >> n) | (a << (bits - n))}[m]); return
        if m in ('ubfx', 'sbfx'):
            lsb, w = ops[2].imm, ops[3].imm; v = G(1) >> lsb & ((1 << w) - 1); self.set(d, sx(v, w) if m == 'sbfx' else v); return
        if m in ('ubfiz', 'sbfiz'):
            lsb, w = ops[2].imm, ops[3].imm; v = G(1) & ((1 << w) - 1); self.set(d, (sx(v, w) if m == 'sbfiz' else v) << lsb); return
        if m == 'bfi':
            lsb, w = ops[2].imm, ops[3].imm; km = ((1 << w) - 1) << lsb; self.set(d, self.get(d) & ~km | (G(1) << lsb) & km); return
        if m == 'bfxil':
            lsb, w = ops[2].imm, ops[3].imm; km = (1 << w) - 1; self.set(d, self.get(d) & ~km | (G(1) >> lsb) & km); return
        if m in ('sxtw', 'sxth', 'sxtb'): self.set(d, sx(G(1), {'w': 32, 'h': 16, 'b': 8}[m[3]])); return
        if m in ('uxtw', 'uxth', 'uxtb'): self.set(d, G(1) & {'w': 0xffffffff, 'h': 0xffff, 'b': 0xff}[m[3]]); return
        if m == 'mul': self.set(d, G(1) * G(2)); return
        if m == 'madd': self.set(d, G(3) + G(1) * G(2)); return
        if m == 'msub': self.set(d, G(3) - G(1) * G(2)); return
        if m == 'mneg': self.set(d, -(G(1) * G(2))); return
        if m == 'umull': self.set(d, (G(1) & 0xffffffff) * (G(2) & 0xffffffff)); return
        if m == 'smull': self.set(d, sx(G(1), 32) * sx(G(2), 32)); return
        if m == 'umaddl': self.set(d, G(3) + (G(1) & 0xffffffff) * (G(2) & 0xffffffff)); return
        if m == 'smaddl': self.set(d, G(3) + sx(G(1), 32) * sx(G(2), 32)); return
        if m == 'umulh': self.set(d, (G(1) * G(2)) >> 64); return
        if m == 'udiv': b = G(2); self.set(d, G(1) // b if b else 0); return
        if m == 'sdiv':
            a, b = sx(G(1), bits), sx(G(2), bits); self.set(d, 0 if not b else (abs(a) // abs(b)) * (1 if (a < 0) == (b < 0) else -1)); return
        if m == 'clz': v = G(1); self.set(d, bits - v.bit_length()); return
        if m in ('csel', 'csinc', 'csinv', 'csneg') and not isv:
            cc = ins.op_str.split(',')[-1].strip()
            if self.cond(cc): self.set(d, G(1))
            else: b = G(2); self.set(d, {'csel': b, 'csinc': b + 1, 'csinv': ~b, 'csneg': -b}[m])
            return
        if m in ('cset', 'csetm'):
            t = self.cond(ins.op_str.split(',')[-1].strip()); self.set(d, (1 if m == 'cset' else mask) if t else 0); return
        if m in ('cinc', 'cinv', 'cneg'):
            t = self.cond(ins.op_str.split(',')[-1].strip()); a = G(1); self.set(d, ({'cinc': a + 1, 'cinv': ~a, 'cneg': -a}[m]) if t else a); return
        # ---- loads / stores ----
        if m in ('ldr', 'ldur', 'ldrb', 'ldurb', 'ldrh', 'ldurh', 'ldrsw', 'ldursw', 'ldrsb', 'ldursb', 'ldrsh', 'ldursh'):
            if ops[1].type == ARM64_OP_IMM:   # literal
                a, wb = ops[1].imm, None
            else: a, wb = self.addr(ins, ops[1], 0)
            size = 1 if 'b' in m[3:] else 2 if 'h' in m[3:] else 4 if m.endswith('sw') else bits // 8
            v = int.from_bytes(self.rd(a, size), 'little')
            if 's' in m[3:]: v = sx(v, size * 8)
            self.set(d, v)
            if wb: self.set(*wb)
            return
        if m in ('str', 'stur', 'strb', 'sturb', 'strh', 'sturh'):
            a, wb = self.addr(ins, ops[1], 0)
            size = 1 if m.endswith('b') else 2 if m.endswith('h') else bits // 8
            self.wr(a, (G(0) & ((1 << size * 8) - 1)).to_bytes(size, 'little'))
            if wb: self.set(*wb)
            return
        if m in ('ldp', 'ldpsw'):
            a, wb = self.addr(ins, ops[2], 0); size = 4 if m == 'ldpsw' else bits // 8
            for i in (0, 1):
                v = int.from_bytes(self.rd(a + i * size, size), 'little'); self.set(R(i), sx(v, 32) if m == 'ldpsw' else v)
            if wb: self.set(*wb)
            return
        if m == 'stp':
            a, wb = self.addr(ins, ops[2], 0); size = bits // 8
            for i in (0, 1): self.wr(a + i * size, (G(i) & ((1 << bits) - 1)).to_bytes(size, 'little'))
            if wb: self.set(*wb)
            return
        # ---- floating point / SIMD, as far as the setup code uses them ----
        f32 = lambda v: struct.unpack('<f', struct.pack('<I', v & 0xffffffff))[0]
        f64 = lambda v: struct.unpack('<d', struct.pack('<Q', v & M64))[0]
        fget = lambda name: (f32 if self.bits(name) == 32 else f64)(self.get(name))
        def fset(name, val):
            self.v[self.reg(name)[1]] = struct.unpack('<I', struct.pack('<f', val))[0] if self.bits(name) == 32 else struct.unpack('<Q', struct.pack('<d', val))[0]
        if m == 'fmov':
            if ops[1].type == ARM64_OP_FP:
                if d is not None and d.startswith('v'):   # fmov v0.4s, #imm
                    w = struct.unpack('<I', struct.pack('<f', ops[1].fp))[0]; self.v[self.reg(d)[1]] = sum(w << 32 * i for i in range(4))
                else: fset(d, ops[1].fp)
            elif ops[1].type == ARM64_OP_IMM: fset(d, float(ops[1].imm))
            else:
                s = R(1)
                if isv: self.v[self.reg(d)[1]] = self.get(s) & ((1 << self.bits(d)) - 1)
                else: self.set(d, self.get(s))
            return
        if m in ('scvtf', 'ucvtf'):
            s = R(1); v = self.get(s); fset(d, float(sx(v, self.bits(s)) if m == 'scvtf' else v)); return
        if m in ('fcvtzs', 'fcvtzu', 'fcvtns', 'fcvtms', 'fcvtps', 'fcvtas', 'fcvtnu', 'fcvtmu', 'fcvtpu'):
            import math
            f = fget(R(1)); r = {'z': int, 'n': round, 'm': math.floor, 'p': math.ceil, 'a': lambda t: int(math.floor(t + 0.5))}[m[4]](f); self.set(d, r); return
        if m in ('fmul', 'fadd', 'fsub', 'fdiv', 'fmax', 'fmin', 'fnmul'):
            a, b = fget(R(1)), fget(R(2))
            fset(d, {'fmul': a * b, 'fadd': a + b, 'fsub': a - b, 'fdiv': a / b if b else float('inf'), 'fmax': max(a, b), 'fmin': min(a, b), 'fnmul': -(a * b)}[m]); return
        if m in ('fabs', 'fneg', 'fsqrt'):
            a = fget(R(1)); fset(d, {'fabs': abs(a), 'fneg': -a, 'fsqrt': a ** 0.5 if a >= 0 else float('nan')}[m]); return
        if m == 'fcvt':
            fset(d, fget(R(1))); return
        if m in ('fcmp', 'fcmpe'):
            a = fget(R(0)); b = 0.0 if ops[1].type != ARM64_OP_REG else fget(R(1))
            if a != a or b != b: self.n, self.z, self.c, self.vf = 0, 0, 1, 1
            elif a == b: self.n, self.z, self.c, self.vf = 0, 1, 1, 0
            elif a < b: self.n, self.z, self.c, self.vf = 1, 0, 0, 0
            else: self.n, self.z, self.c, self.vf = 0, 0, 1, 0
            return
        if m == 'fcsel':
            self.v[self.reg(d)[1]] = self.get(R(1)) if self.cond(ins.op_str.split(',')[-1].strip()) else self.get(R(2)); return
        if m == 'movi':
            imm = ops[1].imm
            if ops[1].shift.type == ARM64_SFT_MSL: imm = (imm << ops[1].shift.value) | ((1 << ops[1].shift.value) - 1)
            elif ops[1].shift.value: imm <<= ops[1].shift.value
            arr = ins.op_str.split(',')[0].split('.')[-1] if '.' in ins.op_str.split(',')[0] else 'd'
            n, eb = {'2d': (2, 64), '4s': (4, 32), '8h': (8, 16), '16b': (16, 8), '2s': (2, 32), '4h': (4, 16), '8b': (8, 8), 'd': (1, 64)}[arr]
            self.v[self.reg(d)[1]] = sum((imm & ((1 << eb) - 1)) << eb * i for i in range(n)); return
        if m == 'dup':
            arr = ins.op_str.split(',')[0].split('.')[-1]; n, eb = {'2d': (2, 64), '4s': (4, 32), '8h': (8, 16), '16b': (16, 8), '2s': (2, 32)}[arr]
            s = self.get(R(1)) & ((1 << eb) - 1); self.v[self.reg(d)[1]] = sum(s << eb * i for i in range(n)); return
        if m == 'prfm': return
        raise Unknown(f'{pc:#x}: {m} {ins.op_str}')
