#!/usr/bin/env python3
"""NXMSLPAK: An uncompressed, indexed binary archive for Metal shader libraries (.metallib),
reflection JSONs (.json), and inputs.tsv to prevent inode bloat in Mac application bundles.

Format:
- Header (32 bytes):
  - Magic (8 bytes): b"NXMSLPAK"
  - Version (uint32): 1
  - Flags (uint32): 0
  - Count (uint32): number of files
  - Index offset (uint64): byte offset to the index table
- Data section:
  - File payloads written sequentially, 16-byte aligned
- Index section (at index_offset):
  - Array of 'count' entries (24 bytes each), sorted alphabetically by name:
    - name_offset (uint32): offset into the string table
    - name_len (uint32): length of filename (excluding null terminator)
    - data_offset (uint64): byte offset in archive
    - data_len (uint64): size in bytes
  - String table:
    - Null-terminated UTF-8 filenames
"""

import os
import struct
from pathlib import Path

MAGIC = b"NXMSLPAK"
VERSION = 1
HEADER_STRUCT = struct.Struct("<8sIIIQ")
ENTRY_STRUCT = struct.Struct("<IIQQ")
ALIGNMENT = 16


def pack(sources, out_pak_path):
    """Pack a collection of file paths into an NXMSLPAK archive.

    sources: iterable of Path objects or file path strings.
    out_pak_path: output Path for the .pak file.
    """
    out_pak = Path(out_pak_path)
    out_pak.parent.mkdir(parents=True, exist_ok=True)

    # Sort files alphabetically by filename for binary search compatibility
    files = sorted([Path(s) for s in sources], key=lambda p: p.name)

    # Build string table and compute file payload positions
    str_table = bytearray()
    name_offsets = []
    for f in files:
        name_bytes = f.name.encode("utf-8") + b"\0"
        name_offsets.append(len(str_table))
        str_table.extend(name_bytes)

    # Data starts after the 32-byte header
    data_start = HEADER_STRUCT.size
    current_offset = data_start
    entries = []

    # Prepare temporary file for atomic write
    tmp_pak = out_pak.with_name(out_pak.name + ".tmp")
    with open(tmp_pak, "wb") as out_f:
        # Placeholder header
        out_f.write(b"\0" * HEADER_STRUCT.size)

        for i, f in enumerate(files):
            # Pad to 16 bytes alignment
            pad = (ALIGNMENT - (current_offset % ALIGNMENT)) % ALIGNMENT
            if pad:
                out_f.write(b"\0" * pad)
                current_offset += pad

            data_offset = current_offset
            data = f.read_bytes()
            data_len = len(data)
            out_f.write(data)
            current_offset += data_len

            entries.append((name_offsets[i], len(f.name.encode("utf-8")), data_offset, data_len))

        # Index offset
        index_offset = current_offset
        for entry in entries:
            out_f.write(ENTRY_STRUCT.pack(*entry))

        # Write string table
        out_f.write(str_table)

        # Write actual header
        out_f.seek(0)
        header_bytes = HEADER_STRUCT.pack(MAGIC, VERSION, 0, len(files), index_offset)
        out_f.write(header_bytes)

    tmp_pak.replace(out_pak)
    return len(files)


def unpack(pak_path, out_dir):
    """Unpack an NXMSLPAK archive into out_dir."""
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    with open(pak_path, "rb") as f:
        header = f.read(HEADER_STRUCT.size)
        magic, ver, flags, count, idx_off = HEADER_STRUCT.unpack(header)
        if magic != MAGIC or ver != VERSION:
            raise ValueError(f"Invalid NXMSLPAK archive: magic={magic!r}, ver={ver}")

        f.seek(idx_off)
        entries = []
        for _ in range(count):
            entries.append(ENTRY_STRUCT.unpack(f.read(ENTRY_STRUCT.size)))

        str_table = f.read()

        for name_off, name_len, data_off, data_len in entries:
            name = str_table[name_off : name_off + name_len].decode("utf-8")
            f.seek(data_off)
            data = f.read(data_len)
            (out_dir / name).write_bytes(data)


def list_entries(pak_path):
    """List all (name, data_offset, data_len) entries in an archive."""
    with open(pak_path, "rb") as f:
        header = f.read(HEADER_STRUCT.size)
        magic, ver, flags, count, idx_off = HEADER_STRUCT.unpack(header)
        if magic != MAGIC or ver != VERSION:
            raise ValueError(f"Invalid NXMSLPAK archive: magic={magic!r}, ver={ver}")

        f.seek(idx_off)
        entries = []
        for _ in range(count):
            entries.append(ENTRY_STRUCT.unpack(f.read(ENTRY_STRUCT.size)))

        str_table = f.read()
        res = []
        for name_off, name_len, data_off, data_len in entries:
            name = str_table[name_off : name_off + name_len].decode("utf-8")
            res.append((name, data_off, data_len))
        return res
