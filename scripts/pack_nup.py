#!/usr/bin/env python3
# A game in a NUP container (one .nup file: https://github.com/a55uka/nup, SPEC.md v1) → the layout build_ipa_from_nsp.py extracts NSPs into. build_ipa_from_nsp.py calls this when it is given
# a .nup file, or a folder that holds one, and goes on with its other steps; run on its own, this script does only this step.
# A NUP holds the same packages as a dump folder (scripts/pack_dump.py): the base game, updates by version and DLC by title ID, each an exefs/ and romfs/ tree, its files stored raw or as one zstd
# frame each, with a SHA-256 per file. Its files are not encrypted, so it needs no keys. Unlike a dump it may carry control data in each package's meta block: the NACP and the icon. Its name,
# publisher and display version records are labels the packer typed (Hyrule Warriors: Age of Imprisonment's says "HWAoI"), not the console's: without a NACP the name comes from the profile.
# The build script can use a numeric display version label from the selected package after checking control data and the profile.
# A container may hold no base game, in two forms that mean the same: no base record, with standalone_update on every update (SPEC §3.1.2), or a base record with no files (title ID 0), as packers
# leave when an update holds the whole game (Pokemon Pokopia's v655360). The title ID is then the program ID of the code present, there is no version 0, and the selected update needs its own ExeFS
# and RomFS, taken as the whole game. The flag does not promise that (the spec says such an update needs a matching base elsewhere), and nothing in a container can prove it, so the scripts say so
# (NO_BASE). What was measured: the update RomFS of both NUPs at hand holds every path of its base (Hyrule Warriors: Age of Imprisonment, Yoshi and the Mysterious Book).
# Writes, under the build folder (<folder of the .nup>/<title ID>_build unless --work-dir names another):
#   extract/v<version>/ExeFS/   the version's code modules
#   extract/v<version>/RomFS.img   its RomFS as one image (dm/tools/romfs_image.py), written straight from the container: the RomFS is never unpacked into loose files
#   extract/v<version>/control.nacp, icon.jpg   the package's NACP and its icon, when it carries them (sips reads a PNG icon by its content); an update without an icon has its base game's
#   extract/v<version>/romfs_programs/<index>.img   for an application of several programs (a Switch 2 Edition of a Switch 1 game), the RomFS of each program not packed, which the
#                                       packed one mounts by program index (nn::fs::MountRom(name, index, ...), hle_fs.c); --program picks the program, default the highest index
#   aoc/<index>.img   each DLC's RomFS (index = DLC title ID − the NACP's AddOnContentBaseId, else − base title ID − 0x1000), the host's DM_AOC_DIR
# An update's RomFS is packed on its own, never laid over the base: the dumps NUPs are packed from hold the patched RomFS of an update (pack_dump.py); an update with no romfs/ uses the base's.
# Every file read is checked against its SHA-256. A repeat run keeps code modules whose content is the same and an image made from the same container tree by the same packer,
# or, after a packer update, an image whose bytes check against the container's hashes.
# Needs the zstd module: compression.zstd (Python 3.14+) or backports.zstd, as the NSP script.
# Usage: pack_nup.py <game.nup> [--version N] [--program N] [--work-dir DIR]   (exit 1 on error)
import argparse, hashlib, mmap, os, re, shutil, struct, sys
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
T_NACP, T_ICON, T_DISPLAY, T_BUILD_ID, T_LICONS = 2, 3, 4, 7, 10   # SPEC §5's numbers
NACP_SIZE = 0x4000   # every NACP's size: a record of another size is something else
SHIFTED = 'its meta block uses type numbers one too low (upstream nup PR #20). Reading each record one type up'
ARTIFACTS = romfs_image.ARTIFACTS   # dump-tool files a producer may have kept (SPEC §6.3): not game files
NO_BASE = 'no base game in this NUP: its update is taken as the whole game (if the game then stops on missing files, the update holds only what changed: pack a NUP with the base game)'


class Package:
    def __init__(self, nup, index, rec):
        self.nup, self.index = nup, index
        self.title_id, self.root, self.exefs, self.romfs, self.version, self.kind, self.flags, r1, r2, self.meta_off, self.meta_size = rec
        if self.kind >= len(KINDS): raise Error(f'{nup.path}: package {index} of unknown kind {self.kind}')
        if r1 or r2 or self.flags & ~STANDALONE: raise Error(f'{nup.path}: package {index} has reserved fields or flags set')
        self._meta_reported = False
        self.name = f'{nup.path.name} ({KINDS[self.kind]}' + (f' {self.version}' if self.kind == UPDATE else f' {self.title_id:016X}' if self.kind == ADDON else '') + ')'   # what pick_nsps prints

    def meta(self, *, say=None):
        """First payload per TLV type, in SPEC §5 numbering, with wrong-size NACP records omitted."""
        recs = {}
        b = self.nup.mm[self.meta_off:self.meta_off + self.meta_size] if self.meta_size else b''
        p = 0
        while p + TLV.size <= len(b):
            t, _, n = TLV.unpack_from(b, p); p += TLV.size
            if p + n > len(b): raise Error(f'{self.nup.path}: package {self.index}: meta record {t:#x} runs past its block')
            recs.setdefault(t, b[p:p + n]); p += n
        shifted, marked = shifted_meta(recs)
        if shifted:
            recs = {t + 1: v for t, v in recs.items()}
        bad_size = None
        if (control := recs.get(T_NACP)) is not None and len(control) != NACP_SIZE:
            bad_size = len(control)
            del recs[T_NACP]
        if say is not None and not self._meta_reported:
            if shifted:
                say(f'{self.name}: {SHIFTED}')
            elif marked:
                say(f'{self.name}: its meta block has a shift marker, but its NACP or icon uses SPEC §5 numbering. The block is read as it is')
            if bad_size is not None:
                say(f'{self.name}: its NACP record is {bad_size:,} bytes, not {NACP_SIZE:,} (0x4000): it is not a NACP, left out')
            self._meta_reported = True
        return recs


def image(v): return v is not None and (v[:2] == b'\xff\xd8' or v[:4] == b'\x89PNG')   # a JPEG's start of image, or PNG's signature


def nacp_like(v):
    if v is None or len(v) != NACP_SIZE or image(v):
        return False
    if not v.startswith(b'META'):
        return True
    # An uncompressed NACP title can start with META too. Check the NPDM's ACI0.
    try:
        program_id(v, 'NUP metadata')
    except (Error, struct.error):
        return True
    return False


def shifted_meta(recs):
    """Detect metadata written by nup PR #20, which started the TLV enum at 0 instead of 1.

    Prefer NACP and image evidence. Fall back to the markers from upstream PR #32:
    type 0 or a type 7 whose length differs from the 32-byte SPEC build ID.
    Return whether to shift, and whether the content overruled a shift marker.
    """
    build_id = recs.get(T_BUILD_ID)
    rule = 0 in recs or (build_id is not None and len(build_id) != 32)
    for test, at in ((nacp_like, T_NACP), (image, T_ICON)):
        low, spec = test(recs.get(at - 1)), test(recs.get(at))
        if low != spec: return low, rule and not low
    return rule, False


class Nup:
    """A validated NUP (SPEC §6.1): the superblock, the TOC's SHA-256, every record's bounds. File content is checked against its SHA-256 as it is read"""
    def __init__(self, path):
        self.path = Path(path)
        self.application, self.aoc_base, self.siblings = None, None, {}   # scan's: the application, its add-on content base ID, the other programs
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


NACP_AOC_BASE = 0x3070   # ApplicationControlProperty: AddOnContentBaseId, then SaveDataOwnerId (the application's ID)


def nacp_ids(k):   # (application ID, add-on content base ID) from the package's NACP; None for each it does not give
    b = k.meta().get(T_NACP)
    if b is None: return None, None
    aoc, owner = struct.unpack_from('<QQ', b, NACP_AOC_BASE)
    return owner or None, aoc or None


def scan(n, program=None):
    """What a NUP holds: (title ID, base package or None, {update version: package}, {DLC title ID: package}), with the checks pack_dump.scan makes. The title ID is the program ID of the
    program packed. One application can hold several programs, each its own base package and updates (a Switch 2 Edition of a Switch 1 game: Super Mario Bros. Wonder's Switch 1 program
    010015100B514000 and its Switch 2 program 040015100B514001, whose code mounts the first one's RomFS by program index, nn::fs::MountRom(name, 0, ...)). Programs are told apart by their
    NPDM's program ID, whose low 12 bits are the program index; they belong to one application when their NACPs name it (SaveDataOwnerId), else when their program IDs agree above those bits.
    program: the index of the program to pack, default the highest (a Switch 2 Edition's Switch 2 program). Sets n.application, n.aoc_base (a DLC's index is its title ID minus this,
    the NACP's AddOnContentBaseId, else the application's + 0x1000) and n.siblings ({index: (program ID, base package or None, {version: update})}: the other programs, whose RomFS
    pack_version packs beside this one's)"""
    bases = [k for k in n.packages if k.kind == BASE]
    updates = [k for k in n.packages if k.kind == UPDATE]
    if not bases and (not updates or any(not k.flags & STANDALONE for k in updates)):
        raise Error(f'{n.path}: no base game: pack the base game and its update into one NUP, or use updates marked standalone_update with complete exefs/ and romfs/')
    real = [k for k in bases if n.tree_bytes(k.root)]   # a base record with no files is no base game
    # With no base the game is identified from the code actually present; each selected version is checked by pack_version.
    progs, loose = {}, []   # program ID: its packages, base first; base packages without code
    for k in real + updates:
        i = n.child(k.exefs, b'main.npdm') if k.exefs != NO_DIR else None
        if i is None:
            if k.kind == BASE: loose.append(k)
            continue
        progs.setdefault(program_id(n.read(i), f'{k.name} exefs/main.npdm'), []).append(k)
    if not progs: raise Error(f'{n.path}: no exefs/main.npdm in the base or updates')
    if loose and (len(progs) > 1 or len(loose) > 1): raise Error(f'{n.path}: a base package without exefs/main.npdm among several programs: which one it belongs to is unknown')
    if loose: next(iter(progs.values())).insert(0, loose[0])   # the one program's base, as it was with one base package (its code in the updates)
    owner = {pid: next((a for k in ks if (a := nacp_ids(k)[0]) is not None), pid & ~0xFFF) for pid, ks in progs.items()}
    if len(set(owner.values())) > 1:
        raise Error(f'{n.path}: packages of different applications: ' + ', '.join(f'program {pid:016X} of {a:016X}' for pid, a in sorted(owner.items())))
    if len({pid & 0xFFF for pid in progs}) != len(progs): raise Error(f'{n.path}: two programs of index ' + ', '.join(f'{pid:016X}' for pid in sorted(progs)))
    layout = {}
    for pid, ks in progs.items():
        own_bases = [k for k in ks if k.kind == BASE]
        if len(own_bases) > 1: raise Error(f'{n.path}: {len(own_bases)} base packages of program {pid:016X}')
        vers = {}
        for k in ks:
            if k.kind != UPDATE: continue
            if k.version in vers: raise Error(f'{n.path}: two updates of version {k.version}' + (f' of program {pid:016X}' if len(progs) > 1 else ''))
            if not k.version: raise Error(f'{n.path}: an update of version 0')
            vers[k.version] = k
        layout[pid & 0xFFF] = (pid, own_bases[0] if own_bases else None, vers)
    if program is None: program = max(layout)
    if program not in layout: raise Error(f'{n.path}: no program {program}; it holds ' + ', '.join(f'{i} ({pid:016X})' for i, (pid, _, _) in sorted(layout.items())))
    app, base, vers = layout.pop(program)
    n.application, n.siblings = owner[app], layout
    newest = [vers[v] for v in sorted(vers, reverse=True)] + ([base] if base else []) + [k for _, b, vs in layout.values() for k in [*vs.values(), *([b] if b else [])]]
    n.aoc_base = next((a for k in newest if (a := nacp_ids(k)[1]) is not None), n.application + 0x1000)
    dlc = {}
    for k in n.packages:
        if k.kind != ADDON: continue
        if not 0 < k.title_id - n.aoc_base <= 2000: raise Error(f'{n.path}: {k.title_id:016X} is not a DLC of {n.application:016X} (its add-on content IDs start at {n.aoc_base:016X})')
        if k.romfs == NO_DIR: raise Error(f'{n.path}: DLC {k.title_id:016X} has no romfs/')
        if k.title_id in dlc: raise Error(f'{n.path}: DLC {k.title_id:016X} more than once')
        dlc[k.title_id] = k
    return app, base, vers, dlc


def sibling_romfs(n, k):
    """{program index: RomFS folder} of the other programs of the application (scan's n.siblings) at the version package k runs: each one's newest update of no higher version, else its
    base (an update without romfs/ uses its base's). On Horizon the programs of an application are patched together, and a program mounts another's RomFS by its index"""
    out = {}
    for idx, (_, sbase, svers) in sorted(n.siblings.items()):
        u = max((v for v in svers if v <= k.version), default=None)
        d = svers[u].romfs if u is not None and svers[u].romfs != NO_DIR else sbase.romfs if sbase is not None else NO_DIR
        if d != NO_DIR: out[idx] = d
    return out


def romfs_bytes(n, k, base):   # the bytes of the images packing version k writes: its own RomFS and the other programs'
    return n.tree_bytes(romfs_of(k, base)) + sum(n.tree_bytes(d) for d in sibling_romfs(n, k).values())


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


def display_label(k):
    """The selected package's numeric display label, bounded to the console's 15 version bytes."""
    raw = k.meta().get(T_DISPLAY, b'')
    if raw == f'v{k.version}'.encode('ascii'): return ''   # a package version tag, e.g. v131072
    label = raw.removeprefix(b'v')
    return label.decode('ascii') if len(label) <= 15 and re.fullmatch(rb'[0-9]+(?:\.[0-9]+){0,2}', label) else ''


def app_package(n):   # the record that holds application metadata when no base game is there (SPEC §5.1): a base record with no files, as packers attach it there, else the first package
    return next((k for k in n.packages if k.kind == BASE), n.packages[0])


def icon_of(meta):   # a package's icon among its records: the icon, else the American English one of its localized icons, else the first of them
    icons = localized(meta.get(T_LICONS, b''))
    return meta.get(T_ICON) or pick(icons, 'en-US') or next(iter(icons.values()), None)


def control(k, out, fallback=None, base=None, *, say=print):
    """The package's control data beside its ExeFS; what it lacks comes from fallback. An update without an icon takes its base game's (base): the NSP script reads the base game's
    control data in the folder the base is extracted to alone, and a build that packs only the update makes none (The Legend of Zelda: Ocarina of Time v131072: the app had no icon).
    The NACP stays the package's own: its display version and languages are that version's"""
    meta = fallback.meta(say=say) if fallback is not None and fallback is not k else {}
    meta.update(k.meta(say=say))
    nacp_bytes = meta.get(T_NACP)
    icon = icon_of(meta) or (icon_of(base.meta(say=say)) if base is not None and base is not k else None)
    for name, data in (('control.nacp', nacp_bytes), ('icon.jpg', icon)):
        if data: (out / name).write_bytes(data)
        else: (out / name).unlink(missing_ok=True)


def romfs_of(k, base):   # the RomFS folder a version runs with: its own, or the base game's for an update without one
    d = k.romfs if k.romfs != NO_DIR else base.romfs if base is not None else NO_DIR
    if d == NO_DIR: raise Error(f'{k.name}: no romfs/ in this package or the base game')
    return d


PROGRAMS = 'romfs_programs'   # beside a version's RomFS.img: <index>.img, the RomFS of each other program of the application (the host's MountRom by program index, hle_fs.c)


def pack_version(n, k, base, app, out, say=print, step=lambda s: None, *, note=None):
    """One version (the base package, or an update package) → out/ExeFS, out/RomFS.img and its control data, and out/romfs_programs/<index>.img for each other program of the
    application (sibling_romfs); returns how pack() got this program's image. note receives metadata diagnostics and defaults to say."""
    if k.exefs == NO_DIR or n.child(k.exefs, b'main') is None or n.child(k.exefs, b'main.npdm') is None: raise Error(f'{k.name}: no exefs/main and exefs/main.npdm')
    if (pid := program_id(n.read(n.child(k.exefs, b'main.npdm')), f'{k.name} exefs/main.npdm')) != app: raise Error(f'{k.name}: program ID {pid:016X}, the base is {app:016X}')
    d = romfs_of(k, base)
    out.mkdir(parents=True, exist_ok=True)
    exefs(n, k, out / 'ExeFS')
    how = pack(n, d, out / 'RomFS.img', say, step)
    others = {f'{idx}.img': sd for idx, sd in sibling_romfs(n, k).items()}
    progs = out / PROGRAMS
    if progs.is_dir():
        for f in progs.iterdir():
            if not any(f.name == w or f.name.startswith(w + '.') for w in others): f.unlink()   # an image and the files beside it (its stamp, the NSP script's format check)
        if not others: progs.rmdir()
    for name, sd in others.items():
        say(f'program {name.removesuffix(".img")} of the application ({n.siblings[int(name.removesuffix(".img"))][0]:016X}): its RomFS → {PROGRAMS}/{name}')
        pack(n, sd, progs / name, say, step)
    control(k, out, app_package(n) if base is None else None, base, say=say if note is None else note)
    return how


def pack_dlc(n, app, dlc, aoc, say=print):   # {DLC title ID: package} → aoc/<index>.img (index = title ID − the add-on content base ID scan found); images of DLC no longer in the container go
    first = n.aoc_base if n.aoc_base is not None else app + 0x1000
    want = {f'{tid - first}.img': k for tid, k in dlc.items()}
    if aoc.is_dir():
        for f in aoc.iterdir():
            if f.name.removesuffix('.inputs.json') not in want: f.unlink()
    for name, k in want.items(): pack(n, k.romfs, aoc / name, say)


def main():
    ap = argparse.ArgumentParser(description='Pack a game in a NUP container into the extraction layout of build_ipa_from_nsp.py')
    ap.add_argument('nup', type=Path, help='the .nup file')
    ap.add_argument('--version', type=int, help='the update version to pack (default: the newest in the container; 0 = the base game)')
    ap.add_argument('--program', type=int, help='a NUP holding several programs of one application (a Switch 2 Edition of a Switch 1 game): the index of the one to pack (default: the highest)')
    ap.add_argument('--work-dir', type=Path, help='the build folder (default: <folder of the .nup>/<title ID>_build)')
    a = ap.parse_args()
    path = a.nup.resolve()
    try:
        with Nup(path) as n:
            app, base, vers, dlc = scan(n, a.program)
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
