# Experimental internal-scene resolution, main 1.1.0 only. Derived from this build's instructions:
# 33170c packs the final scene dimensions into x21 AFTER both dynamic-resolution branches join;
# 3317e4 passes that size in the descriptor at sp+58 to 134850. Its fallback descriptor at 3317fc
# is 1600x900. Retain the original display/viewport/HUD sizing, scan code and frame step.
# No original instruction changes here: runtime_patches.py can opt into a newly assembled stub.
from nso_asm import B

build_id = '8275fbe1ec5191c279f3a943ad8524f4'
scene_site = 0x33170c
SCENE_SIZES = (('SCENE_1440', 2560, 1440), ('SCENE_2160', 3840, 2160), ('SCENE_1800', 3200, 1800), ('SCENE_D1792', 3200, 1792))   # runtime_patches.py's choices 1440p, 2160p, 1800p and diag1792


def resolution_stub(c):
    c.site(scene_site, '358108aa')   # orr x21,x9,x8,lsl #32, common to adaptive and fixed-size paths
    for name, w, h in SCENE_SIZES:   # one stub per runtime choice, each a branch target of its own manifest write
        start = c.here()
        c.emit(0xd2800000 | (w << 5) | 21)   # movz x21,#w
        c.emit(0xf2c00000 | (h << 5) | 21)   # movk x21,#h,lsl #32
        c.emit(B(c.here(), scene_site + 4))
        c.runtime_exports[name] = start


STUBS = {build_id: resolution_stub}

BINDING = [
    {'what': 'experimental 1440p, 2160p and 1800p scene descriptor stubs (inactive by default)', 'now': 'build', 'goal': 'unreviewed', 'purpose': 'rendering',
     'why': 'the final scene allocation descriptor and fallback are immediate code in main 1.1.0; gameplay validation and other builds remain open'},
]

# Read-only scan diagnostics, independently located through the native script bindings in the original NSO.
# Full NSO identity and the instruction at each point are checked by scan_diag.py, including in the rewritten main.
# No PC-relative instruction; x16 is dead at these entries / after calls, and on the short paths to the next call.
# label, instruction offset, original bytes, @ register (None = every call), field offset, optional scalar register.
SCAN_DIAGNOSTICS = {
    '8275fbe1ec5191c279f3a943ad8524f4bab087a3000000000000000000000000': (
        ('target-handle', 0xcb2ca4, '886641b9', 20, 0x164, None),
        ('target-resolved', 0xcb2cb8, 'f44f42a9', 0, 0x2b0, None),
        ('target-range', 0xcb2de0, '08b04579', 0, 0x2d8, None),
        ('scanning-target-state', 0x1213350, '08b042b9', 0, 0x2b0, None),
        ('scanning-script-gate', 0x1213358, '68d243b9', 19, 0x3d0, None),
        ('begin-request', 0x5d9450, 'ff4302d1', None, 0, None),
        ('begin-target', 0x5d9560, 'e10313aa', 0, 0x2d8, None),
        ('begin-range-gate', 0x5d97f0, '68b20579', 19, 0x2d8, None),
        ('progress-call', 0x699d4, 'ff0302d1', 0, 0x2b0, 0),
        ('progress-manager', 0x69a10, '0041201e', 20, 0x2b0, 8),
        ('progress-return', 0x69a64, '88724039', 20, 0x2b0, 8),
    ),
}
