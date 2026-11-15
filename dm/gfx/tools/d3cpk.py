#!/usr/bin/env python3
# Diablo III's console packages (Diablo III: Eternal Collection on the Switch: CPKs/*.cpk and Update/CPKs/*.cpk, 37 files, 16 GB). Not CRI's CPK: the file starts with the u32 0xa1b2c3d4
# (little-endian; the game's reader, main+0x76088c in 2.7.7, also takes it byte-swapped) and a version (6 or 7), then bit-packed tables (per entry a 64-bit name hash and three fields
# of 24, 1 and 17 bits in 2.7.7's CoreCommon.cpk; the widths are header words) and the data in 64 KiB sectors (version 7: header +0x3c; 16 KiB sub-blocks, +0x40, each covered by a CRC).
# Each file of the data is either stored or one zlib stream behind a 6-byte record {u32 decoded length, u16 stored length}; the game's SNO assets (0xdeadbeef headers) are such streams.
# The tables are not read here: streams() finds every stream by its record and keeps one only when it inflates to exactly its decoded length and ends where its stored length says, so a
# false start in stored data (textures, sound) is left out. No stream crosses a sector: in ClientCommon.cpk all 57,942 store at most 0xffff bytes, the stored length's whole range.
# Usage: d3cpk.py <file.cpk> lists the streams (offset, stored and decoded length, the SNO header's first words).
import re, struct, sys, os, zlib
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '../../tools'))
from romfs import open

MAGIC = struct.pack('<I', 0xa1b2c3d4)
HEAD = 0x48   # version 7's header (version 6: 0x40)
ZLIB = re.compile(rb'\x78[\x01\x5e\x9c\xda]')

def is_d3cpk(head):
    """The file's first 8 bytes are a Diablo III package's: the magic and version 6 or 7"""
    return len(head) >= 8 and head[:4] == MAGIC and struct.unpack_from('<I', head, 4)[0] in (6, 7)

SPAN = 6 + 0xffff   # what a stream can take up past the start of its record

def streams(d, start=0, end=None, base=0):
    """The zlib streams whose 6-byte record starts in d[start:end] → [(record offset, stored length, data)], in file order. d holds the file from offset base (a byte range read
    with SPAN bytes more than the range, so its last stream is whole); the offsets are d's"""
    end = len(d) if end is None else end
    out, last = [], -1
    for m in ZLIB.finditer(d, max(start + 6, HEAD - base + 6), min(end + 6, len(d))):
        p = m.start() - 6
        if p < last: continue   # inside the stream just taken
        n, stored = struct.unpack_from('<IH', d, p)
        if not n or stored < 3 or p + 6 + stored > len(d): continue
        z = zlib.decompressobj()
        try: x = z.decompress(d[p + 6:p + 6 + stored], n + 1)
        except zlib.error: continue
        if not z.eof or z.unused_data or len(x) != n: continue
        out.append((p, stored, x)); last = p + 6 + stored
    return out

if __name__ == '__main__':
    with open(sys.argv[1], 'rb') as f: d = f.read()
    if not is_d3cpk(d[:8]): sys.exit(f'{sys.argv[1]}: not a Diablo III package')
    for p, stored, x in streams(d):
        print(f'{p:#x}\t{stored}\t{len(x)}\t{x[:16].hex(" ")}')
