#!/usr/bin/env python3
"""Experimental 2560x1440 and 3840x2160 internal scene allocation, not a validated launcher option.

Usage: runtime_patches.py <original main NSO> <rewritten main> <output manifest>
The game's display and UI remain at their original size; no frame-rate or scan workaround.
This experiment forces the scene allocation descriptor, after dynamic resolution has run.
"""
from pathlib import Path
import struct
import sys

TOOLS = Path(__file__).resolve().parents[2] / 'tools'
sys.path.insert(0, str(TOOLS))
import nso
from nso_asm import B
from runtime_patch import layout, manifest

BUILD = '8275fbe1ec5191c279f3a943ad8524f4bab087a3000000000000000000000000'


def generate(module, patched, fixups):
    if module.build_id.hex() != BUILD:
        raise ValueError('Metroid Prime 4 internal resolution requires main v1.1.0 build ' + BUILD[:32])
    text = module.text_end
    # A newly emitted stub must be within a declared executable patch area, in either file layout.
    patch_off = int(fixups['PATCH_OFF'], 0)
    if patch_off < 0:
        raise ValueError('experimental scene stub currently requires a back patch area')
    writes = []
    for choice, export, w, h in CHOICES:
        stub = int(fixups.get('RUNTIME_' + export, '-1'), 0)
        if stub < max(text, patch_off) or stub + 12 > len(patched):
            raise ValueError(f'missing/out-of-range rewritten scene stub for {choice}')
        expected_stub = struct.pack('<III', 0xd2800000 | (w << 5) | 21, 0xf2c00000 | (h << 5) | 21, B(stub + 8, 0x331710))
        if patched[stub:stub + 12] != expected_stub:
            raise ValueError(f'mismatched rewritten scene stub for {choice}')
        writes += scene_writes(stub, choice)
    return manifest(module, patched, fixups, BUILD, 'DM_INTERNAL_RESOLUTION', writes)


CHOICES = (('1440p', 'SCENE_1440', 2560, 1440), ('2160p', 'SCENE_2160', 3840, 2160))   # patches.py SCENE_SIZES


def scene_writes(stub, choice='1440p'):
    """The writes of a choice (1440p, 2160p): only the scene descriptor (0x33170c), the size the game computes from dynamic
    resolution and its scale requests, which every consumer reads from the target it gets (stock runs draw scenes of
    1244x700 to 1436x808 and scan at each). The next descriptor (0x3317ec / 0x3317f8, a fixed 1600x900) stays as it is:
    its target's size is kept at +0x618 of the renderer and compared with the 1600x900 display rectangle (0x3318b8, a
    flag at +0x620 when they differ). With it raised too, the Scan Visor could not complete a scan; with the scene alone it
    does, at 1440p (README: Scanning does not complete in the 1440p test)."""
    return [(choice, 0x33170c, bytes.fromhex('358108aa'), struct.pack('<I', B(0x33170c, stub)))]


def main(argv):
    original, rewritten, out = map(Path, argv)
    result = generate(nso.load(original.read_bytes(), original.name), rewritten.read_bytes(), layout(rewritten.with_name('main.fixups.tsv')))
    out.write_text(result)
    print('Runtime internal resolution: default (no writes), 1440p (validated in play), experimental 2160p')


if __name__ == '__main__':
    try:
        main(sys.argv[1:])
    except (ValueError, KeyError, OSError) as error:
        sys.exit(f'[RED] runtime patches: {error}')
