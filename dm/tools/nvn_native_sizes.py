#!/usr/bin/env python3
"""Bytes of command memory and control memory the game's own NVN driver uses for each command-buffer call: runs the driver's code in an emulator.

NVN's driver, NVN1 on Switch and NVN2 on Switch 2, is linked into each game's sdk module. Its entry points are named in a table of 32-bit offsets into a blob of names
("CommandBufferDrawElements", alphabetical), and its implementations sit in a pointer table in the same order (NVN2's GetProcAddress hands out thunks that dispatch through a
table filled from it). This tool finds both tables, reads the command buffer's layout from the driver's own getters (GetCommandMemoryUsed and the like), and runs each call
between BeginRecording and EndRecording on a command buffer of that layout with ample memory. The bytes are the write pointers' advance. Imports are stubs (memcpy, memmove
and memset copy; anything else returns 0), and fields the driver keeps beyond the layout start at 0, so a call that needs a device or a program object may stop with an error:
its row says so. hle_nvn.c's compact records (nvn_obj.h OPC_*) were sized against this table; docs/reference/memory.md, "Model heaps sized by the host's command record sizes".

Usage: nvn_native_sizes.py <ExeFS/sdk> [--names]   (prints call, command bytes, control bytes, error; --names: the entry points and their implementations instead)
Needs unicorn (pip install unicorn) and capstone."""
import argparse, os, re, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nso  # noqa: E402

IB, IND, VB, UB, SB, FRAG = 0x10000, 0x18000, 0x20000, 0x30000, 0x40000, 1
# Calls, as host tests make them: entry point (without the CommandBuffer prefix) and arguments; 'P:<kind>' is a pointer to data the runner places (ptr below)
CASES = [
    ('empty', []),
    ('DrawArrays', [('DrawArrays', (4, 0, 36))]),
    ('DrawElements', [('DrawElements', (4, 1, 36, IB))]),
    ('DrawElementsBaseVertex', [('DrawElementsBaseVertex', (4, 1, 36, IB, 5))]),
    ('DrawElementsBaseVertex 0', [('DrawElementsBaseVertex', (4, 1, 36, IB, 0))]),
    ('DrawArraysInstanced', [('DrawArraysInstanced', (4, 0, 36, 0, 2))]),
    ('DrawElementsInstanced', [('DrawElementsInstanced', (4, 1, 36, IB, 5, 0, 2))]),
    ('DrawElementsIndirect', [('DrawElementsIndirect', (4, 1, IB, IND))]),
    ('BindVertexBuffer', [('BindVertexBuffer', (3, VB, 0x100))]),
    ('BindVertexBuffers 2', [('BindVertexBuffers', (0, 2, 'P:ranges2'))]),
    ('BindUniformBuffer', [('BindUniformBuffer', (FRAG, 2, UB, 0x100))]),
    ('BindUniformBuffers 2', [('BindUniformBuffers', (FRAG, 0, 2, 'P:ranges2'))]),
    ('BindStorageBuffer', [('BindStorageBuffer', (FRAG, 1, SB, 0x100))]),
    ('BindTexture', [('BindTexture', (FRAG, 3, 0x1ff00001))]),
    ('BindTextures 2', [('BindTextures', (FRAG, 0, 2, 'P:handles2'))]),
    ('BindImage', [('BindImage', (FRAG, 0, 5))]),
    ('BindBlendState', [('BindBlendState', ('P:BlendState',))]),
    ('BindColorState', [('BindColorState', ('P:ColorState',))]),
    ('BindChannelMaskState', [('BindChannelMaskState', ('P:ChannelMaskState',))]),
    ('BindMultisampleState', [('BindMultisampleState', ('P:MultisampleState',))]),
    ('BindPolygonState', [('BindPolygonState', ('P:PolygonState',))]),
    ('BindDepthStencilState', [('BindDepthStencilState', ('P:DepthStencilState',))]),
    ('BindVertexAttribState 4', [('BindVertexAttribState', (4, 'P:attribs4'))]),
    ('BindVertexStreamState 2', [('BindVertexStreamState', (2, 'P:streams2'))]),
    ('UpdateUniformBuffer 64', [('UpdateUniformBuffer', (UB, 0x1000, 0, 64, 'P:data64'))]),
    ('SetViewports 1', [('SetViewports', (0, 1, 'P:vp1'))]),
    ('SetScissors 1', [('SetScissors', (0, 1, 'P:sc1'))]),
    ('Barrier', [('Barrier', (0x10,))]),
    ('ClearColor', [('ClearColor', (0, 'P:rgba', 0xf))]),
    ('SetStencilRef', [('SetStencilRef', (3, 0x80))]),
    ('SetBlendColor', [('SetBlendColor', ('P:rgba',))]),
    ('SetPrimitiveRestart', [('SetPrimitiveRestart', (1, 0xffff))]),
    ('mesh: vbuf, ubo, tex, draw', [('BindVertexBuffer', (0, VB, 0x1000)), ('BindUniformBuffer', (0, 3, UB, 0x100)), ('BindTexture', (FRAG, 0, 0x1ff00002)),
                                    ('DrawElementsBaseVertex', (4, 1, 300, IB, 7))]),
]
BASE, STUBS, OBJ, CMD, CTL, ARG, STK, RET = 0x80000000, 0x16000000, 0x10000000, 0x11000000, 0x12000000, 0x13000000, 0x14000000, 0x15000000
IDENT = set(b'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_')


def dyn_tags(d):
    mod0 = struct.unpack_from('<I', d, 4)[0]; dyn = mod0 + struct.unpack_from('<i', d, mod0 + 4)[0]; tags = {}
    for i in range(4096):
        t, v = struct.unpack_from('<qQ', d, dyn + i * 16)
        if t == 0: break
        tags.setdefault(t, v)
    return tags


def relative_relocs(d, tags):
    """{slot: target offset} of the module's relative relocations (RELA type 1027 and RELR)"""
    rel = {}
    for k in range(tags.get(8, 0) // 24):
        off, info, add = struct.unpack_from('<QQq', d, tags[7] + k * 24)
        if info & 0xffffffff == 1027: rel[off] = add
    where = 0
    for k in range(tags.get(35, 0) // 8):
        e = struct.unpack_from('<Q', d, tags[36] + k * 8)[0]
        if not e & 1:
            rel[e] = struct.unpack_from('<Q', d, e)[0]; where = e + 8; continue
        for b in range(63):
            if e >> (b + 1) & 1: rel[where + b * 8] = struct.unpack_from('<Q', d, where + b * 8)[0]
        where += 63 * 8
    return rel


def name_table(d):
    """The driver's entry point names, in table order: the table of 32-bit offsets in which the anchor's offset and its successor's differ by the anchor's length + 1"""
    word = b'CommandBufferDrawElementsBaseVertex'
    anchor = d.find(b'\0' + word + b'\0') + 1
    if anchor <= 0: sys.exit('no NVN driver names in this module')
    gap = len(word) + 1
    def names_at(t, s):
        while t >= 4 and struct.unpack_from('<I', d, t)[0]: t -= 4
        out = []
        while t + 4 <= len(d):
            o = struct.unpack_from('<I', d, t)[0]
            if out and not o: break
            nm = d[s + o:d.find(b'\0', s + o)] if s + o < len(d) else b''
            if not re.fullmatch(rb'[A-Za-z0-9_]+', nm): break
            out.append(nm.decode()); t += 4
        return out
    w = struct.unpack_from('<%dI' % (len(d) // 4), d, 0); best = []
    for i in range(len(w) - 1):
        o = w[i]
        if w[i + 1] - o == gap and o <= anchor:
            got = names_at(4 * i, anchor - o)
            if len(got) > len(best): best = got
    return best


def impl_table(d, rel, names, text_end, md):
    """{name: implementation offset}: the pointer table parallel to the names whose getters are leaf functions and whose indexed draws write the index buffer method (0x5f2)"""
    idx = {n: i for i, n in enumerate(names)}
    def leaf(f):
        for i in md.disasm(d[f:f + 48], f):
            if i.mnemonic == 'ret': return True
            if i.mnemonic in ('bl', 'blr', 'b', 'stp', 'pacibsp'): return False
        return False
    def draw(f): return any(i.mnemonic == 'mov' and i.op_str.endswith('#0x5f2') for i in md.disasm(d[f:f + 0x400], f))
    getters = ['CommandBufferGetCommandMemoryUsed', 'CommandBufferIsRecording', 'CommandBufferGetControlMemoryUsed', 'CommandBufferGetCommandMemorySize']
    best, base = 0, None
    for b in sorted(rel):
        fs = [rel.get(b + 8 * idx[n]) for n in getters]
        sc = sum(1 for f in fs if f and f < text_end and leaf(f))
        if sc < 3: continue
        sc += sum(1 for n in ('CommandBufferDrawElements', 'CommandBufferDrawElementsBaseVertex') if (f := rel.get(b + 8 * idx[n])) and f < text_end and draw(f))
        if sc > best: best, base = sc, b
    if base is None or best < 5: sys.exit('the implementation table was not found')
    return {n: rel.get(base + 8 * i, 0) for i, n in enumerate(names)}


def layout(d, impl, md):
    """The command buffer fields the getters read: write pointers, ends, starts, the recording flag"""
    def loads(name):
        f = impl[name]; out = []
        for i in md.disasm(d[f:f + 32], f):
            if i.mnemonic == 'ret': break
            m = re.match(r'(ldr|ldrb|ldp) (\w+)(?:, (\w+))?, \[x0(?:, #(0x[0-9a-f]+|\d+))?\]', f'{i.mnemonic} {i.op_str}')
            if m:
                off = int(m.group(4) or '0', 0)
                out.append((m.group(2), off))
                if m.group(1) == 'ldp': out.append((m.group(3), off + 8))
            m = re.match(r'sub x0, (x\d+), (x\d+)', f'{i.mnemonic} {i.op_str}')
            if m: return {r: o for r, o in out}, m.group(1), m.group(2)
        return {r: o for r, o in out}, None, None
    def diff(name):   # (minuend field, subtrahend field) of a getter that returns a - b
        regs, a, b = loads(name)
        if a not in regs or b not in regs: sys.exit(f'cannot read the layout from {name}')
        return regs[a], regs[b]
    cur, start = diff('CommandBufferGetCommandMemoryUsed')
    end, cur2 = diff('CommandBufferGetCommandMemoryFree')
    ccur, cstart = diff('CommandBufferGetControlMemoryUsed')
    regs, _, _ = loads('CommandBufferIsRecording')
    if cur2 != cur or not regs: sys.exit('unexpected command memory getters')
    return dict(cur=cur, start=start, ccur=ccur, cend=ccur + 8, cstart=cstart, rec=next(iter(regs.values()))), sorted({cur + 8, end})   # the fields, the command ends


class Driver:
    def __init__(self, path):
        from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
        m = nso.read(path); d = bytearray(m.image); self.md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
        tags = dyn_tags(d); rel = relative_relocs(d, tags)
        self.names = name_table(bytes(d)); self.impl = impl_table(bytes(d), rel, self.names, m.text_end, self.md)
        self.layout, self.ends = layout(bytes(d), self.impl, self.md)
        for off, add in rel.items(): struct.pack_into('<Q', d, off, BASE + add)
        symtab, strtab, stubs = tags[6], tags[5], {}
        for tab, sz in ((tags.get(23, 0), tags.get(2, 0)), (tags.get(7, 0), tags.get(8, 0))):   # imports: JUMP_SLOT, GLOB_DAT, ABS64
            for k in range(sz // 24 if tab else 0):
                off, info, add = struct.unpack_from('<QQq', d, tab + k * 24)
                if info & 0xffffffff not in (1025, 1026, 257): continue
                sym = symtab + (info >> 32) * 24
                if struct.unpack_from('<H', d, sym + 6)[0]: struct.pack_into('<Q', d, off, BASE + struct.unpack_from('<Q', d, sym + 8)[0] + add); continue
                o = struct.unpack_from('<I', d, sym)[0]; name = bytes(d[strtab + o:d.index(b'\0', strtab + o)]).decode('latin1')
                struct.pack_into('<Q', d, off, STUBS + 16 * stubs.setdefault(name, len(stubs)) + add)
        self.stubname = {i: n for n, i in stubs.items()}
        self.img, self.size = bytes(d), (len(d) + m.bss + 0xfffff) & ~0xfffff

    def fresh(self):
        from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE   # pyright: ignore[reportMissingImports]  # an optional package, only this tool needs it
        from unicorn.arm64_const import UC_ARM64_REG_X0, UC_CPU_ARM64_MAX   # pyright: ignore[reportMissingImports]
        mu = Uc(UC_ARCH_ARM64, UC_MODE_ARM); mu.ctl_set_cpu_model(UC_CPU_ARM64_MAX)   # pointer authentication (pacibsp / retab) as on the console
        mu.mem_map(BASE, self.size); mu.mem_write(BASE, self.img)
        for a in (0, OBJ, CMD, CTL, ARG, STK, RET, STUBS): mu.mem_map(a, 0x1000000)   # page 0 too: a field left at 0 reads zeros
        mu.mem_write(RET, b'\x00\x00\x20\xd4'); mu.mem_write(STUBS, b'\xc0\x03\x5f\xd6' * (0x1000000 // 4))
        def stub(uc, addr, size, ud):
            name = self.stubname.get((addr - STUBS) // 16, ''); x = [uc.reg_read(UC_ARM64_REG_X0 + i) for i in range(3)]
            if name in ('memcpy', 'memmove', '__memcpy_chk'): uc.mem_write(x[0], bytes(uc.mem_read(x[1], x[2])))
            elif name == 'memset': uc.mem_write(x[0], bytes([x[1] & 0xff]) * x[2])
            else: uc.reg_write(UC_ARM64_REG_X0, 0)
        mu.hook_add(UC_HOOK_CODE, stub, begin=STUBS, end=STUBS + 0x1000000 - 1)
        o, L = bytearray(0x400), self.layout
        for k, v in (('cur', CMD), ('start', CMD), ('ccur', CTL), ('cend', CTL + 0x80000), ('cstart', CTL)): struct.pack_into('<Q', o, L[k], v)
        for e in self.ends: struct.pack_into('<Q', o, e, CMD + 0x80000)
        o[L['rec']] = 1
        mu.mem_write(OBJ, bytes(o)); self.mu, self.argp = mu, ARG

    def arg(self, data):
        a = self.argp; self.mu.mem_write(a, data); self.argp = (a + len(data) + 15) & ~15; return a

    def call(self, name, *args, obj=True):
        from unicorn import UcError   # pyright: ignore[reportMissingImports]
        from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_SP, UC_ARM64_REG_LR, UC_ARM64_REG_PC   # pyright: ignore[reportMissingImports]
        mu = self.mu
        for i, v in enumerate(((OBJ,) if obj else ()) + args): mu.reg_write(UC_ARM64_REG_X0 + i, v & 0xffffffffffffffff)
        mu.reg_write(UC_ARM64_REG_SP, STK + 0x800000); mu.reg_write(UC_ARM64_REG_LR, RET)
        try: mu.emu_start(BASE + self.impl[name], RET, count=500000)
        except UcError as e: return f'{e} at {name}+{mu.reg_read(UC_ARM64_REG_PC) - BASE - self.impl[name]:#x}'
        return None if mu.reg_read(UC_ARM64_REG_PC) == RET else f'{name} did not return'

    def used(self):
        r = lambda k: struct.unpack('<Q', self.mu.mem_read(OBJ + self.layout[k], 8))[0]
        return r('cur') - r('start'), r('ccur') - r('cstart')

    def ptr(self, kind):
        if kind == 'ranges2': return self.arg(struct.pack('<4Q', VB, 0x100, VB + 0x1000, 0x200))
        if kind == 'handles2': return self.arg(struct.pack('<2Q', 0x1ff00001, 0x1ff00002))
        if kind == 'data64': return self.arg(bytes(range(64)))
        if kind == 'vp1': return self.arg(struct.pack('<4f', 0, 0, 1280, 720))
        if kind == 'sc1': return self.arg(struct.pack('<4i', 0, 0, 1280, 720))
        if kind == 'rgba': return self.arg(struct.pack('<4f', 0.25, 0.5, 0.75, 1))
        if kind in ('attribs4', 'streams2'):   # NVN's state sizes: an attribute 4 bytes, a stream 8
            n, sz, f = (4, 4, 'VertexAttribStateSetDefaults') if kind == 'attribs4' else (2, 8, 'VertexStreamStateSetDefaults')
            a = self.arg(bytes(0x20 * n))
            for i in range(n): self.call(f, a + sz * i, obj=False)
            return a
        a = self.arg(bytes(0x80)); self.call(kind + 'SetDefaults', a, obj=False); return a


def main():
    ap = argparse.ArgumentParser(description=(__doc__ or '').split('\n')[0]); ap.add_argument('sdk'); ap.add_argument('--names', action='store_true')
    a = ap.parse_args()
    try: import unicorn  # noqa: F401  # pyright: ignore[reportMissingImports]
    except ImportError: sys.exit('needs unicorn: pip install unicorn')
    drv = Driver(a.sdk)
    if a.names:
        for n in drv.names: print(f'{n}\t{drv.impl[n]:#x}')
        return
    print(f'# {len(drv.names)} entry points; command buffer layout {drv.layout}, command memory ends {drv.ends}')
    print('call\tcommand bytes\tcontrol bytes\terror')
    for name, steps in CASES:
        drv.fresh(); err = drv.call('CommandBufferBeginRecording')
        for fn, args in steps:
            err = err or drv.call('CommandBuffer' + fn, *(drv.ptr(x[2:]) if isinstance(x, str) else x for x in args))
        err = err or drv.call('CommandBufferEndRecording')
        cmd, ctl = drv.used()
        print(f'{name}\t{cmd}\t{ctl}\t{err or ""}')


if __name__ == '__main__':
    main()
