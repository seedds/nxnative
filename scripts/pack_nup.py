#!/usr/bin/env python3
# A game in a NUP container (one .nup file: https://github.com/a55uka/nup, SPEC.md v1) → the layout build_ipa_from_nsp.py extracts NSPs into. build_ipa_from_nsp.py calls this when it is given
# a .nup file, or a folder that holds one, and goes on with its other steps; run on its own, this script does only this step.
# A NUP holds the same packages as a dump folder (scripts/pack_dump.py): the base game, updates by version and DLC by title ID, each an exefs/ and romfs/ tree, its files stored raw or as one zstd
# frame each, with a SHA-256 per file. Its files are not encrypted, so it needs no keys. Unlike a dump it may carry control data in each package's meta block: the NACP and the icon. Its name,
# publisher and display version records are labels the packer typed (Hyrule Warriors: Age of Imprisonment's says "HWAoI"), not the console's: without a NACP the name comes from the profile.
# A container may hold no base game, in two forms that mean the same: no base record, with standalone_update on every update (SPEC §3.1.2), or a base record with no files (title ID 0), as packers
# leave when an update holds the whole game (Pokemon Pokopia's v655360). The title ID is then the program ID of the code present, there is no version 0, and the selected update needs its own ExeFS
# and RomFS, taken as the whole game. The flag does not promise that (the spec says such an update needs a matching base elsewhere), and nothing in a container can prove it, so the scripts say so
# (NO_BASE). What was measured: the update RomFS of both NUPs at hand holds every path of its base (Hyrule Warriors: Age of Imprisonment, Yoshi and the Mysterious Book).
# Writes, under the build folder (<folder of the .nup>/<title ID>_build unless --work-dir names another):
#   extract/v<version>/ExeFS/   the version's code modules
#   extract/v<version>/RomFS.img   its RomFS as one image (dm/tools/romfs_image.py), written straight from the container: the RomFS is never unpacked into loose files
#   extract/v<version>/control.nacp, icon.jpg   the package's NACP and its icon, when it carries them (sips reads a PNG icon by its content)
#   aoc/<index>.img   each DLC's RomFS (index = DLC title ID − base title ID − 0x1000), the host's DM_AOC_DIR
# An update's RomFS is packed on its own, never laid over the base: the dumps NUPs are packed from hold the patched RomFS of an update (pack_dump.py); an update with no romfs/ uses the base's.
# Every file read is checked against its SHA-256. A repeat run keeps code modules whose content is the same and an image made from the same container tree by the same packer,
# or, after a packer update, an image whose bytes check against the container's hashes.
# Needs the zstd module: compression.zstd (Python 3.14+) or backports.zstd, as the NSP script.
# Usage: pack_nup.py <game.nup> [--version N] [--work-dir DIR]   (exit 1 on error)
import argparse, hashlib, mmap, os, shutil, struct, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'dm/tools'))
import romfs_image
try: from compression import zstd
except ImportError:
    try: from backports import zstd   # pyright: ignore[reportMissingImports]  # an optional package, only Python before 3.14 needs it
    except ImportError: zstd = None


class Error(Exception): pass


SUPER = struct.Struct('<4sHHQQ32sQQII16s32s')   # 0x80
TOC = struct.Struct('<4sIQQQQQ')   # 0x30
PKG = struct.Struct('<QIIIIBBHIQI20x')   # 0x40
DIR = struct.Struct('<IIIIQ')   # 0x18
FILE = struct.Struct('<IIQQQ32sBBH12x')   # 0x50
TLV = struct.Struct('<HHI')
NO_DIR = 0xFFFFFFFF
BASE, UPDATE, ADDON = 0, 1, 2
KINDS = ('base', 'update', 'addon', 'addon_update', 'system')
STANDALONE = 1   # package flags bit 0: an update in a container without a base
T_NACP, T_ICON, T_LICONS = 2, 3, 10
ARTIFACTS = romfs_image.ARTIFACTS   # dump-tool files a producer may have kept (SPEC §6.3): not game files
NO_BASE = 'no base game in this NUP: its update is taken as the whole game (if the game then stops on missing files, the update holds only what changed: pack a NUP with the base game)'


class Package:
    def __init__(self, nup, index, rec):
        self.nup, self.index = nup, index
        self.title_id, self.root, self.exefs, self.romfs, self.version, self.kind, self.flags, r1, r2, self.meta_off, self.meta_size = rec
        if self.kind >= len(KINDS): raise Error(f'{nup.path}: package {index} of unknown kind {self.kind}')
        if r1 or r2 or self.flags & ~STANDALONE: raise Error(f'{nup.path}: package {index} has reserved fields or flags set')
        self.name = f'{nup.path.name} ({KINDS[self.kind]}' + (f' {self.version}' if self.kind == UPDATE else f' {self.title_id:016X}' if self.kind == ADDON else '') + ')'   # what pick_nsps prints

    def meta(self):   # {TLV type: payload}
        out, b = {}, self.nup.mm[self.meta_off:self.meta_off + self.meta_size] if self.meta_size else b''
        p = 0
        while p + TLV.size <= len(b):
            t, _, n = TLV.unpack_from(b, p); p += TLV.size
            if p + n > len(b): raise Error(f'{self.nup.path}: package {self.index}: meta record {t:#x} runs past its block')
            out.setdefault(t, bytes(b[p:p + n])); p += n
        return out


class Nup:
    """A validated NUP (SPEC §6.1): the superblock, the TOC's SHA-256, every record's bounds. File content is checked against its SHA-256 as it is read"""
    def __init__(self, path):
        self.path = Path(path)
        with open(self.path, 'rb') as f:   # the map keeps its own descriptor
            try: self.mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
            except ValueError: raise Error(f'{self.path}: empty file') from None
        try: self._read()
        except BaseException:
            self.close(); raise

    def _read(self):
        if len(self.mm) < SUPER.size: raise Error(f'{self.path}: not a NUP (too short)')
        magic, hsize, _, toc_off, toc_size, toc_sha, self.blob, fsize, npkg, flags, self.uuid, _ = SUPER.unpack_from(self.mm)
        if magic[:3] != b'NUP': raise Error(f'{self.path}: not a NUP')
        if magic[3] != 1: raise Error(f'{self.path}: NUP format version {magic[3]}; this reads version 1')
        if hsize < SUPER.size or toc_off != hsize or flags: raise Error(f'{self.path}: unsupported NUP header')
        if fsize != len(self.mm): raise Error(f'{self.path}: {len(self.mm):,} bytes, its header says {fsize:,} (copied or downloaded partly?)')
        if self.blob < toc_off + toc_size or self.blob % 16 or self.blob > fsize: raise Error(f'{self.path}: TOC and data regions overlap')
        toc = self.mm[toc_off:toc_off + toc_size]
        self.toc_sha = toc_sha
        if hashlib.sha256(toc).digest() != toc_sha: raise Error(f'{self.path}: table of contents does not match its SHA-256 (damaged file)')
        tmagic, tpkg, self.ndirs, nfiles, pool_off, pool_size, _ = TOC.unpack_from(toc)
        need = TOC.size + tpkg * PKG.size + self.ndirs * DIR.size + nfiles * FILE.size
        if tmagic != b'TOC\x01' or tpkg != npkg or not npkg or len(toc) < need or pool_off < need or pool_off + pool_size > len(toc) or (pool_off - need) % 4:
            raise Error(f'{self.path}: malformed table of contents')
        pool = toc[pool_off:pool_off + pool_size]
        def name(off, n):
            if off + n > len(pool): raise Error(f'{self.path}: name out of bounds')
            return pool[off:off + n]
        p = TOC.size
        self.packages = [Package(self, i, PKG.unpack_from(toc, p + i * PKG.size)) for i in range(npkg)]; p += npkg * PKG.size
        self.dirs = []   # (name, first child slot, child count)
        for _ in range(self.ndirs):
            o, n, first, count, _ = DIR.unpack_from(toc, p); p += DIR.size
            self.dirs.append((name(o, n), first, count))
        self.files = []   # (name, size, sha256, extent offset, extent size, codec)
        for _ in range(nfiles):
            o, n, size, eoff, esz, sha, codec, cflags, _ = FILE.unpack_from(toc, p); p += FILE.size
            if codec not in (0, 1) or cflags: raise Error(f'{self.path}: file {name(o, n)!r} uses codec {codec}; this reads raw and zstd')
            if size and (eoff < self.blob or eoff + esz > fsize or eoff % 16 or not esz or codec == 0 and esz != size): raise Error(f'{self.path}: file {name(o, n)!r} out of bounds')
            if not size and (eoff or esz or codec): raise Error(f'{self.path}: empty file {name(o, n)!r} with data')
            self.files.append((name(o, n), size, sha, eoff, esz, codec))
        self.slots = struct.unpack_from(f'<{(pool_off - p) // 4}I', toc, p)
        for n, first, count in self.dirs:
            if first + count > len(self.slots): raise Error(f'{self.path}: folder {n!r} out of bounds')
        for s in self.slots:
            if s >= self.ndirs + nfiles: raise Error(f'{self.path}: child {s} out of bounds')
        self._acyclic()
        for k in self.packages:
            if k.root >= self.ndirs or any(d != NO_DIR and d >= self.ndirs for d in (k.exefs, k.romfs)) or k.meta_size and k.meta_off + k.meta_size > fsize:
                raise Error(f'{self.path}: package {k.index} out of bounds')

    def _acyclic(self):
        """Refuses a folder that contains itself, at any depth: the bounds checks pass it, and every walk of the tree (tree_bytes, entries) would recurse or loop for ever"""
        kids = [[s for s in self.slots[first:first + count] if s < self.ndirs] for _, first, count in self.dirs]
        state = bytearray(self.ndirs)   # 0 not seen, 1 on the current path, 2 done
        for top in range(self.ndirs):
            if state[top]: continue
            state[top], stack = 1, [(top, 0)]
            while stack:   # depth first without recursion: a container's depth is not bounded by Python's recursion limit
                d, i = stack[-1]
                if i == len(kids[d]): state[d] = 2; stack.pop(); continue
                stack[-1] = (d, i + 1); c = kids[d][i]
                if state[c] == 1: raise Error(f'{self.path}: folder {self.dirs[c][0]!r} (index {c}) contains itself (damaged file)')
                if not state[c]: state[c] = 1; stack.append((c, 0))

    def close(self):
        self.mm.close()

    def __enter__(self): return self
    def __exit__(self, *a): self.close()

    def children(self, d):   # [(name, is dir, index into dirs or files)], sorted by name (bytes), artifacts left out
        _, first, count = self.dirs[d]
        out = []
        for s in self.slots[first:first + count]:
            if s < self.ndirs: out.append((self.dirs[s][0], True, s))
            elif self.files[s - self.ndirs][0] not in ARTIFACTS: out.append((self.files[s - self.ndirs][0], False, s - self.ndirs))
        return sorted(out)

    def child(self, d, name):   # the index of a file in folder d, or None
        return next((i for n, isdir, i in self.children(d) if n == name and not isdir), None)

    def tree_bytes(self, d):   # the uncompressed size of everything under folder d
        return sum(self.files[i][1] if not isdir else self.tree_bytes(i) for _, isdir, i in self.children(d))

    def copy(self, i, out):
        """File i's content to out (anything with write), streamed and checked against its size and SHA-256; returns its size"""
        name, size, sha, eoff, esz, codec = self.files[i]
        h, n, view = hashlib.sha256(), 0, memoryview(self.mm)
        def put(b):
            nonlocal n
            h.update(b); out.write(b); n += len(b)
        if codec == 0:
            for p in range(eoff, eoff + esz, 8 << 20): put(view[p:min(p + (8 << 20), eoff + esz)])
        elif size:
            if zstd is None: raise Error('reading a NUP needs the zstd module: compression.zstd (Python 3.14+) or pip install backports.zstd')
            d = zstd.ZstdDecompressor()
            try:
                for p in range(eoff, eoff + esz, 4 << 20):
                    put(d.decompress(view[p:min(p + (4 << 20), eoff + esz)]))
                    if n > size: break
            except zstd.ZstdError as e: raise Error(f'{self.path}: {name.decode(errors="replace")}: {e}') from e
            if not d.eof: n = -1
        view.release()
        if n != size or h.digest() != sha: raise Error(f'{self.path}: {name.decode(errors="replace")} does not match its size or SHA-256 (damaged file)')
        return n

    def read(self, i):
        class Buf(bytearray):
            write = bytearray.extend
        b = Buf(); self.copy(i, b); return bytes(b)

    def head(self, i, n=32):   # file i's first n bytes (fewer for a shorter file), for a survey's RomFS manifest; not checked against the SHA-256, which covers the whole file
        name, size, sha, eoff, esz, codec = self.files[i]
        if codec == 0 or not size: return bytes(self.mm[eoff:eoff + min(n, size)])
        if zstd is None: raise Error('reading a NUP needs the zstd module: compression.zstd (Python 3.14+) or pip install backports.zstd')
        d, out, p = zstd.ZstdDecompressor(), b'', eoff
        try:
            while len(out) < min(n, size) and p < eoff + esz and not d.eof:
                out += d.decompress(self.mm[p:min(p + (64 << 10), eoff + esz)], max_length=n - len(out)); p += 64 << 10
                while len(out) < min(n, size) and not d.needs_input and not d.eof: out += d.decompress(b'', max_length=n - len(out))
        except zstd.ZstdError as e: raise Error(f'{self.path}: {name.decode(errors="replace")}: {e}') from e
        return out[:n]


def program_id(npdm, where):   # the ACI0 program ID of an NPDM
    if npdm[:4] != b'META': raise Error(f'{where}: not an NPDM')
    off, size = struct.unpack_from('<II', npdm, 0x70)
    if npdm[off:off + 4] != b'ACI0' or size < 0x18: raise Error(f'{where}: no ACI0 section')
    return struct.unpack_from('<Q', npdm, off + 0x10)[0]


def is_nup(path):
    try:
        with open(path, 'rb') as f: return f.read(3) == b'NUP'
    except OSError: return False


def scan(n):
    """What a NUP holds: (title ID, base package or None, {update version: package}, {DLC title ID: package}), with the checks pack_dump.scan makes"""
    bases = [k for k in n.packages if k.kind == BASE]
    if len(bases) > 1: raise Error(f'{n.path}: {len(bases)} base packages')
    updates = [k for k in n.packages if k.kind == UPDATE]
    if not bases and (not updates or any(not k.flags & STANDALONE for k in updates)):
        raise Error(f'{n.path}: no base game: pack the base game and its update into one NUP, or use updates marked standalone_update with complete exefs/ and romfs/')
    base = bases[0] if bases and n.tree_bytes(bases[0].root) else None   # a base record with no files is no base game
    # With no base the game is identified from the code actually present; each selected version is checked by pack_version.
    ids ={program_id(n.read(i), f'{k.name} exefs/main.npdm') for k in n.packages if k.kind in (BASE, UPDATE) and k.exefs != NO_DIR
           if (i := n.child(k.exefs, b'main.npdm')) is not None}
    if not ids: raise Error(f'{n.path}: no exefs/main.npdm in the base or updates')
    if len(ids) != 1: raise Error(f'{n.path}: base and updates have different program IDs: ' + ', '.join(f'{pid:016X}' for pid in sorted(ids)))
    app = ids.pop()
    if app & 0xFFF: raise Error(f'{n.path}: base program ID {app:016X} is not a base title ID')
    vers, dlc = {}, {}
    for k in n.packages:
        if k.kind == UPDATE:
            if k.version in vers: raise Error(f'{n.path}: two updates of version {k.version}')
            if not k.version: raise Error(f'{n.path}: an update of version 0')
            vers[k.version] = k
        elif k.kind == ADDON:
            if not 0 < k.title_id - app - 0x1000 <= 2000: raise Error(f'{n.path}: {k.title_id:016X} is not a DLC of {app:016X}')
            if k.romfs == NO_DIR: raise Error(f'{n.path}: DLC {k.title_id:016X} has no romfs/')
            if k.title_id in dlc: raise Error(f'{n.path}: DLC {k.title_id:016X} more than once')
            dlc[k.title_id] = k
    return app, base, vers, dlc


def exefs(n, k, dst):   # the package's ExeFS into dst, kept as it is when every file there has the same content
    files = []
    for name, isdir, i in n.children(k.exefs):
        if isdir: continue
        # NUP names are labels (§6.1). Validate them before using them as host paths.
        filename = os.fsdecode(name)
        if filename in ('', '.', '..') or '\0' in filename or Path(filename).name != filename:
            raise Error(f'{n.path}: invalid ExeFS name {name!r}')
        if not name.startswith(b'.'): files.append((name, i))
    def same():
        if not dst.is_dir() or {os.fsencode(p.name) for p in dst.iterdir()} != {name for name, _ in files}: return False
        def sha(f):
            with open(f, 'rb') as h: return hashlib.file_digest(h, 'sha256').digest()
        return all(sha(dst / os.fsdecode(name)) == n.files[i][2] for name, i in files)
    if same(): return
    tmp = dst.with_name(dst.name + '.partial')
    shutil.rmtree(tmp, ignore_errors=True); tmp.mkdir(parents=True)
    for name, i in files:
        with open(tmp / os.fsdecode(name), 'wb') as f: n.copy(i, f)
    shutil.rmtree(dst, ignore_errors=True); tmp.rename(dst)


def entries(n, d):   # romfs_image.entries for a folder of the container: [name, file index, is dir, a, b], breadth-first
    ents, i = [[b'', d, True, 0, 0]], 0
    while i < len(ents):
        if ents[i][2]:
            kids = n.children(ents[i][1])
            ents[i][3], ents[i][4] = len(ents), len(kids)
            ents.extend([name, j, isdir, 0, 0] for name, isdir, j in kids)
        i += 1
    return ents


def verified(n, ents, dst):
    """The identity of the image at dst when it is, byte for byte, what write() makes of ents now (romfs_image.layout's header and index, and each file's bytes
    with the container's SHA-256), and it stayed the same while being read; else None. Reads the image, not the container's file data: 21.6 s for Ocarina's 26.25 GB image."""
    ents = [e.copy() for e in ents]
    off = romfs_image.HEAD.size
    for e in ents:
        if not e[2]: e[3], e[4] = off, n.files[e[1]][1]; off += e[4]
    head, idx = romfs_image.layout(ents, off)
    try:
        before = romfs_image.file_identity(dst)
        if before[1] != off + len(idx): return None   # [inode, size, mtime, ctime]
        with open(dst, 'rb') as f:
            if f.read(len(head)) != head: return None
            f.seek(off)
            if f.read(len(idx)) != idx: return None
            f.seek(len(head))
            for _, i, isdir, _, size in ents:   # the data in entry order, one pass forward
                if isdir: continue
                h, left = hashlib.sha256(), size
                while left:
                    chunk = f.read(min(left, 8 << 20))
                    if not chunk: return None
                    h.update(chunk); left -= len(chunk)
                if h.digest() != n.files[i][2]: return None
        return before if romfs_image.file_identity(dst) == before else None
    except OSError:
        return None


def pack(n, d, img, say=print, step=lambda s: None):
    """The RomFS folder d of the container → img. Returns how: 'kept' (the stamp says the container tree and the packers are the same), 'verified' (the stamp is
    from another packer, but the image checks against the container's hashes: its stamp is renewed) or 'packed'. step(name): the step it is in, for a progress line"""
    img.parent.mkdir(parents=True, exist_ok=True)
    ents, dst = entries(n, d), os.fspath(img)
    digest = hashlib.sha256(Path(romfs_image.__file__).read_bytes() + Path(__file__).read_bytes() + n.toc_sha + struct.pack('<I', d)).hexdigest()   # the TOC hash covers every name and file hash
    if romfs_image.reusable(dst, digest): return 'kept'
    if img.is_file():   # a packer update changes the digest, not the image: check its bytes before unpacking minutes of data again
        step('checking the image'); say(f'checking {img} against {n.path.name}')
        if identity := verified(n, ents, dst):
            romfs_image.stamp(dst, digest, identity)
            say('  kept: the image matches the container')
            return 'verified'
    step('unpacking'); say(f'packing {n.path.name} folder {d} → {img}')
    part = dst + '.part'
    try: romfs_image.publish(romfs_image.write(ents, dst, n.copy), dst, digest)
    except BaseException:
        Path(part).unlink(missing_ok=True); raise
    say(f'  {len(ents):,} entries, {img.stat().st_size / 2**30:.2f} GB')
    return 'packed'


def localized(payload):   # the localized icons record (TLV 0x000A) → {lowercase language tag: image bytes}
    out, p = {}, 0
    while p < len(payload):
        ln = payload[p]; tag = payload[p + 1:p + 1 + ln].decode(errors='replace').lower(); p += 1 + ln
        if p + 4 > len(payload): break
        dn = struct.unpack_from('<I', payload, p)[0]; p += 4
        out.setdefault(tag, payload[p:p + dn]); p += dn
    return out


def pick(tags, code):   # the record for a language code: the exact tag, then its primary subtag
    code = code.lower()
    return tags.get(code) or tags.get(code.split('-')[0])


def app_package(n):   # the record that holds application metadata when no base game is there (SPEC §5.1): a base record with no files, as packers attach it there, else the first package
    return next((k for k in n.packages if k.kind == BASE), n.packages[0])


def control(k, out, fallback=None):   # the package's control data beside its ExeFS; what it lacks comes from fallback
    meta = fallback.meta() if fallback is not None else {}
    meta.update(k.meta())
    nacp_bytes = meta.get(T_NACP)
    icons = localized(meta.get(T_LICONS, b''))
    icon = meta.get(T_ICON) or pick(icons, 'en-US') or next(iter(icons.values()), None)
    for name, data in (('control.nacp', nacp_bytes), ('icon.jpg', icon)):
        if data: (out / name).write_bytes(data)
        else: (out / name).unlink(missing_ok=True)


def romfs_of(k, base):   # the RomFS folder a version runs with: its own, or the base game's for an update without one
    d = k.romfs if k.romfs != NO_DIR else base.romfs if base is not None else NO_DIR
    if d == NO_DIR: raise Error(f'{k.name}: no romfs/ in this package or the base game')
    return d


def pack_version(n, k, base, app, out, say=print, step=lambda s: None):   # one version (the base package, or an update package) → out/ExeFS, out/RomFS.img and its control data; returns how pack() got the image
    if k.exefs == NO_DIR or n.child(k.exefs, b'main') is None or n.child(k.exefs, b'main.npdm') is None: raise Error(f'{k.name}: no exefs/main and exefs/main.npdm')
    if (pid := program_id(n.read(n.child(k.exefs, b'main.npdm')), f'{k.name} exefs/main.npdm')) != app: raise Error(f'{k.name}: program ID {pid:016X}, the base is {app:016X}')
    d = romfs_of(k, base)
    out.mkdir(parents=True, exist_ok=True)
    exefs(n, k, out / 'ExeFS')
    how = pack(n, d, out / 'RomFS.img', say, step)
    control(k, out, app_package(n) if base is None else None)
    return how


def pack_dlc(n, app, dlc, aoc, say=print):   # {DLC title ID: package} → aoc/<index>.img; images of DLC no longer in the container go
    want = {f'{tid - app - 0x1000}.img': k for tid, k in dlc.items()}
    if aoc.is_dir():
        for f in aoc.iterdir():
            if f.name.removesuffix('.inputs.json') not in want: f.unlink()
    for name, k in want.items(): pack(n, k.romfs, aoc / name, say)


def main():
    ap = argparse.ArgumentParser(description='Pack a game in a NUP container into the extraction layout of build_ipa_from_nsp.py')
    ap.add_argument('nup', type=Path, help='the .nup file')
    ap.add_argument('--version', type=int, help='the update version to pack (default: the newest in the container; 0 = the base game)')
    ap.add_argument('--work-dir', type=Path, help='the build folder (default: <folder of the .nup>/<title ID>_build)')
    a = ap.parse_args()
    path = a.nup.resolve()
    try:
        with Nup(path) as n:
            app, base, vers, dlc = scan(n)
            ver = a.version if a.version is not None else max(vers, default=0)
            if ver and ver not in vers: raise Error(f'no update {ver} in {path.name} (have: {", ".join(map(str, sorted(vers))) or "none"})')
            k = vers[ver] if ver else base
            if k is None: raise Error(f'{path.name}: no base game (version 0); choose a standalone update version')
            if base is None: print(NO_BASE, flush=True)
            work =(a.work_dir or path.parent / f'{app:016X}_build').resolve()
            out = work / 'extract' / f'v{ver}'
            print(f'{app:016X} version {ver}: {k.name}', flush=True)
            say = lambda s: print(s, flush=True)
            pack_version(n, k, base, app, out, say)
            pack_dlc(n, app, dlc, work / 'aoc', say)
    except (Error, OSError) as e: sys.exit(f'[RED] {e}')
    print(f'done: {out}' + (f', {len(dlc)} DLC in {work / "aoc"}' if dlc else ''))


if __name__ == '__main__':
    main()
