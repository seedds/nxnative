# Experimental internal-scene resolution, main 1.1.0 only. Derived from this build's instructions:
# 33170c packs the final scene dimensions into x21 AFTER both dynamic-resolution branches join;
# 3317e4 passes that size in the descriptor at sp+58 to 134850. Its fallback descriptor at 3317fc
# is 1600x900. Retain the original display/viewport/HUD sizing, scan code and frame step.
# No original instruction changes here: runtime_patches.py can opt into a newly assembled stub.
from nso_asm import B

build_id = '8275fbe1ec5191c279f3a943ad8524f4'
scene_site = 0x33170c
SCENE_SIZES = (('SCENE_1440', 2560, 1440), ('SCENE_2160', 3840, 2160), ('SCENE_1800', 3200, 1800))   # runtime_patches.py's choices 1440p, 2160p and 1800p


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
