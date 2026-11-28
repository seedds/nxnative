#!/usr/bin/env python3
# Media.Vision's MVGL archives (MDB1, little-endian, no encryption on the Switch):
#   · Legacy layout (Digimon Story Cyber Sleuth: Complete Edition, DSDB*.nx64.mvgl):
#       +0x00 'MDB1', u16 tree entries, u16 name entries, u32 data entries, u32 data start, u32 archive size (0x14 bytes)
#       +0x14 tree entries, 8 bytes each: u16 compare bit, u16 data index (0xffff for the root and inner nodes), u16 left, u16 right (a Patricia tree over the names)
#       then name entries, 0x40 bytes each, by tree index: char[4] extension (space-padded), char[0x3c] name ('\'-separated path without extension)
#       then data entries, 12 bytes each: u32 offset (from data start), u32 length, u32 stored length; stored < length is doboz-compressed (version 0)
#   · Wide layout (Digimon Story Time Stranger, app_*.nx64.mvgl / nx_patch_*.nx64.mvgl):
#       +0x00 'MDB1', u32 tree entries, u32 name entries, u32 data entries, u64 data start, u64 archive size (0x20 bytes)
#       +0x20 tree entries, 16 bytes each: u32 compare bit, u32 data index (0xffffffff for the root and inner nodes), u32 left, u32 right
#       then name entries, 0x80 bytes each: char[4] extension (space-padded), char[0x7c] name
#       then data entries, 16 bytes each: u64 offset (from data start), u32 length, u32 stored length; stored < length is a raw LZ4 block
# The layout is recognized from the header's own data start (header + tables) and archive size; both satisfy exact table-end equality and cannot collide.
# Doboz (Attila T. Afra's doboz, version 0): one attributes byte (bits 0-2 version, 3-5 size width - 1, bit 7 stored), the length and the stored length in that width,
# then the stream: a 32-bit control word per 32 items (bit 0 first, 0 = literal byte, 1 = match), refilled when only its sentinel bit is left; a match is 1 to 4 bytes
# by its low 3 bits (offset and length - 3, see MATCH). The last 8 bytes are always literals.
# Usage: mvgl.py list <archive> [ext]   |   mvgl.py get <archive> <name>.<ext> <out file>
import os, struct, sys
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '../../tools'))
from romfs import os, open

MATCH = [(0xff, 2, 0, 0, 1), (0xffff, 2, 0, 0, 2), (0xffff, 6, 15, 2, 2), (0xffffff, 8, 31, 3, 3),
         (0xff, 2, 0, 0, 1), (0xffff, 2, 0, 0, 2), (0xffff, 6, 15, 2, 2), (0xffffffff, 11, 255, 3, 4)]   # mask, offset shift, length mask, length shift, size
TAIL = 8


def doboz(b):
    if not b: raise ValueError('empty doboz stream')
    a = b[0]
    if a & 7: raise ValueError('doboz version %d' % (a & 7))
    w = ((a >> 3) & 7) + 1
    if len(b) < 1 + 2 * w: raise ValueError('truncated doboz header')
    size, csize = int.from_bytes(b[1:1 + w], 'little'), int.from_bytes(b[1 + w:1 + 2 * w], 'little')
    i = 1 + 2 * w
    if a & 0x80:
        if len(b) < i + size: raise ValueError('truncated stored doboz payload')
        return bytes(b[i:i + size])
    out = bytearray(); src = b + b'\0' * 8; ctl = 1
    while len(out) < size:
        if ctl == 1: ctl = int.from_bytes(src[i:i + 4], 'little'); i += 4
        if not ctl & 1:
            if len(out) < size - TAIL: out.append(src[i]); i += 1
            else:   # the tail: every remaining byte is a literal, whatever the control bits say (doboz's decoder does the same)
                while len(out) < size:
                    if ctl == 1: ctl = int.from_bytes(src[i:i + 4], 'little'); i += 4
                    out.append(src[i]); i += 1; ctl >>= 1
                break
        else:
            word = int.from_bytes(src[i:i + 4], 'little'); m = MATCH[word & 7]
            off = (word & m[0]) >> m[1]; n = ((word >> m[3]) & m[2]) + 3; i += m[4]
            s = len(out) - off
            if not off or s < 0: raise ValueError('doboz match out of range')
            if off >= n: out += out[s:s + n]
            else:
                for k in range(n): out.append(out[s + k])
        ctl >>= 1
    if i - (1 + 2 * w) > csize + 4: raise ValueError('doboz stream overrun')
    return bytes(out)


def lz4_block(b, size):
    import lz4.block
    if size and (not b or size > 255 * len(b)):
        raise ValueError('impossible LZ4 length %#x for %#x bytes' % (size, len(b)))
    try: out = lz4.block.decompress(b, uncompressed_size=size)
    except lz4.block.LZ4BlockError as e: raise ValueError('invalid LZ4 block: %s' % e) from e
    if len(out) != size: raise ValueError('LZ4 decoded %#x bytes, expected %#x' % (len(out), size))
    return out


def read_entry(f, off, n, cn, wide=False):
    f.seek(off); b = f.read(cn)
    if len(b) != cn: raise ValueError('truncated MVGL entry (%#x of %#x bytes)' % (len(b), cn))
    if cn == n: return b
    if cn > n: raise ValueError('MVGL entry stored length %#x exceeds length %#x' % (cn, n))
    return lz4_block(b, n) if wide else doboz(b)


class Archive:
    def __init__(self, path):
        self.f = open(path, 'rb')
        try:
            self.f.seek(0, os.SEEK_END); fsize = self.f.tell(); self.f.seek(0)
            head = self.f.read(0x20)
            if len(head) < 0x14 or head[:4] != b'MDB1': raise ValueError('%s: not an MVGL archive' % path)
            wide = None
            if len(head) >= 0x20:
                _, nt, nn, nd, start, size = struct.unpack('<4sIIIQQ', head)
                if 0x20 + nt * 16 + nn * 0x80 + nd * 16 == start and start <= size <= fsize: wide = True
            if wide is None:
                _, nt, nn, nd, start, size = struct.unpack('<4sHHIII', head[:0x14])
                if 0x14 + nt * 8 + nn * 0x40 + nd * 12 == start and start <= size <= fsize: wide = False
            if wide is None: raise ValueError('%s: unrecognized MVGL header layout' % path)
            self.wide, self.start, self.size = wide, start, size
            hdr_sz, tree_sz, name_sz, data_sz = (0x20, 16, 0x80, 16) if wide else (0x14, 8, 0x40, 12)
            idx_off, idx_fmt, no_idx, data_fmt = (4, '<I', 0xffffffff, '<QII') if wide else (2, '<H', 0xffff, '<III')
            self.f.seek(hdr_sz)
            tree, names, data = self.f.read(nt * tree_sz), self.f.read(nn * name_sz), self.f.read(nd * data_sz)
            if len(tree) != nt * tree_sz or len(names) != nn * name_sz or len(data) != nd * data_sz:
                raise ValueError('%s: truncated MVGL tables' % path)
            self.files = {}   # 'name.ext' → (offset, length, stored length)
            for k in range(nt):
                idx = struct.unpack_from(idx_fmt, tree, k * tree_sz + idx_off)[0]
                if idx == no_idx: continue
                if k >= nn or idx >= nd: raise ValueError('%s: MVGL table index out of range' % path)
                off, n, cn = struct.unpack_from(data_fmt, data, idx * data_sz)
                if cn > n or off + cn > self.size - self.start:
                    raise ValueError('%s: MVGL entry %d lies outside archive data' % (path, idx))
                base = k * name_sz
                ext = names[base:base + 4].rstrip(b'\0 ').decode('latin1')
                name = names[base + 4:base + name_sz].split(b'\0')[0].decode('latin1')
                self.files[name + '.' + ext] = (self.start + off, n, cn)
        except BaseException:
            self.f.close()
            raise

    def close(self):
        self.f.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def read(self, name):
        off, n, cn = self.files[name]
        return read_entry(self.f, off, n, cn, self.wide)


if __name__ == '__main__':
    if len(sys.argv) >= 3 and sys.argv[1] == 'list':
        with Archive(sys.argv[2]) as a:
            for nm, (off, n, cn) in sorted(a.files.items()):
                if len(sys.argv) < 4 or nm.endswith('.' + sys.argv[3]): print('%s\t%d\t%d' % (nm, n, cn))
    elif len(sys.argv) == 5 and sys.argv[1] == 'get':
        with Archive(sys.argv[2]) as a: open(sys.argv[4], 'wb').write(a.read(sys.argv[3]))
    else:
        sys.exit(__doc__ or 'usage: mvgl.py list <archive> [ext] | get <archive> <name.ext> <out>')
