#!/usr/bin/env python3
"""Build audited BC7 -> ASTC 4x4 patches for Z-A data.trpfs.

The source archive is always opened read-only.  ``census`` only writes JSON.
``prove`` runs one selected Trinity file through extract, BNTX conversion,
the game's Oodle encoder, an independent game-decoder round trip, and emits
an exact range-patch bundle.  ``prepare`` extends the same operation to all
selected packs.  ``apply`` verifies the source identity, clones (or explicitly
copies) it to a new path, and applies the exact ranges to that clone.
``relocate`` inserts copied replacement ONEPACK records immediately before the
archive's root FlatBuffer and emits an exact virtual pack layout, root-pointer,
root-index and descriptor edits; ``apply-relocated`` materializes that compact
bundle into a new, atomically published pair.  Trinity requires both parallel
root vectors to remain sorted and each pack offset to be the aligned prefix sum
of descriptor sizes.  A small read-redirect table therefore maps that virtual
layout to unchanged source packs or inserted replacement packs.  The root must
remain at EOF in the virtual layout: appending packs after it makes Trinity
parse the appended payload as root-index data.

Only BC7_UNORM and BC7_sRGB are eligible in the first version.  BC1/BC4 would
grow from 8 to 16 bytes per block, BC5 needs a different no-BC destination
contract, and BC6H is HDR.  A replacement is admitted only when its guest
Oodle stream fits the existing FlatBuffer byte-vector capacity.  Relocated
packs instead append a new vector inside the copied pack, leaving every source
byte immutable.
"""

from __future__ import annotations

import argparse
import bisect
import collections
import ctypes as C
import errno
import hashlib
import json
import math
import mmap
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import time


REPO = Path(__file__).resolve().parents[2]
ONEPACK = b"ONEPACK\0"
REDIRECT_MAGIC = "DM_ROMFS_REDIRECTS_V1"
BC7_TO_ASTC = {0x2001: 0x2D01, 0x2006: 0x2D06}
FORMAT_NAMES = {
    0x1A01: "BC1_UNORM",
    0x1A06: "BC1_sRGB",
    0x1B01: "BC2_UNORM",
    0x1B06: "BC2_sRGB",
    0x1C01: "BC3_UNORM",
    0x1C06: "BC3_sRGB",
    0x1D01: "BC4_UNORM",
    0x1D02: "BC4_SNORM",
    0x1E01: "BC5_UNORM",
    0x1E02: "BC5_SNORM",
    0x1F05: "BC6H_UFLOAT",
    0x1F0A: "BC6H_SFLOAT",
    0x2001: "BC7_UNORM",
    0x2006: "BC7_sRGB",
    0x2D01: "ASTC4x4_UNORM",
    0x2D06: "ASTC4x4_sRGB",
}


class FormatError(ValueError):
    """An archive or texture structure failed a defensive validation."""


class QualityError(ValueError):
    """A transcoded texture did not meet the requested quality floor."""


def sha256_bytes(data: bytes | bytearray | memoryview) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while True:
            block = stream.read(8 << 20)
            if not block:
                return digest.hexdigest()
            digest.update(block)


def sha256_file_with_ranges(path: Path, patches: list[dict], append: bytes | Path = b"") -> str:
    """Hash a virtual source with disjoint replacements and optional append."""
    digest = hashlib.sha256()
    cursor = 0
    with path.open("rb") as stream:
        for patch in sorted(patches, key=lambda row: row["offset"]):
            offset = patch["offset"]
            if offset < cursor:
                raise ValueError("virtual hash ranges overlap")
            left = offset - cursor
            while left:
                block = stream.read(min(left, 8 << 20))
                if not block:
                    raise EOFError("virtual hash range leaves source")
                digest.update(block)
                left -= len(block)
            original = read_exact(stream, patch["bytes"])
            if sha256_bytes(original) != patch["source_sha256"]:
                raise ValueError(f"source range differs at {offset:#x}")
            replacement = bytes.fromhex(patch["replacement_hex"])
            if len(replacement) != patch["bytes"]:
                raise ValueError("virtual replacement has a different size")
            digest.update(replacement)
            cursor = offset + patch["bytes"]
        while True:
            block = stream.read(8 << 20)
            if not block:
                break
            digest.update(block)
    if isinstance(append, Path):
        with append.open("rb") as stream:
            while True:
                block = stream.read(8 << 20)
                if not block:
                    break
                digest.update(block)
    else:
        digest.update(append)
    return digest.hexdigest()


def patched_bytes(source: bytes | bytearray | memoryview, base: int,
                  patches: list[dict], offset_key: str = "source_offset") -> bytes:
    """Return one bounded source range with exact, non-overlapping edits."""
    result = bytearray(source)
    previous_end = base
    for patch in sorted(patches, key=lambda row: row[offset_key]):
        offset = patch[offset_key]
        size = patch["bytes"]
        if offset < previous_end or offset < base or offset + size > base + len(result):
            raise ValueError("inserted-archive edit range is invalid or overlaps")
        relative = offset - base
        original = bytes(result[relative:relative + size])
        if (original.hex() != patch["source_hex"] or
                sha256_bytes(original) != patch["source_sha256"]):
            raise ValueError(f"source range differs at {offset:#x}")
        replacement = bytes.fromhex(patch["replacement_hex"])
        if len(replacement) != size or sha256_bytes(replacement) != patch["replacement_sha256"]:
            raise ValueError(f"replacement range differs at {offset:#x}")
        result[relative:relative + size] = replacement
        previous_end = offset + size
    return bytes(result)


def sha256_inserted_archive(path: Path, root_offset: int, append: Path,
                            header_edits: list[dict], final_root: bytes) -> str:
    """Hash patched prefix + inserted packs + already-patched shifted root."""
    size = path.stat().st_size
    if not 16 <= root_offset < size:
        raise ValueError("archive root offset is outside the source")
    digest = hashlib.sha256()
    rows = sorted(header_edits, key=lambda row: row["source_offset"])
    cursor = 0
    with path.open("rb") as stream:
        for row in rows:
            offset, count = row["source_offset"], row["bytes"]
            if offset < cursor or offset + count > root_offset:
                raise ValueError("archive header edit is outside the prefix or overlaps")
            left = offset - cursor
            while left:
                block = read_exact(stream, min(left, 8 << 20))
                digest.update(block); left -= len(block)
            original = read_exact(stream, count)
            if (original.hex() != row["source_hex"] or
                    sha256_bytes(original) != row["source_sha256"]):
                raise ValueError(f"archive header source differs at {offset:#x}")
            replacement = bytes.fromhex(row["replacement_hex"])
            if len(replacement) != count or sha256_bytes(replacement) != row["replacement_sha256"]:
                raise ValueError(f"archive header replacement differs at {offset:#x}")
            digest.update(replacement); cursor = offset + count
        left = root_offset - cursor
        while left:
            block = read_exact(stream, min(left, 8 << 20))
            digest.update(block); left -= len(block)
    with append.open("rb") as stream:
        while True:
            block = stream.read(8 << 20)
            if not block:
                break
            digest.update(block)
    digest.update(final_root)
    return digest.hexdigest()


def align_up(value: int, alignment: int) -> int:
    if alignment <= 0 or alignment & (alignment - 1):
        raise ValueError("alignment must be a positive power of two")
    return (value + alignment - 1) & -alignment


def virtual_pack_layout(packs: list[dict], relocated: list[dict],
                        source_root_offset: int, physical_root_offset: int,
                        root_bytes: int, physical_file_bytes: int) -> tuple[list[int], list[dict], int]:
    """Build Trinity's aligned virtual pack sequence and its physical read map.

    The two root vectors are parallel and hash-sorted.  Their order therefore
    cannot change, while Trinity also requires the offset difference between
    adjacent entries to equal the current descriptor size rounded to 16 bytes.
    Only the virtual offsets move; each whole-pack read maps back to either the
    immutable source range or the inserted replacement range.
    """
    ordered = sorted(packs, key=lambda row: row["pack_index"])
    if not ordered or [row["pack_index"] for row in ordered] != list(range(len(ordered))):
        raise FormatError("pack indices are not one complete ordered sequence")
    replacements = {row["pack_index"]: row for row in relocated}
    if len(replacements) != len(relocated):
        raise FormatError("relocated pack table contains duplicate indices")
    unknown = set(replacements) - {row["pack_index"] for row in ordered}
    if unknown:
        raise FormatError("relocated pack table contains an unknown index")

    # Refuse a source with a different physical invariant.  The virtual map is
    # safe only because every source pack occupies exactly its aligned span.
    for number, pack in enumerate(ordered):
        expected_end = pack["pack_offset"] + align_up(pack["pack_size"], 16)
        actual_end = (ordered[number + 1]["pack_offset"]
                      if number + 1 < len(ordered) else source_root_offset)
        if expected_end != actual_end:
            raise FormatError(f"source pack {pack['pack_index']} is not a contiguous aligned span")

    virtual = ordered[0]["pack_offset"]
    if virtual <= 0 or virtual > physical_file_bytes:
        raise FormatError("archive prefix span is invalid")
    offsets = []
    redirects = [{
        "kind": "header", "virtual_offset": 0, "virtual_bytes": virtual,
        "physical_offset": 0, "physical_bytes": virtual,
    }]
    for pack in ordered:
        replacement = replacements.get(pack["pack_index"])
        size = replacement["appended_bytes"] if replacement else pack["pack_size"]
        physical = replacement["appended_offset"] if replacement else pack["pack_offset"]
        span = align_up(size, 16)
        if (physical < 0 or span <= 0 or physical > physical_file_bytes or
                span > physical_file_bytes - physical):
            raise FormatError(f"physical pack {pack['pack_index']} leaves the materialized archive")
        offsets.append(virtual)
        redirects.append({
            "pack_index": pack["pack_index"],
            "virtual_offset": virtual, "virtual_bytes": span,
            "physical_offset": physical, "physical_bytes": span,
        })
        virtual += span
    virtual_root_offset = virtual
    if (physical_root_offset < 0 or root_bytes <= 0 or
            physical_root_offset + root_bytes > physical_file_bytes):
        raise FormatError("physical root leaves the materialized archive")
    redirects.append({
        "kind": "root", "virtual_offset": virtual_root_offset,
        "virtual_bytes": root_bytes, "physical_offset": physical_root_offset,
        "physical_bytes": root_bytes,
    })
    return offsets, redirects, virtual_root_offset


def redirect_table_bytes(rows: list[dict], virtual_file_bytes: int,
                         physical_file_bytes: int,
                         target: str = "arc/data.trpfs") -> bytes:
    """Serialize the bounded whole-read virtual-to-physical mapping."""
    if (not rows or not target or any(c.isspace() for c in target) or
            virtual_file_bytes <= 0 or physical_file_bytes <= 0):
        raise FormatError("redirect table header is invalid")
    lines = [f"{REDIRECT_MAGIC}\t{target}\t{len(rows)}\t{virtual_file_bytes}\t{physical_file_bytes}\n"]
    previous_virtual_end = 0
    for row in rows:
        values = tuple(row[key] for key in
                       ("virtual_offset", "virtual_bytes", "physical_offset", "physical_bytes"))
        virtual_offset, virtual_bytes, physical_offset, physical_bytes = values
        if (any(not isinstance(value, int) or value < 0 for value in values) or
                virtual_bytes <= 0 or physical_bytes < virtual_bytes or
                virtual_offset != previous_virtual_end or
                virtual_offset + virtual_bytes > virtual_file_bytes or
                physical_offset + physical_bytes > physical_file_bytes):
            raise FormatError("redirect table range is invalid or overlaps")
        previous_virtual_end = virtual_offset + virtual_bytes
        lines.append("\t".join(str(value) for value in values) + "\n")
    if previous_virtual_end != virtual_file_bytes:
        raise FormatError("redirect table does not cover the virtual file")
    return "".join(lines).encode("ascii")


def parse_redirect_table(data: bytes) -> dict:
    """Parse and canonically reserialize one redirect table."""
    try:
        lines = data.decode("ascii").splitlines(keepends=True)
        header = lines[0].removesuffix("\n").split("\t")
        if len(header) != 5 or header[0] != REDIRECT_MAGIC:
            raise FormatError("redirect table magic or header differs")
        target = header[1]
        count, virtual_bytes, physical_bytes = map(int, header[2:])
        rows = []
        for line in lines[1:]:
            fields = line.removesuffix("\n").split("\t")
            if len(fields) != 4:
                raise FormatError("redirect table row width differs")
            values = tuple(map(int, fields))
            rows.append(dict(zip(("virtual_offset", "virtual_bytes",
                                  "physical_offset", "physical_bytes"), values)))
    except (IndexError, UnicodeDecodeError, ValueError) as exc:
        raise FormatError("redirect table is malformed") from exc
    if count != len(rows):
        raise FormatError("redirect table record count differs")
    if redirect_table_bytes(rows, virtual_bytes, physical_bytes, target) != data:
        raise FormatError("redirect table is not canonical")
    return {"target": target, "records": rows,
            "virtual_file_bytes": virtual_bytes,
            "physical_file_bytes": physical_bytes}


def fsync_directory(path: Path) -> None:
    descriptor = os.open(path, os.O_RDONLY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def read_exact(stream, size: int) -> bytes:
    result = bytearray()
    while len(result) < size:
        block = stream.read(size - len(result))
        if not block:
            raise EOFError(f"stream ended after {len(result)} of {size} bytes")
        result += block
    return bytes(result)


def ensure_private_path(path: Path) -> None:
    """Derived game data must not be created in the source repository."""
    resolved = path.resolve(strict=False)
    try:
        resolved.relative_to(REPO)
    except ValueError:
        return
    raise ValueError(f"derived game data must be outside the source repository: {resolved}")


def fb_field(data, table: int, index: int) -> int | None:
    if table < 4 or table + 4 > len(data):
        raise FormatError("FlatBuffer table is out of bounds")
    vtable = table - struct.unpack_from("<i", data, table)[0]
    if vtable < 0 or vtable + 4 > len(data):
        raise FormatError("FlatBuffer vtable is out of bounds")
    vsize = struct.unpack_from("<H", data, vtable)[0]
    if vsize < 4 or vtable + vsize > len(data):
        raise FormatError("FlatBuffer vtable size is invalid")
    if 4 + 2 * index >= vsize:
        return None
    offset = struct.unpack_from("<H", data, vtable + 4 + 2 * index)[0]
    field = table + offset if offset else None
    if field is not None and field >= len(data):
        raise FormatError("FlatBuffer field is out of bounds")
    return field


def fb_vector(data, table: int, index: int) -> tuple[int, int]:
    field = fb_field(data, table, index)
    if field is None:
        return 0, 0
    if field + 4 > len(data):
        raise FormatError("FlatBuffer vector reference is truncated")
    vector = field + struct.unpack_from("<I", data, field)[0]
    if vector < 0 or vector + 4 > len(data):
        raise FormatError("FlatBuffer vector is out of bounds")
    start, count = vector + 4, struct.unpack_from("<I", data, vector)[0]
    return start, count


def fb_refs(data, table: int, index: int) -> list[int]:
    start, count = fb_vector(data, table, index)
    if count > (len(data) - start) // 4:
        raise FormatError("FlatBuffer reference vector is truncated")
    result = []
    for item in range(count):
        at = start + item * 4
        target = at + struct.unpack_from("<I", data, at)[0]
        if target < 0 or target >= len(data):
            raise FormatError("FlatBuffer reference is out of bounds")
        result.append(target)
    return result


def fb_root(data, at: int = 0) -> int:
    if at < 0 or at + 4 > len(data):
        raise FormatError("FlatBuffer root reference is truncated")
    root = at + struct.unpack_from("<I", data, at)[0]
    if root < 0 or root >= len(data):
        raise FormatError("FlatBuffer root is out of bounds")
    return root


def bntx_textures(data: bytes | bytearray) -> list[dict]:
    """Return defensively validated BRTI metadata, including embedded BNTX."""
    rows = []
    search = 0
    while True:
        base = data.find(b"BNTX\0\0\0\0", search)
        if base < 0:
            return rows
        search = base + 8
        if base + 0x50 > len(data) or data[base + 12:base + 14] != b"\xff\xfe":
            continue
        size = struct.unpack_from("<I", data, base + 0x1C)[0]
        if size < 0x50 or base + size > len(data):
            continue
        count = struct.unpack_from("<I", data, base + 0x24)[0]
        pointer_array = struct.unpack_from("<Q", data, base + 0x28)[0]
        if count > 65536 or pointer_array + count * 8 > size:
            continue
        for texture_index in range(count):
            brti = struct.unpack_from("<Q", data, base + pointer_array + texture_index * 8)[0]
            if brti + 0x78 > size or data[base + brti:base + brti + 4] != b"BRTI":
                raise FormatError("invalid BRTI pointer")
            q = base + brti
            fmt = struct.unpack_from("<I", data, q + 0x1C)[0]
            width, height, depth, layers = struct.unpack_from("<4I", data, q + 0x24)
            mips = struct.unpack_from("<H", data, q + 0x16)[0]
            image_size, alignment, channels = struct.unpack_from("<3I", data, q + 0x50)
            mip_table = struct.unpack_from("<Q", data, q + 0x70)[0]
            if not (0 < width <= 16384 and 0 < height <= 16384 and
                    0 < depth <= 2048 and 0 < layers <= 2048 and 0 < mips <= 16):
                raise FormatError("invalid BRTI dimensions")
            if mip_table + mips * 8 > size:
                raise FormatError("invalid BRTI mip pointer table")
            offsets = list(struct.unpack_from("<" + "Q" * mips, data, base + mip_table))
            if (not offsets or offsets[0] + image_size > size or
                    any(x < offsets[0] or x >= offsets[0] + image_size for x in offsets)):
                raise FormatError("invalid BRTI image bounds")
            payload = data[base + offsets[0]:base + offsets[0] + image_size]
            rows.append({
                "bntx_base": base,
                "bntx_size": size,
                "texture_index": texture_index,
                "brti_offset": brti,
                "format": fmt,
                "format_name": FORMAT_NAMES.get(fmt, f"format_{fmt:08x}"),
                "width": width,
                "height": height,
                "depth": depth,
                "layers": layers,
                "mips": mips,
                "image_bytes": image_size,
                "alignment": alignment,
                "channels": channels,
                "tile_mode": struct.unpack_from("<H", data, q + 0x12)[0],
                "block_height_log2": struct.unpack_from("<I", data, q + 0x34)[0] & 7,
                "mip_offsets": [x - offsets[0] for x in offsets],
                "data_offset": offsets[0],
                "payload_sha256": sha256_bytes(payload),
            })


def descriptor_catalog(path: Path) -> dict:
    """Parse the immutable trpfd fields needed to name and relocate packs."""
    data = path.read_bytes()
    root = fb_root(data)
    hash_at, hash_count = fb_vector(data, root, 0)
    hashes = struct.unpack_from("<" + "Q" * hash_count, data, hash_at)
    strings = []
    for ref in fb_refs(data, root, 1):
        length = struct.unpack_from("<I", data, ref)[0]
        if ref + 4 + length > len(data):
            raise FormatError("descriptor pack name is truncated")
        strings.append(data[ref + 4:ref + 4 + length].decode("utf-8", "replace"))
    pack_of_hash = {}
    for file_hash, info in zip(hashes, fb_refs(data, root, 2)):
        field = fb_field(data, info, 0)
        pack_index = struct.unpack_from("<Q", data, field)[0] if field is not None else 0
        if pack_index >= len(strings):
            raise FormatError("descriptor file info names an invalid pack")
        pack_of_hash[file_hash] = pack_index
    pack_infos = []
    for pack_index, info in enumerate(fb_refs(data, root, 3)):
        size_field, count_field = fb_field(data, info, 0), fb_field(data, info, 1)
        if size_field is None or count_field is None:
            raise FormatError(f"descriptor pack {pack_index} is missing size/count")
        pack_infos.append({
            "descriptor_index": pack_index,
            "name": strings[pack_index],
            "size": struct.unpack_from("<Q", data, size_field)[0],
            "file_count": struct.unpack_from("<Q", data, count_field)[0],
            "size_field_offset": size_field,
        })
    if len(pack_infos) != len(strings):
        raise FormatError("descriptor pack name/info count mismatch")
    return {
        "bytes": data,
        "pack_of_hash": pack_of_hash,
        "pack_infos": pack_infos,
        "names_by_hash": {file_hash: strings[pack_index]
                          for file_hash, pack_index in pack_of_hash.items()},
    }


def descriptor_pack_names(path: Path) -> dict[int, str]:
    return descriptor_catalog(path)["names_by_hash"]


def trpfs_records(data, names: dict[int, str], descriptor: dict | None = None) -> tuple[list[dict], list[dict]]:
    if data[:8] != ONEPACK:
        raise FormatError("archive does not start with ONEPACK\\0")
    root_at = struct.unpack_from("<Q", data, 8)[0]
    root = fb_root(data, root_at)
    offset_at, pack_count = fb_vector(data, root, 1)
    if pack_count > (len(data) - offset_at) // 8:
        raise FormatError("pack offset vector is truncated")
    packs, files = [], []
    for pack_index, pack_offset in enumerate(struct.unpack_from("<" + "Q" * pack_count, data, offset_at)):
        pack_root = fb_root(data, pack_offset)
        hash_at, hash_count = fb_vector(data, pack_root, 0)
        hashes = struct.unpack_from("<" + "Q" * hash_count, data, hash_at)
        tables = fb_refs(data, pack_root, 1)
        if len(tables) != len(hashes):
            raise FormatError(f"pack {pack_index} hash/file count mismatch")
        pack_name = names.get(hashes[0], f"pack-{pack_index}") if hashes else f"pack-{pack_index}"
        descriptor_index = None
        pack_size = None
        descriptor_size_field = None
        if descriptor is not None and hashes:
            descriptor_index = descriptor["pack_of_hash"].get(hashes[0])
            if descriptor_index is None:
                raise FormatError(f"pack {pack_index} has no descriptor entry")
            pack_info = descriptor["pack_infos"][descriptor_index]
            pack_size = pack_info["size"]
            descriptor_size_field = pack_info["size_field_offset"]
            if pack_info["file_count"] != len(tables):
                raise FormatError(f"pack {pack_index} descriptor file count mismatch")
            if pack_offset + pack_size > len(data):
                raise FormatError(f"pack {pack_index} descriptor size leaves the archive")
        pack_common = {
            "pack_index": pack_index,
            "pack_offset": pack_offset,
            "pack_offset_field": offset_at + pack_index * 8,
            "pack_name": pack_name,
            "file_count": len(tables),
            "descriptor_index": descriptor_index,
            "pack_size": pack_size,
            "descriptor_size_field": descriptor_size_field,
        }
        packs.append(pack_common)
        for file_index, (file_hash, table) in enumerate(zip(hashes, tables)):
            compression_field = fb_field(data, table, 1)
            level_field = fb_field(data, table, 2)
            raw_field = fb_field(data, table, 3)
            payload_field = fb_field(data, table, 4)
            payload_at, stored_size = fb_vector(data, table, 4)
            if payload_at + stored_size > len(data):
                raise FormatError(f"pack {pack_index} file {file_index} payload is truncated")
            files.append({
                "pack_index": pack_index,
                **pack_common,
                "file_index": file_index,
                "file_hash": file_hash,
                "file_table_offset": table,
                "compression_field_offset": compression_field,
                "raw_size_field_offset": raw_field,
                "payload_field_offset": payload_field,
                "compression": struct.unpack_from("<b", data, compression_field)[0] if compression_field is not None else 0,
                "compression_level": data[level_field] if level_field is not None else 0,
                "raw_size": struct.unpack_from("<Q", data, raw_field)[0] if raw_field is not None else 0,
                "payload_offset": payload_at,
                "payload_capacity": stored_size,
            })
    return packs, files


class GuestDecoder:
    def __init__(self, host: Path, modules: Path, log_path: Path):
        environment = dict(os.environ)
        environment.pop("DM_OODLE_COMPRESS_JOB", None)
        environment.pop("DM_WINDOW", None)
        environment["DM_OODLE_SERVE"] = "1"
        self.log_path = log_path
        self.log = log_path.open("wb")
        self.process = subprocess.Popen(
            [str(host), str(modules), "main", "sdk"], env=environment,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.log)

    def decode(self, payload: bytes, raw_size: int) -> bytes:
        if self.process.stdin is None or self.process.stdout is None:
            raise RuntimeError("guest decoder pipes are unavailable")
        try:
            self.process.stdin.write(struct.pack("<QQ", len(payload), raw_size))
            self.process.stdin.write(payload)
            self.process.stdin.flush()
            got = struct.unpack("<Q", read_exact(self.process.stdout, 8))[0]
            if got != raw_size:
                raise RuntimeError(f"guest decoder returned {got}, expected {raw_size}; see {self.log_path}")
            return read_exact(self.process.stdout, got)
        except (BrokenPipeError, EOFError, struct.error) as exc:
            raise RuntimeError(f"guest decoder failed; see {self.log_path}") from exc

    def close(self) -> None:
        if self.process.stdin is not None and not self.process.stdin.closed:
            self.process.stdin.close()
        returncode = self.process.wait(timeout=30)
        if self.process.stdout is not None:
            self.process.stdout.close()
        self.log.close()
        if returncode:
            raise RuntimeError(f"guest decoder exited with {returncode}; see {self.log_path}")

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, traceback):
        try:
            self.close()
        except Exception:
            if exc is None:
                raise


class GuestEncoder:
    def __init__(self, host: Path, modules: Path, work: Path, log_path: Path,
                 compressor: int = 9, level: int = 7):
        self.host, self.modules, self.work, self.log_path = host, modules, work, log_path
        self.compressor, self.level = compressor, level

    def encode(self, raw: bytes, tag: str) -> bytes:
        if not raw or len(raw) > 64 << 20:
            raise ValueError("guest compression job accepts 1..64 MiB")
        safe_tag = "".join(c if c.isalnum() or c in "-_" else "_" for c in tag)[:80]
        job = self.work / f"compress-{safe_tag}.job"
        output = Path(str(job) + ".out")
        job.write_bytes(b"DMCJ" + struct.pack("<III", self.compressor, self.level, len(raw)) + raw)
        environment = dict(os.environ)
        environment.pop("DM_OODLE_SERVE", None)
        environment.pop("DM_WINDOW", None)
        environment["DM_OODLE_COMPRESS_JOB"] = str(job)
        try:
            with self.log_path.open("ab") as log:
                process = subprocess.run(
                    [str(self.host), str(self.modules), "main", "sdk"], env=environment,
                    stdout=subprocess.DEVNULL, stderr=log, timeout=180)
            if process.returncode or not output.is_file():
                raise RuntimeError(f"guest encoder failed for {tag}; see {self.log_path}")
            return output.read_bytes()
        finally:
            job.unlink(missing_ok=True)
            output.unlink(missing_ok=True)


class TextureDesc(C.Structure):
    _fields_ = [(name, C.c_int) for name in
                ("target", "format", "flags", "samples", "width", "height",
                 "depth", "levels", "stride", "pkg_block_h")]


class TextureLayout(C.Structure):
    _fields_ = [("size", C.c_uint64), ("layer_size", C.c_uint64)] + [
        (name, C.c_uint32) for name in ("align", "layers", "bpb", "tileH", "tileD")]


def build_texture_library(output: Path) -> None:
    sources = [
        REPO / "dm/gfx/tex/nvn_deswizzle.c",
        REPO / "dm/gfx/nvn/nvn_tex.c",
        REPO / "dm/gfx/tex/nvn_bcdec.c",
    ]
    subprocess.run(["clang", "-O2", "-dynamiclib", "-o", str(output), *(str(x) for x in sources)],
                   check=True)


def write_tga(path: Path, width: int, height: int, rgba: bytes) -> None:
    if len(rgba) != width * height * 4:
        raise ValueError("RGBA byte count does not match TGA dimensions")
    bgra = bytearray(len(rgba))
    bgra[0::4], bgra[1::4], bgra[2::4], bgra[3::4] = rgba[2::4], rgba[1::4], rgba[0::4], rgba[3::4]
    header = struct.pack("<BBBHHBHHHHBB", 0, 0, 2, 0, 0, 0, 0, 0,
                         width, height, 32, 0x28)  # top-left, 8 alpha bits
    path.write_bytes(header + bgra)


def read_tga_rgba(path: Path) -> tuple[int, int, bytes]:
    data = path.read_bytes()
    if len(data) < 18:
        raise FormatError("ASTC decoder wrote a truncated TGA")
    ident, cmap, kind = data[0], data[1], data[2]
    width, height, bits, descriptor = struct.unpack_from("<HHBB", data, 12)
    if cmap or kind not in (2, 10) or bits not in (24, 32) or not width or not height:
        raise FormatError("ASTC decoder wrote an unsupported TGA")
    bpp = bits // 8
    at = 18 + ident
    pixels = []
    if kind == 2:
        need = width * height * bpp
        if at + need > len(data):
            raise FormatError("ASTC decoder TGA pixels are truncated")
        pixels = [data[p:p + bpp] for p in range(at, at + need, bpp)]
    else:
        total = width * height
        while len(pixels) < total:
            if at >= len(data):
                raise FormatError("ASTC decoder TGA RLE is truncated")
            packet = data[at]
            at += 1
            count = (packet & 0x7F) + 1
            if packet & 0x80:
                if at + bpp > len(data):
                    raise FormatError("ASTC decoder TGA RLE pixel is truncated")
                pixel = data[at:at + bpp]
                at += bpp
                pixels.extend([pixel] * count)
            else:
                need = count * bpp
                if at + need > len(data):
                    raise FormatError("ASTC decoder TGA raw packet is truncated")
                pixels.extend(data[p:p + bpp] for p in range(at, at + need, bpp))
                at += need
        if len(pixels) != total:
            raise FormatError("ASTC decoder TGA RLE overruns its dimensions")
    rows = [pixels[y * width:(y + 1) * width] for y in range(height)]
    if not descriptor & 0x20:  # bottom origin -> top origin
        rows.reverse()
    if descriptor & 0x10:  # right origin -> left origin
        rows = [list(reversed(row)) for row in rows]
    rgba = bytearray(width * height * 4)
    out = 0
    for row in rows:
        for pixel in row:
            rgba[out:out + 4] = bytes((pixel[2], pixel[1], pixel[0], pixel[3] if bpp == 4 else 255))
            out += 4
    return width, height, bytes(rgba)


def rgba_metrics(reference: bytes, decoded: bytes) -> dict:
    if len(reference) != len(decoded) or len(reference) % 4:
        raise ValueError("quality images have different byte counts")
    sum_squared = 0
    maximum = 0
    for left, right in zip(reference, decoded):
        delta = left - right
        absolute = abs(delta)
        maximum = max(maximum, absolute)
        sum_squared += delta * delta
    rmse = math.sqrt(sum_squared / len(reference))
    psnr = 999.0 if not rmse else 20 * math.log10(255 / rmse)
    return {"psnr_db": psnr, "rmse_8bit": rmse, "maximum_error_8bit": maximum}


class BntxConverter:
    def __init__(self, library: Path, encoder: Path, scratch: Path, cache: Path,
                 preset: str, jobs: int, minimum_psnr: float, maximum_rmse: float):
        self.library_path, self.encoder = library, encoder
        self.scratch, self.cache = scratch, cache
        self.preset, self.jobs = preset, jobs
        self.minimum_psnr, self.maximum_rmse = minimum_psnr, maximum_rmse
        self.encoder_sha256 = sha256_file(encoder)
        self.lib = C.CDLL(str(library.resolve()))
        self.lib.nvn_tex_layout.argtypes = [C.POINTER(TextureDesc), C.POINTER(TextureLayout)]
        self.lib.nvn_tex_level_offset.argtypes = [C.POINTER(TextureDesc), C.c_uint]
        self.lib.nvn_tex_level_offset.restype = C.c_uint64
        self.lib.nvn_deswizzle.argtypes = [C.POINTER(TextureDesc), C.c_uint, C.c_uint,
                                           C.c_void_p, C.c_void_p, C.c_size_t]
        self.lib.nvn_deswizzle.restype = C.c_int
        self.lib.nvn_swizzle.argtypes = [C.POINTER(TextureDesc), C.c_uint, C.c_uint,
                                         C.c_void_p, C.c_size_t, C.c_void_p]
        self.lib.nvn_swizzle.restype = C.c_int
        self.lib.nvn_bc_decode.argtypes = [C.c_uint, C.c_void_p, C.c_size_t, C.c_uint,
                                           C.c_uint, C.c_void_p, C.c_size_t]
        self.lib.nvn_bc_decode.restype = C.c_int
        cache.mkdir(parents=True, exist_ok=True)

    def convert_texture(self, data: bytes, texture: dict, label: str) -> tuple[bytes, dict]:
        at = texture["bntx_base"] + texture["data_offset"]
        original = data[at:at + texture["image_bytes"]]
        recipe = {
            "source_sha256": sha256_bytes(original),
            "format": texture["format"], "width": texture["width"], "height": texture["height"],
            "layers": texture["layers"], "mips": texture["mips"],
            "block_height_log2": texture["block_height_log2"],
            "encoder_sha256": self.encoder_sha256, "preset": self.preset,
            "block": "4x4", "minimum_psnr_db": self.minimum_psnr,
            "maximum_rmse_8bit": self.maximum_rmse,
        }
        key = sha256_bytes(json.dumps(recipe, sort_keys=True).encode())
        payload_cache, report_cache = self.cache / f"{key}.bin", self.cache / f"{key}.json"
        if payload_cache.is_file() and report_cache.is_file():
            payload, report = payload_cache.read_bytes(), json.loads(report_cache.read_text())
            if len(payload) == len(original) and report.get("payload_sha256") == sha256_bytes(payload):
                return payload, report

        if texture["format"] not in BC7_TO_ASTC or texture["depth"] != 1 or texture["tile_mode"] != 0:
            raise FormatError(f"unsupported target geometry in {label}")
        nvn_format = 0x4E if texture["format"] == 0x2006 else 0x4D
        desc = TextureDesc(4 if texture["layers"] > 1 else 1, nvn_format, 0, 1,
                           texture["width"], texture["height"], texture["layers"],
                           texture["mips"], 0, texture["block_height_log2"] + 1)
        layout = TextureLayout()
        self.lib.nvn_tex_layout(C.byref(desc), C.byref(layout))
        if layout.size != len(original):
            raise FormatError(f"NVN storage size mismatch in {label}: {layout.size} != {len(original)}")
        offsets = [self.lib.nvn_tex_level_offset(C.byref(desc), level)
                   for level in range(texture["mips"])]
        if offsets != texture["mip_offsets"]:
            raise FormatError(f"NVN mip layout mismatch in {label}")

        storage = C.create_string_buffer(original)
        result = C.create_string_buffer(original)
        quality = []
        profile = "s" if texture["format"] == 0x2006 else "l"
        for layer in range(texture["layers"]):
            for level in range(texture["mips"]):
                width = max(1, texture["width"] >> level)
                height = max(1, texture["height"] >> level)
                pitch = ((width + 3) // 4) * 16
                block_bytes = pitch * ((height + 3) // 4)
                bc = C.create_string_buffer(block_bytes)
                if self.lib.nvn_deswizzle(C.byref(desc), level, layer, storage, bc, pitch):
                    raise FormatError(f"NVN deswizzle failed in {label}")
                pixels = C.create_string_buffer(width * height * 4)
                if self.lib.nvn_bc_decode(nvn_format, bc, pitch, width, height, pixels, width * 4):
                    raise FormatError(f"BC7 decode failed in {label}")
                rgba = pixels.raw[:width * height * 4]
                stem = f"t{texture['texture_index']}-l{layer}-m{level}"
                source_path = self.scratch / f"{stem}.tga"
                astc_path = self.scratch / f"{stem}.astc"
                decoded_path = self.scratch / f"{stem}-decoded.tga"
                write_tga(source_path, width, height, rgba)
                subprocess.run([str(self.encoder), f"-c{profile}", str(source_path), str(astc_path),
                                "4x4", f"-{self.preset}", "-j", str(self.jobs), "-silent"],
                               check=True, capture_output=True)
                subprocess.run([str(self.encoder), f"-d{profile}", str(astc_path), str(decoded_path),
                                "-silent"], check=True, capture_output=True)
                decoded_width, decoded_height, decoded = read_tga_rgba(decoded_path)
                if (decoded_width, decoded_height) != (width, height):
                    raise FormatError(f"ASTC readback dimensions differ in {label}")
                metrics = rgba_metrics(rgba, decoded)
                metrics.update({"layer": layer, "mip": level})
                if metrics["psnr_db"] < self.minimum_psnr or metrics["rmse_8bit"] > self.maximum_rmse:
                    raise QualityError(
                        f"{label} layer={layer} mip={level} PSNR={metrics['psnr_db']:.2f} "
                        f"RMSE={metrics['rmse_8bit']:.3f}")
                encoded = astc_path.read_bytes()
                if encoded[:7] != b"\x13\xab\xa1\x5c\x04\x04\x01" or len(encoded) != 16 + block_bytes:
                    raise FormatError(f"ASTC encoder wrote an invalid 4x4 stream in {label}")
                blocks = C.create_string_buffer(encoded[16:])
                if self.lib.nvn_swizzle(C.byref(desc), level, layer, blocks, pitch, result):
                    raise FormatError(f"NVN swizzle failed in {label}")
                check = C.create_string_buffer(block_bytes)
                if self.lib.nvn_deswizzle(C.byref(desc), level, layer, result, check, pitch):
                    raise FormatError(f"NVN ASTC readback failed in {label}")
                if check.raw != encoded[16:]:
                    raise FormatError(f"NVN ASTC swizzle round trip differs in {label}")
                quality.append(metrics)
                source_path.unlink(missing_ok=True)
                astc_path.unlink(missing_ok=True)
                decoded_path.unlink(missing_ok=True)
        payload = result.raw[:len(original)]
        report = {"recipe": recipe, "payload_sha256": sha256_bytes(payload),
                  "minimum_psnr_db": min(x["psnr_db"] for x in quality),
                  "maximum_rmse_8bit": max(x["rmse_8bit"] for x in quality),
                  "subresources": quality}
        payload_cache.with_suffix(".bin.partial").write_bytes(payload)
        payload_cache.with_suffix(".bin.partial").replace(payload_cache)
        report_cache.with_suffix(".json.partial").write_text(json.dumps(report, indent=2) + "\n")
        report_cache.with_suffix(".json.partial").replace(report_cache)
        return payload, report

    def convert_blob(self, raw: bytes, label: str) -> tuple[bytes, list[dict], list[dict]]:
        result = bytearray(raw)
        converted, retained = [], []
        for texture in bntx_textures(raw):
            if texture["format"] not in BC7_TO_ASTC:
                continue
            try:
                payload, quality = self.convert_texture(raw, texture, label)
            except (FormatError, QualityError, subprocess.CalledProcessError) as exc:
                retained.append({"texture": texture, "reason": str(exc)})
                continue
            at = texture["bntx_base"] + texture["data_offset"]
            result[at:at + len(payload)] = payload
            format_at = texture["bntx_base"] + texture["brti_offset"] + 0x1C
            struct.pack_into("<I", result, format_at, BC7_TO_ASTC[texture["format"]])
            converted.append({"texture": texture, "new_format": BC7_TO_ASTC[texture["format"]],
                              "new_payload_sha256": sha256_bytes(payload), "quality": quality})
        return bytes(result), converted, retained


def decode_record(data, record: dict, decoder: GuestDecoder) -> bytes:
    start, size = record["payload_offset"], record["payload_capacity"]
    payload = bytes(data[start:start + size])
    if record["compression"] == 3:
        return decoder.decode(payload, record["raw_size"])
    # Trinity uses both an absent/default compression field and -1 for stored
    # records.  Size equality is the authoritative stored-file invariant used
    # by the engine's existing shader scanner.
    if size == record["raw_size"]:
        return payload
    raise FormatError(
        f"unsupported compression {record['compression']} for pack {record['pack_index']} "
        f"file {record['file_index']} ({size} -> {record['raw_size']})")


def bc7_file_selection(path: Path) -> set[tuple[int, int]]:
    """Read pack/file identities from a census or append-relocated manifest."""
    document = json.loads(path.read_text())
    wanted = {
        (row["pack_index"], row["file_index"])
        for row in document.get("rows", [])
        if row.get("format") in BC7_TO_ASTC
    }
    if document.get("mode") in ("append-relocated-packs",
                                  "insert-relocated-packs-before-root",
                                  "virtual-pack-layout-before-root"):
        for group in ("converted", "quality_retained"):
            for row in document.get(group, []):
                texture, record = row.get("texture", {}), row.get("record", {})
                if texture.get("format") in BC7_TO_ASTC:
                    wanted.add((record["pack_index"], record["file_index"]))
    if not wanted:
        raise ValueError(f"selection contains no BC7 records: {path}")
    return wanted


def selected_records(records: list[dict], args) -> list[dict]:
    selected = records
    if getattr(args, "census", None) is not None:
        wanted = bc7_file_selection(args.census)
        selected = [x for x in selected if (x["pack_index"], x["file_index"]) in wanted]
        print(f"BC7_SELECTED {len(wanted)} files from {args.census}", flush=True)
    if getattr(args, "fslog", None) is not None:
        touched = fslog_pack_indices(records, args.fslog, getattr(args, "fslog_before", None))
        selected = [x for x in selected if x["pack_index"] in touched]
        print(f"FSLOG_SELECTED {len(touched)} packs from {args.fslog}", flush=True)
    if getattr(args, "pack_index", None) is not None:
        wanted = set(args.pack_index)
        selected = [x for x in selected if x["pack_index"] in wanted]
    if getattr(args, "file_index", None) is not None:
        selected = [x for x in selected if x["file_index"] == args.file_index]
    if getattr(args, "limit_packs", 0):
        packs = sorted({x["pack_index"] for x in selected})[:args.limit_packs]
        selected = [x for x in selected if x["pack_index"] in set(packs)]
    return selected


def fslog_pack_indices(records: list[dict], path: Path, before: float | None = None) -> set[int]:
    """Map DM_FSLOG data.trpfs read ranges back to descriptor-sized packs."""
    by_pack = {}
    for record in records:
        if record["pack_size"] is not None:
            by_pack.setdefault(record["pack_index"],
                               (record["pack_offset"], record["pack_offset"] + record["pack_size"]))
    intervals = sorted((start, end, pack_index) for pack_index, (start, end) in by_pack.items())
    starts = [row[0] for row in intervals]
    touched = set()
    for number, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
        fields = line.split("\t")
        if len(fields) < 4 or not fields[1].replace("\\", "/").endswith("/arc/data.trpfs"):
            continue
        try:
            seconds, offset, length = float(fields[0]), int(fields[2], 0), int(fields[3], 0)
        except ValueError as exc:
            raise FormatError(f"invalid DM_FSLOG row {number}") from exc
        if before is not None and seconds > before:
            continue
        if offset < 0 or length <= 0:
            continue
        end = offset + length
        at = max(0, bisect.bisect_right(starts, offset) - 1)
        while at < len(intervals) and intervals[at][0] < end:
            start, pack_end, pack_index = intervals[at]
            if pack_end > offset:
                touched.add(pack_index)
            at += 1
    if not touched:
        raise ValueError(f"DM_FSLOG contains no reads intersecting a Trinity pack: {path}")
    return touched


def archive_and_records(args):
    archive = args.archive.resolve()
    descriptor = (args.descriptor or archive.with_suffix(".trpfd")).resolve()
    if not archive.is_file() or not descriptor.is_file():
        raise FileNotFoundError("data.trpfs and data.trpfd are both required")
    catalog = descriptor_catalog(descriptor)
    source = archive.open("rb")
    mapping = mmap.mmap(source.fileno(), 0, access=mmap.ACCESS_READ)
    packs, records = trpfs_records(mapping, catalog["names_by_hash"], catalog)
    return archive, descriptor, source, mapping, packs, records, catalog


def census(args) -> None:
    ensure_private_path(args.output)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    archive, descriptor, source, data, packs, records, catalog = archive_and_records(args)
    records = selected_records(records, args)
    work = args.output.with_suffix(args.output.suffix + ".partial-work")
    if work.exists():
        raise FileExistsError(work)
    work.mkdir()
    formats = collections.Counter()
    image_bytes = collections.Counter()
    rows, errors = [], []
    started = time.time()
    try:
        with GuestDecoder(args.host.resolve(), args.modules.resolve(), work / "decoder.log") as decoder:
            for number, record in enumerate(records, 1):
                try:
                    raw = decode_record(data, record, decoder)
                    textures = bntx_textures(raw)
                    for texture in textures:
                        formats[texture["format_name"]] += 1
                        image_bytes[texture["format_name"]] += texture["image_bytes"]
                        rows.append({**{k: record[k] for k in ("pack_index", "pack_offset", "pack_name",
                                                              "file_index", "file_hash", "compression",
                                                              "compression_level", "raw_size",
                                                              "payload_offset", "payload_capacity")}, **texture})
                except (FormatError, RuntimeError, struct.error) as exc:
                    errors.append({"record": record, "reason": str(exc)})
                if number % 100 == 0:
                    print(f"CENSUS {number}/{len(records)} files, {len(rows)} textures", flush=True)
        result = {
            "schema": 1, "mode": "read-only-census", "archive_name": archive.name,
            "archive_bytes": archive.stat().st_size, "descriptor_name": descriptor.name,
            "packs_total": len(packs), "files_selected": len(records), "texture_instances": len(rows),
            "formats": {name: {"textures": formats[name], "image_bytes": image_bytes[name]}
                        for name in sorted(formats)},
            "errors": errors, "rows": rows, "elapsed_seconds": time.time() - started,
            "source_archive_modified": False,
        }
        args.output.with_suffix(args.output.suffix + ".partial").write_text(json.dumps(result, indent=2) + "\n")
        args.output.with_suffix(args.output.suffix + ".partial").replace(args.output)
        print(json.dumps({k: v for k, v in result.items() if k not in ("rows", "errors")}, indent=2))
        print(f"CENSUS_OUTPUT {args.output}")
    finally:
        data.close()
        source.close()
        shutil.rmtree(work, ignore_errors=True)


def make_patch_bundle(args, all_selected: bool) -> None:
    ensure_private_path(args.output)
    ensure_private_path(args.cache)
    if args.output.exists() or args.output.with_name(args.output.name + ".partial").exists():
        raise FileExistsError(f"refusing to overwrite output: {args.output}")
    work = args.output.with_name(args.output.name + ".partial")
    work.mkdir(parents=True)
    (work / "patches").mkdir()
    (work / "evidence").mkdir()
    args.cache.mkdir(parents=True, exist_ok=True)
    archive, descriptor, source, data, packs, records, catalog = archive_and_records(args)
    records = selected_records(records, args)
    if not all_selected and len(records) != 1:
        raise ValueError("prove requires exactly one selected file; pass --pack-index and --file-index")
    patch_rows, converted_rows, retained_rows, skipped_rows = [], [], [], []
    helper_tmp = tempfile.TemporaryDirectory(prefix="za-astc-helper-")
    try:
        library = args.library.resolve() if args.library else Path(helper_tmp.name) / "libza_texture_tools.dylib"
        if not args.library:
            build_texture_library(library)
        scratch = Path(helper_tmp.name) / "scratch"
        scratch.mkdir()
        converter = BntxConverter(library, args.astcenc.resolve(), scratch, args.cache.resolve(),
                                  args.preset, args.jobs, args.minimum_psnr, args.maximum_rmse)
        encoder = GuestEncoder(args.host.resolve(), args.modules.resolve(), work,
                               work / "encoder.log", args.oodle_compressor, args.oodle_level)
        with GuestDecoder(args.host.resolve(), args.modules.resolve(), work / "decoder.log") as decoder:
            for number, record in enumerate(records, 1):
                tag = f"p{record['pack_index']}-f{record['file_index']}"
                try:
                    if record["compression"] != 3:
                        raise FormatError("stored/non-Oodle files cannot receive an Oodle payload in place")
                    raw = decode_record(data, record, decoder)
                    original_textures = bntx_textures(raw)
                    if not any(x["format"] in BC7_TO_ASTC for x in original_textures):
                        skipped_rows.append({"record": record, "reason": "no eligible BC7 texture"})
                        continue
                    converted_raw, converted, retained = converter.convert_blob(raw, tag)
                    retained_rows.extend({"record": record, **row} for row in retained)
                    if converted_raw == raw:
                        skipped_rows.append({"record": record, "reason": "all eligible textures retained"})
                        continue
                    compressed = encoder.encode(converted_raw, tag)
                    if len(compressed) > record["payload_capacity"]:
                        skipped_rows.append({"record": record, "reason": "recompressed payload exceeds vector capacity",
                                             "compressed_bytes": len(compressed),
                                             "capacity": record["payload_capacity"]})
                        continue
                    verified = decoder.decode(compressed, len(converted_raw))
                    if verified != converted_raw:
                        raise RuntimeError("independent guest decoder round trip differs")
                    after_textures = bntx_textures(verified)
                    converted_textures = bntx_textures(converted_raw)
                    if ([x["format"] for x in after_textures] !=
                            [x["format"] for x in converted_textures]):
                        raise FormatError("post-compression BNTX format verifier failed")
                    patch_offset = record["payload_offset"] - 4
                    capacity = record["payload_capacity"]
                    original_range = bytes(data[patch_offset:record["payload_offset"] + capacity])
                    replacement = (struct.pack("<I", len(compressed)) + compressed +
                                   bytes(capacity - len(compressed)))
                    if len(replacement) != len(original_range):
                        raise AssertionError("same-slot replacement changed range length")
                    patch_name = f"patches/{len(patch_rows):06d}.bin"
                    (work / patch_name).write_bytes(replacement)
                    patch_rows.append({
                        "offset": patch_offset, "bytes": len(replacement), "patch": patch_name,
                        "source_sha256": sha256_bytes(original_range),
                        "replacement_sha256": sha256_bytes(replacement),
                        "pack_index": record["pack_index"], "pack_offset": record["pack_offset"],
                        "pack_name": record["pack_name"], "file_index": record["file_index"],
                        "file_hash": f"{record['file_hash']:016x}", "raw_bytes": len(raw),
                        "old_compressed_bytes": capacity, "new_compressed_bytes": len(compressed),
                        "exact_guest_decoder_roundtrip": True,
                    })
                    converted_rows.extend({"record": record, **row} for row in converted)
                    if not all_selected:
                        (work / "evidence/original.raw").write_bytes(raw)
                        (work / "evidence/converted.raw").write_bytes(converted_raw)
                        (work / "evidence/recompressed.oodle").write_bytes(compressed)
                    print(f"PATCH_READY {tag}: {capacity} -> {len(compressed)} bytes, "
                          f"{len(converted)} textures", flush=True)
                except (FormatError, RuntimeError, ValueError, struct.error,
                        subprocess.CalledProcessError) as exc:
                    skipped_rows.append({"record": record, "reason": str(exc)})
                if all_selected and number % 50 == 0:
                    print(f"PREPARE {number}/{len(records)} files, {len(patch_rows)} patches", flush=True)
        # Patch ranges must be disjoint and wholly within the immutable source.
        patch_rows.sort(key=lambda row: row["offset"])
        previous_end = 0
        for row in patch_rows:
            if row["offset"] < previous_end or row["offset"] + row["bytes"] > len(data):
                raise AssertionError("patch ranges overlap or leave the source archive")
            previous_end = row["offset"] + row["bytes"]
        manifest = {
            "schema": 1, "game": "0100F43008C44000", "archive_name": archive.name,
            "archive_bytes": archive.stat().st_size, "archive_sha256": sha256_file(archive),
            "descriptor_name": descriptor.name, "policy": (
                "BC7_UNORM/BC7_sRGB to ASTC4x4 only; preserve storage layout; retain any texture "
                "that fails geometry/quality; skip any Oodle stream larger than its original vector"),
            "patches": patch_rows, "converted_texture_instances": len(converted_rows),
            "quality_retained_texture_instances": len(retained_rows),
            "skipped_files": skipped_rows, "converted": converted_rows, "quality_retained": retained_rows,
            "source_archive_modified": False, "descriptor_modified": False,
            "neighboring_pack_bytes_modified": False, "ready_to_apply_to_verified_clone": bool(patch_rows),
            "astcenc_sha256": converter.encoder_sha256,
            "oodle_compressor": args.oodle_compressor, "oodle_level": args.oodle_level,
            "minimum_psnr_db": args.minimum_psnr, "maximum_rmse_8bit": args.maximum_rmse,
            "astcenc_preset": args.preset,
        }
        (work / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        work.replace(args.output)
        print(json.dumps({k: v for k, v in manifest.items()
                          if k not in ("patches", "converted", "quality_retained", "skipped_files")}, indent=2))
        print(f"PATCH_BUNDLE {args.output}")
    except Exception:
        (work / "FAILED").write_text("The partial directory is not an installable patch bundle.\n")
        raise
    finally:
        helper_tmp.cleanup()
        data.close()
        source.close()


def relocated_pack_bytes(data, pack: dict, replacements: list[dict]) -> bytes:
    """Copy one pack and point selected file tables at newly appended vectors."""
    if pack["pack_size"] is None or pack["descriptor_size_field"] is None:
        raise FormatError("pack has no descriptor size metadata")
    start, size = pack["pack_offset"], pack["pack_size"]
    result = bytearray(data[start:start + size])
    if len(result) != size:
        raise FormatError("source pack is truncated")
    root = fb_root(result)
    tables = fb_refs(result, root, 1)
    if len(tables) != pack["file_count"]:
        raise FormatError("relocated source pack file count differs")
    seen = set()
    for item in sorted(replacements, key=lambda row: row["record"]["file_index"]):
        record = item["record"]
        compressed = (item["compressed"] if "compressed" in item else
                      Path(item["compressed_file"]).read_bytes())
        file_index = record["file_index"]
        if file_index in seen or file_index >= len(tables):
            raise FormatError("relocated pack has a duplicate/invalid file index")
        seen.add(file_index)
        field_absolute = record["payload_field_offset"]
        if field_absolute is None:
            raise FormatError("relocated file has no payload-vector field")
        field = field_absolute - start
        if field < 0 or field + 4 > size or tables[file_index] + 4 > size:
            raise FormatError("relocated file table leaves its source pack")
        vector = align_up(len(result), 4)
        result.extend(bytes(vector - len(result)))
        relative = vector - field
        if relative <= 0 or relative > 0xFFFFFFFF:
            raise FormatError("relocated payload cannot be represented as a FlatBuffer uoffset")
        result.extend(struct.pack("<I", len(compressed)))
        result.extend(compressed)
        struct.pack_into("<I", result, field, relative)
    result.extend(bytes(align_up(len(result), 8) - len(result)))

    # Reparse from the relocated pack itself.  Dead original vectors may remain,
    # but every rewritten field must resolve wholly inside the new pack.
    root = fb_root(result)
    tables = fb_refs(result, root, 1)
    for item in replacements:
        record = item["record"]
        compressed = (item["compressed"] if "compressed" in item else
                      Path(item["compressed_file"]).read_bytes())
        payload_at, payload_size = fb_vector(result, tables[record["file_index"]], 4)
        if payload_at + payload_size > len(result) or bytes(result[payload_at:payload_at + payload_size]) != compressed:
            raise FormatError("relocated payload-vector verifier failed")
    return bytes(result)


def make_relocated_bundle(args) -> None:
    """Create replacement packs plus a virtual sequential archive mapping."""
    ensure_private_path(args.output)
    ensure_private_path(args.cache)
    work = args.output.with_name(args.output.name + ".partial")
    if args.output.exists() or work.exists():
        raise FileExistsError(f"refusing to overwrite output: {args.output}")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    work.mkdir()
    (work / "evidence").mkdir()
    (work / "streams").mkdir()
    args.cache.mkdir(parents=True, exist_ok=True)
    archive = descriptor = source = data = catalog = None
    helper_tmp = tempfile.TemporaryDirectory(prefix="za-astc-helper-")
    try:
        archive, descriptor, source, data, packs, records, catalog = archive_and_records(args)
        selected = selected_records(records, args)
        if not selected:
            raise ValueError("relocate selection contains no files")
        proof_mode = len(selected) == 1
        pack_by_index = {pack["pack_index"]: pack for pack in packs}
        library = args.library.resolve() if args.library else Path(helper_tmp.name) / "libza_texture_tools.dylib"
        if not args.library:
            build_texture_library(library)
        scratch = Path(helper_tmp.name) / "scratch"
        scratch.mkdir()
        converter = BntxConverter(library, args.astcenc.resolve(), scratch, args.cache.resolve(),
                                  args.preset, args.jobs, args.minimum_psnr, args.maximum_rmse)
        encoder = GuestEncoder(args.host.resolve(), args.modules.resolve(), work,
                               work / "encoder.log", args.oodle_compressor, args.oodle_level)
        prepared = collections.defaultdict(list)
        converted_rows, retained_rows, skipped_rows = [], [], []
        with GuestDecoder(args.host.resolve(), args.modules.resolve(), work / "decoder.log") as decoder:
            for number, record in enumerate(selected, 1):
                tag = f"p{record['pack_index']}-f{record['file_index']}"
                try:
                    if record["compression"] != 3:
                        raise FormatError("relocation currently accepts only existing Oodle records")
                    raw = decode_record(data, record, decoder)
                    original_textures = bntx_textures(raw)
                    if not any(x["format"] in BC7_TO_ASTC for x in original_textures):
                        skipped_rows.append({"record": record, "reason": "no eligible BC7 texture"})
                        continue
                    converted_raw, converted, retained = converter.convert_blob(raw, tag)
                    retained_rows.extend({"record": record, **row} for row in retained)
                    if converted_raw == raw:
                        skipped_rows.append({"record": record, "reason": "all eligible textures retained"})
                        continue
                    compressed = encoder.encode(converted_raw, tag)
                    verified = decoder.decode(compressed, len(converted_raw))
                    if verified != converted_raw:
                        raise RuntimeError("independent guest decoder round trip differs")
                    expected = [x["format"] for x in bntx_textures(converted_raw)]
                    if [x["format"] for x in bntx_textures(verified)] != expected:
                        raise FormatError("post-compression BNTX format verifier failed")
                    stream_path = work / "streams" / f"{tag}.oodle"
                    stream_path.write_bytes(compressed)
                    item = {"record": record, "compressed_file": str(stream_path)}
                    prepared[record["pack_index"]].append(item)
                    converted_rows.extend({"record": record, **row} for row in converted)
                    if proof_mode:
                        (work / "evidence/original.raw").write_bytes(raw)
                        (work / "evidence/converted.raw").write_bytes(converted_raw)
                        (work / "evidence/recompressed.oodle").write_bytes(compressed)
                    print(f"RELOCATE_READY {tag}: {record['payload_capacity']} -> "
                          f"{len(compressed)} bytes, {len(converted)} textures", flush=True)
                except (FormatError, RuntimeError, ValueError, struct.error,
                        subprocess.CalledProcessError) as exc:
                    skipped_rows.append({"record": record, "reason": str(exc)})
                if number % 50 == 0:
                    print(f"RELOCATE {number}/{len(selected)} files", flush=True)

        archive_edits, descriptor_edits, relocated = [], [], []
        archive_bytes = archive.stat().st_size
        if data[:8] != ONEPACK:
            raise FormatError("archive does not start with ONEPACK\\0")
        root_offset = struct.unpack_from("<Q", data, 8)[0]
        if not 16 <= root_offset < archive_bytes:
            raise FormatError("archive root offset is outside the source")
        root_bytes = archive_bytes - root_offset
        # The root FlatBuffer is small (about 211 KiB for Z-A).  Keeping it
        # bounded makes both bundle generation and device-side verification
        # fail closed if a different archive layout is supplied accidentally.
        if root_bytes > 64 << 20:
            raise FormatError("archive root tail is unexpectedly large")
        descriptor_result = bytearray(catalog["bytes"])
        append_name = "append.bin"
        append_path = work / append_name
        with append_path.open("wb") as append_stream:
            for pack_index in sorted(prepared):
                pack = pack_by_index[pack_index]
                replacement_pack = relocated_pack_bytes(data, pack, prepared[pack_index])
                absolute = align_up(root_offset + append_stream.tell(), 16)
                append_stream.write(bytes(absolute - (root_offset + append_stream.tell())))
                append_relative = append_stream.tell()
                append_stream.write(replacement_pack)
                replacement_span = align_up(len(replacement_pack), 16)
                append_stream.write(bytes(replacement_span - len(replacement_pack)))
                old_offset = bytes(data[pack["pack_offset_field"]:pack["pack_offset_field"] + 8])
                if old_offset != struct.pack("<Q", pack["pack_offset"]):
                    raise FormatError("archive pack-offset slot does not match parsed offset")
                descriptor_field = pack["descriptor_size_field"]
                old_size = bytes(catalog["bytes"][descriptor_field:descriptor_field + 8])
                new_size = struct.pack("<Q", len(replacement_pack))
                if old_size != struct.pack("<Q", pack["pack_size"]):
                    raise FormatError("descriptor pack-size slot does not match parsed size")
                descriptor_result[descriptor_field:descriptor_field + 8] = new_size
                descriptor_edits.append({
                    "offset": descriptor_field, "bytes": 8,
                    "source_hex": old_size.hex(), "replacement_hex": new_size.hex(),
                    "source_sha256": sha256_bytes(old_size),
                    "replacement_sha256": sha256_bytes(new_size),
                    "descriptor_index": pack["descriptor_index"], "pack_index": pack_index,
                })
                relocated.append({
                    "pack_index": pack_index, "descriptor_index": pack["descriptor_index"],
                    "pack_name": pack["pack_name"], "source_offset": pack["pack_offset"],
                    "source_bytes": pack["pack_size"],
                    "source_sha256": sha256_bytes(data[pack["pack_offset"]:
                                                       pack["pack_offset"] + pack["pack_size"]]),
                    "appended_offset": absolute, "append_relative_offset": append_relative,
                    "appended_bytes": len(replacement_pack),
                    "appended_span_bytes": replacement_span,
                    "appended_sha256": sha256_bytes(replacement_pack),
                    "files_replaced": len(prepared[pack_index]),
                })
        shutil.rmtree(work / "streams")
        append_bytes = append_path.stat().st_size
        append_sha = sha256_file(append_path)
        physical_root_offset = root_offset + append_bytes
        physical_archive_bytes = archive_bytes + append_bytes
        virtual_offsets, redirect_rows, virtual_root_offset = virtual_pack_layout(
            packs, relocated, root_offset, physical_root_offset, root_bytes,
            physical_archive_bytes)
        offset_field = packs[0]["pack_offset_field"]
        if any(pack["pack_offset_field"] != offset_field + pack["pack_index"] * 8
               for pack in packs):
            raise FormatError("pack offset vector fields are not contiguous")
        source_vector = bytes(data[offset_field:offset_field + len(packs) * 8])
        expected_vector = struct.pack("<" + "Q" * len(packs),
                                      *(pack["pack_offset"] for pack in packs))
        if source_vector != expected_vector:
            raise FormatError("pack offset vector differs from parsed offsets")
        replacement_vector = struct.pack("<" + "Q" * len(virtual_offsets), *virtual_offsets)
        archive_edits = [{
            "source_offset": offset_field,
            "output_offset": offset_field + append_bytes,
            "bytes": len(source_vector),
            "source_hex": source_vector.hex(),
            "replacement_hex": replacement_vector.hex(),
            "source_sha256": sha256_bytes(source_vector),
            "replacement_sha256": sha256_bytes(replacement_vector),
            "pack_count": len(packs),
        }]
        old_root_pointer = struct.pack("<Q", root_offset)
        new_root_pointer = struct.pack("<Q", virtual_root_offset)
        header_edits = [{
            "source_offset": 8, "output_offset": 8, "bytes": 8,
            "source_hex": old_root_pointer.hex(),
            "replacement_hex": new_root_pointer.hex(),
            "source_sha256": sha256_bytes(old_root_pointer),
            "replacement_sha256": sha256_bytes(new_root_pointer),
        }]
        source_root = bytes(data[root_offset:archive_bytes])
        final_root = patched_bytes(source_root, root_offset, archive_edits)
        virtual_archive_bytes = virtual_root_offset + root_bytes
        redirects_name = "redirects.tsv"
        redirects_data = redirect_table_bytes(
            redirect_rows, virtual_archive_bytes, physical_archive_bytes)
        (work / redirects_name).write_bytes(redirects_data)
        descriptor_name = "data.relocated.trpfd"
        (work / descriptor_name).write_bytes(descriptor_result)
        archive_sha = sha256_file(archive)
        descriptor_sha = sha256_bytes(catalog["bytes"])
        manifest = {
            "schema": 4, "mode": "virtual-pack-layout-before-root",
            "game": "0100F43008C44000",
            "archive_name": archive.name, "archive_bytes": archive_bytes,
            "archive_sha256": archive_sha, "descriptor_name": descriptor.name,
            "descriptor_bytes": len(catalog["bytes"]), "descriptor_sha256": descriptor_sha,
            "archive_root_offset": root_offset, "archive_root_bytes": root_bytes,
            "archive_root_sha256": sha256_bytes(source_root),
            "final_archive_root_offset": physical_root_offset,
            "virtual_archive_root_offset": virtual_root_offset,
            "virtual_archive_bytes": virtual_archive_bytes,
            "final_archive_root_sha256": sha256_bytes(final_root),
            "archive_header_edits": header_edits,
            "archive_index_edits": archive_edits, "descriptor_size_edits": descriptor_edits,
            "append": {"file": append_name, "bytes": append_bytes, "sha256": append_sha,
                       "output_offset": root_offset},
            "redirects": {"file": redirects_name, "bytes": len(redirects_data),
                          "sha256": sha256_bytes(redirects_data),
                          "records": len(redirect_rows), "target": "arc/data.trpfs",
                          "virtual_file_bytes": virtual_archive_bytes,
                          "physical_file_bytes": physical_archive_bytes},
            "relocated_packs": relocated,
            "final_archive_bytes": physical_archive_bytes,
            "final_archive_sha256": sha256_inserted_archive(
                archive, root_offset, append_path, header_edits, final_root),
            "final_descriptor_bytes": len(descriptor_result),
            "final_descriptor_sha256": sha256_bytes(descriptor_result),
            "replacement_descriptor": descriptor_name,
            "converted_texture_instances": len(converted_rows),
            "quality_retained_texture_instances": len(retained_rows),
            "converted": converted_rows, "quality_retained": retained_rows,
            "skipped_files": skipped_rows,
            "policy": ("insert copied packs before the physical root; keep the parallel hash and "
                       "offset vectors in hash order; rebuild every virtual offset as the aligned "
                       "prefix sum of descriptor sizes; map arbitrary partial and cross-segment "
                       "reads to immutable source or inserted physical ranges"),
            "source_archive_modified": False, "source_descriptor_modified": False,
            "ready_to_apply_to_new_output_directory": bool(relocated),
            "astcenc_sha256": converter.encoder_sha256, "astcenc_preset": args.preset,
            "oodle_compressor": args.oodle_compressor, "oodle_level": args.oodle_level,
            "minimum_psnr_db": args.minimum_psnr, "maximum_rmse_8bit": args.maximum_rmse,
        }
        (work / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        work.replace(args.output)
        print(json.dumps({key: value for key, value in manifest.items()
                          if key not in ("converted", "quality_retained", "skipped_files",
                                         "archive_index_edits", "descriptor_size_edits")}, indent=2))
        print(f"RELOCATED_BUNDLE {args.output}")
    except Exception:
        (work / "FAILED").write_text("The partial directory is not an installable bundle.\n")
        raise
    finally:
        helper_tmp.cleanup()
        if data is not None:
            data.close()
        if source is not None:
            source.close()


def clone_or_copy(source: Path, destination: Path, allow_copy: bool) -> str:
    libc = C.CDLL(None, use_errno=True)
    clone = getattr(libc, "clonefile", None)
    if clone is not None:
        clone.argtypes = [C.c_char_p, C.c_char_p, C.c_uint32]
        clone.restype = C.c_int
        if clone(os.fsencode(source), os.fsencode(destination), 0) == 0:
            return "clonefile"
        error = C.get_errno()
        if error not in (errno.ENOTSUP, errno.EXDEV, errno.EINVAL):
            raise OSError(error, os.strerror(error), str(destination))
    if not allow_copy:
        raise OSError("APFS clonefile is unavailable; pass --allow-full-copy to permit a full copy")
    shutil.copyfile(source, destination)
    return "full-copy"


def apply_bundle(args) -> None:
    bundle, source, destination = args.bundle.resolve(), args.archive.resolve(), args.output.resolve()
    ensure_private_path(destination)
    manifest = json.loads((bundle / "manifest.json").read_text())
    if destination.exists() or destination.with_name(destination.name + ".partial").exists():
        raise FileExistsError(f"refusing to overwrite output: {destination}")
    if source.stat().st_size != manifest["archive_bytes"] or sha256_file(source) != manifest["archive_sha256"]:
        raise ValueError("source archive identity does not match patch manifest")
    patches = manifest["patches"]
    with source.open("rb") as stream:
        for row in patches:
            stream.seek(row["offset"])
            expected = stream.read(row["bytes"])
            if sha256_bytes(expected) != row["source_sha256"]:
                raise ValueError(f"source range differs at {row['offset']:#x}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    partial = destination.with_name(destination.name + ".partial")
    method = clone_or_copy(source, partial, args.allow_full_copy)
    try:
        with partial.open("r+b") as output:
            for row in patches:
                replacement = (bundle / row["patch"]).read_bytes()
                if len(replacement) != row["bytes"] or sha256_bytes(replacement) != row["replacement_sha256"]:
                    raise ValueError(f"patch payload is invalid: {row['patch']}")
                output.seek(row["offset"])
                output.write(replacement)
            output.flush()
            os.fsync(output.fileno())
        with partial.open("rb") as output:
            for row in patches:
                output.seek(row["offset"])
                if sha256_bytes(output.read(row["bytes"])) != row["replacement_sha256"]:
                    raise RuntimeError(f"written patch did not verify at {row['offset']:#x}")
        partial.replace(destination)
    except Exception:
        partial.unlink(missing_ok=True)
        raise
    result = {"output": str(destination), "bytes": destination.stat().st_size,
              "patches": len(patches), "creation_method": method,
              "source_unchanged_sha256": sha256_file(source),
              "output_sha256": sha256_file(destination)}
    print(json.dumps(result, indent=2))


def apply_relocated_bundle(args) -> None:
    """Publish a verified trpfs/trpfd pair as one atomically renamed directory."""
    bundle = args.bundle.resolve()
    source_archive = args.archive.resolve()
    source_descriptor = args.descriptor.resolve()
    output = args.output.resolve()
    ensure_private_path(output)
    manifest_bytes = (bundle / "manifest.json").read_bytes()
    manifest = json.loads(manifest_bytes)
    if (manifest.get("schema") != 4 or
            manifest.get("mode") != "virtual-pack-layout-before-root" or
            not manifest.get("ready_to_apply_to_new_output_directory")):
        raise ValueError("bundle is not a ready virtual-layout relocated manifest")
    if output.exists() or output.with_name(output.name + ".partial").exists():
        raise FileExistsError(f"refusing to overwrite output: {output}")
    if (source_archive.stat().st_size != manifest["archive_bytes"] or
            sha256_file(source_archive) != manifest["archive_sha256"]):
        raise ValueError("source archive identity does not match relocated manifest")
    source_descriptor_bytes = source_descriptor.read_bytes()
    if (len(source_descriptor_bytes) != manifest["descriptor_bytes"] or
            sha256_bytes(source_descriptor_bytes) != manifest["descriptor_sha256"]):
        raise ValueError("source descriptor identity does not match relocated manifest")
    append_path = bundle / manifest["append"]["file"]
    if (append_path.stat().st_size != manifest["append"]["bytes"] or
            sha256_file(append_path) != manifest["append"]["sha256"]):
        raise ValueError("relocated append payload identity differs")
    redirect_meta = manifest.get("redirects")
    if (not isinstance(redirect_meta, dict) or
            Path(str(redirect_meta.get("file", ""))).name != redirect_meta.get("file")):
        raise ValueError("redirect table metadata is invalid")
    redirect_path = bundle / redirect_meta["file"]
    redirect_data = redirect_path.read_bytes()
    if (len(redirect_data) != redirect_meta.get("bytes") or
            sha256_bytes(redirect_data) != redirect_meta.get("sha256")):
        raise ValueError("redirect table identity differs")
    parsed_redirects = parse_redirect_table(redirect_data)
    archive_bytes = manifest["archive_bytes"]
    append_bytes = manifest["append"]["bytes"]
    root_offset = manifest.get("archive_root_offset")
    root_bytes = manifest.get("archive_root_bytes")
    final_root_offset = manifest.get("final_archive_root_offset")
    final_archive_bytes = manifest.get("final_archive_bytes")
    virtual_root_offset = manifest.get("virtual_archive_root_offset")
    virtual_archive_bytes = manifest.get("virtual_archive_bytes")
    if (not isinstance(root_offset, int) or not isinstance(root_bytes, int) or
            not isinstance(final_root_offset, int) or not isinstance(final_archive_bytes, int) or
            not isinstance(virtual_root_offset, int) or not isinstance(virtual_archive_bytes, int) or
            root_offset < 16 or root_bytes <= 0 or root_offset + root_bytes != archive_bytes or
            final_root_offset != root_offset + append_bytes or
            final_archive_bytes != archive_bytes + append_bytes or
            virtual_root_offset + root_bytes != virtual_archive_bytes or
            parsed_redirects["target"] != redirect_meta.get("target") or
            len(parsed_redirects["records"]) != redirect_meta.get("records") or
            parsed_redirects["virtual_file_bytes"] != virtual_archive_bytes or
            parsed_redirects["physical_file_bytes"] != final_archive_bytes or
            manifest["append"].get("output_offset") != root_offset):
        raise ValueError("relocated archive layout is invalid")
    with source_archive.open("rb") as stream:
        stream.seek(root_offset)
        source_root = read_exact(stream, root_bytes)
    if sha256_bytes(source_root) != manifest.get("archive_root_sha256"):
        raise ValueError("source archive root identity differs")
    source_rows = manifest["archive_header_edits"] + manifest["archive_index_edits"]
    previous_source_end = previous_output_end = -1
    for row in sorted(source_rows, key=lambda item: item["source_offset"]):
        source_offset, output_offset, count = row["source_offset"], row["output_offset"], row["bytes"]
        if (count <= 0 or source_offset < 0 or source_offset + count > archive_bytes or
                source_offset < previous_source_end):
            raise ValueError("source archive edit ranges overlap or leave the source")
        previous_source_end = source_offset + count
        with source_archive.open("rb") as stream:
            stream.seek(source_offset); original = stream.read(count)
        if (original.hex() != row["source_hex"] or
                sha256_bytes(original) != row["source_sha256"]):
            raise ValueError(f"source archive edit range differs at {source_offset:#x}")
        replacement = bytes.fromhex(row["replacement_hex"])
        if len(replacement) != count or sha256_bytes(replacement) != row["replacement_sha256"]:
            raise ValueError(f"archive replacement differs at {output_offset:#x}")
    for row in manifest["archive_header_edits"]:
        if row["source_offset"] >= root_offset or row["output_offset"] != row["source_offset"]:
            raise ValueError("archive header edit is not inside the unchanged prefix")
    for row in manifest["archive_index_edits"]:
        if (row["source_offset"] < root_offset or
                row["output_offset"] != row["source_offset"] + append_bytes):
            raise ValueError("archive index edit does not follow the shifted root")
    for row in sorted(source_rows, key=lambda item: item["output_offset"]):
        output_offset, count = row["output_offset"], row["bytes"]
        if output_offset < previous_output_end or output_offset + count > final_archive_bytes:
            raise ValueError("output archive edit ranges overlap or leave the output")
        previous_output_end = output_offset + count
    for row in manifest["descriptor_size_edits"]:
        original = source_descriptor_bytes[row["offset"]:row["offset"] + row["bytes"]]
        if (original.hex() != row["source_hex"] or
                sha256_bytes(original) != row["source_sha256"]):
            raise ValueError(f"source descriptor edit range differs at {row['offset']:#x}")

    output.parent.mkdir(parents=True, exist_ok=True)
    partial = output.with_name(output.name + ".partial")
    partial.mkdir()
    journal = partial / ".apply-journal.json"
    published = False

    def write_journal(stage: str, **extra) -> None:
        state = {
            "schema": 1, "stage": stage,
            "manifest_sha256": sha256_bytes(manifest_bytes),
            "source_archive": str(source_archive), "source_descriptor": str(source_descriptor),
            "rollback": "remove this unpublished partial directory; both sources are immutable",
            **extra,
        }
        temporary = journal.with_suffix(".json.partial")
        temporary.write_text(json.dumps(state, indent=2) + "\n")
        with temporary.open("rb") as stream:
            os.fsync(stream.fileno())
        temporary.replace(journal)
        fsync_directory(partial)

    try:
        write_journal("created")
        output_archive = partial / manifest["archive_name"]
        output_descriptor = partial / manifest["descriptor_name"]
        output_redirects = partial / redirect_meta["file"]
        creation_method = clone_or_copy(source_archive, output_archive, args.allow_full_copy)
        shutil.copyfile(source_descriptor, output_descriptor)
        shutil.copyfile(redirect_path, output_redirects)
        with output_redirects.open("rb") as stream:
            os.fsync(stream.fileno())
        write_journal("cloned", archive_creation_method=creation_method)
        with output_archive.open("r+b") as stream, source_archive.open("rb") as source_stream:
            stream.truncate(final_archive_bytes)
            source_stream.seek(root_offset)
            stream.seek(final_root_offset)
            left = root_bytes
            while left:
                block = read_exact(source_stream, min(left, 8 << 20))
                stream.write(block); left -= len(block)
            stream.seek(root_offset)
            with append_path.open("rb") as append_stream:
                shutil.copyfileobj(append_stream, stream, length=8 << 20)
            for row in manifest["archive_header_edits"] + manifest["archive_index_edits"]:
                replacement = bytes.fromhex(row["replacement_hex"])
                if len(replacement) != row["bytes"] or sha256_bytes(replacement) != row["replacement_sha256"]:
                    raise ValueError("archive replacement differs from manifest")
                stream.seek(row["output_offset"])
                stream.write(replacement)
            stream.flush()
            os.fsync(stream.fileno())
        with output_descriptor.open("r+b") as stream:
            for row in manifest["descriptor_size_edits"]:
                replacement = bytes.fromhex(row["replacement_hex"])
                if len(replacement) != row["bytes"] or sha256_bytes(replacement) != row["replacement_sha256"]:
                    raise ValueError("descriptor replacement differs from manifest")
                stream.seek(row["offset"])
                stream.write(replacement)
            stream.flush()
            os.fsync(stream.fileno())
        write_journal("written", archive_creation_method=creation_method)
        with output_archive.open("rb") as stream:
            stream.seek(root_offset)
            if sha256_bytes(read_exact(stream, append_bytes)) != manifest["append"]["sha256"]:
                raise RuntimeError("inserted append payload verification failed")
            stream.seek(final_root_offset)
            if sha256_bytes(read_exact(stream, root_bytes)) != manifest["final_archive_root_sha256"]:
                raise RuntimeError("shifted archive root verification failed")
        got_archive = sha256_file(output_archive)
        got_descriptor = sha256_file(output_descriptor)
        if (got_archive != manifest["final_archive_sha256"] or
                got_descriptor != manifest["final_descriptor_sha256"] or
                sha256_file(output_redirects) != redirect_meta["sha256"]):
            raise RuntimeError("relocated output full-file verification failed")
        marker = {
            "schema": 1, "manifest_sha256": sha256_bytes(manifest_bytes),
            "archive_sha256": got_archive, "descriptor_sha256": got_descriptor,
            "redirects_sha256": sha256_bytes(redirect_data),
            "relocated_packs": len(manifest["relocated_packs"]),
        }
        (partial / "READY.json").write_text(json.dumps(marker, indent=2) + "\n")
        with (partial / "READY.json").open("rb") as stream:
            os.fsync(stream.fileno())
        write_journal("verified", archive_creation_method=creation_method)
        fsync_directory(partial)
        partial.replace(output)
        fsync_directory(output.parent)
        published = True
    finally:
        if not published:
            shutil.rmtree(partial, ignore_errors=True)
    print(json.dumps({
        "output_directory": str(output), "archive_creation_method": creation_method,
        "archive_bytes": manifest["final_archive_bytes"],
        "archive_sha256": manifest["final_archive_sha256"],
        "descriptor_sha256": manifest["final_descriptor_sha256"],
        "redirects_sha256": redirect_meta["sha256"],
        "virtual_archive_bytes": virtual_archive_bytes,
        "relocated_packs": len(manifest["relocated_packs"]), "atomic_ready_marker": True,
        "sources_unchanged": True,
    }, indent=2))


def common_archive_args(parser) -> None:
    parser.add_argument("archive", type=Path, help="user-owned arc/data.trpfs (opened read-only)")
    parser.add_argument("--descriptor", type=Path, help="arc/data.trpfd (default: beside archive)")
    parser.add_argument("--host", type=Path, required=True, help="Mac dmhost built for Z-A")
    parser.add_argument("--modules", type=Path, required=True, help="directory containing patched main/sdk modules")
    parser.add_argument("--pack-index", type=int, action="append", help="select a pack index (repeatable)")
    parser.add_argument("--file-index", type=int, help="select this file index within selected packs")
    parser.add_argument("--limit-packs", type=int, default=0, help="process only the first N selected packs")
    parser.add_argument("--fslog", type=Path,
                        help="select packs intersected by reads in a DM_FSLOG TSV")
    parser.add_argument("--fslog-before", type=float,
                        help="with --fslog, ignore reads after this many seconds since boot")
    parser.add_argument("--census", type=Path,
                        help="select BC7 pack/file identities from a census or relocated manifest")


def conversion_args(parser) -> None:
    common_archive_args(parser)
    parser.add_argument("output", type=Path, help="new patch-bundle directory outside the repository")
    parser.add_argument("--astcenc", type=Path, required=True, help="Arm astcenc 5.x executable")
    parser.add_argument("--library", type=Path, help="prebuilt texture helper dylib (otherwise compiled with clang)")
    parser.add_argument("--cache", type=Path, required=True, help="private converted-texture cache outside the repository")
    parser.add_argument("--preset", choices=("fast", "medium", "thorough", "verythorough", "exhaustive"), default="medium")
    parser.add_argument("--jobs", type=int, default=2, help="astcenc worker threads")
    parser.add_argument("--minimum-psnr", type=float, default=40.0)
    parser.add_argument("--maximum-rmse", type=float, default=2.55)
    parser.add_argument("--oodle-compressor", type=int, default=9, help="9 = Mermaid")
    parser.add_argument("--oodle-level", type=int, default=7)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    census_parser = commands.add_parser("census", help="read-only Trinity/BNTX inventory")
    common_archive_args(census_parser)
    census_parser.add_argument("output", type=Path, help="JSON output outside the repository")
    prove_parser = commands.add_parser("prove", help="one-file end-to-end proof and exact patch")
    conversion_args(prove_parser)
    prepare_parser = commands.add_parser("prepare", help="prepare patches for all selected records")
    conversion_args(prepare_parser)
    relocate_parser = commands.add_parser(
        "relocate", help="append copied replacement packs and emit exact trpfs/trpfd index edits")
    conversion_args(relocate_parser)
    apply_parser = commands.add_parser("apply", help="apply a bundle to a verified new clone/copy")
    apply_parser.add_argument("bundle", type=Path)
    apply_parser.add_argument("archive", type=Path, help="immutable source data.trpfs")
    apply_parser.add_argument("output", type=Path, help="new cloned/patched data.trpfs")
    apply_parser.add_argument("--allow-full-copy", action="store_true",
                              help="permit a 4.75 GB copy if APFS clonefile is unavailable")
    relocated_apply = commands.add_parser(
        "apply-relocated", help="publish a relocated trpfs/trpfd pair in a new directory")
    relocated_apply.add_argument("bundle", type=Path)
    relocated_apply.add_argument("archive", type=Path, help="immutable source data.trpfs")
    relocated_apply.add_argument("descriptor", type=Path, help="immutable source data.trpfd")
    relocated_apply.add_argument("output", type=Path, help="new directory containing the verified pair")
    relocated_apply.add_argument("--allow-full-copy", action="store_true",
                                  help="permit a 4.75 GB copy if APFS clonefile is unavailable")
    args = parser.parse_args()
    if args.command == "census":
        census(args)
    elif args.command == "prove":
        make_patch_bundle(args, all_selected=False)
    elif args.command == "prepare":
        make_patch_bundle(args, all_selected=True)
    elif args.command == "relocate":
        make_relocated_bundle(args)
    elif args.command == "apply":
        apply_bundle(args)
    else:
        apply_relocated_bundle(args)


if __name__ == "__main__":
    main()
