"""Read RTCore module metadata without loading RTCore or modifying GPU code.

The outer record contains fourteen little-endian u32 words. The inner record
uses the RTCore serializer's aligned, length-prefixed values. Unknown numeric
fields and record bytes are retained; unsupported executable variants fail in
``make_link_plan``. A plan is an ID/region map, not an executable linked image.
In particular, this module does not implement native instruction relocations.
"""
from __future__ import annotations

from dataclasses import dataclass
from enum import IntEnum
import struct
from typing import Sequence


class InvalidModule(ValueError):
    """Malformed metadata, or an unsupported required linking operation."""


class RelocationKind(IntEnum):
    LOGICAL_ID_MOV = -56


@dataclass(frozen=True)
class Region:
    offset: int
    size: int
    name: str
    selector: int


@dataclass(frozen=True)
class SerializedRegion:
    name: str
    fields: tuple[int, ...]
    wide_fields: tuple[int, ...]
    raw: bytes


@dataclass(frozen=True)
class Entry:
    name: str
    fields: tuple[int, ...]
    flags: bytes
    regions: tuple[SerializedRegion, ...]
    fields_offset: int
    raw: bytes

    @property
    def kind(self) -> int:
        return self.fields[8]

    @property
    def native_stack_bytes(self) -> int:
        """The public owned-module query's entry+0x1c value."""
        return self.fields[1]

    @property
    def continuation_frame_bytes(self) -> int:
        """The public owned-module query's entry+0x20 value."""
        return self.fields[2]


@dataclass(frozen=True)
class Resource:
    name: str
    kind: int
    size: int
    alignment: int
    offset: int
    data: bytes
    fields: tuple[int, int, int]
    raw: bytes


@dataclass(frozen=True)
class Relocation:
    patch_offset: int
    addend: int
    template: int
    kind: int
    region_base: int
    target: int
    tail: int

    @property
    def instruction_offset(self) -> int:
        return self.region_base + self.patch_offset


@dataclass(frozen=True)
class Module:
    header: tuple[int, ...]
    fingerprint: bytes
    regions: tuple[Region, ...]
    entry_region_counts: tuple[int, ...]
    entries: tuple[Entry, ...]
    resources: tuple[Resource, ...]
    relocations: tuple[Relocation, ...]
    native_module160_bytes: int
    module_fields: tuple[int, int, int]
    metadata: bytes
    code: bytes


@dataclass(frozen=True)
class LinkedRegion:
    logical_id: int
    entry_index: int
    region_index: int
    region: Region


@dataclass(frozen=True)
class PlannedRelocation:
    relocation: Relocation
    target_logical_id: int


@dataclass(frozen=True)
class LinkPlan:
    fingerprint: bytes
    regions: tuple[LinkedRegion, ...]
    relocations: tuple[PlannedRelocation, ...]


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise InvalidModule(message)


def _span(data: bytes, offset: int, size: int, label: str) -> bytes:
    _require(0 <= offset <= len(data) and 0 <= size <= len(data) - offset,
             f'{label} outside input')
    return data[offset:offset + size]


def _sum64(a: int, b: int, label: str) -> int:
    _require(a <= (1 << 64) - 1 - b, f'{label} overflows u64')
    return a + b


class _Reader:
    def __init__(self, data: bytes, start: int, size: int):
        _span(data, start, size, 'inner module')
        self.data, self.pos, self.end = data, start, start + size

    def take(self, size: int) -> bytes:
        _require(0 <= size <= self.end - self.pos, 'truncated inner record')
        result = self.data[self.pos:self.pos + size]
        self.pos += size
        return result

    def align(self, alignment: int) -> None:
        self.take((-self.pos) & (alignment - 1))

    def u8(self) -> int:
        return self.take(1)[0]

    def u32(self) -> int:
        return struct.unpack('<I', self.take(4))[0]

    def u64(self) -> int:
        return struct.unpack('<Q', self.take(8))[0]

    def words(self, count: int) -> tuple[int, ...]:
        return struct.unpack('<' + 'I' * count, self.take(count * 4))

    def count(self, minimum_size: int = 1) -> int:
        self.align(8)
        count = self.u64()
        _require(count <= (self.end - self.pos) // minimum_size,
                 'inner count exceeds remaining input')
        return count

    def vector(self, stride: int) -> bytes:
        return self.take(self.count(stride) * stride)

    def name(self) -> str:
        raw = self.vector(1)
        _require(bool(raw) and b'\0' not in raw, 'empty or embedded-NUL symbol name')
        try:
            return raw.decode('utf-8')
        except UnicodeDecodeError as exc:
            raise InvalidModule('invalid UTF-8 symbol name') from exc


def _serialized_region(r: _Reader) -> SerializedRegion:
    start = r.pos
    name = r.name()
    r.align(4)
    fields = r.words(17)
    r.align(8)
    wide = tuple(r.u64() for _ in range(8))
    for _ in range(r.count(72)):  # named resource-access records, runtime stride0x60
        r.name()
        r.align(4)
        r.take(64)
    for _ in range(r.count(16)):  # u32, padding, u64
        r.align(4)
        r.u32()
        r.align(8)
        r.u64()
    r.u8()
    r.align(4)
    r.u32()
    r.vector(8)                  # opaque key/value pairs
    r.take(2)
    return SerializedRegion(name, fields, wide, r.data[start:r.pos])


def _entry(r: _Reader) -> Entry:
    start = r.pos
    name = r.name()
    r.align(4)
    fields_offset = r.pos
    fields = r.words(9)
    flags = r.take(9)
    r.align(4)
    r.u32()
    regions = tuple(_serialized_region(r) for _ in range(r.count(80)))
    _require(fields[0] == len(regions), 'entry region count disagrees with inner records')
    r.align(4)
    r.u32()
    r.vector(12)
    r.align(4)
    r.take(6)
    r.align(4)
    r.u32()
    return Entry(name, fields, flags, regions, fields_offset, r.data[start:r.pos])


def _resource(r: _Reader) -> Resource:
    start = r.pos
    name = r.name()
    r.align(4)
    kind = r.u32()
    r.align(8)
    size, alignment, offset = r.u64(), r.u64(), r.u64()
    _require(alignment > 0 and alignment & (alignment - 1) == 0,
             'resource alignment is not a power of two')
    _require(offset % alignment == 0, 'misaligned resource offset')
    _sum64(offset, size, 'resource extent')
    data = r.vector(1)
    r.align(4)
    f0 = r.u32()
    r.align(8)
    f1, f2 = r.u64(), r.u32()
    return Resource(name, kind, size, alignment, offset, data, (f0, f1, f2),
                    r.data[start:r.pos])


def _inner(metadata: bytes, start: int, size: int):
    r = _Reader(metadata, start, size)
    fingerprint = r.take(16)
    r.vector(1)
    r.align(8)
    r.take(0xc5)                 # fixed compiler/options record, retained in metadata
    r.vector(1)                  # opaque module+0x108 string
    r.take(3)
    r.align(4)
    r.u32()
    _require(r.count(1) == 0, 'unsupported nonempty module parameter list')
    r.align(4)
    r.u32()
    r.name()                     # module+0x148 name
    r.align(8)
    fixed = r.take(0x24)
    module160 = struct.unpack_from('<I', fixed, 8)[0]
    r.align(4)
    r.u32()
    for _ in range(r.count(48)):  # runtime0x90 records
        r.align(4)
        r.take(8)
        r.vector(8)
        r.vector(4)
        r.align(4)
        r.take(9)
        r.vector(24)
        r.vector(4)
    entries = tuple(_entry(r) for _ in range(r.count(80)))
    for _ in range(r.count(20)):  # named opaque u32/u8/u32 records
        r.name()
        r.align(4)
        r.take(12)
    r.u8()
    for _ in range(2):          # two string -> u64 maps
        for _ in range(r.count(16)):
            r.name()
            r.align(8)
            r.u64()
    for _ in range(r.count(16)):  # string -> string-set map
        r.name()
        for _ in range(r.count(8)):
            r.name()
    r.align(4)
    module250, module254 = r.u32(), r.u32()
    resources = tuple(_resource(r) for _ in range(r.count(56)))
    r.u8()
    r.align(4)
    module278 = r.u32()
    r.vector(1)
    _require(r.pos == r.end, 'unconsumed inner metadata variant')
    return fingerprint, entries, resources, module160, (module250, module254, module278)


def parse_module(metadata: bytes, code: bytes, *,
                 expected_fingerprint: bytes | None = None) -> Module:
    """Validate and inspect a module; retain unknown numeric fields unchanged.

    Fingerprints are opaque16-byte cache identities, not version whitelists.
    When supplied, ``expected_fingerprint`` must match exactly. Nonnegative
    region selectors and unknown relocation kinds can be inspected here, but
    cannot be used by ``make_link_plan``.
    """
    metadata, code = bytes(metadata), bytes(code)
    _require(len(code) > 0 and len(code) % 16 == 0, 'code is empty or not 16-byte aligned')
    h = struct.unpack('<14I', _span(metadata, 0, 56, 'outer header'))
    _require(h[3] >= 56 and h[3] % 8 == 0, 'misaligned inner metadata')
    _require(h[4] > 0, 'invalid outer region count')
    _require(0 < h[11] <= h[4], 'invalid outer entry count')

    def words(offset, count, label):
        _require(offset % 4 == 0, f'misaligned {label}')
        _require(count <= (len(metadata) - min(offset, len(metadata))) // 4,
                 f'{label} count exceeds input')
        return struct.unpack('<' + 'I' * count, _span(metadata, offset, count * 4, label))

    starts, sizes = words(h[5], h[4], 'region starts'), words(h[6], h[4], 'region sizes')
    names, selectors = words(h[7], h[4], 'region names'), words(h[8], h[4], 'region selectors')
    entry_counts = words(h[12], h[11], 'entry region counts')
    _require(all(entry_counts) and sum(entry_counts) == h[4], 'entry regions do not cover region table')
    regions = []
    for offset, size, name_offset, selector in zip(starts, sizes, names, selectors):
        _require(offset % 16 == size % 16 == 0 and size > 0, 'unaligned or empty region')
        _span(code, offset, size, 'region code')
        _require(name_offset < len(metadata), 'region name outside metadata')
        end = metadata.find(b'\0', name_offset)
        _require(end >= 0 and end > name_offset, 'empty or unterminated region name')
        try:
            name = metadata[name_offset:end].decode('utf-8')
        except UnicodeDecodeError as exc:
            raise InvalidModule('invalid UTF-8 region name') from exc
        regions.append(Region(offset, size, name, selector if selector < (1 << 31) else selector - (1 << 32)))
    fingerprint, entries, resources, module160, module_fields = _inner(metadata, h[3], h[2])
    if expected_fingerprint is not None:
        _require(len(expected_fingerprint) == 16, 'expected fingerprint must contain 16 bytes')
        _require(fingerprint == bytes(expected_fingerprint), 'inner fingerprint mismatch')
    _require(len(entries) == h[11], 'inner/outer entry count mismatch')
    _require(tuple(len(e.regions) for e in entries) == entry_counts, 'inner/outer region count mismatch')
    _require(tuple(s.name for e in entries for s in e.regions) == tuple(r.name for r in regions),
             'inner/outer region names disagree')
    raw = _span(metadata, h[10], h[9] * 40, 'relocation table')
    _require(h[10] % 8 == 0, 'misaligned relocation table')
    relocs = []
    for i in range(h[9]):
        values = struct.unpack_from('<QQQiIII', raw, 40 * i)
        relocation = Relocation(*values)
        sources = [r for r in regions if r.offset == relocation.region_base]
        _require(bool(sources), 'relocation base has no region')
        _require(relocation.patch_offset % 16 == 0 and
                 any(relocation.patch_offset + 16 <= r.size for r in sources),
                 'relocation instruction outside region')
        _span(code, relocation.instruction_offset, 16, 'relocation instruction')
        if relocation.kind < 0:
            _require(relocation.target < len(regions), 'logical relocation target outside module')
        else:
            _require(relocation.target < len(code), 'code relocation target outside module')
        relocs.append(relocation)
    return Module(h, fingerprint, tuple(regions), entry_counts, entries, resources,
                  tuple(relocs), module160, module_fields, metadata, code)


def make_link_plan(module: Module, logical_ids: Sequence[int], *,
                   expected_fingerprint: bytes | None = None) -> LinkPlan:
    """Assign an immutable namespace; do not patch or claim to link GPU code.

    IDs are caller-owned positive signed16 values, contiguous within each
    entry's continuation range. Native MOV addends/highword/template mutation
    are not yet fully established, so even supported relocations remain plans.
    Pass one common expected fingerprint when combining plans from modules.
    """
    ids = tuple(logical_ids)
    _require(len(ids) == len(module.regions), 'logical ID count differs from region count')
    _require(all(type(i) is int and 0 < i < 0x8000 for i in ids), 'logical ID outside positive signed16 domain')
    _require(len(set(ids)) == len(ids), 'duplicate logical IDs')
    if expected_fingerprint is not None:
        _require(len(expected_fingerprint) == 16 and bytes(expected_fingerprint) == module.fingerprint,
                 'link fingerprint mismatch')
    _require(module.header[13] in (0, 1), 'unsupported outer metadata flag')
    # The external machine-code variant has no inline byte extent, memory kind3
    # and subtype7. The separately constructed external data variant is3/6;
    # it must not be treated as callable machine code.
    _require(module.module_fields == (0, 3, 7), 'unsupported executable module fields')
    _require(all(r.selector == -1 for r in module.regions), 'unsupported internal region selector')
    _require(all(e.kind in (0, 7, 8) for e in module.entries), 'unsupported executable entry kind')
    _require(all(r.kind == 3 for r in module.resources), 'unsupported executable resource kind')
    _require(all(len(r.data) <= r.size for r in module.resources), 'resource initializer exceeds extent')
    _require(len({r.name for r in module.resources}) == len(module.resources), 'duplicate resource symbols')
    _require(len({r.name for r in module.regions}) == len(module.regions), 'duplicate region symbols')
    ordered = sorted(module.regions, key=lambda r: r.offset)
    _require(all(a.offset + a.size <= b.offset for a, b in zip(ordered, ordered[1:])),
             'unsupported overlapping executable regions')
    linked = []
    cursor = 0
    for ei, count in enumerate(module.entry_region_counts):
        part = ids[cursor:cursor + count]
        _require(part == tuple(range(part[0], part[0] + count)), 'continuation IDs are not contiguous')
        for ri in range(cursor, cursor + count):
            linked.append(LinkedRegion(ids[ri], ei, ri, module.regions[ri]))
        cursor += count
    plans = []
    sites = set()
    for r in module.relocations:
        _require(r.kind == RelocationKind.LOGICAL_ID_MOV, f'unsupported executable relocation kind {r.kind}')
        _require(r.addend == 0 and r.tail == 0, 'unsupported logical MOV addend or trailing field')
        _require(r.template < (1 << 32) and r.template & 0xfff in (0x802, 0x882),
                 'unsupported logical MOV template')
        source = struct.unpack_from('<Q', module.code, r.instruction_offset)[0]
        _require(source == r.template, 'logical MOV template disagrees with original instruction')
        _require(r.instruction_offset not in sites, 'duplicate relocation instruction')
        sites.add(r.instruction_offset)
        plans.append(PlannedRelocation(r, ids[r.target]))
    return LinkPlan(module.fingerprint, tuple(linked), tuple(plans))
