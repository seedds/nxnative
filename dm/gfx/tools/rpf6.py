#!/usr/bin/env python3
"""Rockstar's RAGE package format, version 6 (Red Dead Redemption on Switch: 134 .rpf files in the RomFS).

Header, big-endian: 'RPF6', u32 entry count, u32 TOC length, u32 encryption (0xfffffffd). The TOC starts at +0x10 and is AES-256 (ECB) encrypted, the cipher applied 16 times in a row;
the key is a 32-byte window of the game's own `main` (found by decrypting the first records, never printed). It holds 20-byte big-endian records:
  directory: hash, 0, 0x80000000 | first child index, child count, 0 (record 0 is the root: hash 0, no name table; names are Jenkins hashes)
  file:      hash, stored length, data offset / 8, flags | length, 0   (flag 0x40000000: one zstd frame of `length` bytes; otherwise stored)
Entries are not encrypted. No names are kept, so a file is recognised by its first bytes (RDR's shader archive `shaders.rpf` holds 'rgxd' resources)."""
import concurrent.futures as cf, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import uepak

MAGIC, ROUNDS, AES_FLAG, COMPRESSED = b'RPF6', 16, 0xfffffffd, 0x40000000
ZSTD = bytes.fromhex('28b52ffd')

def header(path):
    with open(path, 'rb') as f: h = f.read(16)
    if h[:4] != MAGIC: return None
    n, tsz, enc = struct.unpack_from('>3I', h, 4)
    if enc != AES_FLAG: sys.exit(f'[RED] {path}: RPF6 TOC encryption {enc:#x}, only {AES_FLAG:#x} (AES) is read')
    return n, tsz

def decrypt(key, blk):
    for _ in range(ROUNDS): blk = uepak.aes_decrypt(key, blk)
    return blk

def _scan(args):
    seg, blk = args
    for o in range(0, len(seg) - 31, 4):
        w = seg[o:o + 32]
        if len(set(w)) < 20: continue
        h, a, c, _, z = struct.unpack_from('>5I', decrypt(w, blk))   # the root record: no name, no size, a directory with children
        if h == 0 and a == 0 and c >> 31 and z == 0: return w
    return None

def find_key(img, lo, hi, path, jobs=8):
    """The AES key among main's 32-byte windows (read-only and data segments, 4-aligned, at least 20 distinct bytes): the one that decrypts the TOC's first two blocks to a root record
    (hash 0, 0, directory flag, …, 0). A scan of Red Dead Redemption's 31 MB takes about two minutes with 8 processes."""
    with open(path, 'rb') as f: f.seek(16); blk = f.read(32)
    step = ((hi - lo) // (jobs * 8) + 3) & ~3
    parts = [(img[a:a + step + 28], blk) for a in range(lo, hi - 32, step)]
    with cf.ProcessPoolExecutor(jobs) as ex:
        for k in ex.map(_scan, parts):
            if k: return k
    return None

def entries(path, key):
    """→ ([(hash, stored length, data offset, length, compressed)] of the plain files, the number of records of other kinds). Only these two kinds are read: shaders.rpf has nothing else.
    The other archives also hold resources (bit 31 of the length word, in 26,892 records of 128 archives) and records with a non-zero last word; their packing is not known here"""
    hd = header(path)
    if not hd: sys.exit(f'[RED] {path}: not an RPF6 archive')
    n = hd[0]
    with open(path, 'rb') as f: f.seek(16); toc = f.read((n * 20 + 15) & ~15)
    toc = decrypt(key, toc)
    out, other = [], 0
    for i in range(n):
        h, stored, off, fl, z = struct.unpack_from('>5I', toc, i * 20)
        if off >> 31: continue   # a directory
        if z or fl >> 31: other += 1; continue
        out.append((h, stored, off * 8, fl & ~COMPRESSED, bool(fl & COMPRESSED)))
    return out, other

def read(f, entry, zstd, limit=None):
    """An entry's bytes, None when it is compressed with something other than zstd (its first `limit` when given: the first blocks of the zstd frame are enough for them)"""
    _, stored, off, length, z = entry
    f.seek(off)
    raw = f.read(min(stored, 1 << 18) if limit and z else stored)   # a zstd block is at most 128 KB
    if not z: return raw[:limit] if limit else raw
    if raw[:4] != ZSTD: return None   # another packing (resource archives hold such plain files; not read)
    d = zstd.ZstdDecompressor()
    out = d.decompress(raw, max_length=limit) if limit else d.decompress(raw)
    if not limit and len(out) != length: sys.exit(f'[RED] RPF6 entry {entry[0]:#010x}: {len(out)} bytes, expected {length}')
    return out
