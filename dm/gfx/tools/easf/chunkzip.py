# Adapted from ohxiaoguang/Touchline23, MIT; original authorship retained.
"""Minimal independent reader for EA 'chunkzip' containers (FIFA 23 / Switch).

Header (all big-endian), 0x30 bytes:
  0x00  8  b'chunkzip'
  0x08  4  version (2)
  0x0C  4  total uncompressed size
  0x10  4  chunk size (163840)
  0x14  4  chunk count
  0x18  4  16 (block alignment)
  0x1C 12  zero
  0x28  4  first block compressed size
  0x2C  4  1
Then `count` independent raw-deflate blocks. Every block starts on a 16-byte
boundary and is followed by a small record before the next boundary.

Verified on FIFA 23 (01001C8016B4E000) shader.big / shaderboot.big:
65 and 2 blocks, byte-exact reconstruction of the declared total size.
"""
import struct
import zlib


def parse_header(d):
    if len(d) < 0x30:
        raise ValueError("truncated chunkzip header")
    if d[:8] != b"chunkzip":
        raise ValueError("not a chunkzip container: %r" % d[:8])
    return dict(
        ver=struct.unpack(">I", d[8:12])[0],
        total=struct.unpack(">I", d[12:16])[0],
        chunk=struct.unpack(">I", d[16:20])[0],
        count=struct.unpack(">I", d[20:24])[0],
        align=struct.unpack(">I", d[24:28])[0],
        first_comp=struct.unpack(">I", d[0x28:0x2C])[0],
    )


def _inflate_at(d, off, want, scan=96):
    """Find a raw-deflate stream near `off` that yields exactly `want` bytes."""
    for probe in range(scan):
        a = off + probe
        if a + 8 > len(d):
            break
        try:
            o = zlib.decompressobj(-15)
            out = o.decompress(d[a:], want)
        except zlib.error:
            continue
        if o.eof and len(out) == want:
            return out, len(d) - a - len(o.unused_data), a
    raise ValueError("no deflate block producing %d bytes near %#x" % (want, off))


def decode(d):
    """Return (header, plaintext, blocks). blocks = [(start, comp_len, out_len)]."""
    h = parse_header(d)
    total, chunk = h["total"], h["chunk"]
    if h["ver"] != 2 or h["align"] != 16 or not chunk or h["count"] != (total + chunk - 1) // chunk:
        raise ValueError("invalid chunkzip geometry")
    out = bytearray()
    blocks = []
    off = 0x30
    while len(out) < total:
        want = min(chunk, total - len(out))
        off = (off + 15) & ~15
        plain, used, at = _inflate_at(d, off, want)
        blocks.append((at, used, len(plain)))
        out += plain
        off = at + used
    if len(out) != total:
        raise ValueError("size mismatch: %d != %d" % (len(out), total))
    return h, bytes(out), blocks
