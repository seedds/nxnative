#!/usr/bin/env python3
"""Write a RomFS overlay whose resource size table gives model files extra heap room.

Opt-in stopgap adapted from lyccccy/nxnative PR #27 (a571156ab9d04ac6147a10ae612b3a94d7c5c2d9).
The remaining runtime overhead is unresolved; this does not fix the allocator.
The original report concerned a world-load crash while the game
sets up Model/Obj_TreeConiferous.bfres, a 0x78-byte allocation from the file's resource
heap returns null (main+0x155a79c, written through at main+0x155a7a4). That heap is sized
by the RSTB entry (0x29e520 = the 0x19b010-byte file plus 0x103378 of room), and on the
Mac the setup sometimes needs a little more than that room. A larger RSTB entry only
makes the game create a larger heap for the file; the file itself is unchanged.

usage: botw_model_heap_overlay.py ROMFS OVERLAY_DIR
"""
import argparse, struct, zlib
import sys
import os
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'dm' / 'tools'))
from romfs import Path as RomPath

MAX_TABLE = 16 * 1024 * 1024  # Maximum decoded resource table size.
MAX_ENCODED_TABLE = 16 + MAX_TABLE + (MAX_TABLE + 7) // 8

# Spyder/F5: set both paths here, or pass them on the command line.
ROMFS = None
OVERLAY_DIR = None

PAD_ALL_MODELS = 0x10000              # every Model/*.bfres that is not a texture archive
PAD_NAMED = {'Model/Obj_TreeConiferous.bfres': 0x80000}
RSTB = 'System/Resource/ResourceSizeTable.product.srsizetable'


def yaz0_decode(d):
    if len(d) < 16 or len(d) > MAX_ENCODED_TABLE or d[:4] != b'Yaz0':
        raise ValueError('invalid Yaz0 header or oversized input')
    size = struct.unpack('>I', d[4:8])[0]
    if size > MAX_TABLE:
        raise ValueError('oversized Yaz0 output')
    out, p = bytearray(), 16
    while len(out) < size:
        if p >= len(d):
            raise ValueError('truncated Yaz0 control byte')
        code = d[p]; p += 1
        for bit in range(8):
            if len(out) >= size:
                break
            if code & (0x80 >> bit):
                if p >= len(d):
                    raise ValueError('truncated Yaz0 literal')
                out.append(d[p]); p += 1
                continue
            if p + 2 > len(d):
                raise ValueError('truncated Yaz0 backreference')
            b1, b2 = d[p], d[p + 1]; p += 2
            dist, n = ((b1 & 15) << 8 | b2) + 1, b1 >> 4
            if n == 0:
                if p >= len(d):
                    raise ValueError('truncated Yaz0 length')
                n = d[p] + 0x12; p += 1
            else:
                n += 2
            if dist > len(out) or len(out) + n > size:
                raise ValueError('invalid Yaz0 backreference range')
            for _ in range(n):
                out.append(out[-dist])
    return bytes(out)


def yaz0_store(d):
    """Yaz0 with every byte a literal: valid for any Yaz0 reader, no compression needed."""
    out = bytearray(b'Yaz0' + struct.pack('>I', len(d)) + bytes(8))
    for i in range(0, len(d), 8):
        chunk = d[i:i + 8]
        out.append(0xFF)
        out += chunk
    return bytes(out)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('romfs', nargs='?', default=ROMFS)
    parser.add_argument('overlay_dir', nargs='?', default=OVERLAY_DIR)
    args = parser.parse_args(argv)
    if not args.romfs or not args.overlay_dir:
        parser.error('pass ROMFS and OVERLAY_DIR, or set them at the top of the script for F5')
    romfs, overlay = Path(args.romfs), Path(args.overlay_dir)
    if romfs.resolve() == overlay.resolve() or romfs.resolve() in overlay.resolve().parents:
        parser.error('OVERLAY_DIR must differ from ROMFS; the original table is never overwritten')
    source = RomPath(romfs)
    with (source / RSTB).open('rb') as f:
        compressed = f.read(MAX_ENCODED_TABLE + 1)
    data = bytearray(yaz0_decode(compressed))
    if len(data) < 12 or data[:4] != b'RSTB':
        raise ValueError('invalid RSTB header')
    ncrc, nname = struct.unpack('<II', data[4:12])
    if len(data) != 12 + ncrc * 8 + nname * 132:
        raise ValueError('invalid RSTB entry counts or trailing data')
    names = {}
    for f in source.joinpath('Model').glob('*.sbfres'):
        n = 'Model/' + f.name[:-len('.sbfres')] + '.bfres'
        if '.Tex' not in n:
            crc = zlib.crc32(n.encode())
            if crc in names and names[crc] != n:
                raise ValueError(f'ambiguous model CRC32: {names[crc]} and {n}')
            names[crc] = n
    padded, extra = 0, 0
    seen_crc, seen_names = set(), set()
    for i in range(ncrc):
        p = 12 + i * 8
        crc, size = struct.unpack('<II', data[p:p + 8])
        if crc in seen_crc:
            raise ValueError('duplicate RSTB CRC32 entry')
        seen_crc.add(crc)
        name = names.get(crc)
        if not name:
            continue
        add = PAD_NAMED.get(name, PAD_ALL_MODELS)
        if size + add > 0xffffffff:
            raise ValueError('RSTB budget overflow')
        data[p + 4:p + 8] = struct.pack('<I', size + add)
        padded += 1; extra += add
    p = 12 + ncrc * 8
    for i in range(nname):
        q = p + i * 132
        raw_name = data[q:q + 128]
        if b'\0' not in raw_name:
            raise ValueError('unterminated RSTB name')
        name = raw_name.split(b'\0')[0].decode('utf-8')
        if not name or name in seen_names:
            raise ValueError('empty or duplicate RSTB name')
        seen_names.add(name)
        if name.startswith('Model/') and name.endswith('.bfres') and '.Tex' not in name:
            add = PAD_NAMED.get(name, PAD_ALL_MODELS)
            size = struct.unpack('<I', data[q + 128:q + 132])[0]
            if size + add > 0xffffffff:
                raise ValueError('RSTB budget overflow')
            data[q + 128:q + 132] = struct.pack('<I', size + add)
            padded += 1; extra += add
    if not padded:
        raise ValueError('no eligible model budgets found; no overlay written')
    out = overlay.resolve() / RSTB
    # Resolve the chosen root once, then reject redirects below it before mkdir.
    for parent in (out.parent, out.parent.parent):
        if parent.is_symlink():
            raise ValueError('symlinked output directory')
    out.parent.mkdir(parents=True, exist_ok=True)
    if out.resolve() == (romfs / RSTB).resolve():
        parser.error('output aliases the source table')
    encoded = yaz0_store(bytes(data))
    if yaz0_decode(encoded) != bytes(data):
        raise ValueError('overlay round-trip validation failed')
    # Publish only a complete file; link fails if any destination already exists.
    with tempfile.NamedTemporaryFile(dir=out.parent, prefix='.rstb-', delete=False) as f:
        temporary = Path(f.name)
        try:
            f.write(encoded)
            f.flush()
            os.fsync(f.fileno())
            f.close()
            os.link(temporary, out)
        finally:
            temporary.unlink()
    print(f'{padded} model entries padded, {extra / 2**20:.1f} MB if all were loaded at once -> {out}')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError) as exc:
        sys.exit(f'error: {exc}')
