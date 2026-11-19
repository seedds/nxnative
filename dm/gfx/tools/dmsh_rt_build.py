"""Plan one immutable NVN2 RT library from all discovered stage-8 modules.

This does not group shaders by names or change machine-code bytes. Logical MOV
relocations describe emitter interpretations of the original instructions.
The two named constant resources are runtime inputs; absent initializer bytes
are retained as absent, rather than manufactured as zero-filled GPU data.
"""
from __future__ import annotations

from collections.abc import Iterable, Mapping
from dataclasses import dataclass
import hashlib
import re
import struct
from types import MappingProxyType

from dmsh_rt import InvalidModule, Module, make_link_plan, parse_module


MANIFEST_VERSION = 1
CODE_ALIGNMENT = 256
_U64_MAX = (1 << 64) - 1
_RESOURCE_SIZES = {'coreParams': 0x140, 'descriptorBaseArray': 0x1430}


@dataclass(frozen=True)
class ResourceLayout:
    name: str
    kind: int
    size: int
    alignment: int
    offset: int
    data: bytes
    fields: tuple[int, int, int]


@dataclass(frozen=True)
class PlannedEntry:
    address: int
    id: int
    owner: int
    kind: int
    frame: int
    entry_index: int
    region_index: int

    def job_record(self) -> dict:
        return {name: getattr(self, name) for name in ('address', 'id', 'owner', 'kind', 'frame')}


@dataclass(frozen=True)
class PlannedModule:
    key: int
    address: int
    length: int
    control_sha256: str
    metadata_sha256: str
    code_sha256: str
    entries: tuple[PlannedEntry, ...]


@dataclass(frozen=True)
class PlannedMove:
    address: int
    template: int
    id: int


@dataclass(frozen=True)
class BuiltJob:
    code: bytes
    modules: tuple[PlannedModule, ...]
    moves: tuple[PlannedMove, ...]
    layout: tuple[ResourceLayout, ...]
    fingerprint: bytes
    local_memory_bytes: int

    @property
    def module_entries(self) -> Mapping[int, tuple[PlannedEntry, ...]]:
        """All module primary/continuation IDs, indexed by normalized u64 key."""
        return MappingProxyType({module.key: module.entries for module in self.modules})

    @property
    def rt(self) -> dict:
        """A fresh JSON-safe translator job; mutating it cannot alter this plan."""
        offsets = {resource.name: resource.offset for resource in self.layout}
        return {
            'core_offset': offsets['coreParams'],
            'descriptor_offset': offsets['descriptorBaseArray'],
            'modules': [{'address': module.address, 'length': module.length} for module in self.modules],
            'entries': [entry.job_record() for module in self.modules for entry in module.entries],
            'moves': [dict(address=move.address, template=move.template, id=move.id) for move in self.moves],
        }

    @property
    def manifest(self) -> dict:
        """Our versioned manifest, without interpreting RTCore's fingerprint."""
        return {
            'version': MANIFEST_VERSION,
            'fingerprint': self.fingerprint.hex(),
            'code_alignment': CODE_ALIGNMENT,
            'code_bytes': len(self.code),
            'code_sha256': _sha(self.code),
            'local_memory_bytes': self.local_memory_bytes,
            'bank_bytes': max(r.offset + r.size for r in self.layout),
            'resources': [dict(name=r.name, kind=r.kind, size=r.size, alignment=r.alignment,
                               offset=r.offset, data=r.data.hex(), fields=list(r.fields)) for r in self.layout],
            'modules': [dict(key=f'{m.key:016x}', address=m.address, length=m.length,
                             control_sha256=m.control_sha256, metadata_sha256=m.metadata_sha256,
                             code_sha256=m.code_sha256,
                             entries=[dict(entry.job_record(), entry_index=entry.entry_index,
                                           region_index=entry.region_index) for entry in m.entries])
                        for m in self.modules],
            'rt': self.rt,
        }


def _sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise InvalidModule(message)


def _key(value) -> int:
    if isinstance(value, str) and re.fullmatch(r'[0-9a-fA-F]{16}', value):
        return int(value, 16)
    if type(value) is int and 0 <= value <= _U64_MAX:
        return value
    raise InvalidModule('RT key must be a u64 or sixteen hex digits')


def _bytes(value, label: str) -> bytes:
    _require(isinstance(value, (bytes, bytearray, memoryview)), f'RT {label} must contain bytes')
    return bytes(value)


def _module(control: bytes, code: bytes) -> Module:
    _require(len(control) >= 0x68, 'truncated RT control header')
    _require(struct.unpack_from('<II', control) == (0x98761234, 0xc9), 'unsupported RT control schema')
    _require(control[0x56] == 8, 'RT control is not stage 8')
    size, offset, code_size = struct.unpack_from('<III', control, 0x5c)
    _require(size > 0 and offset >= 0x68 and offset % 8 == 0 and
             offset <= len(control) and size <= len(control) - offset, 'RT metadata extent outside control')
    _require(code_size == len(code) and struct.unpack_from('<I', control, 0x44)[0] == len(code),
             'RT control code size disagrees with code bytes')
    module = parse_module(control[offset:offset + size], code)
    _require(module.native_module160_bytes == 0 and all(e.native_stack_bytes == 0 for e in module.entries),
             'unsupported native RT stack requirement')
    _require(all(e.kind == 0 or len(e.regions) == 1 for e in module.entries),
             'unsupported non-raygen continuation regions')
    _require(all(e.continuation_frame_bytes <= 0x10000 for e in module.entries),
             'RT continuation frame exceeds 64 KiB')
    return module


def _layout(module: Module) -> tuple[ResourceLayout, ...]:
    resources = tuple(sorted((ResourceLayout(r.name, r.kind, r.size, r.alignment, r.offset, r.data, r.fields)
                              for r in module.resources), key=lambda r: (r.offset, r.name)))
    _require({r.name for r in resources} == _RESOURCE_SIZES.keys(), 'unsupported or missing RT resource symbols')
    _require(all(r.kind == 3 and r.size == _RESOURCE_SIZES[r.name] for r in resources),
             'unsupported named RT resource extent or kind')
    _require(all(a.offset + a.size <= b.offset for a, b in zip(resources, resources[1:])),
             'RT resource extents overlap')
    return resources


def make_job(records: Iterable[Mapping]) -> BuiltJob:
    """Return deterministic code, translator input, IDs and a named resource map.

    Each record supplies ``key``, ``control`` and ``code``. Identical repeated
    keys collapse; conflicting copies fail. Inputs are copied, sorted by u64
    key and concatenated at 256-byte boundaries. IDs start at one and remain
    positive signed16 values. All entries retain metadata kinds and frame sizes.
    Compatibility requires equal opaque fingerprints and equal named resource
    layouts, including opaque fields and initializer bytes. No shader tool,
    native linker, GPU execution or filesystem access is performed here.
    """
    unique = {}
    for record in records:
        _require(isinstance(record, Mapping) and all(k in record for k in ('key', 'control', 'code')),
                 'RT record needs key, control and code')
        key = _key(record['key'])
        payload = (_bytes(record['control'], 'control'), _bytes(record['code'], 'code'))
        _require(key not in unique or unique[key] == payload, f'conflicting RT records for key {key:016x}')
        unique[key] = payload
    _require(bool(unique), 'RT library has no modules')
    image = bytearray()
    modules, moves = [], []
    fingerprint, layout = b'', ()
    next_id = 1
    raygen_frame, callback_frame = -1, 0
    for key, (control, code) in sorted(unique.items()):
        module = _module(control, code)
        _require(next_id + len(module.regions) <= 0x8000, 'RT library exceeds positive signed16 logical IDs')
        if not fingerprint:
            fingerprint = module.fingerprint
        plan = make_link_plan(module, tuple(range(next_id, next_id + len(module.regions))),
                              expected_fingerprint=fingerprint)
        current_layout = _layout(module)
        if not layout:
            layout = current_layout
        _require(layout == current_layout, 'RT modules disagree on named resource layout')
        address = (len(image) + CODE_ALIGNMENT - 1) & -CODE_ALIGNMENT
        _require(address <= _U64_MAX - len(code), 'RT linked code extent overflows u64')
        image.extend(bytes(address - len(image)))
        image.extend(code)
        owner_ids = {}
        entries = []
        for linked in plan.regions:
            entry = module.entries[linked.entry_index]
            owner = owner_ids.setdefault(linked.entry_index, linked.logical_id)
            entries.append(PlannedEntry(address + linked.region.offset, linked.logical_id, owner, entry.kind,
                                        entry.continuation_frame_bytes, linked.entry_index, linked.region_index))
        for entry in module.entries:
            if entry.kind == 0:
                raygen_frame = max(raygen_frame, entry.continuation_frame_bytes)
            else:
                callback_frame = max(callback_frame, entry.continuation_frame_bytes)
        modules.append(PlannedModule(key, address, len(code), _sha(control), _sha(module.metadata),
                                     _sha(code), tuple(entries)))
        moves.extend(PlannedMove(address + m.relocation.instruction_offset, m.relocation.template,
                                 m.target_logical_id) for m in plan.relocations)
        next_id += len(module.regions)
    _require(raygen_frame >= 0, 'RT library has no ray-generation entry')
    arena = (max(32, raygen_frame + callback_frame) + 31) & -32
    _require(arena <= 0x10000, 'RT continuation arena exceeds 64 KiB')
    return BuiltJob(bytes(image), tuple(modules), tuple(sorted(moves, key=lambda m: m.address)),
                    layout, fingerprint, arena)
