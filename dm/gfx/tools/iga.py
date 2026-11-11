#!/usr/bin/env python3
# Alchemy IGA archives (Vicarious Visions / Toys for Bob's Alchemy engine), version 13: Crash Team Racing Nitro-Fueled keeps nearly all its data in 401 of them under archives/ (*.pak,
# 'IGA\x1a'). The header (0x38 bytes, u32 little-endian): magic, version, table of contents length, file count, sector size (0x800), two hash-search values, then the counts of the
# u32, u16 and u8 block tables, the name table's offset, a zero word, its length and a flags word. Then a u32 name hash per file, then a 16-byte record per file {offset, a word not
# used here, size, mode}, then the three block tables. mode 0xffffffff is a file stored as it is; otherwise its top four bits name the compression (2: LZMA) and the rest the index of
# its first block. The file is cut into 0x8000-byte blocks; block k's entry gives, in sectors from the file's offset, where block k starts, its top bit set when the block is
# compressed. Which table holds a file's entries follows from the file's size: up to 0x7f sectors the u8 table (bit 7 = compressed), up to 0x7fff sectors the u16 table (bit 15);
# checked on all 7,290 IGZ files of five archives, each of which then starts with its 'IGZ' magic. A compressed block is a u32 compressed length, the five LZMA property bytes
# (lc / lp / pb, dictionary size) and a raw LZMA1 stream. The name table holds one u32 offset per file, then NUL-terminated names (the build path first).
# shader_split.py reads every IGA through it (iga_jobs): Crash Team Racing's shaders are NVN control sections each followed by its code area in the IGZ files of shaders_nx.pak.
# Usage: iga.py <file.pak> [--extract <dir> [<name regex>]] lists (or extracts) the files.
import lzma, re, struct, sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '../../tools'))
from romfs import open
from pathlib import Path

MAGIC = b'IGA\x1a'
HEAD = struct.Struct('<4sI12I')
BLOCK = 0x8000
STORED, LZMA = 0xffffffff, 2


def files(path):
    """→ [(name, offset, size, mode, (block table entries of the file, compressed-bit mask))] of an IGA version 13 archive; ValueError for any other layout"""
    with open(path, 'rb') as f:
        h = f.read(HEAD.size)
        if len(h) < HEAD.size or h[:4] != MAGIC: raise ValueError(f'{path}: not an IGA archive')
        _, ver, toc, n, sect, _div, _slop, nl, nm, ns, name_off, _, name_len, _flags = HEAD.unpack(h)
        if ver != 13: raise ValueError(f'{path}: IGA version {ver} (only 13 is read)')
        tables = HEAD.size + 4 * n + 16 * n
        if not sect or tables + 4 * nl + 2 * nm + ns > toc + HEAD.size: raise ValueError(f'{path}: the tables run past the table of contents ({toc:#x} bytes)')
        f.seek(0); t = f.read(tables + 4 * nl + 2 * nm + ns)
        f.seek(name_off); names = f.read(name_len)
    if len(t) < tables + 4 * nl + 2 * nm + ns or len(names) < 4 * n: raise ValueError(f'{path}: truncated')
    small = t[tables + 4 * nl + 2 * nm:tables + 4 * nl + 2 * nm + ns]
    medium = struct.unpack_from(f'<{nm}H', t, tables + 4 * nl)
    out = []
    for i in range(n):
        off, _, size, mode = struct.unpack_from('<4I', t, HEAD.size + 4 * n + 16 * i)
        no = struct.unpack_from('<I', names, 4 * i)[0]; end = names.find(b'\0', no)
        if end < 0: raise ValueError(f'{path}: name {i} runs past the name table')
        name = names[no:end].decode('utf-8', 'replace')
        if mode == STORED: out.append((name, off, size, mode, None)); continue
        if mode >> 28 != LZMA: raise ValueError(f'{path}#{name}: compression {mode >> 28} (only LZMA is read)')
        first, count = mode & 0xfffffff, (size + BLOCK - 1) // BLOCK
        if size <= 0x7f * sect: tbl, bit = small, 0x80
        elif size <= 0x7fff * sect: tbl, bit = medium, 0x8000
        else: raise ValueError(f'{path}#{name}: {size} bytes, past the u16 block table (the u32 table is not read)')
        if first + count > len(tbl): raise ValueError(f'{path}#{name}: blocks {first}..{first + count} past its block table ({len(tbl)})')
        out.append((name, off, size, mode, (tuple((tbl[first + k] & (bit - 1)) * sect for k in range(count)), tuple(bool(tbl[first + k] & bit) for k in range(count)))))
    return out


def lzma_block(c, n, where):
    """One compressed block: u32 compressed length, five LZMA property bytes, the raw LZMA1 stream; decoded to n bytes"""
    if len(c) < 9: raise ValueError(f'{where}: truncated block')
    cn, p = struct.unpack_from('<I', c)[0], c[4]
    if p >= 9 * 5 * 5 or 9 + cn > len(c): raise ValueError(f'{where}: bad LZMA block header')
    flt = [{'id': lzma.FILTER_LZMA1, 'lc': p % 9, 'lp': p // 9 % 5, 'pb': p // 45, 'dict_size': struct.unpack_from('<I', c, 5)[0]}]
    try: d = lzma.LZMADecompressor(lzma.FORMAT_RAW, filters=flt).decompress(c[9:9 + cn], max_length=n)
    except lzma.LZMAError as e: raise ValueError(f'{where}: {e}') from None
    if len(d) != n: raise ValueError(f'{where}: {len(d)} bytes, the block holds {n}')
    return d


def read(path, ent, f=None):
    """A file's bytes; f: the archive already open (a range of files reads through one handle)"""
    if f is None:
        with open(path, 'rb') as f: return read(path, ent, f)
    nm, off, size, mode, blocks = ent
    if blocks is None:
        f.seek(off); d = f.read(size)
    else:
        starts, packed = blocks; d = bytearray()
        for k, (at, z) in enumerate(zip(starts, packed)):
            n = min(BLOCK, size - k * BLOCK)
            f.seek(off + at)
            if z:
                c = f.read(4); c += f.read(5 + struct.unpack_from('<I', c)[0]) if len(c) == 4 else b''
                d += lzma_block(c, n, f'{path}#{nm} block {k}')
            else: d += f.read(n)
        d = bytes(d)
    if len(d) != size: raise ValueError(f'{path}#{nm}: {len(d)} bytes, the archive says {size}')
    return d


if __name__ == '__main__':
    p = sys.argv[1]
    es = files(p)
    if '--extract' in sys.argv:
        i = sys.argv.index('--extract'); out = Path(sys.argv[i + 1]); rx = re.compile(sys.argv[i + 2]) if len(sys.argv) > i + 2 else None
        for e in es:
            if rx and not rx.search(e[0]): continue
            dst = out / e[0].lstrip('/'); dst.parent.mkdir(parents=True, exist_ok=True); dst.write_bytes(read(p, e))
    else:
        for nm, off, size, mode, blocks in es: print(f'{nm}\t{off:#x}\t{size}\t{"stored" if blocks is None else f"{len(blocks[0])} LZMA blocks"}')
