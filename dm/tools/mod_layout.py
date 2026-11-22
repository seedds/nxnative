#!/usr/bin/env python3
"""Give a Switch module's writable data a 16 KB page of its own: move data and bss up by whole 4 KB pages so data starts at or past the end of text rounded up to 16 KB.
Usage: mod_layout.py <in.nso|in.nro> [out.nro]   (prints what it moves; writes out.nro only for an NRO)

Why: Horizon maps modules at 4 KB, the Mac and iOS hosts at 16 KB. loader.c seals [0, text_end rounded up to 16 KB) read + execute (and gen_embed.py puts the same range in __TEXT),
so a module whose data starts below that boundary has read-only data: the first R_AARCH64_RELATIVE into it stops the game with SIGBUS in relocate()
(FINAL FANTASY VIII Remastered: Math.nro is text 0x1000, ro 0x1000, data 0x1000; Platform.nro text 0x1000, ro 0x2000, data 0x1000). Keep ro in place to preserve the
linker's relative references between text and ro, including signed jump-table entries. References crossing into or out of the moved part change by D:
  - the NRO header (when NRO0: module size, ro size and data offset), the MOD0 offset at +4, and MOD0's offsets whose target and MOD0 lie on different sides
  - the dynamic section's address tags, symbol values (excluding STT_TLS), RELA / JMPREL offsets and RELATIVE (and symbol-less ABS64) addends, DT_RELR offsets and slot addends
  - in text: adrp (D is whole 4 KB pages, so the low 12 bits an add / ldr supplies stay right), adr and literal loads whose target is in the moved part
  - .eh_frame_hdr's table, .eh_frame's pc-relative / data-relative pointers, and any LSDA explicit lpstart pointer
patch_nso.py calls relayout_module() on every NSO and NRO right after nso.load(), leaving on-disk files and nro.tsv untouched."""
import os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nso, nso_code

PAGE, HOST_PAGE = 0x1000, 0x4000
PTR_TAGS = {3, 4, 5, 6, 7, 12, 13, 17, 23, 25, 26, 32, 36, 0x6ffffef5, 0x6ffffff0, 0x6ffffffc, 0x6ffffffe}   # PLTGOT HASH STRTAB SYMTAB RELA INIT FINI REL JMPREL INIT/FINI/PREINIT_ARRAY RELR GNU_HASH VERSYM VERDEF VERNEED
R_ABS64, R_GLOB_DAT, R_JUMP_SLOT, R_RELATIVE = 257, 1025, 1026, 1027
FMT_INFO = {
    0x3: ('<I', 0, (1 << 32) - 1),
    0xB: ('<i', -(1 << 31), (1 << 31) - 1),
    0x0: ('<Q', 0, (1 << 64) - 1),
    0x4: ('<Q', 0, (1 << 64) - 1),
    0xC: ('<q', -(1 << 63), (1 << 63) - 1),
}


class Error(ValueError):
    pass


def shift_needed(text_end, data_off):
    """D (bytes, a multiple of 4 KB) that moves data to a 16 KB page past the code, 0 when it already is"""
    rx = (text_end + HOST_PAGE - 1) & ~(HOST_PAGE - 1)
    return 0 if data_off >= rx else rx - data_off


def relayout_module(mod):
    """(new nso.Module, D, notes) for mod (NSO or NRO). Returns (mod, 0, []) unchanged when data already sits on its own 16 KB page"""
    (to, tsz), (ro, rsz), (do, dsz) = mod.segments
    D = shift_needed(tsz, do)
    if not D: return mod, 0, []
    if to or tsz > ro or ro + rsz > do or do + dsz != len(mod.image) or ro % PAGE or do % PAGE or D % PAGE:
        raise Error(f'unexpected segments text {to:#x}+{tsz:#x} ro {ro:#x}+{rsz:#x} data {do:#x}+{dsz:#x} image {len(mod.image):#x}')
    mv = lambda a: a + D if a >= do else a
    old = bytes(mod.image)
    new = bytearray(old[:do]) + bytearray(D) + bytearray(old[do:])
    n = {'adrp': 0, 'adr': 0, 'literal': 0, 'reloc': 0, 'relr': 0, 'sym': 0, 'eh': 0, 'dyn': 0}

    def put(fmt, o_old, v): struct.pack_into(fmt, new, mv(o_old), v)

    # An NRO's segments are contiguous, so include the padding in its read-only segment.
    if len(old) >= 0x40 and old[0x10:0x14] == b'NRO0':
        size = struct.unpack_from('<I', old, 0x18)[0]
        if size != len(old): raise Error(f'NRO header size {size:#x} != image {len(old):#x}')
        rsz += D
        struct.pack_into('<I', new, 0x18, size + D)
        struct.pack_into('<I', new, 0x2c, rsz)
        struct.pack_into('<I', new, 0x30, do + D)

    # MOD0: offset at +4, and six offsets relative to MOD0 itself
    m0 = struct.unpack_from('<I', old, 4)[0]
    if m0 + 28 > len(old) or old[m0:m0 + 4] != b'MOD0': raise Error(f'no MOD0 at {m0:#x}')
    struct.pack_into('<I', new, 4, mv(m0))
    mod0 = list(struct.unpack_from('<6i', old, m0 + 4))
    for i, r in enumerate(mod0): put('<i', m0 + 4 + 4 * i, mv(m0 + r) - mv(m0))

    # dynamic
    dyn = m0 + mod0[0]; o = dyn; tags = {}
    while True:
        t, v = struct.unpack_from('<qQ', old, o)
        if t == 0: break
        tags[t] = v
        if t in PTR_TAGS and mv(v) != v: put('<Q', o + 8, mv(v)); n['dyn'] += 1
        o += 16
    if 17 in tags: raise Error('DT_REL relocations (only RELA is expected on AArch64)')

    # symbols: nchain of DT_HASH, else up to DT_STRTAB (STT_TLS values are TLS block offsets, not module addresses)
    symtab, strtab = tags.get(6), tags.get(5)
    if symtab is not None:
        if 4 in tags: nsym = struct.unpack_from('<I', old, tags[4] + 4)[0]
        elif strtab is not None: nsym = (strtab - symtab) // 24
        else: raise Error('DT_SYMTAB without DT_HASH or DT_STRTAB')
        for i in range(nsym):
            s = symtab + 24 * i
            st_type = old[s + 4] & 0xF
            shndx = struct.unpack_from('<H', old, s + 6)[0]; val = struct.unpack_from('<Q', old, s + 8)[0]
            if shndx and shndx < 0xff00 and st_type != 6 and mv(val) != val: put('<Q', s + 8, mv(val)); n['sym'] += 1

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
            if not v & 1:
                put('<Q', e, mv(v)); slots = [v]; where = v + 8
            else:
                if where is None: raise Error(f'DT_RELR bitmap before start at {e:#x}')
                slots = [where + 8 * b for b in range(63) if v >> (b + 1) & 1]; where += 63 * 8
            for a in slots: put('<Q', a, mv(struct.unpack_from('<Q', old, a)[0])); n['relr'] += 1

    # text: adrp / adr / literal loads into the moved part.
    # +4 is the MOD0 u32 offset (never code); small inline literal pools in text (below nso_code.DATA_MIN) are collected from in-text literal loads so their bytes are not rewritten.
    from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    blocks = list(nso_code.data_blocks(bytearray(old), tsz, md)) + [(4, 8)]
    if m0 < tsz: blocks.append((m0, min(tsz, m0 + 28)))   # MOD0 in text: b'MOD0' (0x30444F4D) matches the adr bitmask
    code = lambda a: not any(s <= a < e for s, e in blocks)
    sx = lambda v, b: v - (1 << b) if v >> (b - 1) & 1 else v
    lit_words = set()
    for a in range(0, tsz, 4):
        if not code(a): continue
        w = struct.unpack_from('<I', old, a)[0]
        if w & 0x3B000000 == 0x18000000 and (w >> 30) != 3:   # ldr / ldrsw (literal), not prfm or unallocated opc=3 V=1
            t = a + sx((w >> 5) & 0x7FFFF, 19) * 4
            if 0 <= t < tsz:
                opc, vec = w >> 30, (w >> 26) & 1
                width = 4 if opc == 0 else 8 if opc == 1 else (16 if vec else 4)
                for p in range(t & ~3, min(tsz, t + width), 4): lit_words.add(p)
    for a in range(0, tsz, 4):
        if not code(a) or a in lit_words: continue
        w = struct.unpack_from('<I', old, a)[0]
        if w & 0x1F000000 == 0x10000000:   # adr / adrp
            imm = sx(((w >> 5) & 0x7FFFF) << 2 | (w >> 29) & 3, 21)
            if w >> 31:
                t = (a & ~0xFFF) + (imm << 12)
                if t < do: continue
                imm += D >> 12; n['adrp'] += 1
            else:
                t = a + imm
                if t < do: continue
                imm += D; n['adr'] += 1
            if not -(1 << 20) <= imm < 1 << 20: raise Error(f'adr(p) at {a:#x} out of range after the move')
            imm &= 0x1FFFFF
            w = (w & 0x9F00001F) | (imm & 3) << 29 | (imm >> 2) << 5
            struct.pack_into('<I', new, a, w)
        elif w & 0x3B000000 == 0x18000000 and (w & 0xFF000000) != 0xDC000000:   # ldr / ldrsw / prfm (literal), excluding unallocated opc=3 V=1
            imm = sx((w >> 5) & 0x7FFFF, 19); t = a + imm * 4
            if t < do: continue
            imm += D >> 2
            if not -(1 << 18) <= imm < 1 << 18: raise Error(f'literal load at {a:#x} out of range after the move')
            struct.pack_into('<I', new, a, (w & 0xFF00001F) | (imm & 0x7FFFF) << 5); n['literal'] += 1

    # .eh_frame_hdr and .eh_frame (old offsets; a pointer read at o from base b to target t is rewritten as mv(t) - mv(b))
    ehs, ehe = m0 + mod0[3], m0 + mod0[4]
    if ehe - ehs >= 8 and old[ehs] == 1:
        def fix(o, enc, optional=False):
            """rewrite the encoded pointer at old offset o; returns (old target, size)"""
            enc &= 0x7F   # strip DW_EH_PE_indirect: adjust the stored pointer itself, not its dereference
            fmt, app = enc & 0xF, enc & 0x70
            info_f = FMT_INFO.get(fmt)
            if not info_f: raise Error(f'pointer encoding {enc:#x} at {o:#x}')
            f, lo_v, hi_v = info_f
            v = struct.unpack_from(f, old, o)[0]; sz = struct.calcsize(f)
            if optional and v == 0: return 0, sz
            base = o if app == 0x10 else ehs if app == 0x30 else 0 if app == 0 else None
            if base is None: raise Error(f'pointer application {app:#x} at {o:#x}')
            t = base + v; nv = mv(t) - mv(base)
            if not lo_v <= nv <= hi_v: raise Error(f'encoded pointer {enc:#x} at {o:#x} overflows after the move ({nv:#x})')
            if nv != v: put(f, o, nv); n['eh'] += 1
            return t, sz
        _, penc, cenc, tenc = old[ehs:ehs + 4]
        o = ehs + 4
        ehf = None
        if penc != 0xFF:
            ehf, s = fix(o, penc); o += s
        if cenc == 0xFF:
            cnt = 0
        elif (cenc & 0xF) in (0x3, 0xB):
            cnt = struct.unpack_from('<I' if (cenc & 0xF) == 3 else '<i', old, o)[0]; o += 4
        else:
            raise Error(f'eh_frame_hdr count encoding {cenc:#x}')
        if tenc not in (0xFF, 0):
            for _ in range(cnt):
                o += fix(o, tenc)[1]; o += fix(o, tenc)[1]
        # walk .eh_frame
        if ehf is not None:
            cies, o = {}, ehf
            while o + 4 <= len(old):
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
                    r_enc, l_enc, has_p = 0, None, False
                    if aug.startswith(b'z'):
                        _, p = nso_code._uleb(old, p)
                        for ch in aug[1:].decode():
                            if ch == 'R': r_enc = old[p]; p += 1
                            elif ch == 'L': l_enc = old[p]; p += 1
                            elif ch == 'P': pe = old[p]; p += 1; p += fix(p, pe)[1]; has_p = True
                            elif ch in 'SB': pass
                            else: raise Error(f'CIE augmentation {aug!r}')
                    cies[o] = (r_enc, l_enc, has_p, aug.startswith(b'z'))
                else:
                    cie = o + 4 - cid
                    r_enc, l_enc, has_p, z = cies[cie]
                    p = o + 8
                    if r_enc != 0xFF:
                        p += fix(p, r_enc)[1]
                        rfmt = FMT_INFO.get(r_enc & 0xF)
                        if not rfmt: raise Error(f'FDE range encoding {r_enc:#x} at {p:#x}')
                        p += struct.calcsize(rfmt[0])   # pc range: no application
                    if z:
                        aug_len, p = nso_code._uleb(old, p)
                        if aug_len > 0 and l_enc is not None and l_enc != 0xFF:
                            lsda, _ = fix(p, l_enc, optional=True)
                            if lsda and has_p:
                                lpe = old[lsda]
                                if lpe != 0xFF: fix(lsda + 1, lpe)
                o = end

    notes = [f'moved data by {D:#x}: {do:#x} -> {do + D:#x}',
             ', '.join(f'{k} {v}' for k, v in n.items())]
    return nso.Module(new, ((0, tsz), (ro, rsz), (do + D, dsz)), mod.bss, mod.build_id), D, notes


def main():
    src = sys.argv[1]
    dst = sys.argv[2] if len(sys.argv) > 2 else None
    with open(src, 'rb') as f: raw = f.read()
    try:
        mod = nso.load(raw, src)
        out, D, notes = relayout_module(mod)
    except (nso.Error, Error) as e:
        sys.exit(f'[RED] {src}: {e}')
    if dst:
        if raw[0x10:0x14] != b'NRO0': sys.exit(f'[RED] {src}: writing an output file is only supported for NROs (NSOs are relaid out in memory by patch_nso.py)')
        with open(dst, 'wb') as f: f.write(bytes(out.image) if D else raw)
    print(f'{os.path.basename(src)}: ' + ('; '.join(notes) if D else 'data already on a 16 KB page of its own'))


if __name__ == '__main__': main()
