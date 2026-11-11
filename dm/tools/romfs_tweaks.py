#!/usr/bin/env python3
"""RomFS tweaks: settings whose values pick a rewritten copy of some of the game's own files (docs/settings.md, Decoration distance).

A game profile's romfs_tweaks.tsv lists, per setting value, which numbers to change in which RomFS files. The packer runs this script on the
player's own RomFS (an image the NSP script made, or a folder) and writes <out>/<VAR>/<value>/<RomFS path> for each value; the Mac app carries
them in Contents/Resources/dm/tweaks, and its launcher (dm/mac/launcher.c romfs_tweaks) adds the chosen value's folder to DM_ROMFS_OVERLAY.
Nothing of the game is in the repository: the rows name keys and numbers only, and every copy is made at build time from the player's dump.

  romfs_tweaks.py <romfs_tweaks.tsv> <RomFS image or folder> <out dir>

Files: zstd-compressed or plain, a SARC whose BYML members are rewritten, or a BYML (version 2 to 7, little endian). A row changes every scalar
under its key, in every member: `scale` multiplies a float (or an int, rounded), `set` writes the number. A row that changes nothing stops the
build (a game version whose files moved or renamed the key), as does a file the RomFS does not have."""
import os, struct, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import romfs_image

try: from compression import zstd   # Python 3.14
except ImportError:
    from backports import zstd   # pyright: ignore[reportMissingImports]  # an optional package, only Python before 3.14 needs it


def rows(tsv):   # [(var, value, file, key, op, number)]
    out = []
    for n, line in enumerate(Path(tsv).read_text(encoding='utf-8').splitlines(), 1):
        if not line.strip() or line.startswith('#'): continue
        c = line.split('\t')
        if len(c) != 6: sys.exit(f'{tsv}:{n}: 6 columns expected (variable, value, file, key, op, number), {len(c)} found')
        var, value, file, key, op, num = c
        if not var.startswith('DM_') or not value or '/' in value or value.startswith('.'): sys.exit(f'{tsv}:{n}: a DM_ variable and a value that is a folder name: {var} {value}')
        if op not in ('scale', 'set'): sys.exit(f'{tsv}:{n}: op is scale or set: {op}')
        out.append((var, value, file.strip('/'), key, op, float(num)))
    return out


class Source:   # a RomFS image (romfs_image.py) or a folder
    def __init__(self, path):
        self.path = Path(path)
        self.index = None if self.path.is_dir() else romfs_image.read_index(self.path)

    def read(self, rel):
        if self.index is None:
            p = self.path / rel
            if not p.is_file(): sys.exit(f'romfs_tweaks: {rel} is not in {self.path}')
            return p.read_bytes()
        e = self.index.get(rel)
        if not e or e[0]: sys.exit(f'romfs_tweaks: {rel} is not in {self.path}')
        with open(self.path, 'rb') as f:
            f.seek(e[1]); return f.read(e[2])


def sarc_members(b):   # {name: (start, end)} of a SARC's members, offsets in b
    hl, bom, size, doff = struct.unpack_from('<HHII', b, 4)
    if bom != 0xFEFF: sys.exit('romfs_tweaks: only little-endian SARC archives are read')
    n = struct.unpack_from('<H', b, hl + 6)[0]; names = hl + 12 + 16 * n + 8; out = {}
    for i in range(n):
        h, attr, s, e = struct.unpack_from('<IIII', b, hl + 12 + 16 * i)
        o = names + (attr & 0xffffff) * 4
        out[b[o:b.index(b'\0', o)].decode('utf-8', 'replace') if attr >> 24 else f'#{i}'] = (doff + s, doff + e)
    return out


def byml_scalars(b, base):   # [(key, type, value offset in b)] of a BYML's int and float values; key: the dictionary key they sit under (arrays keep their dictionary's)
    if b[base:base + 2] != b'YB': return []
    ko, so, ro = struct.unpack_from('<III', b, base + 4)
    keys = []
    if ko:
        n = struct.unpack_from('<I', b, base + ko)[0] >> 8
        offs = struct.unpack_from(f'<{n + 1}I', b, base + ko + 4)
        keys = [b[base + ko + o:b.index(b'\0', base + ko + o)].decode('utf-8', 'replace') for o in offs[:n]]
    out, seen = [], set()

    def node(off, key):
        if off in seen or not ro: return
        seen.add(off); t = b[base + off]; n = struct.unpack_from('<I', b, base + off)[0] >> 8
        if t == 0xC1:   # dictionary: (key index:24, type:8, value) entries
            for i in range(n):
                e = base + off + 4 + 8 * i
                val(b[e + 3], e + 4, keys[struct.unpack_from('<I', b, e)[0] & 0xffffff])
        elif t == 0xC0:   # array: the types, padded to 4, then the values
            vs = base + off + 4 + ((n + 3) & ~3)
            for i in range(n): val(b[base + off + 4 + i], vs + 4 * i, key)

    def val(t, at, key):
        if t in (0xC0, 0xC1): node(struct.unpack_from('<I', b, at)[0], key)
        elif t in (0xD1, 0xD2, 0xD3): out.append((key, t, at))

    if ro: node(ro, '')
    return out


def rewrite(data, rules, label):   # data with the rules applied; {rule index: values changed}
    raw = data[:4] == b'\x28\xb5\x2f\xfd'
    b = bytearray(zstd.decompress(data) if raw else data)
    spans = sarc_members(b).values() if b[:4] == b'SARC' else [(0, len(b))]
    hits = {i: 0 for i in range(len(rules))}
    for s, e in spans:
        for key, t, at in byml_scalars(bytes(b), s):
            for i, (k, op, num) in enumerate(rules):
                if k != key: continue
                if t == 0xD2:
                    v = struct.unpack_from('<f', b, at)[0]; struct.pack_into('<f', b, at, v * num if op == 'scale' else num)
                else:
                    fmt = '<i' if t == 0xD1 else '<I'; v = struct.unpack_from(fmt, b, at)[0]
                    struct.pack_into(fmt, b, at, max(0, round(v * num if op == 'scale' else num)) if t == 0xD3 else round(v * num if op == 'scale' else num))
                hits[i] += 1
    return (zstd.compress(bytes(b), 19) if raw else bytes(b)), hits


def build(tsv, romfs, out):
    src, groups = Source(romfs), {}
    for var, value, file, key, op, num in rows(tsv): groups.setdefault((var, value, file), []).append((key, op, num))
    out = Path(out)
    for (var, value, file), rules in groups.items():
        data, hits = rewrite(src.read(file), rules, file)
        for i, n in hits.items():
            if not n: sys.exit(f'romfs_tweaks: {var}={value}: {file} has no {rules[i][0]} value (another version of the game?)')
        dst = out / var / value / file; dst.parent.mkdir(parents=True, exist_ok=True)
        tmp = dst.with_name(dst.name + '.tmp'); tmp.write_bytes(data); os.replace(tmp, dst)
        print(f'tweak {var}={value}: {file}, ' + ', '.join(f'{rules[i][0]} x{n}' for i, n in hits.items()), flush=True)


if __name__ == '__main__':
    if len(sys.argv) != 4: sys.exit((__doc__ or '').split('\n\n')[2])
    build(*sys.argv[1:])
