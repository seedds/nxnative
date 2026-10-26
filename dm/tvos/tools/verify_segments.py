#!/usr/bin/env python3
"""Verify static guest offsets, protections and bytes in a linked tvOS app."""
import argparse
from pathlib import Path
import re
import struct


def verify(executable, generated):
    data = executable.read_bytes()
    magic, cpu, subtype, kind, count, size, flags, reserved = struct.unpack_from('<8I', data)
    assert magic == 0xFEEDFACF and cpu == 0x100000C and kind == 2
    sections, segment_ranges = {}, []
    off = 32; platform = None
    for _ in range(count):
        cmd, length = struct.unpack_from('<II', data, off)
        if cmd == 0x19:
            s = struct.unpack_from('<II16sQQQQiiII', data, off)
            name = s[2].rstrip(b'\0').decode()
            assert not (s[8] & 2 and s[8] & 4), f'RWX segment: {name}'
            if s[4]: segment_ranges.append((s[3], s[3] + s[4], name))
            for i in range(s[9]):
                sec = struct.unpack_from('<16s16sQQIIIIIIII', data, off + 72 + i * 80)
                sections[(name, sec[0].rstrip(b'\0').decode())] = (s, sec)
        elif cmd == 0x32:
            platform = struct.unpack_from('<I', data, off + 8)[0]
        off += length
    assert off == 32 + size
    assert platform == 3, f'expected native tvOS platform, got {platform}'
    for previous, current in zip(sorted(segment_ranges), sorted(segment_ranges)[1:]):
        assert previous[1] <= current[0], f'segment overlap: {previous} {current}'
    bins = {Path(p).name.removesuffix('.patched.bin'): Path(p)
            for p in re.findall(r'\.incbin "([^"]+)"', (generated / 'embed.S').read_text())}
    for mod, path in bins.items():
        hdr = dict(line[2:].split() for line in
                   (generated / 'dm' / f'{mod}.fixups.tsv').read_text().splitlines()
                   if line.startswith('# '))
        raw = path.read_bytes(); xend = (int(hdr['TEXT_END'], 16) + 0x3fff) & ~0x3fff
        patch = int(hdr['PATCH_OFF'], 16); end = (len(raw) + 0x3fff) & ~0x3fff
        base = sections[(f'__DM{mod}0', f'__dm{mod}x0')][1][2]
        for segname, section, lo, hi, expected in (
                (f'__DM{mod}0', f'__dm{mod}x0', 0, xend, 5),
                (f'__DM{mod}D', '__data', xend, patch, 3),
                (f'__DM{mod}1', f'__dm{mod}x1', patch, end, 5)):
            seg, sec = sections[(segname, section)]
            assert seg[7] == seg[8] == expected, f'{segname}: unexpected protections'
            assert sec[2] == base + lo and sec[3] == hi - lo
            assert sec[2] % 0x4000 == 0 and sec[3] % 0x4000 == 0
            expected_bytes = raw[lo:min(hi, len(raw))] + bytes(max(0, hi - len(raw)))
            assert data[sec[4]:sec[4] + sec[3]] == expected_bytes, f'{segname}: input bytes changed'
        print(f'{mod}: native tvOS RX/RW/RX, original bytes and module-relative addresses PASS')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('executable', type=Path)
    p.add_argument('generated', type=Path)
    a = p.parse_args(); verify(a.executable, a.generated)
