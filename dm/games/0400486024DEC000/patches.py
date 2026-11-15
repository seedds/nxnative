# The Legend of Zelda: Ocarina of Time (0400486024DEC000): the rewriter's patches of this game's own code. dm/tools/patch_nso.py loads every profile's patches.py;
# MATCH_FRAME_STEP as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
import struct
from nso_asm import ADRP, B

# The game's frame ratio. Once a frame the function at main+0x6e6be0 (v131072) divides its base rate (60.0, or 30.0 while bit 0 of [x21+0x24f] is set) by the int frame rate at
# [x21+0x2c8] (fdiv s8,s1,s0 at +0xe4) and hands the result to the time object (main+0x3b4e300: ratio at +0x10, 1 / ratio at +0x14, round(60 / ratio) at +0x1c), which the game
# steps by; the frame's own time never enters it, so a frame that takes two vsyncs at rate 60 plays at half speed. The stub gives the ratio the host's step instead: step × base / 60
# (the division's value when the host keeps frames 60 / rate vsyncs), and counts the vsyncs stepped. Until the host has decided a step, and at fps_target 0, the division runs.
# s0 and x16 / x17 are free there (s0 is written again on every path before it is read; the function uses neither x16 nor x17); s1 keeps the base rate.
# Lichii (a GPL emulator; only the fact was taken) found the division (its notes: main+0x6E6CC4, 60 / [x21+0x2C8])
def frame_step(ctx):
    here, emit, fs_site, fs_put, FS_STATE_OFF, base = ctx.here, ctx.emit, ctx.site, ctx.put, ctx.state, ctx.base
    site = base + 0xe4
    fs_site(site, '2818201e')   # fdiv s8,s1,s0
    start = here(); emit(ADRP(16, here(), FS_STATE_OFF))
    for w in (0xb9400211,   # ldr w17,[x16]          target fps (0 = off)
              0x340001b1,   # cbz w17,orig (+13)
              0xb9402211,   # ldr w17,[x16,#0x20]    step in vsyncs
              0x34000171,   # cbz w17,orig (+11)     not decided yet
              0x1e230220,   # ucvtf s0,w17
              0xf9401a10,   # ldr x16,[x16,#0x30]
              0x8b110210):  # add x16,x16,x17
        emit(w)
    emit(ADRP(17, here(), FS_STATE_OFF))
    for w in (0xf9001a30,   # str x16,[x17,#0x30]    vsyncs stepped
              0x1e210808,   # fmul s8,s0,s1          step × base rate
              0x52a84e10,   # mov w16,#0x42700000    60.0
              0x1e270200,   # fmov s0,w16
              0x1e201908):  # fdiv s8,s8,s0          / 60
        emit(w)
    emit(B(here(), site + 4))
    emit(0x1e201828); emit(B(here(), site + 4))   # orig: fdiv s8,s1,s0; back
    fs_put(site, [B(site, start)], 'frame ratio: the host\'s step × base rate / 60 instead of base rate / the game\'s frame rate (state page target fps; 0 = the game\'s ratio)')
    dynamic_resolution(ctx)
    screen_wide(ctx)

# The game's dynamic resolution (game__gfx__DynamicResolutionParam: LoadThreshold, ReduceCounterMax, IncreaseCounterMax and a list of levels, each a ResolutionReduceScale and a
# Priority; on v131072 levels 0..9 at 0.75..0.333 of 2048x1152, 1536x864 to 683x384). Its update (main+0x805aa0) reads the frame's GPU load (about 0.2 ms a unit) into +0x8, subtracts
# its small model of the fixed costs and divides the rest by the square of the current scale: it takes the rest of the frame's cost to grow with the pixel count, as on the Switch 2
# its constants were tuned for. The level function (main+0x8065e0) adds that back times each level's scale squared and returns the best level under the threshold (+0x10, 83 = about
# 16.4 ms). On Apple GPUs this game's cost hardly depends on the resolution (mesh, compute and the upscaler's fixed 2048x1152 dominate): 683x384 cost 14.3 ms and 1536x864 14.7 ms on
# an M4 Pro, yet at 683x384 the extrapolation put 1536x864 at about 300 units against the 83 it allows, so the controller sat at its floor for good (game note, Dynamic resolution).
# The stub replaces the level function. With DM_DYNAMIC_RESOLUTION 0 (the default; state +0x40 0) it returns 0: full resolution. With 1 it climbs on what was measured at each
# level, with no model of how the cost grows: one level down when the load (+0x8) has been over the threshold for 8 calls in a row, and that level is marked as not fitting; one level
# up when the load fits and the level above was never measured, fitted when it was last measured, or was measured more than 600 calls (about 10 s) ago; else the current level
# (+0x174). Nothing is decided or recorded until a level has held 8 calls: the load is read a few frames late, so before that it may be the previous level's. The 8 calls over in a
# row keep a single slow frame, or the first frames at a new size (its render targets), from marking a level that fits (on the full library at level 1: one burst to 90 against loads
# of 77-80 at levels 2 and 1). The game's counters still pace the steps (down after ReduceCounterMax frames, up after IncreaseCounterMax); its level count (+0x1a0) is the floor.
# The memory is in the state page: +0x200 calls, +0x204 the last call's level, +0x208 calls at it, +0x20c calls over in a row, +0x210 the load measured at each level (16 floats),
# +0x250 the call it was measured at (0 = never). Run against a model of the update (a level moves after the stub asked 5 times up or 2 times down; the load two frames late): a load
# of 70 at every level climbs from the floor to full size in 9 steps, as does Ocarina's measured 72-80 with a spike every 37 frames and a burst after each change; a load that grows
# with the pixel count settles at the highest level that fits and tries the one above once per 600 calls; a scene that gets lighter climbs again, a heavier one steps down.
# The function is a leaf: x2..x11, x16, x17 and s0..s2 are free, w0 its only result (the original also stores its predicted cost at +0x18, which only it and the update's own
# decision read). The update's fingerprint is required too: +0x8 and +0x174 are its fields
drs_controller = (   # drs_stub.s, assembled with clang; x0 the controller, x16 the state page (the ADRP before it)
    0xb9404211, 0x35000071, 0x2a1f03e0, 0xd65f03c0,                                                   # off (state +0x40 0): level 0
    0xbd400800, 0xbd401001, 0xb9417402, 0xb941a003, 0x7100005f, 0x1a9fa042, 0x528001e9, 0x6b09005f,   # load, threshold, level (0..15), count
    0x1a89d042, 0xb9420204, 0x11000484, 0xb9020204, 0xb9420605, 0xb9420a06, 0x6b0200bf, 0x1a8617e6,   # calls + 1; calls at this level
    0xb9020602, 0xb9020a06, 0x35000046, 0xb9020e1f, 0x91084207, 0x91094208, 0x710020df, 0x5400040b,   # a new level: none over; under 8 calls: stay
    0x1e212000, 0x540001ad, 0xb9420e0b, 0x1100056b, 0xb9020e0b, 0x7100217f, 0x5400032b,               # over: under 8 in a row, stay
    0xbc2258e0, 0xb8225904, 0x51000469, 0x1100044a, 0x6b09015f, 0x1a89d140, 0xd65f03c0,               # 8 in a row: record it, down one (to the floor)
    0xb9020e1f, 0xbc2258e0, 0xb8225904, 0x34000182, 0x51000449, 0xb869590a, 0x340000ea, 0x4b0a008a,   # fits: record it; at 0 stay; the level above's record
    0x7109615f, 0x54000088, 0xbc6958e2, 0x1e212040, 0x540000aa, 0x2a0903e0, 0xd65f03c0,               # never, stale or fitting: up one
    0x2a1f03e0, 0xd65f03c0, 0x2a0203e0, 0xd65f03c0)                                                   # 0; or stay
def dynamic_resolution(ctx):
    here, emit, fs_site, fs_put, FS_STATE_OFF = ctx.here, ctx.emit, ctx.site, ctx.put, ctx.state
    site = ctx.others.get('dynamic resolution')
    if site is None or ctx.others.get('dynamic resolution update') is None: return   # not both found in this build (a red line from the locator): the game's own dynamic resolution
    fs_site(site, '017c40bd')   # ldr s1,[x0,#0x7c]
    start = here(); emit(ADRP(16, here(), FS_STATE_OFF))
    for w in drs_controller: emit(w)
    fs_put(site, [B(site, start)], 'dynamic resolution level: 0 (full resolution), or with state +0x40 (DM_DYNAMIC_RESOLUTION) a climb on the load measured at each level')
    ctx.capabilities.add('DYNAMIC_RESOLUTION')

# A screen wider than 16:9 (DM_SCREEN_WIDE, loader.c: an ultrawide monitor's 3440x1440). The engine lays its picture out from the size of its frame buffer: given a wider one it
# makes its swap chain, its 3D targets (1920x1080 → 2580x1080) and its composite that wide, and its layouts keep their size and stay at the screen's edges and centre (hearts and
# items at the corners, menus in the middle), all with no further change. Two things hold 16:9 by themselves:
#   the sizes. nnMain's create arg (main+0x1202ee0 on v131072: mov x8,#2560; movk x8,#1440,lsl #32; stur x8,[x21,#0x14]) sizes the swap chain, and the framework's constructor
#     (main+0x31f39f4: 2560x1440 at +0xb68, the docked frame buffer; main+0x31f3a00: 1920x1080 at +0xb70, the handheld one) the frame buffer the views and layouts follow.
#     With only the second widened the game composes 3440 wide and presents the left 2560 of it
#   the scene cameras' aspect. Three camera updates copy their own aspect field (+0x140, 16/9 from their parameters) into the view's sead::PerspectiveProjection (near +0x98,
#     far +0x9c, fovy +0xa0 and the half angle's sin / cos / tan, aspect +0xb0, offset +0xb4; each store sets the dirty flag at +8): main+0x4c5334, main+0x2e1bb64 and
#     main+0x2e1c7c8, all ldr s0,[x19,#0x140]. With the sizes alone the 16:9 view was stretched over the wide picture (Link's hat 1.34 times as wide as at 16:9). The other
#     writers of +0xb0 take the viewport's ratio (main+0x28e72a4, main+0x3b50988, main+0x3b50b50 under mIsCalcAspectFromViewport) or another projection's (main+0x653bd4,
#     main+0x6545f4) and follow by themselves; main+0x2dd7164 is a fixed 1280x720 or 144x144 view of its own and stays
# The size stubs give the width of the screen's ratio at the site's own height, (height x ratio + 0x8000) >> 16 with the ratio in 16.16 fixed point (state +0xa0; 1440 → 3440,
# 1080 → 2580 for 3440x1440), and the site's original mov when it is 0 (16:9). The camera stubs run the original load and replace an aspect within 0x100 ulps of 16/9 with
# the screen's ratio (state +0xa4, a float; 0 = off), so a camera with a shape of its own keeps it; they use no flags. x16 / x17 are free at every site (none of the five
# functions uses them but main+0x4c48c0, as a temporary at +0xdc..+0xe4, far before its site). The picture keeps the vertical angle and shows more at the sides.
# All five functions are needed (found by fingerprint, as the frame ratio's): with one missing nothing is patched, since sizes without the cameras stretch the picture
_wide_sizes = (   # (function, offset in it, original word, the height the site sets, what)
    ('wide: nnMain create arg', 0x200, 0xd2814008, 1440, 'swap chain size 2560x1440: the width of the screen\'s ratio'),
    ('wide: framework constructor', 0x164, 0xd2814008, 1440, 'docked frame buffer 2560x1440: the width of the screen\'s ratio'),
    ('wide: framework constructor', 0x170, 0xd280f008, 1080, 'handheld frame buffer 1920x1080: the width of the screen\'s ratio'))
_wide_cameras = (('wide: camera update A', 0xa74), ('wide: camera update B', 0x204), ('wide: camera update C', 0x428))   # ldr s0,[x19,#0x140] before the projection's aspect store
def screen_wide(ctx):
    here, emit, fs_site, fs_put, FS_STATE_OFF = ctx.here, ctx.emit, ctx.site, ctx.put, ctx.state
    fns = {label: ctx.others.get(label) for label in [l for l, *_ in _wide_sizes] + [l for l, _ in _wide_cameras]}
    if any(v is None for v in fns.values()): return   # not all found in this build (a red line from the locator): the game's own 16:9
    for label, off, orig, height, what in _wide_sizes:
        site = fns[label] + off
        fs_site(site, struct.pack('<I', orig).hex())
        start = here(); emit(ADRP(16, here(), FS_STATE_OFF))
        for w in (0xb940a210,                    # ldr w16,[x16,#0xa0]    the ratio, 16.16 (0 = 16:9)
                  0x340000d0,                    # cbz w16,orig (+6)
                  0x52800011 | height << 5,      # mov w17,#height
                  0x9bb17e10,                    # umull x16,w16,w17
                  0x91402210,                    # add x16,x16,#0x8000
                  0xd350fe08):                   # lsr x8,x16,#16         the width
            emit(w)
        emit(B(here(), site + 4))
        emit(orig); emit(B(here(), site + 4))    # orig: mov x8,#width; back
        fs_put(site, [B(site, start)], what + ' (state +0xa0, DM_SCREEN_WIDE; 0 = the game\'s)')
    for label, off in _wide_cameras:
        site = fns[label] + off
        fs_site(site, '604241bd')                # ldr s0,[x19,#0x140]
        start = here(); emit(0xbd414260)         # the original load
        emit(ADRP(16, here(), FS_STATE_OFF))
        for w in (0xb940a611,                    # ldr w17,[x16,#0xa4]    the ratio as a float (0 = off)
                  0x34000131,                    # cbz w17,back (+9)
                  0x1e260010,                    # fmov w16,s0
                  0x5291a731,                    # mov w17,#0x8d39
                  0x72a7fc71,                    # movk w17,#0x3fe3,lsl #16   16/9 (0x3fe38e39) - 0x100
                  0x4b110210,                    # sub w16,w16,w17
                  0x53097e10,                    # lsr w16,w16,#9
                  0x35000070):                   # cbnz w16,back (+3)     not within 0x100 of 16/9: the camera's own aspect
            emit(w)
        emit(ADRP(16, here(), FS_STATE_OFF))
        emit(0xbd40a600)                         # ldr s0,[x16,#0xa4]
        emit(B(here(), site + 4))
        fs_put(site, [B(site, start)], 'scene camera: a 16:9 aspect becomes the screen\'s ratio in its projection (state +0xa4, DM_SCREEN_WIDE; 0 = the game\'s)')
    ctx.capabilities.add('SCREEN_WIDE')

MATCH_FRAME_STEP = {'main': [
    {'name': 'frame ratio from the host\'s step', 'size': 0x9b4, 'fingerprints': ['6ae2aff5344d14568e4dbaf93455e2d8278384bf892ed6c6eca87fef98fbcd1d'], 'stub': frame_step,   # v131072 main+0x6e6be0
     'others': {'dynamic resolution': {'size': 0x294, 'fingerprints': ['7af6ceb0aedf8e0c17dc08ce17fcd6a7cba6d394a0d09e3b504c6d668dc418e7']},   # v131072 main+0x8065e0
                'dynamic resolution update': {'size': 0xb24, 'fingerprints': ['5a423697838976baf286cffa5e2ceef0d4e037abbcd9d371e90e9e702d61b3d4']},   # main+0x805aa0
                'wide: nnMain create arg': {'size': 0x570, 'fingerprints': ['f2c5558922a471ab585d374cc9293013d48da1562d43169367efdc8ea90a3fe8']},   # main+0x1202ce0
                'wide: framework constructor': {'size': 0x273c, 'fingerprints': ['2227b93a9fa9dd0fd9aeb64359649df6e2ade519a8e47fa560f840efcdb5d7e7']},   # main+0x31f3890
                'wide: camera update A': {'size': 0x1700, 'fingerprints': ['efc2b7111d6387f88960348730d936b4bec9e6d08a19f67bd33e75d752f61df8']},   # main+0x4c48c0
                'wide: camera update B': {'size': 0xa40, 'fingerprints': ['876b8ff1dc66ff0cb867028d9043fc71adbfac31acc093d040644550e37ac770']},   # main+0x2e1b960
                'wide: camera update C': {'size': 0x638, 'fingerprints': ['755b131403f6a7d8bb991716f403bb482bbcee61ec0dbf2f04421cc74a749166']}}},   # main+0x2e1c3a0
]}

BINDING = [
    {'what': 'frame ratio from the host\'s step', 'now': 'game', 'goal': 'game', 'purpose': 'frame rate',
     'why': "the game's own frame-ratio division, found by its function's fingerprint on any build that keeps the function",
     'checked': 'v131072 main: one function with this fingerprint, the site and its free registers read by disassembly (2026-11-08)'},
    {'what': 'dynamic resolution: full size, or a climb on the load measured at each level', 'now': 'game', 'goal': 'game', 'purpose': 'rendering',
     'why': "the game's own dynamic-resolution level function and update, found by their functions' fingerprints on any build that keeps both; switchable (DM_DYNAMIC_RESOLUTION)",
     'checked': 'v131072 main: one function with each fingerprint (the level function a leaf, x2..x11 / x16 / x17 / s0..s2 free), its callers, the update and the controller fields read by disassembly, the fields read in play (2026-11-08); the climb run natively against a model of the update and in play to full size (2026-11-09)'},
    {'what': 'wide screen: the swap chain and frame buffer sizes and the scene cameras\' aspect (DM_SCREEN_WIDE)', 'now': 'game', 'goal': 'game', 'purpose': 'rendering',
     'why': "the game's own size constants and camera updates, each found by its function's fingerprint on any build that keeps all five; switchable (DM_SCREEN_WIDE)",
     'checked': 'v131072 main: one function with each fingerprint, the sites and x16 / x17 read by disassembly; on an M5 Pro at 3440x1440 the title, the load menu and Goron City rendered 3440x1440 with 2580x1080 3D targets, the HUD at the screen\'s edges and the scene at 16:9\'s proportions with more at the sides (2026-11-12)'},
]
