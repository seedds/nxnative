#!/usr/bin/env python3
# Compressed files in a shader library image (dm/gfx/nvn/msl_imgz.h is the host's reader): a 16-byte header {b'DMZ1', u32 libcompression algorithm, u64 original size}, then the
# algorithm's raw stream. Apple's libcompression through ctypes, so any Python on macOS reads and writes them (the build's py315t and the tools' /usr/bin/python3 alike) and the
# host needs no vendored codec.
# Which algorithm for which file (measured on a 147,000-file, 20.7 GB library, 400 random files a type, single-thread decode on an M4 Pro):
#   metallibs  LZBITMAP  1.80-1.89x, ~5 GB/s decode (zstd-3: 1.93x at 1.2 GB/s; LZ4: 1.34x)   what the host loads, often during play: decode speed first
#   .metal     LZFSE     7.7x                                                                     the host never reads them; the tools do
#   .json      LZFSE     5.6x, 2 µs a file
import ctypes, struct

MAGIC = b'DMZ1'
HEAD = struct.Struct('<4sIQ')
ALG = {'lz4': 0x101, 'lzfse': 0x801, 'lzbitmap': 0x702, 'zlib': 0x205}
MAX = 1 << 30   # msl_imgz.h MSL_IMGZ_MAX

_lc = None


def _lib():
    global _lc
    if _lc is None:
        lc = ctypes.CDLL('/usr/lib/libcompression.dylib')
        for fn in (lc.compression_encode_buffer, lc.compression_decode_buffer):
            fn.restype = ctypes.c_size_t; fn.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_int]
        _lc = lc
    return _lc


def is_dmz(data): return len(data) >= HEAD.size and bytes(data[:4]) == MAGIC


def header_ok(alg, raw): return alg in ALG.values() and 0 < raw <= MAX   # a known algorithm and a size in range (msl_imgz.h checks the same)


def codec_for(name):
    """The algorithm a library file is stored with (None: stored as it is). The table above"""
    if name.endswith('.metallib'): return 'lzbitmap'
    if name.endswith(('.metal', '.json')): return 'lzfse'
    return None


def encode(data, codec):
    """data as a DMZ1 file, or data itself when it does not get smaller (a tiny or incompressible file stays plain: the header tells which)"""
    data = bytes(data)
    if not data or is_dmz(data) or len(data) > MAX: return data
    alg = ALG[codec]; cap = len(data) + 4096
    out = ctypes.create_string_buffer(cap)
    n = _lib().compression_encode_buffer(out, cap, data, len(data), None, alg)
    if not n or n + HEAD.size >= len(data): return data
    blob = HEAD.pack(MAGIC, alg, len(data)) + out.raw[:n]
    if decode(blob) != data: raise ValueError(f'{codec}: the stream does not decode to its input')   # cheap next to the encode, and nothing wrong is published
    return blob


def decode(blob):
    """The original bytes of a DMZ1 file; a plain file is returned as it is"""
    if not is_dmz(blob): return bytes(blob)
    _, alg, raw = HEAD.unpack_from(blob)
    if not header_ok(alg, raw): raise ValueError(f'DMZ1: unknown algorithm {alg:#x} or original size {raw} out of range')
    out = bytearray(raw + 1)   # one byte of room past the original catches a stream that makes more
    src = blob if isinstance(blob, bytes) else bytes(blob)
    body = (ctypes.cast(ctypes.c_char_p(src), ctypes.c_void_p).value or 0) + HEAD.size   # the stream in place, not a copy (src lives through the call)
    n = _lib().compression_decode_buffer((ctypes.c_char * len(out)).from_buffer(out), raw + 1, body, len(src) - HEAD.size, None, alg)
    if n != raw: raise ValueError(f'DMZ1: the stream decodes to {n} bytes, its header names {raw}')
    del out[raw:]
    return bytes(out)
