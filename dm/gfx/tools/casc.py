#!/usr/bin/env python3
# Blizzard's CASC storage as Diablo II: Resurrected ships it on the Switch (its .build.config: "build-playbuild-installer = ngdptool_casc2"): the RomFS folder data/ holds the files' bytes in
# archives named <chunk>-<archive>.data (two decimal digits, eight decimal digits: 00-00000001.data … 17-00000002.data, 40 files, 8.4 GB in 1.0.34) and lists them in text manifests, not in
# the PC client's binary .idx files. build_key.txt holds the current build's config key; the manifest whose "## config-key" comment line names it (build_manifest.psv) lists the current files.
# A manifest is pipe-separated text: comment lines start with '#', its header line names the columns with their types (FileId!STRING:0|KeyLayout!DEC:4|Chunk!DEC:8|Archive!DEC:8|CKey!HEX:16|
# CSize!DEC:4|EKey!HEX:16|ESize!DEC:4|Encoding!STRING:0|Path!STRING:0|Flags!STRING:0), one line per file. The encoded key's last five bytes are the file's offset in its archive (key-layout-0
# "8 8 40 1": chunk and archive in the two bytes before it, which match the Chunk and Archive columns in every line of 1.0.34); ESize is its stored length, CSize its decoded length.
# Encoding (ESpec): 'z' one zlib stream, 'n' stored, 'b:…' a BLTE container ('BLTE', u32 BE header length, u8 flags 0x0f, u24 BE chunk count, per chunk {u32 BE stored length, u32 BE decoded
# length, 16-byte MD5}, then the chunks, each a mode byte 'N' (stored) or 'Z' (zlib) and its data). 1.0.34's build manifest: 98,298 'z', 997 'n', 33,118 'b:64K*=n' (music, video).
# Usage: casc.py <data folder> lists the current files (path, archive, offset, stored and decoded length, encoding).
import os, struct, sys, zlib
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '../../tools'))
from romfs import os, open

COLUMNS = ('FileId', 'Chunk', 'Archive', 'CSize', 'EKey', 'ESize', 'Encoding')


class Error(ValueError):
    pass


def archive_name(chunk, archive):
    return f'{chunk:02d}-{archive:08d}.data'


def is_casc(folder):
    """A folder of this layout: build_key.txt beside at least one manifest (.psv) and one archive"""
    try: names = os.listdir(folder)
    except OSError: return False
    return 'build_key.txt' in names and any(n.endswith('.psv') for n in names) and any(n.endswith('.data') and len(n) == 16 and n[2] == '-' for n in names)


def manifest(text):
    """A manifest's text → (its config key or None, [{column: value}] for its file lines); Error when the header names a column this reader needs no column for"""
    key, cols, rows = None, None, []
    for line in text.splitlines():
        if not line: continue
        if line.startswith('#'):
            k, _, v = line.lstrip('#').partition('=')
            if k.strip() == 'config-key': key = v.strip()
            continue
        f = line.split('|')
        if cols is None:
            if '!' not in f[0]: raise Error(f'no header line before the first file line: {line[:80]!r}')
            cols = [c.split('!')[0] for c in f]
            if missing := [c for c in COLUMNS if c not in cols]: raise Error(f'manifest without the columns {", ".join(missing)}')
            continue
        if len(f) != len(cols): raise Error(f'{len(f)} fields where the header names {len(cols)}: {line[:80]!r}')
        rows.append(dict(zip(cols, f)))
    if cols is None: raise Error('no header line')
    return key, rows


def files(folder):
    """The current build's files: the manifest whose config key build_key.txt names → [(file ID, archive name, offset, stored length, decoded length, encoding)], in manifest order.
    Error when no manifest names that key, or an encoded key's chunk and archive bytes differ from its line's columns"""
    with open(os.path.join(folder, 'build_key.txt'), 'rb') as f: want = f.read().decode('ascii').strip()
    for n in sorted(os.listdir(folder)):
        if not n.endswith('.psv'): continue
        with open(os.path.join(folder, n), 'rb') as f: text = f.read().decode('utf-8')
        if not text.startswith(('#', 'FileId')) or 'FileId!' not in text[:4096]: continue   # archive_manifest.psv lists the archives, not files
        key, rows = manifest(text)
        if key != want: continue
        out = []
        for r in rows:
            e, chunk, arch = bytes.fromhex(r['EKey']), int(r['Chunk']), int(r['Archive'])
            if len(e) != 16 or (e[9], e[10]) != (chunk, arch): raise Error(f'{n}: {r["FileId"]}: encoded key {r["EKey"]} does not end in chunk {chunk}, archive {arch} and an offset')
            out.append((r['FileId'], archive_name(chunk, arch), int.from_bytes(e[11:], 'big'), int(r['ESize']), int(r['CSize']), r['Encoding']))
        return out
    raise Error(f'no manifest in {folder} has the config key {want} that build_key.txt names')


def blte(d):
    """A BLTE container's decoded bytes (chunks 'N' stored or 'Z' zlib); Error for another chunk mode or a length that does not add up"""
    if d[:4] != b'BLTE': raise Error(f'not BLTE: {d[:4].hex()}')
    hl, = struct.unpack_from('>I', d, 4)
    if not hl: return decode_chunk(d[8:])
    if d[8] != 0x0f: raise Error(f'BLTE chunk table flags {d[8]:#x}')
    n = int.from_bytes(d[9:12], 'big'); at, out = hl, bytearray()
    if 12 + 24 * n != hl: raise Error(f'BLTE header of {hl} bytes for {n} chunks')
    for i in range(n):
        stored, size = struct.unpack_from('>II', d, 12 + 24 * i)
        x = decode_chunk(d[at:at + stored])
        if len(x) != size: raise Error(f'BLTE chunk {i}: {len(x)} bytes where the table says {size}')
        out += x; at += stored
    if at != len(d): raise Error(f'BLTE chunks end at {at}, the container at {len(d)}')
    return bytes(out)


def decode_chunk(c):
    if c[:1] == b'N': return c[1:]
    if c[:1] == b'Z': return zlib.decompress(c[1:])
    raise Error(f'BLTE chunk mode {c[:1]!r}')


def decode(d, enc, size):
    """A file's stored bytes → its bytes, by its encoding; Error when they do not decode to its decoded length"""
    if enc == 'n': x = d
    elif enc == 'z': x = zlib.decompress(d)
    elif enc.startswith('b:'): x = blte(d)
    else: raise Error(f'encoding {enc!r}')
    if len(x) != size: raise Error(f'{len(x)} bytes where the manifest says {size}')
    return x


def read(f, ent):
    """One file of an open archive (an entry of files())"""
    _, _, off, stored, size, enc = ent
    f.seek(off); d = f.read(stored)
    if len(d) != stored: raise Error(f'{stored} bytes at {off:#x} run past the archive\'s end')
    return decode(d, enc, size)


if __name__ == '__main__':
    for e in files(sys.argv[1]): print(*e, sep='\t')
