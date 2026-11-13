# Experimental internal-scene resolution, main 1.1.0 only. Derived from this build's instructions:
# 33170c packs the final scene dimensions into x21 AFTER both dynamic-resolution branches join;
# 3317e4 passes that size in the descriptor at sp+58 to 134850. Its fallback descriptor at 3317fc
# is 1600x900. Legacy choices retain display sizing and frame step; full-output
# choices separately change verified allocation/display constants. The 1800p
# and 2160p choices opt into the native-size scan-query bridge below.
# No original instruction changes here: runtime_patches.py can opt into a newly assembled stub.
from nso_asm import B, BL, BLR, ADRP, ADDI, SUBI, STP, LDP, STRX, LDRX

build_id = '8275fbe1ec5191c279f3a943ad8524f4'
scene_site = 0x33170c
SCENE_SIZES = (('SCENE_1440', 2560, 1440), ('SCENE_2160', 3840, 2160), ('SCENE_1800', 3200, 1800), ('SCENE_D1792', 3200, 1792))   # runtime_patches.py's choices 1440p, 2160p, 1800p and diag1792
FULL_SCENE_SIZES = (('SCENE_F1080', 1920, 1080), ('SCENE_F1440', 2560, 1440), ('SCENE_F2160', 3840, 2160), ('SCENE_F2880', 5120, 2880), ('SCENE_FNATIVE', 1600, 900), ('SCENE_F1610', 3200, 2000))



def resolution_stub(c):
    c.site(scene_site, '358108aa')   # orr x21,x9,x8,lsl #32, common to adaptive and fixed-size paths
    for name, w, h in SCENE_SIZES:   # one stub per runtime choice, each a branch target of its own manifest write
        start = c.here()
        c.emit(0xd2800000 | (w << 5) | 21)   # movz x21,#w
        c.emit(0xf2c00000 | (h << 5) | 21)   # movk x21,#h,lsl #32
        c.emit(B(c.here(), scene_site + 4))
        c.runtime_exports[name] = start


# This is a borrowed query view, used only from 336a8c through 336cfc. The
# original shared-pointer owner remains in x19/sp+c0 and is released normally.
SCAN_SOURCE_SITE = 0x336a8c
SCAN_SOURCE_ORIGINAL = '881740b9'
SCAN_BOOTSTRAP = 0x138df60
SCAN_EXTENSION = b'nxnativeMp4ScanSourceV1\0'
# Check every fixed-address ABI assumption against the legal original executable.
SCAN_SOURCE_ABI = (
    (SCAN_SOURCE_SITE, SCAN_SOURCE_ORIGINAL),
    (SCAN_BOOTSTRAP, 'b02400f0115243f910821a9120021fd6'),
    (0x3369ac, '9ae22591'),  # x26 = renderer+978; [x26]+28 is the NVN command buffer
    (0x1d6664, '6a544129'),  # view target/format at +8/+c
    (0x1d6710, '89ab4229'),  # view dimensions at +14/+18
    (0x1d6780, '991f40f9'),  # pointer to NVNtexture at +38
    (0x1d678c, '97230191'),  # inline NVNtextureView at +48 (16 bytes)
    (0x336cfc, '8087422d'),  # last use of the borrowed prefix
    (0x336d00, 'fce75ef9'),  # x28 replaced; the original owner is still released
    (0x13c1b88, '35000000'),  # original engine-format table: 0x12 -> NVN D24S8
    (0x13c1b90, '34000000'),  # 0x13 -> NVN D32F
    (0x13c1bf0, '0a000000'),  # 0x1f -> NVN R32F
)


def scan_source_words(start):
    """Preserve live integer/SIMD registers across a private host call. x28 is
    the sole changed pointer; the displaced LDR still executes before rejoining.
    No platform register x18, guest TLS, SVC or new shader translation is needed.
    """
    words = []
    def emit(w):
        words.append(w)
    def here():
        return start + 4 * len(words)
    emit(SUBI(31, 31, 0x2a0))
    for n in range(0, 18, 2):
        emit(STP(n, n + 1, 31, n * 8))
    emit(STRX(30, 31, 18))
    # Q stores, unsigned immediate scaled by 16. Preserve all 128 bits, including
    # the upper halves of callee-saved v8-v15 (not preserved by the C ABI).
    for n in range(32):
        emit(0x3d800000 | ((10 + n) << 10) | (31 << 5) | n)
    string_adrp = len(words); emit(0); string_add = len(words); emit(0)
    emit(BL(here(), SCAN_BOOTSTRAP))
    no_extension = len(words); emit(0)
    emit(0xaa0003f0)  # mov x16,x0 (extension function)
    emit(LDRX(0, 26))
    emit(ADDI(0, 0, 0x28))
    emit(0xaa1c03e1)  # mov x1,x28 (engine query view)
    emit(BLR(16))
    emit(0xaa0003fc)  # mov x28,x0 (borrowed view, or original on refusal)
    restore = here()
    for n in range(32):
        emit(0x3dc00000 | ((10 + n) << 10) | (31 << 5) | n)
    emit(LDRX(30, 31, 18))
    for n in range(0, 18, 2):
        emit(LDP(n, n + 1, 31, n * 8))
    emit(ADDI(31, 31, 0x2a0))
    emit(int.from_bytes(bytes.fromhex(SCAN_SOURCE_ORIGINAL), 'little'))
    emit(B(here(), SCAN_SOURCE_SITE + 4))
    string = here()
    literal = SCAN_EXTENSION + bytes((-len(SCAN_EXTENSION)) % 4)
    for off in range(0, len(literal), 4):
        emit(int.from_bytes(literal[off:off + 4], 'little'))
    words[string_adrp] = ADRP(0, start + 4 * string_adrp, string)
    words[string_add] = ADDI(0, 0, string & 0xfff)
    # cbz x0,restore: absent extension retains the original size rejection.
    words[no_extension] = 0xb4000000 | (((restore - (start + 4 * no_extension)) // 4) << 5)
    return words


SCENE_CONTROL_EXTENSION = b'nxnativeMp4SceneSizeV1\0'
SCENE_CONTROL_ABI = ((0x33147c, 'ed330129'),)  # stp w13,w12,[sp,#8]: input H,W before native DR


def scene_control_words(start, width, height):
    """A scene allocation choice with the native controller independently selectable.
    Keep every register/flag except the displaced ORR's x21 result. The host
    receives native dimensions, selected maximum and the native input extent.
    """
    words = []
    def emit(w): words.append(w)
    def here(): return start + 4 * len(words)
    emit(0xaa088135)  # original orr x21,x9,x8,lsl#32
    emit(SUBI(31, 31, 0x2b0))
    for n in range(0, 18, 2): emit(STP(n, n + 1, 31, n * 8))
    emit(STRX(30, 31, 18)); emit(STRX(21, 31, 20))
    emit(0xd53b4211); emit(STRX(17, 31, 19))  # NZCV in spare scalar spill
    for n in range(32): emit(0x3d800000 | ((11 + n) << 10) | (31 << 5) | n)
    emit(0xd2800000 | width << 5 | 21); emit(0xf2c00000 | height << 5 | 21)
    adr = len(words); emit(0); add = len(words); emit(0)
    emit(BL(here(), SCAN_BOOTSTRAP)); missing = len(words); emit(0)
    emit(0xaa0003f0)  # mov x16,x0
    emit(LDRX(0, 31, 20)); emit(0xaa1503e1)  # native packed; selected packed
    emit(0xb9400000 | ((0x2b0 + 12) // 4) << 10 | 31 << 5 | 2)
    emit(0xb9400000 | ((0x2b0 + 8) // 4) << 10 | 31 << 5 | 3)
    emit(0xaa038042)  # orr x2,x2,x3,lsl#32 (native full-resolution input)
    emit(BLR(16)); emit(0xaa0003f5)  # mov x21,x0
    restore = here()
    for n in range(32): emit(0x3dc00000 | ((11 + n) << 10) | (31 << 5) | n)
    emit(LDRX(30, 31, 18)); emit(LDRX(17, 31, 19)); emit(0xd51b4211)
    for n in range(0, 18, 2): emit(LDP(n, n + 1, 31, n * 8))
    emit(ADDI(31, 31, 0x2b0)); emit(B(here(), scene_site + 4))
    literal = here(); data = SCENE_CONTROL_EXTENSION + bytes((-len(SCENE_CONTROL_EXTENSION)) % 4)
    for off in range(0, len(data), 4): emit(int.from_bytes(data[off:off+4], 'little'))
    words[adr] = ADRP(0, start + 4 * adr, literal); words[add] = ADDI(0, 0, literal & 0xfff)
    words[missing] = 0xb4000000 | (((restore - (start + 4 * missing)) // 4) << 5)
    return words


def scan_source_stub(c):
    resolution_stub(c)
    for off, original in SCENE_CONTROL_ABI: c.site(off, original)
    for name, w, h in FULL_SCENE_SIZES:
        start = c.here()
        for word in scene_control_words(start, w, h): c.emit(word)
        c.runtime_exports[name] = start
    for off, original in SCAN_SOURCE_ABI:
        c.site(off, original)
    start = c.here()
    for word in scan_source_words(start):
        c.emit(word)
    c.runtime_exports['SCAN_SOURCE_V1'] = start
    shadow_stub(c)
    lod_distance_stub(c)
    fog_scale_stub(c)


# Shadow resolution (runtime_patches.py shadow_writes, DM_SHADOW_RESOLUTION=2x). The frame's shadow code (main+0x137a63c) gives each shadow-casting
# light a square tile of 1 << floor(log2(its requested size)) (w28 = 1 at 0x137a6f0, lsl at 0x137a768), packs the tiles into a 2048 atlas (0x137a82c,
# 0x137a840, 0x137a850), sets each tile as the viewport (0x137ad1c, found with DM_NVN_SHADOW_TRACE) and turns pixel rects into texture coordinates with
# the reciprocal of a (2048.0, 1.0) pair loaded at 0x137abf4. This stub loads (4096.0, 1.0) instead; x8 is free there (0x137abf8 sets it again).
SHADOW_UV_SITE = 0x137abf4


def shadow_uv_words(start, size_hi16=0x4580):
    """(size, 1.0) in d0: size_hi16 is the float's upper half, 0x4580 for 4096.0, 0x4600 for 8192.0."""
    return [0xd2a00000 | size_hi16 << 5 | 8,   # movz x8,#size_hi16,lsl #16
            0xf2e7f008,   # movk x8,#0x3f80,lsl #48  (1.0f in the upper half)
            0x9e670100,   # fmov d0,x8
            B(start + 12, SHADOW_UV_SITE + 4)]


SHADOW_UV_STUBS = (('SHADOW_UV4096', 0x4580), ('SHADOW_UV8192', 0x4600))   # runtime_patches.py's shadow choices 2x and 4x


def shadow_stub(c):
    c.site(SHADOW_UV_SITE, '009942fd')   # ldr d0,[x8,#0x530]
    for name, hi in SHADOW_UV_STUBS:
        start = c.here()
        for word in shadow_uv_words(start, hi): c.emit(word)
        c.runtime_exports[name] = start


# LOD distance (runtime_patches.py lod_writes, DM_MODEL_LOD=x2 / x4). The visibility job (main+0x13a24c) scales each instance's offset from the view's camera
# by the view's factor [view+0x3c8] (loaded at 0x13a268) and compares the result with the model's level thresholds; an offset past all of them takes the
# far branch (0x13a4d4). These stubs multiply that factor by 0.5 or 0.25, so every threshold lies 2 or 4 times as far away. s2 is free there: every path
# writes it before reading it (0x13a38c, 0x13a3a8, 0x13a3c4, 0x13a440).
LOD_DISTANCE_SITE = 0x13a268
LOD_DISTANCE_STUBS = (('LOD_DISTANCE_X2', 0x1e2c1002), ('LOD_DISTANCE_X4', 0x1e2a1002))   # fmov s2,#0.5 / #0.25


def lod_distance_words(start, fmov):
    return [0xbd43c920,   # ldr s0,[x9,#0x3c8]  (the displaced load)
            fmov,
            0x1e220800,   # fmul s0,s0,s2
            B(start + 12, LOD_DISTANCE_SITE + 4)]


def lod_distance_stub(c):
    c.site(LOD_DISTANCE_SITE, '20c943bd')
    for name, fmov in LOD_DISTANCE_STUBS:
        start = c.here()
        for word in lod_distance_words(start, fmov): c.emit(word)
        c.runtime_exports[name] = start


# Volumetric fog density (runtime_patches.py fog_writes, DM_FOG_QUALITY=high). The fog effect's node (method at main+0xf54498) passes its grid scale
# [node+0x1a0] (loaded at 0xf54720) to the sizing function 0x185428: a froxel grid of round(scale * ceil(w / 16)) (aligned to 4) by
# round(scale * ceil(h / 16)) tiles and 64 * scale depth slices, and a target of 8 pixels per tile (1600x904 at 1800p with scale 1, lsl #3 at 0x185574).
# This stub doubles the scale: tiles of 8 pixels, 128 slices, the target at the scene's full size. s1 is free there: the call at 0xf5473c takes s0
# only, and the callee writes s1 before reading it (0x1854ec).
FOG_SCALE_SITE = 0xf54720
FOG_SCALE_STUBS = (('FOG_SCALE_X2', 0x1e201001),)   # fmov s1,#2.0


def fog_scale_words(start, fmov):
    return [0xbd41a280,   # ldr s0,[x20,#0x1a0]  (the displaced load)
            fmov,
            0x1e210800,   # fmul s0,s0,s1
            B(start + 12, FOG_SCALE_SITE + 4)]


def fog_scale_stub(c):
    c.site(FOG_SCALE_SITE, '80a241bd')
    for name, fmov in FOG_SCALE_STUBS:
        start = c.here()
        for word in fog_scale_words(start, fmov): c.emit(word)
        c.runtime_exports[name] = start


STUBS = {build_id: scan_source_stub}

BINDING = [
    {'what': '1800p/2160p native-size scan-query bridge (inactive by default)', 'now': 'build', 'goal': 'build', 'purpose': 'rendering',
     'why': 'the borrowed engine-view ABI and call site belong to main 1.1.0; the full original build and every ABI instruction are checked',
     'checked': 'original v1.1.0, live 3200x1800 size-guard rejection and synthetic GPU resampling/readback; 1800p scanning user-confirmed; 2160p gameplay confirmation pending'},
    {'what': 'scene stubs and experimental 1080p-to-5K full-output choices (inactive by default)', 'now': 'build', 'goal': 'unreviewed', 'purpose': 'rendering',
     'why': 'main 1.1.0 immediate scene/canvas/display dimensions require validated runtime writes; full-output gameplay and other builds remain open'},
]

# Read-only scan diagnostics, independently located through the native script bindings in the original NSO.
# Full NSO identity and the instruction at each point are checked by scan_diag.py, including in the rewritten main.
# No PC-relative instruction; x16 is dead at these entries / after calls, and on the short paths to the next call.
# label, instruction offset, original bytes, @ register (None = every call), field offset, optional scalar register.
SCAN_BUILD_ID = '8275fbe1ec5191c279f3a943ad8524f4bab087a3000000000000000000000000'
SCAN_DIAGNOSTICS = {
    SCAN_BUILD_ID: (
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

# Acquisition upstream of BeginScan. Same read-only probe ABI as SCAN_DIAGNOSTICS.
# @/^ polls are limited to one printed observation per 30 presented frames; every BeginScan entry is kept.
SCAN_ACQUISITION_DIAGNOSTICS = {
    SCAN_BUILD_ID: (
        *[p for p in SCAN_DIAGNOSTICS[SCAN_BUILD_ID] if p[0] in
          ('target-handle', 'target-resolved', 'begin-request', 'begin-target', 'progress-call')],
        ('acquire-input', 0x402924, '0a8040b9', 0, 0x80, None),
        ('acquire-context-state', 0x402934, '09d14439', 8, 0x130, None),
        ('acquire-context-pointer', 0x402938, '08c940f9', 8, 0x190, None),
        ('acquire-eligible', 0x40294c, '08b840b9', 0, 0x80, None),
        ('spatial-candidates', 0x402b50, 'e0031baa', 22, 0x80, None),
        ('project-depth', 0x40342c, '4020281e', 28, 0x18, None),
        ('project-bounds', 0x4034a0, '40212c1e', 28, 0x18, None),
        ('project-accepted', 0x4034c4, 'e93f40f9', 28, 0x18, None),
        ('raw-candidates', 0x221ed8, '97de43f9', 20, 0x218, None),
        ('filtered-candidates', 0x21ea8c, 'e93340f9', 19, 0x218, None),
        ('selected-candidate', 0x222b2c, 'ff0304d1', 0, 0x120, None),
    ),
}

# Renderer query size and CPU counter lifecycle, original v1.1.0 only.
# x16 is dead at these non-PC-relative sites, between calls/after the ring-copy loop.
SCAN_QUERY_DIAGNOSTICS = {
    SCAN_BUILD_ID: (
        *[p for p in SCAN_ACQUISITION_DIAGNOSTICS[SCAN_BUILD_ID] if p[0] in
          ('target-handle', 'begin-request', 'progress-call', 'raw-candidates', 'filtered-candidates')],
        ('counter-binding-slot', 0xd325e8, '097992b9', 8, 0x1270, None),
        ('counter-binding-resource', 0xd33144, '1f0b0031', 8, 0x0, None),
        # Before the runtime branch: probes relocate only non-PC-relative instructions.
        ('query-source-size', 0x336a6c, 'e8e35ef9', 28, 0x14, None),
        ('query-size-accepted', 0x336ab0, 'e0364bbd', 28, 0x14, None),
        ('query-copy-request', 0x336c5c, '7f0a00b9', 28, 0x14, None),
        ('counter-copy-slot0', 0xf747f0, '88be8052', 1, 0x0, None),
        ('counter-copy-slot1', 0xf748f4, '88be8052', 1, 0x0, None),
        ('counter-copy-slot2', 0xf749f8, '88be8052', 1, 0x0, None),
        ('counter-before-clear', 0xf74b68, 'e1031f2a', 0, 0x0, None),
        ('counter-after-clear', 0xf74b74, 'e81340f9', 0, 0x0, None),
        ('counter-consumed', 0x11fdd6c, '8c421491', 11, 0x4f0, None),
    ),
}

# Read-only vibration observations, independently identified in the legal v1.1.0 main.
# No guest instruction is changed in the packaged module; the optional Mac probe
# trampolines validate the full build, rewritten layout and original words first.
# label, site, original instruction, optional memory-base register, field offset.
RUMBLE_DIAGNOSTICS = {
    SCAN_BUILD_ID: (
        ('graph-handle-count', 0xaab7ac, '1f040071', None, 0),
        ('source-enable-flags', 0x2815dc, '08ad4f39', 8, 0x3e8),
        ('source-curve-left', 0x281878, 'e10313aa', 4, 0),
        ('request-left', 0xa4e00, '3f200071', 2, 0),
        ('logical-pad-map', 0xa4e08, 'fd7bbea9', 0, 0x28),
        ('effect-file-request', 0xd350b8, '3f200071', None, 0),
    ),
}
