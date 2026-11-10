#!/usr/bin/env python3
"""Community ExeFS patches (60 fps, resolution and quality mods) for patch_nso.py: the files of Atmosphère's exefs_patches/<name>/<build ID>.ips layout, IPS, IPS32 and IPSwitch's
.pchtxt, from a folder the player gives (DM_EXEFS_PATCHES). Nothing is fetched, and no patch file belongs in the repository.
The parsers are ported from Ryujinx (MIT, Copyright (c) Ryujinx Team and Contributors; dm/third_party/ryujinx/LICENSE.txt): src/Ryujinx.HLE/Loaders/Mods/IPSPatcher.cs, IPSwitchPatcher.cs and
MemPatch.cs, with the same formats and meaning, except that a malformed file, an offset outside the module and two records that write different bytes over each other are errors here
(Ryujinx logs a warning and goes on, and the last record wins).
Offsets: IPS and .pchtxt offsets of an NSO count its 0x100-byte header, so a record's image offset is its offset less 0x100; an NRO's count from its start, which is the image's (its header
is part of its text: Atmosphère's ro patcher and Ryujinx's ApplyNroPatches add no header). A .pchtxt first adds its `@flag offset_shift` (module-relative addresses of an NSO take `@flag offset_shift 0x100`).
Matching: an IPS file by its name (the text before the first dot), a .pchtxt by its `@nsobid-` line; both against the module's build ID with trailing zeros removed (the header holds 32 bytes)."""
import os
from pathlib import Path

NSO_HEADER = 0x100


class Error(ValueError):
    pass


def parse_ips(data, label='ips'):
    """IPS ('PATCH' … 'EOF') or IPS32 ('IPS32' … 'EEOF') → [(offset, bytes)], RLE records expanded; bytes after the trailer are ignored"""
    if data[:5] == b'IPS32': wide, tail = True, b'EEOF'
    elif data[:5] == b'PATCH': wide, tail = False, b'EOF'
    else: raise Error(f'{label}: not an IPS file (no PATCH or IPS32 header)')
    out, o, w = [], 5, 4 if wide else 3
    def take(n):
        nonlocal o
        if o + n > len(data): raise Error(f'{label}: record cut short at byte {o:#x}')
        o += n; return data[o - n:o]
    while (field := take(len(tail))) != tail:
        off, size = int.from_bytes(field[:w], 'big'), int.from_bytes(take(2), 'big')
        if size: out.append((off, bytes(take(size))))
        else: n = int.from_bytes(take(2), 'big'); out.append((off, bytes(take(1)) * n))   # RLE: a 16-bit count and one fill byte
    return out


def _clean(line):
    """A .pchtxt line without its comment (Ryujinx stops at any '/' outside a string) and with the C escapes inside strings decoded; the quotes stay"""
    out, state, esc = [], 'code', {'a': '\a', 'b': '\b', 'f': '\f', 'n': '\n', 'r': '\r', 't': '\t', 'v': '\v', '\\': '\\'}
    for c in line:
        if state == 'code':
            if c == '/': break
            if c == '"': state = 'str'
        elif state == 'str':
            if c == '"': state = 'code'
            elif c == '\\': state = 'esc'; continue
        else: state, c = 'str', esc.get(c, '?')
        out.append(c)
    return ''.join(out).strip()


def pchtxt_bid(text):
    """The build ID of a .pchtxt's first non-blank line, `@nsobid-<build ID>` (trailing zeros removed, lower case), or None"""
    line = next((l.strip() for l in text.splitlines() if l.strip()), '')
    return line[8:].strip().rstrip('0').lower() if line.startswith('@nsobid-') else None


def parse_pchtxt(text, label='pchtxt'):
    """IPSwitch text → [(offset, bytes)] of its enabled sections, offset_shift added: `@enabled` / `@disabled` start a section, a blank line ends it, `@stop` ends the file, other `@` lines are
    ignored; a record is `<hex offset> <hex bytes>` or `<hex offset> "string"` (written with a terminating zero)"""
    out, enabled, shift = [], False, 0
    for n, raw in enumerate(text.splitlines(), 1):
        if not raw.strip(): enabled = False; continue
        line = _clean(raw)
        if not line or line.startswith('#'): continue
        if line.startswith('@stop'): break
        if line.startswith('@enabled'): enabled = True
        elif line.startswith('@disabled'): enabled = False
        elif line.startswith('@flag'):
            tok = line.split(None, 2)
            if len(tok) > 1 and tok[1] == 'offset_shift':
                try: shift = int(tok[2], 0)
                except (IndexError, ValueError): raise Error(f'{label}:{n}: bad offset_shift') from None
        elif enabled and not line.startswith('@'):
            tok = line.split(None, 1)
            try: off = int(tok[0], 16) + shift
            except ValueError: raise Error(f'{label}:{n}: bad offset {tok[0]!r}') from None
            if len(tok) < 2: raise Error(f'{label}:{n}: no bytes after the offset')
            if tok[1][0] == '"': data = tok[1].strip('"').encode('ascii', 'replace') + b'\0'
            else:
                try: data = bytes.fromhex(tok[1].replace(' ', ''))
                except ValueError: raise Error(f'{label}:{n}: bad hex bytes {tok[1]!r}') from None
            out.append((off, data))
    return out


def same_bid(a, b): return a.lower().rstrip('0') == b.lower().rstrip('0')


def find(root, build_id):
    """[(file relative to root, [(offset, bytes)])] of every .ips and .pchtxt under root, at any depth, that names this build ID (hex), in path order"""
    if not os.path.isdir(root): raise Error(f'DM_EXEFS_PATCHES is not a folder: {root}')
    found = []
    for d, dirs, files in os.walk(root):
        dirs.sort()
        for f in sorted(files):
            path = os.path.join(d, f); rel, ext = os.path.relpath(path, root), os.path.splitext(f)[1].lower()
            if ext == '.ips' and same_bid(f.split('.')[0], build_id): found.append((rel, parse_ips(Path(path).read_bytes(), rel)))
            elif ext == '.pchtxt':
                text = Path(path).read_text(encoding='utf-8-sig', errors='replace')
                if (bid := pchtxt_bid(text)) is not None and same_bid(bid, build_id): found.append((rel, parse_pchtxt(text, rel)))
    return found


def load(root, build_id, image_end, header=NSO_HEADER):
    """The patches of one module → (files [(name, records, bytes)], writes [(image offset, bytes, name)] by offset). image_end is the end of the data segment (bss is not in the file);
    header is what the offsets count before the image: NSO_HEADER for an NSO, 0 for an NRO.
    Error: a record inside the NSO header or past image_end, or two records (of one file or two) that write different bytes to the same byte; the same bytes twice are fine"""
    files, writes = [], []
    for name, recs in find(root, build_id):
        files.append((name, len(recs), sum(len(b) for _, b in recs)))
        for off, data in recs:
            if not data: continue
            if off < header: raise Error(f'{name}: record at {off:#x} is inside the NSO header (its first {header:#x} bytes)')
            if off - header + len(data) > image_end: raise Error(f'{name}: record at {off:#x} (+{len(data):#x}) ends past the module\'s data ({image_end + header:#x})')
            writes.append((off - header, data, name))
    writes.sort(key=lambda w: w[0])
    owner = {}   # image byte → (value, file): records are small next to the image
    for off, data, name in writes:
        for i, b in enumerate(data):
            if (prev := owner.get(off + i)) and prev[0] != b: raise Error(f'image offset {off + i:#x}: {prev[1]} and {name} write different bytes')
            owner[off + i] = (b, name)
    return files, writes


def overlap(writes, owned):
    """Error when a write overlaps a range another rewrite owns: owned = [(image offset, length, what)]"""
    for off, data, name in writes:
        for at, n, what in owned:
            if off < at + n and at < off + len(data): raise Error(f'{name}: record at image offset {off:#x} (+{len(data):#x}) overlaps {what} at {at:#x}')
