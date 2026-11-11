#!/usr/bin/env python3
# Pack an extracted RomFS folder into one image file the host reads in place of the folder (hle_fs.c, "RomFS image"): DM_ROMFS may name the image instead of a folder.
# One file instead of hundreds of thousands copies to an iPhone at full speed (Tears of the Kingdom's 304,000 files ran the phone's file service out of open files), and the host
# keeps one descriptor open for the whole RomFS.
# Layout (little-endian):
#   header  32 bytes: magic "DMROMFS1", u64 index offset, u64 index size, u64 entry count
#   data    each file's bytes, one after another
#   index   entry count entries of 24 bytes: u64 a, u64 b, u32 name offset, u16 name length, u8 type (0 directory, 1 file), u8 flags
#           a directory: a = its first child's entry, b = its number of children; a file: a = data offset in the image, b = size
#           flags: 0, or 1 on a file that failed the NCA's hash check and was written as read (nx_extract --allow-damaged; the host names it in its log)
#           then the names, one pool. Entry 0 is the root (empty name). Each directory's children are consecutive entries sorted by name (bytes), so a lookup is a
#           binary search per path component; directories are numbered breadth-first.
# The index comes last, so a copy cut off partway has no valid index (the header points past the end of the file) and the host refuses it.
# Names are the guest names: a host name chosen on a case collision (.dm-case-N) is replaced by the one in its xattr, as hle_fs.c's guest_name does.
# Usage: romfs_image.py [--reuse] <RomFS folder> <out image>   (written to <out>.part, renamed at the end)
# --reuse checks the source tree's names and file metadata, the packer's version and the image's identity before keeping a previous image.
import argparse, hashlib, json, os, struct, subprocess
from pathlib import Path

MAGIC = b'DMROMFS1'
HEAD = struct.Struct('<8sQQQ')
ENT = struct.Struct('<QQIHBB')
GUEST_NAME = 'org.d2rmobile.guest-name'


def guest(parent, name):
    if not name.startswith('.dm-case-'): return name
    r = subprocess.run(['xattr', '-p', GUEST_NAME, os.path.join(parent, name)], capture_output=True, text=True)
    return r.stdout.rstrip('\n') if r.returncode == 0 else name


def children(path):
    out = []
    with os.scandir(path) as it:
        for e in it:
            out.append((guest(path, e.name).encode(), e.path, e.is_dir(follow_symlinks=True)))
    out.sort()
    return out


def entries(src):
    ents = [[b'', src, True, 0, 0]]   # name, host path, is dir, a, b
    i = 0
    while i < len(ents):   # breadth-first: each directory's children land as one consecutive run
        name, path, isdir, _, _ = ents[i]
        if isdir:
            kids = children(path)
            ents[i][3], ents[i][4] = len(ents), len(kids)
            ents.extend([n, p, d, 0, 0] for n, p, d in kids)
        i += 1
    return ents


def file_identity(path):   # not st_dev: macOS numbers a volume when it mounts it, so a reboot moved the Data volume from 16777234 to 16777232 and a 24 GB image was packed again, 8 minutes over SMB
    s = os.stat(path)
    return [s.st_ino, s.st_size, s.st_mtime_ns, s.st_ctime_ns]


def source_digest(src, ents):
    h = hashlib.sha256(Path(__file__).read_bytes())
    h.update(os.fsencode(os.path.realpath(src)) + b'\0')
    for name, path, isdir, _, _ in ents:
        h.update(struct.pack('<I?', len(name), isdir)); h.update(name)
        h.update(json.dumps(file_identity(path)).encode() + b'\0')
    return h.hexdigest()


def reusable(dst, digest):   # whether dst is the image the stamp beside it says was made from inputs with this digest
    try: return json.loads(Path(dst + '.inputs.json').read_text()) == {'source': digest, 'image': file_identity(dst)}
    except (OSError, ValueError): return False


def layout(ents, end):
    """The header and the index (entries, then the name pool) of an image of ents whose files' a and b are filled in, the file data ending at end.
    write() writes these bytes and a check of an existing image compares them, so the two cannot disagree on the format."""
    pool, idx = bytearray(), bytearray()
    for name, _, isdir, a, b in ents:
        idx += ENT.pack(a, b, len(pool), len(name), 0 if isdir else 1, 0); pool += name
    idx += pool
    return HEAD.pack(MAGIC, end, len(idx), len(ents)), bytes(idx)


def write(ents, dst, copy):
    """ents (entries(): [name, source, is dir, a, b], breadth-first) → dst + '.part'; copy(source, out) writes one file's bytes to out and returns how many. Fills in each file's a and b"""
    part = dst + '.part'
    with open(part, 'wb') as out:
        out.write(b'\0' * HEAD.size)
        off = HEAD.size
        for e in ents:
            if e[2]: continue
            e[3], e[4] = off, copy(e[1], out); off += e[4]
        head, idx = layout(ents, off)
        out.write(idx)
        out.seek(0); out.write(head)
    return part


def stamp(dst, digest, image):   # the stamp reusable() checks: dst, with identity image, is the image of inputs with this digest
    Path(dst + '.inputs.json.part').write_text(json.dumps({'source': digest, 'image': image}))
    os.replace(dst + '.inputs.json.part', dst + '.inputs.json')


def publish(part, dst, digest):   # the written image in place, with its stamp
    os.replace(part, dst)
    stamp(dst, digest, file_identity(dst))


def copy_file(path, out):
    n = 0
    with open(path, 'rb') as f:
        while b := f.read(8 << 20): out.write(b); n += len(b)
    return n


def pack(src, dst, reuse=False):
    src, dst = os.fspath(src), os.fspath(dst)
    ents = entries(src)
    digest = source_digest(src, ents)
    if reuse and reusable(dst, digest): return len(ents)
    part = write(ents, dst, copy_file)
    # A changing source must not publish a mixed image or a reuse stamp for stale data.
    if source_digest(src, entries(src)) != digest:
        os.unlink(part)
        raise RuntimeError('RomFS changed while packing; run the packer again when its files are stable')
    publish(part, dst, digest)
    return len(ents)


# Layered RomFS trees: an update pack (pack_dump.py: updates/<version>/romfs holding only the files the update adds or changes, and romfs_removed.txt the base files it drops)
# laid over the base game's RomFS. roots[0] wins: a file there hides the same path in the roots after it, and a folder merges with the same folder in them. removed lists guest paths
# ('Voice/USen/x.bwav', no leading slash) that the roots after the first lose, a folder with everything under it; the first root keeps a removed path it holds itself.
# A folder that had files and lost all of them to the removals goes too, as it would from the update's own tree; an empty folder in a root stays.
def removed_paths(lines):   # the guest paths of a romfs_removed.txt's lines: blank lines and # comments left out, a leading slash or romfs/ dropped, backslashes as slashes
    out = set()
    for line in lines:
        p = line.strip().replace('\\', '/')
        if not p or p.startswith('#'): continue
        p = p.lstrip('/')
        if p.startswith('romfs/'): p = p[len('romfs/'):]
        out.add(p.rstrip('/'))
    return out


def _merge(dirs, rel, removed):   # dirs: the host folders of one guest folder, top root first, as (layer, path) → (sorted [(name, host path, is dir, merged children)], whether removals emptied it)
    kids, lost = {}, False
    for layer, d in dirs:
        for name, path, isdir in children(d):
            r = rel + '/' + name.decode('utf-8', 'surrogateescape') if rel else name.decode('utf-8', 'surrogateescape')
            if layer and r in removed: lost = True; continue
            k = kids.get(name)
            if k is None: kids[name] = [path, isdir, [(layer, path)] if isdir else None, r]
            elif k[1] and isdir: k[2].append((layer, path))
    out = []
    for name in sorted(kids):
        path, isdir, sub, r = kids[name]
        if isdir:
            merged, emptied = _merge(sub, r, removed)
            if emptied and not merged: lost = True; continue
            out.append((name, path, True, merged))
        else: out.append((name, path, False, None))
    return out, lost and not out


def layered_entries(roots, removed=()):
    """entries() of the roots laid over each other: [name, host path, is dir, a, b], breadth-first, with each entry's guest path in a sixth field"""
    roots = [os.fspath(r) for r in roots]
    top, _ = _merge(list(enumerate(roots)), '', set(removed))
    ents, subs, i = [[b'', roots[0], True, 0, 0, '']], [top], 0
    while i < len(ents):
        if ents[i][2]:
            kids = subs[i]
            ents[i][3], ents[i][4] = len(ents), len(kids)
            for name, path, isdir, merged in kids:
                ents.append([name, path, isdir, 0, 0, (ents[i][5] + '/' if ents[i][5] else '') + name.decode('utf-8', 'surrogateescape')]); subs.append(merged)
        else: subs[i] = None
        i += 1
    return ents


def layered_digest(roots, removed, ents):
    h = hashlib.sha256(Path(__file__).read_bytes())
    for r in roots: h.update(os.fsencode(os.path.realpath(r)) + b'\0')
    for p in sorted(removed): h.update(p.encode('utf-8', 'surrogateescape') + b'\0')
    for name, path, isdir, *_ in ents:
        h.update(struct.pack('<I?', len(name), isdir)); h.update(name)
        h.update(json.dumps(file_identity(path)).encode() + b'\0')
    return h.hexdigest()


def pack_layers(roots, dst, removed=(), reuse=False):   # pack() of the roots laid over each other; returns the number of entries
    roots, dst, removed = [os.fspath(r) for r in roots], os.fspath(dst), set(removed)
    if len(roots) == 1 and not removed: return pack(roots[0], dst, reuse)
    ents = layered_entries(roots, removed)
    digest = layered_digest(roots, removed, ents)
    if reuse and reusable(dst, digest): return len(ents)
    part = write([e[:5] for e in ents], dst, copy_file)
    if layered_digest(roots, removed, layered_entries(roots, removed)) != digest:
        os.unlink(part)
        raise RuntimeError('RomFS changed while packing; run the packer again when its files are stable')
    publish(part, dst, digest)
    return len(ents)


def _clonefile():   # libc's clonefile(2): an APFS copy-on-write clone of one file, which takes no space of its own; None off macOS
    try:
        import ctypes, ctypes.util
        f = ctypes.CDLL(ctypes.util.find_library('c'), use_errno=True).clonefile
        f.argtypes, f.restype = (ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint32), ctypes.c_int
        return f
    except (OSError, AttributeError): return None


def clone_tree(roots, dst, removed=(), reuse=False):
    """The roots laid over each other as a folder at dst, each file an APFS clone of its source (a copy when the volume cannot clone): a RomFS folder the host reads as it reads
    an image (DM_ROMFS), at almost no disk space when the sources are on the same APFS volume. Written as <dst>.partial and renamed; reused while the stamp beside it matches.
    Returns (entries, files cloned, files copied)"""
    import shutil
    roots, dst, removed = [os.fspath(r) for r in roots], os.fspath(dst).rstrip('/'), set(removed)
    ents = layered_entries(roots, removed)
    digest = layered_digest(roots, removed, ents)
    if reuse and os.path.isdir(dst) and reusable(dst, digest): return len(ents), 0, 0
    part = dst + '.partial'
    if os.path.lexists(part): shutil.rmtree(part)
    os.makedirs(part)
    clone, cloned, copied = _clonefile(), 0, 0
    for name, path, isdir, _, _, rel in ents[1:]:
        out = os.path.join(part, rel)
        if os.path.lexists(out): raise RuntimeError(f'{rel}: another name in the RomFS differs from it only in letter case, which this volume cannot keep apart; pack an image instead')
        if isdir: os.mkdir(out); continue
        if clone and clone(os.fsencode(path), os.fsencode(out), 0) == 0: cloned += 1
        else: shutil.copy2(path, out); copied += 1
    if layered_digest(roots, removed, layered_entries(roots, removed)) != digest:
        shutil.rmtree(part)
        raise RuntimeError('RomFS changed while packing; run the packer again when its files are stable')
    if os.path.lexists(dst): shutil.rmtree(dst)
    os.replace(part, dst)
    stamp(dst, digest, file_identity(dst))
    return len(ents), cloned, copied


def read_index(img):   # for tests and checks: {guest path: (is dir, offset, size)}
    with open(img, 'rb') as f:
        magic, ioff, isz, n = HEAD.unpack(f.read(HEAD.size))
        assert magic == MAGIC
        f.seek(ioff); raw = f.read(isz)
    pool = raw[n * ENT.size:]
    e = [ENT.unpack_from(raw, k * ENT.size) for k in range(n)]
    out, todo = {}, [(0, '')]
    while todo:
        k, path = todo.pop()
        a, b, no, nl, t, _ = e[k]
        if t == 0:
            if path: out[path] = (True, 0, 0)
            todo.extend((c, (path + '/' if path else '') + pool[e[c][2]:e[c][2] + e[c][3]].decode()) for c in range(a, a + b))
        else: out[path] = (False, a, b)
    return out


# File names that promise a format, and the first bytes that format starts with. A file whose name promises one and whose bytes start with none of its heads is damaged:
# a NUP or a dump folder carries no NCA hashes, so this is the check its extraction has. A zstd stream starts with a frame (28 b5 2f fd) or a skippable frame (5? 2a 4d 18).
# Found on a NUP of Ocarina of Time whose three .zs textures began with 12,288 zero bytes: the game asked for one again on every pass of its background loader (PR #1530).
FORMAT_HEADS = {'.zs': ((b'\x28\xb5\x2f\xfd', b'\xff\xff\xff\xff'), (b'\x50\x2a\x4d\x18', b'\xf0\xff\xff\xff'))}   # suffix: ((bytes, mask), ...)


def format_mismatches(img, progress=None):   # [(guest path, size, first 4 bytes)] of the image's files whose bytes do not start as their name promises (FORMAT_HEADS); empty files are left out. progress(done, total): every 4,096 files
    files = sorted((off, size, p) for p, (d, off, size) in read_index(img).items() if not d and size and os.path.splitext(p)[1].lower() in FORMAT_HEADS)
    bad = []
    with open(img, 'rb') as f:
        for k, (off, size, p) in enumerate(files):   # in image order: one pass forward over the disk
            if progress and not k % 4096: progress(k, len(files))
            f.seek(off); head = f.read(min(size, 4))   # not into the next file's bytes
            if not any(len(head) == 4 and all(h & m == w & m for h, w, m in zip(head, want, mask)) for want, mask in FORMAT_HEADS[os.path.splitext(p)[1].lower()]):
                bad.append((p, size, head))
    return bad


def folder_format_mismatches(folder, progress=None):   # format_mismatches of a RomFS folder (clone_tree's): [(guest path, size, first 4 bytes)]
    files = []
    for d, _, names in os.walk(folder):
        for n in names:
            if os.path.splitext(n)[1].lower() in FORMAT_HEADS: files.append(os.path.join(d, n))
    files.sort()
    bad = []
    for k, path in enumerate(files):
        if progress and not k % 4096: progress(k, len(files))
        size = os.path.getsize(path)
        if not size: continue
        with open(path, 'rb') as f: head = f.read(4)
        if not any(len(head) == 4 and all(h & m == w & m for h, w, m in zip(head, want, mask)) for want, mask in FORMAT_HEADS[os.path.splitext(path)[1].lower()]):
            bad.append((os.path.relpath(path, folder), size, head))
    return bad


ID_MAGIC = 'DMROMFSID1'
_FNV_OFFSET, _FNV_PRIME, _MASK = 0xcbf29ce484222325, 0x100000001b3, (1 << 64) - 1


def _clean(text):   # one token of the identity line: no spaces, no newlines
    return ''.join(c if c.isalnum() or c in '._-+' else '_' for c in str(text)) or '-'


def identity(img, title='', version='', build=''):
    """The identity line of a complete image (dm/host/romfs_identity.h defines the format and the C reader): its size and an FNV-1a 64 hash of its header and index
    (the file data is not read, so a 14 GB image takes a moment). Raises ValueError for a cut off or invalid image."""
    size = os.path.getsize(img)
    with open(img, 'rb') as f:
        head = f.read(HEAD.size)
        if len(head) != HEAD.size: raise ValueError(f'not a RomFS image: {img}')
        magic, ioff, isz, _ = HEAD.unpack(head)
        if magic != MAGIC or ioff < HEAD.size or ioff > size or isz != size - ioff: raise ValueError(f'incomplete or invalid RomFS image: {img}')
        h, rest = _FNV_OFFSET, b''
        f.seek(ioff)
        def mix(h, data):
            for w in struct.unpack(f'<{len(data) // 8}Q', data): h = ((h ^ w) * _FNV_PRIME) & _MASK
            return h
        h = mix(h, head)
        left = isz
        while left:
            chunk = f.read(min(left, 8 << 20))
            if not chunk: raise ValueError(f'incomplete RomFS image: {img}')
            left -= len(chunk)
            chunk = rest + chunk
            keep = len(chunk) % 8 if left else 0
            rest = chunk[len(chunk) - keep:]; chunk = chunk[:len(chunk) - keep]
            if not left and len(chunk) % 8: chunk += b'\0' * (8 - len(chunk) % 8)
            h = mix(h, chunk)
    return f'{ID_MAGIC} title={_clean(title)} version={_clean(version)} build={_clean(build)} size={size} index={h:016x}'


def parse_identity(text):   # {field: value} of an identity line; size is an int
    parts = text.split()
    if not parts or parts[0] != ID_MAGIC: raise ValueError('not an identity line')
    d = dict(p.split('=', 1) for p in parts[1:] if '=' in p)
    if 'size' not in d or 'index' not in d: raise ValueError('identity line without size or index')
    d['size'] = int(d['size']); return d


def same_identity(a, b):   # two lines name the same image when size and index agree (title, version and build only describe it)
    x, y = parse_identity(a), parse_identity(b)
    return (x['size'], x['index']) == (y['size'], y['index'])


if __name__ == '__main__':
    import sys
    if sys.argv[1:2] == ['--identity']:   # romfs_image.py --identity IMAGE [TITLE [VERSION [BUILD]]]: the identity line of IMAGE
        try: print(identity(*sys.argv[2:6]))
        except (OSError, ValueError, TypeError) as e: sys.exit(f'[RED] {e}')
        sys.exit(0)
    ap = argparse.ArgumentParser(description='Pack a RomFS folder into an indexed image')
    ap.add_argument('--reuse', action='store_true', help='keep the image only while its source tree, packer and image are unchanged')
    ap.add_argument('source'); ap.add_argument('image')
    args = ap.parse_args()
    n = pack(args.source, args.image, reuse=args.reuse)
    print(f'{args.image}: {n} entries, {os.path.getsize(args.image)} bytes')
