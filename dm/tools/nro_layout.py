"""Separate an NRO's writable segment from text on Apple's 16 KB pages.
Code and ro keep their offsets. Data and bss move together; ELF pointers and
PC-relative instructions follow them. Ordinary, already separated modules are unchanged.
"""
import struct

import capstone
import nso
import nso_code


def separate_data(mod):
    img, text = mod.image, mod.text_end
    data, size = mod.segments[2]
    start = (text + 0x3fff) & ~0x3fff
    if img[0x10:0x14] != b'NRO0' or data >= start: return mod
    delta, end = start - data, len(img) + mod.bss
    old = bytes(img)

    def moved(v): return v + delta if data <= v <= end else v
    def at(v): return v + delta if v >= data else v
    out = bytearray(old[:data] + bytes(delta) + old[data:])

    def put(fmt, p, v): struct.pack_into(fmt, out, at(p), v)
    def u32(p): return struct.unpack_from('<I', old, p)[0]
    def u64(p): return struct.unpack_from('<Q', old, p)[0]
    m0 = u32(4)
    if old[m0:m0 + 4] != b'MOD0': raise nso.Error('NRO shared page: no MOD0')
    # MOD0 fields are relative to MOD0 itself, including the bss end sentinel.
    for p in range(m0 + 4, m0 + 28, 4):
        v = struct.unpack_from('<i', old, p)[0]
        if v: put('<i', p, moved(m0 + v) - at(m0))
    put('<I', 0x18, len(out)); put('<I', 0x30, start)
    for p in (0x60, 0x68, 0x70, 0x78):
        if u32(p): put('<I', p, moved(u32(p)))

    dyn = m0 + struct.unpack_from('<i', old, m0 + 4)[0]
    tags = {}
    # Pointer-valued DT tags only: lengths, flags and symbol indices are not addresses.
    pointers = {3, 4, 5, 6, 7, 12, 13, 17, 21, 23, 25, 26, 32, 36,
                0x6ffffef5, 0x6ffffff0, 0x6ffffffc, 0x6ffffffe}
    for p in range(dyn, len(old) - 15, 16):
        tag, val = struct.unpack_from('<QQ', old, p)
        if not tag: break
        tags[tag] = val
        if tag in pointers and val: put('<Q', p + 8, moved(val))
    else: raise nso.Error('NRO shared page: unterminated dynamic table')
    if 17 in tags: raise nso.Error('NRO shared page: REL relocations are unsupported')
    for table, count in ((tags.get(7, 0), tags.get(8, 0)), (tags.get(23, 0), tags.get(2, 0))):
        for p in range(table, table + count, 24):
            target, info, addend = struct.unpack_from('<QQq', old, p)
            put('<Q', p, moved(target))
            if info & 0xffffffff == 1027: put('<q', p + 16, moved(addend))   # R_AARCH64_RELATIVE
    # RELR also stores each relative addend in the target slot itself.
    cursor = None
    for p in range(tags.get(36, 0), tags.get(36, 0) + tags.get(35, 0), 8):
        v = u64(p)
        if not v & 1:
            put('<Q', p, moved(v)); slots = [v]; cursor = v + 8
        else:
            if cursor is None: raise nso.Error('NRO shared page: RELR bitmap without an address')
            slots = [cursor + 8 * i for i in range(63) if v >> (i + 1) & 1]
            cursor += 63 * 8
        for slot in slots: put('<Q', slot, moved(u64(slot)))
    if 6 in tags:
        n = u32(tags[4] + 4) if 4 in tags else (tags[5] - tags[6]) // 24
        for p in range(tags[6], tags[6] + n * 24, 24):
            section = struct.unpack_from('<H', old, p + 6)[0]
            if section and section < 0xff00: put('<Q', p + 8, moved(u64(p + 8)))

    # Unwind records here must remain in ro. Reject exception metadata until its
    # LSDA type-table references can also be rebased; never emit a partial rewrite.
    ehs, ehe = nso_code.eh_frame_hdr(old)
    for _, f in nso_code.fde_table(old):
        c = f + 4 - u32(f + 4)
        p = c + 9
        aug = old[p:old.index(b'\0', p)]
        if b'P' in aug or f >= data or c >= data:
            raise nso.Error('NRO shared page: exception or writable unwind metadata needs rebasing')
    if ehs >= data and ehe > ehs:
        raise nso.Error('NRO shared page: writable unwind header needs rebasing')

    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    blocks = nso_code.data_blocks(old, text, md)
    for p in range(0, text, 4):
        if any(a <= p < b for a, b in blocks): continue
        w = u32(p)
        # ADRP and ADR encode a signed 21-bit immediate split around the opcode.
        if w & 0x1f000000 == 0x10000000:
            imm = ((w >> 5 & 0x7ffff) << 2) | (w >> 29 & 3)
            if imm & 0x100000: imm -= 0x200000
            page = bool(w & 0x80000000)
            target = (p & ~0xfff) + (imm << 12) if page else p + imm
            dest = moved(target)
            if dest == target: continue
            imm = (dest - (p & ~0xfff)) >> 12 if page else dest - p
            if not -(1 << 20) <= imm < 1 << 20: raise nso.Error('NRO shared page: ADR target out of reach')
            put('<I', p, (w & ~0x60ffffe0) | ((imm & 3) << 29) | ((imm >> 2 & 0x7ffff) << 5))
        elif w & 0x3b000000 == 0x18000000:   # LDR literal (integer or SIMD), PRFM
            imm = w >> 5 & 0x7ffff
            if imm & 0x40000: imm -= 0x80000
            target = p + imm * 4
            dest = moved(target)
            if dest == target: continue
            imm = (dest - p) // 4
            if not -(1 << 18) <= imm < 1 << 18: raise nso.Error('NRO shared page: literal target out of reach')
            put('<I', p, (w & ~0xffffe0) | ((imm & 0x7ffff) << 5))
    return nso.Module(out, (*mod.segments[:2], (start, size)), mod.bss, mod.build_id)
