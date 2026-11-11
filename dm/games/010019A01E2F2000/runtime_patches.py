#!/usr/bin/env python3
"""Experimental 2560x1440 internal scene allocation, not a validated launcher option.

Usage: runtime_patches.py <original main NSO> <rewritten main> <output manifest>
The game's display and UI remain at their original size; no frame-rate or scan workaround.
This experiment forces the two scene allocation descriptors, after dynamic resolution has run.
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
    stub = int(fixups.get('RUNTIME_SCENE_1440', '-1'), 0)
    text = module.text_end
    # A newly emitted stub must be within a declared executable patch area, in either file layout.
    patch_off = int(fixups['PATCH_OFF'], 0)
    if patch_off < 0:
        raise ValueError('experimental scene stub currently requires a back patch area')
    if stub < max(text, patch_off) or stub + 12 > len(patched):
        raise ValueError('missing/out-of-range rewritten scene stub')
    expected_stub = struct.pack('<III', 0xd2800000 | (2560 << 5) | 21,
                                0xf2c00000 | (1440 << 5) | 21, B(stub + 8, 0x331710))
    if patched[stub:stub + 12] != expected_stub:
        raise ValueError('mismatched rewritten scene stub')
    # Only the internal scene descriptors change. The second descriptor (1600x900 by default)
    # feeds the scene resource lookup at 134470. Do not change output framebuffer dimensions.
    writes = [('1440p', 0x33170c, bytes.fromhex('358108aa'), struct.pack('<I', B(0x33170c, stub))),
              ('1440p', 0x3317ec, bytes.fromhex('08c880d2'), struct.pack('<I', 0xd2800000 | (2560 << 5) | 8)),
              ('1440p', 0x3317f8, bytes.fromhex('8870c0f2'), struct.pack('<I', 0xf2c00000 | (1440 << 5) | 8))]
    return manifest(module, patched, fixups, BUILD, 'DM_INTERNAL_RESOLUTION', writes)


def main(argv):
    original, rewritten, out = map(Path, argv)
    result = generate(nso.load(original.read_bytes(), original.name), rewritten.read_bytes(), layout(rewritten.with_name('main.fixups.tsv')))
    out.write_text(result)
    print('Runtime internal resolution: default (no writes), experimental 1440p; gameplay validation pending')


if __name__ == '__main__':
    try:
        main(sys.argv[1:])
    except (ValueError, KeyError, OSError) as error:
        sys.exit(f'[RED] runtime patches: {error}')
