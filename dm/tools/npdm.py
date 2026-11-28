#!/usr/bin/env python3
"""Bounds-checked reader for Switch NPDM (META) headers and their ACI0 section."""
import struct

META_SIZE = 0x80
ACI0_MIN_SIZE = 0x18


class Error(ValueError):
    pass


def aci0(data: bytes | bytearray | memoryview) -> memoryview:
    """Return the bounds-checked ACI0 section declared by an NPDM META header."""
    view = memoryview(data)
    if len(view) < META_SIZE or view[:4] != b'META':
        raise Error('not an NPDM')
    off, size = struct.unpack_from('<II', view, 0x70)
    if off < META_SIZE or size < ACI0_MIN_SIZE or off + size > len(view) or view[off:off + 4] != b'ACI0':
        raise Error('no ACI0 section')
    return view[off:off + size]


def program_id(data: bytes | bytearray | memoryview) -> int:
    """Return the 64-bit program ID from the NPDM's header-declared ACI0 section."""
    return struct.unpack_from('<Q', aci0(data), 0x10)[0]


def sys_resource(data: bytes | bytearray | memoryview) -> int:
    """Return SystemResourceSize from META +0x14, or 0 when the bytes are not a META header."""
    if len(data) < 0x18 or data[:4] != b'META':
        return 0
    return struct.unpack_from('<I', data, 0x14)[0]


def aarch64(data: bytes | bytearray | memoryview) -> bool:
    """Return Is64BitInstruction (META +0x0C bit 0), or True when the bytes are not a META header."""
    if len(data) <= 0x0C or data[:4] != b'META':
        return True
    return bool(data[0x0C] & 1)
