#!/usr/bin/env python3
# Pack an extracted RomFS folder into one image file the host reads in place of the folder (hle_fs.c, "RomFS image"): DM_ROMFS may name the image instead of a folder.
# One file instead of hundreds of thousands copies to an iPhone at full speed (Tears of the Kingdom's 304,000 files ran the phone's file service out of open files), and the host
# keeps one descriptor open for the whole RomFS.
# Layout (little-endian):
#   header  32 bytes: magic "DMROMFS1", u64 index offset, u64 index size, u64 entry count
#   data    each file's bytes, one after another
#   index   entry count entries of 24 bytes: u64 a, u64 b, u32 name offset, u16 name length, u8 type (0 directory, 1 file), u8 0
#           a directory: a = its first child's entry, b = its number of children; a file: a = data offset in the image, b = size
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


def file_identity(path):
    s = os.stat(path)
    return [s.st_dev, s.st_ino, s.st_size, s.st_mtime_ns, s.st_ctime_ns]


def source_digest(src, ents):
    h = hashlib.sha256(Path(__file__).read_bytes())
    h.update(os.fsencode(os.path.realpath(src)) + b'\0')
    for name, path, isdir, _, _ in ents:
        h.update(struct.pack('<I?', len(name), isdir)); h.update(name)
        h.update(json.dumps(file_identity(path)).encode() + b'\0')
    return h.hexdigest()


def pack(src, dst, reuse=False):
    src, dst = os.fspath(src), os.fspath(dst)
    ents = entries(src)
    digest = source_digest(src, ents)
    stamp = dst + '.inputs.json'
    if reuse:
        try:
            cached = json.loads(Path(stamp).read_text())
            if cached == {'source': digest, 'image': file_identity(dst)}: return len(ents)
        except (OSError, ValueError): pass
    part = dst + '.part'
    with open(part, 'wb') as out:
        out.write(b'\0' * HEAD.size)
        off = HEAD.size
        for e in ents:
            if e[2]: continue
            with open(e[1], 'rb') as f:
                n = 0
                while True:
                    b = f.read(8 << 20)
                    if not b: break
                    out.write(b); n += len(b)
            e[3], e[4] = off, n; off += n
        pool, idx = bytearray(), bytearray()
        for name, _, isdir, a, b in ents:
            idx += ENT.pack(a, b, len(pool), len(name), 0 if isdir else 1, 0); pool += name
        out.write(idx); out.write(pool)
        out.seek(0); out.write(HEAD.pack(MAGIC, off, len(idx) + len(pool), len(ents)))
    # A changing source must not publish a mixed image or a reuse stamp for stale data.
    if source_digest(src, entries(src)) != digest:
        os.unlink(part)
        raise RuntimeError('RomFS changed while packing; run the packer again when its files are stable')
    os.replace(part, dst)
    Path(stamp + '.part').write_text(json.dumps({'source': digest, 'image': file_identity(dst)}))
    os.replace(stamp + '.part', stamp)
    return len(ents)


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


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description='Pack a RomFS folder into an indexed image')
    ap.add_argument('--reuse', action='store_true', help='keep the image only while its source tree, packer and image are unchanged')
    ap.add_argument('source'); ap.add_argument('image')
    args = ap.parse_args()
    n = pack(args.source, args.image, reuse=args.reuse)
    print(f'{args.image}: {n} entries, {os.path.getsize(args.image)} bytes')
