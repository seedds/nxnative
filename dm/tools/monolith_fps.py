#!/usr/bin/env python3
"""Find Monolith's integer frame interval through reviewed engine functions.

The wait and duration getter read the same interval and context; the setter
saves the previous interval beside it. The context's period method, three
vtable slots before the wait, returns 1/60 second. Zero means two periods.
The game's simulation derives its elapsed-time step from this interval too.

Whole unwind-function fingerprints retain constants, registers, branches and
direct callees (nso_match). Addresses come from relative relocations, never a
build or title ID. A missing or ambiguous part leaves the engine untouched.
Reviewed: Xenoblade Chronicles X DE 1.0.0/1.0.1, Xenoblade Chronicles 2 2.1.0,
and Xenoblade Chronicles 3 2.2.1. Their different code generation is explicit.
"""
from dataclasses import dataclass
import struct

from loader_census import adrp, got_values, ldr64
from nso_match import Image, MatchError


@dataclass(frozen=True)
class Signature:
    size: int
    fingerprint: str


@dataclass(frozen=True)
class Implementation:
    name: str
    wait: Signature
    duration: Signature
    setter: Signature
    margin_literal: tuple[int, int] | None = None


REVIEWED = (
    Implementation('Monolith X',
        Signature(292, '117445a6c8a8990b03e8a0e8af7a1e52dbee46f8efac38e7f1fb9596981ef86e'),
        Signature(152, '74a04be38c6f48c37b3ad9a1e3e341e35d0d19f0802f15a46d218517ebef526e'),
        Signature(32, '5b3276a0309456b63d7a5ebf4477d5b4249b49b769e9033d540beb29823345ce')),
    Implementation('Monolith 2',
        Signature(448, '86b7a37806e9cc4aa4840fe5517c9ee620c5becea0839ab48bbe2335bc788536'),
        Signature(96, 'e4096168156c605e698a4d5cadbb39819fcf1b6bbbc3f1621dbc4f1c25e569aa'),
        Signature(32, '87d85cb2e136e4dd79434e7199db92e401b96aacce8ed6cb5dfa5fb816962dac'),
        (0x70, 0x7c)),
    Implementation('Monolith 3',
        Signature(292, '12c6abb0e0a13b604256f1d30b2d32c3f1f54d064e2d26112cf249e363af4a5f'),
        Signature(100, 'e71a92e3001a7622d9e715d446c065457ebfd2c4b0463b1894a65274c323c2e8'),
        Signature(32, '87d85cb2e136e4dd79434e7199db92e401b96aacce8ed6cb5dfa5fb816962dac')),
)
RET, PERIOD, MARGIN = 0xd65f03c0, 0x3c888889, 0x3f7f3b64


def _matches(module, signature):
    # Size filtering avoids disassembling unrelated functions in a large main.
    out = []
    for start, end in module.functions.items():
        if end - start != signature.size: continue
        try:
            if module.fingerprint(start) == signature.fingerprint: out.append(start)
        except MatchError: pass
    return out


def _got_refs(module, start, relocations):
    """Relative-relocation targets loaded through ADRP/LDR in one function."""
    pages, refs = {}, set()
    for ins in module.md.disasm(module.image[start:module.functions[start]], start):
        word = struct.unpack('<I', ins.bytes)[0]
        page, load = adrp(word, ins.address), ldr64(word)
        target = None
        if load and load[1] in pages:
            target = relocations.get(pages[load[1]] + load[2])
        for reg in ins.regs_access()[1]:
            name = ins.reg_name(reg)
            if name.startswith(('x', 'w')) and name[1:].isdigit(): pages.pop(int(name[1:]), None)
        if page: pages[page[0]] = page[1]
        if target is not None: refs.add(target)
        if ins.mnemonic in ('bl', 'blr'):
            for reg in range(19): pages.pop(reg, None)
        elif ins.mnemonic in ('b', 'br', 'ret'):
            pages.clear()
    return refs


def _setter_fields(module, start, relocations):
    """Execute only the setter's address loads, integer load and two stores."""
    values, writes = {}, []
    end = module.functions[start]
    for at in range(start, end, 4):
        word = struct.unpack_from('<I', module.image, at)[0]
        page, load = adrp(word, at), ldr64(word)
        if page: values[page[0]] = ('page', page[1]); continue
        if load:
            base = values.get(load[1])
            if base is None or base[0] != 'page': raise MatchError('setter has no GOT address')
            target = relocations.get(base[1] + load[2])
            if target is None: raise MatchError('setter GOT slot has no relative relocation')
            values[load[0]] = ('address', target)
            continue
        if word & 0xffc00000 == 0xb9400000:  # ldr wD,[xB,#offset]
            base = values.get((word >> 5) & 31)
            if base is None or base[0] != 'address' or (word >> 10) & 0xfff:
                raise MatchError('setter does not read the interval directly')
            values[word & 31] = ('read', base[1])
            continue
        if word & 0xffc00000 == 0xb9000000:  # str wS,[xB,#offset]
            base = values.get((word >> 5) & 31)
            if base is None or base[0] != 'address' or (word >> 10) & 0xfff:
                raise MatchError('setter does not write the interval directly')
            source = ('input', 0) if word & 31 == 0 else values.get(word & 31)
            writes.append((base[1], source))
            continue
        if word == RET and at == end - 4: continue
        raise MatchError('setter contains an unreviewed operation')
    if len(writes) != 2 or writes[0][1] != ('input', 0) or writes[1][1] != ('read', writes[0][0]):
        raise MatchError('setter does not save the old interval')
    current, previous = writes[0][0], writes[1][0]
    if previous != current + 4: raise MatchError('previous interval is not beside the current one')
    return current, previous


def _literal(img, page_at, load_at):
    page = adrp(struct.unpack_from('<I', img, page_at)[0], page_at)
    word = struct.unpack_from('<I', img, load_at)[0]
    if page is None or word & 0xffc00000 != 0xbd400000 or (word >> 5) & 31 != page[0]:
        raise MatchError('float literal has no reviewed ADRP/LDR address')
    return page[1] + ((word >> 10) & 0xfff) * 4


def _period(module, start, ro_off, data_off):
    """The context method must return exactly 1/60, including literal data."""
    size = module.functions.get(start, start) - start
    if size == 16:
        a, b, c, d = struct.unpack_from('<4I', module.image, start)
        if (a & 0xffe00000 == 0x52800000 and b & 0xffe00000 == 0x72a00000
                and a & 31 == b & 31 and c == 0x1e270000 | (a & 31) << 5 and d == RET):
            return ((a >> 5) & 0xffff) | ((b >> 5) & 0xffff) << 16 == PERIOD
    if size == 12:
        a, b, c = struct.unpack_from('<3I', module.image, start)
        if b & 31 == 0 and c == RET:
            try: literal = _literal(module.image, start, start + 4)
            except MatchError: return False
            return ro_off <= literal <= data_off - 4 and struct.unpack_from('<I', module.image, literal)[0] == PERIOD
    return False


def find(img, text_end, ro_off, data_off):
    """(interval offset, evidence), or (None, why); a broken match is [RED]."""
    try: module = Image(img, text_end)
    except MatchError: return None, 'no Monolith frame interval (no complete function boundaries)'
    found, failures, relocations = [], [], None
    for impl in REVIEWED:
        waits = _matches(module, impl.wait)
        if not waits: continue
        try:
            if len(waits) != 1: raise MatchError(f'{len(waits)} matching waits')
            duration, setter = _matches(module, impl.duration), _matches(module, impl.setter)
            if len(duration) != 1 or len(setter) != 1:
                raise MatchError(f'{len(duration)} duration getters and {len(setter)} setters')
            wait, duration, setter = waits[0], duration[0], setter[0]
            if relocations is None: relocations = got_values(img)
            current, previous = _setter_fields(module, setter, relocations)
            if current & 3 or not data_off <= current <= len(img) - 8:
                raise MatchError('interval pair is outside writable module data')
            if any(struct.unpack_from('<i', img, off)[0] not in (-1, 0, 1, 2, 3, 4) for off in (current, previous)):
                raise MatchError('interval pair has unexpected initial values')
            shared = _got_refs(module, wait, relocations) & _got_refs(module, duration, relocations)
            if current not in shared or len(shared) != 2:
                raise MatchError('wait and duration getter do not share the interval and context')
            context, = shared - {current}
            if context & 7 or not data_off <= context <= len(img) - 8:
                raise MatchError('context pointer is outside writable module data')
            tables = [slot for slot, target in relocations.items() if target == wait and slot >= ro_off + 24
                      and _period(module, relocations.get(slot - 24, -1), ro_off, data_off)]
            if not tables: raise MatchError('wait has no context vtable with a 1/60 period')
            if impl.margin_literal is not None:
                at = _literal(img, wait + impl.margin_literal[0], wait + impl.margin_literal[1])
                if not ro_off <= at <= data_off - 4 or struct.unpack_from('<I', img, at)[0] != MARGIN:
                    raise MatchError('wait margin literal is not 0.997')
            found.append((current, f'{impl.name} frame interval at {current:#x}: setter {setter:#x}, duration {duration:#x}, wait {wait:#x}, context {context:#x}'))
        except (MatchError, KeyError, IndexError, struct.error) as error:
            failures.append(f'{impl.name}: {error}')
    if failures: return None, '[RED] Monolith frame interval: ' + '; '.join(failures)
    if len(found) > 1: return None, f'[RED] Monolith frame interval: {len(found)} complete implementations qualify'
    return found[0] if found else (None, 'no reviewed Monolith frame interval')


if __name__ == '__main__':
    import sys
    import nso
    for path in sys.argv[1:]:
        try: mod = nso.read(path)
        except nso.Error as error: print(f'{path}\t-\t{error}'); continue
        image = bytes(mod.image) + bytes(mod.bss)
        offset, why = find(image, mod.text_end, mod.segments[1][0], mod.segments[2][0])
        print(f'{path}\t{mod.build_id[:16].hex()}\t{why}')
