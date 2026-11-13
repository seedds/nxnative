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
                ('2160p_full', 'SCENE_F2160', 3840, 2160), ('2880p_full', 'SCENE_F2880', 5120, 2880), ('native_full', 'SCENE_FNATIVE', 1600, 900),
                ('1610_full', 'SCENE_F1610', 3200, 2000))   # 16:10, experimental: every canvas and display size of the full-output path at 3200x2000

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


# DM_SHADOW_RESOLUTION=2x or 4x, a second manifest (main.shadow.runtime.tsv): shadow tiles 2 or 4 times their size in a 4096 or 8192 atlas.
# (offset, original, 2x replacement, 4x replacement)
SHADOW_SITES = (
    (0x137a6f0, 0x5280003c, 0x5280005c, 0x5280009c),   # mov w28,#1 -> #2 / #4: tile size 2 or 4 << floor(log2(requested))
    (0x137a82c, 0x52810008, 0x52820008, 0x52840008),   # packer: mov w8,#0x800 -> #0x1000 / #0x2000
    (0x137a840, 0x5281000d, 0x5282000d, 0x5284000d),   # packer: mov w13,#0x800 -> #0x1000 / #0x2000
    (0x137a850, 0x712001bf, 0x714005bf, 0x714009bf),   # packer: cmp w13,#0x800 -> #0x1000 / #0x2000 (a tile larger than the atlas is skipped)
    (0xa1e8b8, 0xb21503e8, 0xb21403e8, 0xb21303e8),    # atlas created: mov x8,#0x80000000800 -> #0x100000001000 / #0x200000002000
    (0xa1e8bc, 0xd281000a, 0xd282000a, 0xd284000a),    # atlas created: mov x10,#0x800 -> #0x1000 / #0x2000
    (0xa1e8c0, 0xf2c10009, 0xf2c20009, 0xf2c40009),    # atlas created: movk x9,#0x800,lsl #32 -> #0x1000 / #0x2000
    (0xda3a94, 0xb21503e9, 0xb21403e9, 0xb21303e9),    # atlas getter: mov x9,#0x80000000800 -> #0x100000001000 / #0x200000002000
    (0xda3a98, 0xf2c1000a, 0xf2c2000a, 0xf2c4000a),    # atlas getter: movk x10,#0x800,lsl #32 -> #0x1000 / #0x2000
    (0xda3aa0, 0xd2810009, 0xd2820009, 0xd2840009),    # atlas getter: mov x9,#0x800 -> #0x1000 / #0x2000
)
SHADOW_CHOICES = (('2x', 'SHADOW_UV4096', 0x4580), ('4x', 'SHADOW_UV8192', 0x4600))   # patches.py SHADOW_UV_STUBS


def shadow_writes(stubs):
    """stubs: {choice: address of its (size, 1.0) stub}."""
    writes = []
    for k, (choice, _, _) in enumerate(SHADOW_CHOICES):
        if choice not in stubs: continue
        writes += [(choice, off, struct.pack('<I', old), struct.pack('<I', new[k])) for off, old, *new in SHADOW_SITES]
        writes.append((choice, 0x137abf4, struct.pack('<I', 0xfd429900), struct.pack('<I', B(0x137abf4, stubs[choice]))))   # ldr d0,[x8,#0x530] -> the stub
    return writes


def generate_shadow(module, patched, fixups):
    if module.build_id.hex() != BUILD:
        raise ValueError('Metroid Prime 4 shadow resolution requires main v1.1.0 build ' + BUILD[:32])
    stubs = {}
    for choice, export, hi in SHADOW_CHOICES:
        stub = int(fixups.get('RUNTIME_' + export, '-1'), 0)
        if stub < max(module.text_end, int(fixups['PATCH_OFF'], 0)) or stub + 16 > len(patched):
            raise ValueError(f'missing/out-of-range rewritten shadow stub for {choice}')
        if patched[stub:stub + 16] != struct.pack('<4I', 0xd2a00000 | hi << 5 | 8, 0xf2e7f008, 0x9e670100, B(stub + 12, 0x137abf8)):
            raise ValueError(f'mismatched rewritten shadow stub for {choice}')
        stubs[choice] = stub
    return manifest(module, patched, fixups, BUILD, 'DM_SHADOW_RESOLUTION', shadow_writes(stubs))


# DM_MODEL_LOD=high, a third manifest (main.lod.runtime.tsv): every model drawn at its most detailed level. The model item's render method (main+0x6a2f0,
# found through DM_NVN_DRAW_TRACE) reads its instance's 4-byte visibility record {from LOD, LOD, transition}: the transition byte at 0x6a670 sends a
# cross-fade between two levels to 0x6b0d8, and the LOD byte at 0x6a678 indexes the model's level table ([model+0x18] + LOD * 0x60, at 0x6aa38).
# Both reads become 0: level 0, no cross-fade. The visibility job that writes the record is unchanged.
LOD_SITES = (
    (0x6a670, 0x39400928, 0x52800008),   # ldrb w8,[x9,#2] -> mov w8,#0: no LOD cross-fade
    (0x6a678, 0x39400528, 0x52800008),   # ldrb w8,[x9,#1] -> mov w8,#0: level 0
)


LOD_DISTANCE = (('x2', 'LOD_DISTANCE_X2', 0x1e2c1002), ('x4', 'LOD_DISTANCE_X4', 0x1e2a1002))   # patches.py LOD_DISTANCE_STUBS: fmov s2,#0.5 / #0.25


def lod_writes(stubs=None):
    """high: level 0 at draw time; x2 / x4: the visibility job's distance factor halved or quartered (stubs: {choice: stub address})."""
    writes = [('high', off, struct.pack('<I', old), struct.pack('<I', new)) for off, old, new in LOD_SITES]
    for choice, _, _ in LOD_DISTANCE:
        if stubs and choice in stubs:
            writes.append((choice, 0x13a268, struct.pack('<I', 0xbd43c920), struct.pack('<I', B(0x13a268, stubs[choice]))))   # ldr s0,[x9,#0x3c8] -> the stub
    return writes


def generate_lod(module, patched, fixups):
    if module.build_id.hex() != BUILD:
        raise ValueError('Metroid Prime 4 model LOD requires main v1.1.0 build ' + BUILD[:32])
    stubs = {}
    for choice, export, fmov in LOD_DISTANCE:
        stub = int(fixups.get('RUNTIME_' + export, '-1'), 0)
        if stub < max(module.text_end, int(fixups['PATCH_OFF'], 0)) or stub + 16 > len(patched):
            raise ValueError(f'missing/out-of-range rewritten LOD distance stub for {choice}')
        if patched[stub:stub + 16] != struct.pack('<4I', 0xbd43c920, fmov, 0x1e220800, B(stub + 12, 0x13a26c)):
            raise ValueError(f'mismatched rewritten LOD distance stub for {choice}')
        stubs[choice] = stub
    return manifest(module, patched, fixups, BUILD, 'DM_MODEL_LOD', lod_writes(stubs))


# DM_FOG_QUALITY=high, a fourth manifest (main.fog.runtime.tsv): the volumetric fog's froxel grid twice as dense in each direction, 128 depth slices,
# its target at full scene resolution (patches.py FOG_SCALE_STUBS). The grid scale is the game's own parameter, so the shaders take the grid size
# from the game: nothing else changes.
FOG = (('high', 'FOG_SCALE_X2', 0x1e201001),)


def fog_writes(stubs):
    return [(choice, 0xf54720, struct.pack('<I', 0xbd41a280), struct.pack('<I', B(0xf54720, stubs[choice]))) for choice, _, _ in FOG]   # ldr s0,[x20,#0x1a0] -> stub


def generate_fog(module, patched, fixups):
    if module.build_id.hex() != BUILD:
        raise ValueError('Metroid Prime 4 fog quality requires main v1.1.0 build ' + BUILD[:32])
    stubs = {}
    for choice, export, fmov in FOG:
        stub = int(fixups.get('RUNTIME_' + export, '-1'), 0)
        if stub < max(module.text_end, int(fixups['PATCH_OFF'], 0)) or stub + 16 > len(patched):
            raise ValueError(f'missing/out-of-range rewritten fog stub for {choice}')
        if patched[stub:stub + 16] != struct.pack('<4I', 0xbd41a280, fmov, 0x1e210800, B(stub + 12, 0xf54724)):
            raise ValueError(f'mismatched rewritten fog stub for {choice}')
        stubs[choice] = stub
    return manifest(module, patched, fixups, BUILD, 'DM_FOG_QUALITY', fog_writes(stubs))


def main(argv):
    original, rewritten, out = map(Path, argv)
    module, image, fixups = nso.load(original.read_bytes(), original.name), rewritten.read_bytes(), layout(rewritten.with_name('main.fixups.tsv'))
    out.write_text(generate(module, image, fixups))
    out.with_name('main.shadow.runtime.tsv').write_text(generate_shadow(module, image, fixups))   # a second variable, loaded beside main.runtime.tsv (loader.c)
    out.with_name('main.lod.runtime.tsv').write_text(generate_lod(module, image, fixups))
    out.with_name('main.fog.runtime.tsv').write_text(generate_fog(module, image, fixups))
    print('Runtime resolution: default (zero writes); preserved 1440p/1800p/2160p supersampling; experimental full-output 1080p/1440p/2160p/2880p; shadow resolution: default, 2x, 4x; model LOD: default, x2, x4, high; fog: default, high')


if __name__ == '__main__':
    try:
        main(sys.argv[1:])
    except (ValueError, KeyError, OSError) as error:
        sys.exit(f'[RED] runtime patches: {error}')
