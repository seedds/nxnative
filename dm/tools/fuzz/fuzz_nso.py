#!/usr/bin/env python3
"""Fuzz the NSO loader of the shader tools, shader_split.nso_image: it reads the game's ExeFS main. uepak.nso_image is the same code, and dm/tools/nso.py, patch_nso.py and
nso_code.py repeat it at module level (where it cannot be called), so what is found here holds for them too.
Seeds: an NSO0 header with three small segments, text LZ4-compressed, ro and data stored (and the same with every segment compressed).
Clean rejection: neither declares one, so the errors that name malformed data count as one: ValueError, struct.error (a header cut short) and lz4's LZ4BlockError. The loaded image
must have the length the header declares (data segment offset + size): a shorter or longer one means segments were laid out at the wrong place. Any other exception, a wrong
length, or an allocation beyond fuzzlib.allowed() (a segment size or offset far past what the file can hold) is a finding. Usage: see fuzzlib.py"""
import struct
import sys

import fuzzlib
import lz4.block

shader_split = fuzzlib.import_shader_split()

HARNESS = 'nso'
fuzzlib.guard(shader_split)


def nso(segments, compress):
    """NSO0: +0xc flags (bit i: segment i compressed), +0x10 + 0x10 i {file offset, memory offset, size}, +0x3c bss, +0x40 module ID, +0x60 stored sizes"""
    head, body, mem, stored = bytearray(0x100), bytearray(), 0, []
    head[:4] = b'NSO0'
    for i, seg in enumerate(segments):
        raw = lz4.block.compress(seg, store_size=False) if compress[i] else seg
        struct.pack_into('<III', head, 0x10 + 0x10 * i, 0x100 + len(body), mem, len(seg))
        body += raw; stored.append(len(raw)); mem += (len(seg) + 0xfff) & ~0xfff
    struct.pack_into('<I', head, 0xc, sum(1 << i for i, c in enumerate(compress) if c))
    struct.pack_into('<I', head, 0x3c, 0x1000); head[0x40:0x60] = bytes(range(32)); struct.pack_into('<III', head, 0x60, *stored)
    return bytes(head + body)


def seeds():
    text = bytes.fromhex('fd7bbfa9fd030091') * 32 + bytes.fromhex('c0035fd6')   # stp / mov / ret
    ro, data = b'fuzz ro segment\0' * 8, struct.pack('<4Q', 1, 2, 3, 4)
    return {'text-lz4': nso([text, ro, data], (1, 0, 0)), 'all-lz4': nso([text, ro, data], (1, 1, 1))}


_file = fuzzlib.InputFile('main')


def check(data):
    p = _file.write(data)
    try: img = shader_split.nso_image(p)
    except (ValueError, struct.error, lz4.block.LZ4BlockError): return   # errors that name malformed data
    _, mo, sz = struct.unpack_from('<III', data, 0x30)
    if len(img) != mo + sz: raise fuzzlib.broken('shader_split.py:nso_image', f'the image is {len(img):#x} bytes, the header lays out {mo + sz:#x}')


if __name__ == '__main__':
    sys.exit(fuzzlib.main(HARNESS, check, seeds))
