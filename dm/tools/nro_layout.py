#!/usr/bin/env python3
"""Give a small NRO's data segment a 16 KB page of its own: move ro and data (and bss) up by whole 4 KB pages so data starts at or past the end of text rounded up to 16 KB.
Usage: nro_layout.py <in.nro> <out.nro>   (writes the input unchanged when it already fits; prints what it did)

Why: Horizon maps NROs at 4 KB, the Mac and iOS hosts at 16 KB. loader.c seals [0, text_end rounded up to 16 KB) read + execute, so data that starts below that boundary is
read-only: the first R_AARCH64_RELATIVE into it stops the game with SIGBUS in relocate() (FINAL FANTASY VIII Remastered: Math.nro is text 0x1000, ro 0x1000, data 0x1000;
Platform.nro text 0x1000, ro 0x2000, data 0x1000). Moving ro with data keeps every ro <-> data distance (text takes the zero padding before them,
so the segments stay back to back and the header's ro-relative offsets stay right); only references between text and the moved part change, by D:
  - the NRO header (MOD0 offset at +4, module size, ro / data offsets) and MOD0's offsets whose target and MOD0 lie on different sides
  - the dynamic section's address tags, symbol values, RELA / JMPREL offsets and RELATIVE (and symbol-less ABS64) addends, DT_RELR offsets and the addends in their slots
  - in text: adrp (D is whole 4 KB pages, so the low 12 bits an add / ldr supplies stay right), adr and literal loads whose target is in the moved part
  - .eh_frame_hdr's table and .eh_frame's pc-relative / data-relative pointers (they sit in ro and point into text)
The image the game hands nn::ro::LoadModule is the original: nro.tsv keeps its SHA-256 and size, the host finds this copy by that hash (hle_ro.c)."""
import os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nso_code

HOST_PAGE = 0x4000
PTR_TAGS = {3, 4, 5, 6, 7, 12, 13, 17, 23, 25, 26, 32, 36, 0x6ffffef5, 0x6ffffff0, 0x6ffffffc, 0x6ffffffe}   # PLTGOT HASH STRTAB SYMTAB RELA INIT FINI REL JMPREL INIT/FINI/PREINIT_ARRAY RELR GNU_HASH VERSYM VERDEF VERNEED
R_ABS64, R_GLOB_DAT, R_JUMP_SLOT, R_RELATIVE = 257, 1025, 1026, 1027


class Error(ValueError):
    pass


def shift_needed(d):
    """D (bytes, a multiple of 4 KB) that moves data to a 16 KB page past the code, 0 when it already is"""
    te = struct.unpack_from('<I', d, 0x24)[0]; ro = struct.unpack_from('<I', d, 0x28)[0]; do = struct.unpack_from('<I', d, 0x30)[0]
    rx = (te + HOST_PAGE - 1) & ~(HOST_PAGE - 1)
    return 0 if do >= rx else rx - do


def relayout(d):
    """(new bytes, D, notes) of the NRO bytes d"""
    if d[0x10:0x14] != b'NRO0': raise Error('not an NRO')
    D = shift_needed(d)
    if not D: return bytes(d), 0, []
    (to, tsz), (ro, rsz), (do, dsz) = (struct.unpack_from('<II', d, 0x20 + 8 * i) for i in range(3))
    size = struct.unpack_from('<I', d, 0x18)[0]
    if to or tsz != ro or ro + rsz != do or do + dsz != size: raise Error(f'unexpected segments text {to:#x}+{tsz:#x} ro {ro:#x}+{rsz:#x} data {do:#x}+{dsz:#x} size {size:#x}')
    mv = lambda a: a + D if a >= ro else a
    old = bytes(d)
    new = bytearray(old[:ro]) + bytearray(D) + bytearray(old[ro:])
    n = {'adrp': 0, 'adr': 0, 'literal': 0, 'reloc': 0, 'relr': 0, 'sym': 0, 'eh': 0, 'dyn': 0}

    def put(fmt, o_old, v): struct.pack_into(fmt, new, mv(o_old), v)

    # NRO header (in text, not moved)
    m0 = struct.unpack_from('<I', old, 4)[0]
    struct.pack_into('<I', new, 4, mv(m0)); struct.pack_into('<I', new, 0x18, size + D); struct.pack_into('<I', new, 0x24, tsz + D)   # text takes the zero padding: segments stay back to back
    struct.pack_into('<I', new, 0x28, ro + D); struct.pack_into('<I', new, 0x30, do + D)
    # MOD0: offsets relative to MOD0 itself
    if old[m0:m0 + 4] != b'MOD0': raise Error(f'no MOD0 at {m0:#x}')
    mod = list(struct.unpack_from('<6i', old, m0 + 4))
    for i, r in enumerate(mod): put('<i', m0 + 4 + 4 * i, mv(m0 + r) - mv(m0))
    # dynamic
    dyn = m0 + mod[0]; o = dyn; tags = {}
    while True:
        t, v = struct.unpack_from('<qQ', old, o)
        if t == 0: break
        tags[t] = v
        if t in PTR_TAGS and mv(v) != v: put('<Q', o + 8, mv(v)); n['dyn'] += 1
        o += 16
    if 17 in tags: raise Error('DT_REL relocations (only RELA is expected on AArch64)')
    # symbols: nchain of DT_HASH, else up to DT_STRTAB
    symtab, strtab = tags.get(6), tags.get(5)
    nsym = struct.unpack_from('<I', old, tags[4] + 4)[0] if 4 in tags else (strtab - symtab) // 24
    for i in range(nsym):
        s = symtab + 24 * i
        shndx = struct.unpack_from('<H', old, s + 6)[0]; val = struct.unpack_from('<Q', old, s + 8)[0]
        if shndx and shndx < 0xff00 and mv(val) != val: put('<Q', s + 8, mv(val)); n['sym'] += 1
    # RELA and JMPREL
    for tab, sz in ((7, 8), (23, 2)):
        if tab not in tags: continue
        for k in range(tags[sz] // 24):
            r = tags[tab] + 24 * k
            off, info, add = struct.unpack_from('<QQq', old, r)
            typ, si = info & 0xffffffff, info >> 32
            if typ not in (R_ABS64, R_GLOB_DAT, R_JUMP_SLOT, R_RELATIVE): raise Error(f'relocation type {typ} at {r:#x}')
            if typ == R_RELATIVE or (typ == R_ABS64 and si == 0): add = mv(add)
            struct.pack_into('<QQq', new, mv(r), mv(off), info, add); n['reloc'] += 1
    # DT_RELR: even entries are addresses, odd ones bitmaps; the addend is the slot's content
    if 36 in tags:
        where = None
        for k in range(tags[35] // 8):
            e = tags[36] + 8 * k; v = struct.unpack_from('<Q', old, e)[0]
            slots = []
            if not v & 1:
                put('<Q', e, mv(v)); slots = [v]; where = v + 8
            else:
                slots = [where + 8 * b for b in range(63) if v >> (b + 1) & 1]; where += 63 * 8
            for a in slots: put('<Q', a, mv(struct.unpack_from('<Q', old, a)[0])); n['relr'] += 1
    # text: adrp / adr / literal loads into the moved part
    from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    blocks = nso_code.data_blocks(bytearray(old), tsz, md)
    code = lambda a: not any(s <= a < e for s, e in blocks)
    sx = lambda v, b: v - (1 << b) if v >> (b - 1) & 1 else v
    for a in range(0, tsz, 4):
        if not code(a): continue
        w = struct.unpack_from('<I', old, a)[0]
        if w & 0x1F000000 == 0x10000000:   # adr / adrp
            imm = sx(((w >> 5) & 0x7FFFF) << 2 | (w >> 29) & 3, 21)
            if w >> 31:
                t = (a & ~0xFFF) + (imm << 12)
                if t < ro: continue
                imm += D >> 12; n['adrp'] += 1
            else:
                t = a + imm
                if t < ro: continue
                imm += D; n['adr'] += 1
            if not -(1 << 20) <= imm < 1 << 20: raise Error(f'adr(p) at {a:#x} out of range after the move')
            imm &= 0x1FFFFF
            w = (w & 0x9F00001F) | (imm & 3) << 29 | (imm >> 2) << 5
            struct.pack_into('<I', new, a, w)
        elif w & 0x3B000000 == 0x18000000:   # ldr / ldrsw / prfm (literal), general and SIMD
            imm = sx((w >> 5) & 0x7FFFF, 19); t = a + imm * 4
            if t < ro: continue
            imm += D >> 2
            if not -(1 << 18) <= imm < 1 << 18: raise Error(f'literal load at {a:#x} out of range after the move')
            struct.pack_into('<I', new, a, (w & 0xFF00001F) | (imm & 0x7FFFF) << 5); n['literal'] += 1
    # .eh_frame_hdr and .eh_frame (old offsets; a pointer read at o from base b to target t is rewritten as mv(t) - mv(b))
    ehs, ehe = m0 + mod[3], m0 + mod[4]
    if ehe - ehs >= 12 and old[ehs] == 1 and old[ehs + 3] not in (0xFF, 0):
        def fix(o, enc):
            """rewrite the encoded pointer at old offset o; returns (old target, size)"""
            fmt, app = enc & 0xF, enc & 0x70
            f = {0x3: '<I', 0xB: '<i', 0x0: '<Q', 0x4: '<Q', 0xC: '<q'}.get(fmt)
            if not f: raise Error(f'pointer encoding {enc:#x} at {o:#x}')
            v = struct.unpack_from(f, old, o)[0]; sz = struct.calcsize(f)
            base = o if app == 0x10 else ehs if app == 0x30 else 0 if app == 0 else None
            if base is None: raise Error(f'pointer application {app:#x} at {o:#x}')
            t = base + v; nv = mv(t) - mv(base)
            if nv != v: put(f, o, nv); n['eh'] += 1
            return t, sz
        _, penc, cenc, tenc = old[ehs:ehs + 4]
        o = ehs + 4
        ehf, s = fix(o, penc); o += s
        cnt = struct.unpack_from('<I', old, o)[0] if cenc & 0xF == 3 else None
        if cnt is None: raise Error(f'eh_frame_hdr count encoding {cenc:#x}')
        o += 4
        for _ in range(cnt):
            o += fix(o, tenc)[1]; o += fix(o, tenc)[1]
        # walk .eh_frame
        cies, o = {}, ehf
        while True:
            ln = struct.unpack_from('<I', old, o)[0]
            if ln == 0: break
            if ln == 0xFFFFFFFF: raise Error('64-bit eh_frame record')
            cid = struct.unpack_from('<I', old, o + 4)[0]; end = o + 4 + ln
            if cid == 0:   # CIE
                p = o + 9; ver = old[o + 8]
                aug = old[p:old.index(b'\0', p)]; p += len(aug) + 1
                _, p = nso_code._uleb(old, p)
                while old[p] & 0x80: p += 1
                p += 1
                p = p + 1 if ver == 1 else nso_code._uleb(old, p)[1]
                info = {'R': 0, 'L': None}
                if aug.startswith(b'z'):
                    _, p = nso_code._uleb(old, p)
                    for ch in aug[1:].decode():
                        if ch == 'R': info['R'] = old[p]; p += 1
                        elif ch == 'L': info['L'] = old[p]; p += 1
                        elif ch == 'P': pe = old[p]; p += 1; p += fix(p, pe & 0x7F)[1]
                        elif ch in 'SB': pass
                        else: raise Error(f'CIE augmentation {aug!r}')
                cies[o] = (info, aug.startswith(b'z'))
            else:
                cie = o + 4 - cid
                info, z = cies[cie]
                p = o + 8
                p += fix(p, info['R'])[1]
                p += struct.calcsize({0x3: '<I', 0xB: '<i', 0x0: '<Q', 0x4: '<Q', 0xC: '<q'}[info['R'] & 0xF])   # pc range: no application
                if z:
                    _, p = nso_code._uleb(old, p)
                    if info['L'] is not None and info['L'] != 0xFF: fix(p, info['L'] & 0x7F)
            o = end
    notes = [f'moved ro and data by {D:#x}: ro {ro:#x} -> {ro + D:#x}, data {do:#x} -> {do + D:#x}'] + [', '.join(f'{k} {v}' for k, v in n.items())]
    return bytes(new), D, notes


def main():
    src, dst = sys.argv[1], sys.argv[2]
    d = open(src, 'rb').read()
    try: out, D, notes = relayout(d)
    except Error as e: sys.exit(f'[RED] {src}: {e}')
    open(dst, 'wb').write(out)
    print(f'{os.path.basename(src)}: ' + ('; '.join(notes) if D else 'data already on a 16 KB page of its own'))


if __name__ == '__main__': main()
