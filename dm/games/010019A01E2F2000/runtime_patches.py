#!/usr/bin/env python3
"""Build-validated scene supersampling and experimental full-output modes up to 5120x2880.

Usage: runtime_patches.py <original main NSO> <rewritten main> <output manifest>
Legacy choices preserve their composition. Full-output choices change scene,
allocation canvases and display size together; gameplay confirmation is pending.
"""
from pathlib import Path
import importlib.util
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
    spec = importlib.util.spec_from_file_location('mp4_scan_patch', Path(__file__).with_name('patches.py'))
    assert spec is not None and spec.loader is not None
    profile = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(profile)
    writes = []
    for choice, export, w, h in CHOICES + FULL_CHOICES:
        stub = int(fixups.get('RUNTIME_' + export, '-1'), 0)
        if stub < max(text, patch_off) or stub + 12 > len(patched):
            raise ValueError(f'missing/out-of-range rewritten scene stub for {choice}')
        expected_stub = struct.pack('<III', 0xd2800000 | (w << 5) | 21, 0xf2c00000 | (h << 5) | 21, B(stub + 8, 0x331710))
        if choice.endswith('_full'):
            expected_stub = b''.join(struct.pack('<I', word) for word in profile.scene_control_words(stub, w, h))
        if stub + len(expected_stub) > len(patched) or patched[stub:stub + len(expected_stub)] != expected_stub:
            raise ValueError(f'mismatched rewritten scene stub for {choice}')
        writes += scene_writes(stub, choice)
    stub = int(fixups.get('RUNTIME_SCAN_SOURCE_V1', '-1'), 0)
    expected = b''.join(struct.pack('<I', word) for word in profile.scan_source_words(stub)) if stub >= 0 else b''
    if stub < max(text, patch_off) or stub + len(expected) > len(patched) or patched[stub:stub + len(expected)] != expected:
        raise ValueError('missing/out-of-range/mismatched rewritten scan-source stub')
    for off, original in profile.SCAN_SOURCE_ABI + profile.SCENE_CONTROL_ABI:
        before = bytes.fromhex(original)
        if module.image[off:off + len(before)] != before or patched[off:off + len(before)] != before:
            raise ValueError(f'scan-source ABI mismatch at {off:#x}')
    # Both high-resolution choices use a native-size scan source. The previously
    # confirmed 1800p writes are unchanged; default/1440p/old diagnostics stay as before.
    for choice in ('1800p', '2160p') + tuple(c for c, *_ in FULL_CHOICES):
        writes.append((choice, profile.SCAN_SOURCE_SITE, bytes.fromhex(profile.SCAN_SOURCE_ORIGINAL),
                       struct.pack('<I', B(profile.SCAN_SOURCE_SITE, stub))))
    for off, before in OUTPUT_ABI:
        if module.image[off:off + len(before)] != before or patched[off:off + len(before)] != before:
            raise ValueError(f'output ABI mismatch at {off:#x}')
    for choice, _, w, h in FULL_CHOICES:
        writes += output_writes(choice, w, h)
    return manifest(module, patched, fixups, BUILD, 'DM_INTERNAL_RESOLUTION', writes)


# 1800p is twice the 1600x900 output in each direction: the game's composite reads the scene with one bilinear tap per output pixel (pass dumps:
# sampler 288, a single-level scene), which at exactly 2:1 lands between four texels and averages them, a 2x2 box filter. At 2160p (2.4:1) the same tap
# covers 4 of about 5.8 texels at a drifting phase, so the edges keep stair steps.
CHOICES = (('1440p', 'SCENE_1440', 2560, 1440), ('2160p', 'SCENE_2160', 3840, 2160), ('1800p', 'SCENE_1800', 3200, 1800),
           ('diag1792', 'SCENE_D1792', 3200, 1792))   # patches.py SCENE_SIZES. diag1792: a diagnostic, not a mode: its half-resolution target is 1600x896,
# inside the fixed 1600x900 one, where 1800p's is 1600x904 (900 rounded up to 8); whether scanning works there tells which size the Scan Visor needs



# Full-output choices are separate from the preserved supersampling modes.
FULL_CHOICES = (('1080p_full', 'SCENE_F1080', 1920, 1080), ('1440p_full', 'SCENE_F1440', 2560, 1440),
                ('2160p_full', 'SCENE_F2160', 3840, 2160), ('2880p_full', 'SCENE_F2880', 5120, 2880), ('native_full', 'SCENE_FNATIVE', 1600, 900))

# Canvas allocation descriptors, traced into 134470/134ddc (not global floating
# point HUD coordinates). Each pair is MOVZ Xr,#1600 / MOVK Xr,#900,LSL#32.
# da3af4 has exactly two direct callers, both construct allocation descriptors;
# 80be90/98 feed the display rectangle that a1edcc allocates its final target from.
OUTPUT_PAIRS = ((0xa1cd8, 0xa1ce4, 8), (0x12e3a8, 0x12e3b4, 8), (0x12f9a0, 0x12f9ac, 8),
    (0x132458, 0x132464, 8), (0x133568, 0x133574, 8), (0x136edc, 0x136ee8, 8),
    (0x1f5990, 0x1f5994, 8), (0x3317ec, 0x3317f8, 8), (0x335d54, 0x335d5c, 8),
    (0x3e5060, 0x3e5068, 8), (0xa1e6e4, 0xa1e6f0, 19), (0xd315a4, 0xd315a8, 8),
    (0x137f4b0, 0x137f4b8, 8), (0xda3af8, 0xda3afc, 0), (0xda3fd8, 0xda3fe8, 9))
# Full/half-resolution scratch chain derives its power-of-two backing/mip count
# from these dimensions; keep the original sizing algorithm, not a size clamp.
OUTPUT_SCALARS = ((0x80be90, 0, 1600, 'w', 1), (0x80be98, 0, 900, 'h', 1),
    (0xda2928, 27, 1600, 'w', 1), (0xda2924, 25, 900, 'h', 1),
    (0xda293c, 27, 800, 'w', 2), (0xda2938, 25, 450, 'h', 2))
# Check the allocation/rectangle ABI as well as every changed instruction.
OUTPUT_ABI = tuple((site, struct.pack('<I', 0x94000000 | (((target-site)//4) & 0x3ffffff)))
    for site, target in ((0x97d0d0, 0x80be60), (0x97d180, 0x80be60), (0xa1edcc, 0x134ddc),
        (0xa1da0, 0x134470), (0x12e498, 0x134470), (0x12f9e0, 0x134370),
        (0x1324e8, 0x134470), (0x1335e8, 0x134470), (0x136f74, 0x134470),
        (0x1f599c, 0x134ddc), (0x33186c, 0x134470), (0x335d64, 0x134ddc),
        (0x3e507c, 0x134ddc), (0xa1e9d0, 0x134470), (0xd315b0, 0x134ddc),
        (0x137f530, 0x134470), (0x137c768, 0xda3af4), (0x6e6c0, 0xda3af4), (0xda2c58, 0x134470)))


def output_writes(choice, width, height):
    if (choice, width, height) not in [(c, w, h) for c, _, w, h in FULL_CHOICES]:
        raise ValueError('unsupported full-output dimensions')
    writes = []
    def add(off, old, new): writes.append((choice, off, struct.pack('<I', old), struct.pack('<I', new)))
    for a, b, r in OUTPUT_PAIRS:
        add(a, 0xd2800000 | 1600 << 5 | r, 0xd2800000 | width << 5 | r)
        add(b, 0xf2c00000 | 900 << 5 | r, 0xf2c00000 | height << 5 | r)
    for off, r, old, axis, divisor in OUTPUT_SCALARS:
        v = (width if axis == 'w' else height) // divisor
        add(off, 0x52800000 | old << 5 | r, 0x52800000 | v << 5 | r)
    return writes


def scene_writes(stub, choice='1440p'):
    """Change the final scene descriptor after dynamic resolution. The fixed
    1600x900 composition descriptor stays unchanged. generate() adds the separate
    scan-source bridge for 1800p and 2160p; default/1440p keep their existing paths.
    """
    return [(choice, 0x33170c, bytes.fromhex('358108aa'), struct.pack('<I', B(0x33170c, stub)))]


def main(argv):
    original, rewritten, out = map(Path, argv)
    result = generate(nso.load(original.read_bytes(), original.name), rewritten.read_bytes(), layout(rewritten.with_name('main.fixups.tsv')))
    out.write_text(result)
    print('Runtime resolution: default (zero writes); preserved 1440p/1800p/2160p supersampling; experimental full-output 1080p/1440p/2160p/2880p')


if __name__ == '__main__':
    try:
        main(sys.argv[1:])
    except (ValueError, KeyError, OSError) as error:
        sys.exit(f'[RED] runtime patches: {error}')
