# The Legend of Zelda: Ocarina of Time (0400486024DEC000): the rewriter's patches of this game's own code. dm/tools/patch_nso.py loads every profile's patches.py;
# MATCH_FRAME_STEP as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
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
    0xb9404211, 0x35000071, 'DRS_OFF_B', 0xd65f03c0,                                                    # off (state +0x40 0): level 0 at the render scale (the block at the end)
    0xbd400800, 0xbd401001, 0xb9417402, 0xb941a003, 0x7100005f, 0x1a9fa042, 0x528001e9, 0x6b09005f,   # load, threshold, level (0..15), count
    0x1a89d042, 0xb9420204, 0x11000484, 0xb9020204, 0xb9420605, 0xb9420a06, 0x6b0200bf, 0x1a8617e6,   # calls + 1; calls at this level
    0xb9020602, 0xb9020a06, 0x35000046, 0xb9020e1f, 0x91084207, 0x91094208, 0x710020df, 0x5400040b,   # a new level: none over; under 8 calls: stay
    0x1e212000, 0x540001ad, 0xb9420e0b, 0x1100056b, 0xb9020e0b, 0x7100217f, 0x5400032b,               # over: under 8 in a row, stay
    0xbc2258e0, 0xb8225904, 0x51000469, 0x1100044a, 0x6b09015f, 0x1a89d140, 0xd65f03c0,               # 8 in a row: record it, down one (to the floor)
    0xb9020e1f, 0xbc2258e0, 0xb8225904, 0x34000182, 0x51000449, 0xb869590a, 0x340000ea, 0x4b0a008a,   # fits: record it; at 0 stay; the level above's record
    0x7109615f, 0x54000088, 0xbc6958e2, 0x1e212040, 0x540000aa, 0x2a0903e0, 0xd65f03c0,               # never, stale or fitting: up one
    0x2a1f03e0, 0xd65f03c0, 0x2a0203e0, 0xd65f03c0,                                                   # 0; or stay
    # off, level 0 at the host's render scale (state +0x10, DM_RENDER_SCALE): its scale (the list's first entry, [first + -8]; head +0x198, end +0x190) = the game's own (kept at
    # state +0x2a0 on the first call) x the render scale, at most 1.0, the native size of the output. 1.0, the default, writes the game's own back
    0xf940cc03, 0x91064004, 0xeb04007f, 0x54000260, 0xbd42a201, 0x1e202028, 0x54000061, 0xbc5f8061,   # first entry (none: level 0); the game's scale, kept once
    0xbd02a201, 0xbd401200, 0x1e202008, 0x5400016d, 0x1e200821, 0x1e2e1002, 0x1e227821,               # x the render scale (0 or less: as it is), at most 1.0
    0x1e2d1003, 0x1e230803, 0x1e222060, 0x1e2f1004, 0x1e240863, 0x1e21cc61,                           # above 1.333: 1.5 x 0.75 x the render scale (the 3840x2160 output, below)
    0xbc1f8061, 0x2a1f03e0, 0xd65f03c0)                                                               # level 0
drs_off_len = 24
drs_controller = tuple(0x14000000 | (len(drs_controller) - drs_off_len - 2) if w == 'DRS_OFF_B' else w for w in drs_controller)   # b to the off block, the last 22 words
def dynamic_resolution(ctx):
    here, emit, fs_site, fs_put, FS_STATE_OFF = ctx.here, ctx.emit, ctx.site, ctx.put, ctx.state
    site = ctx.others.get('dynamic resolution')
    if site is None or ctx.others.get('dynamic resolution update') is None: return   # not both found in this build (a red line from the locator): the game's own dynamic resolution
    fs_site(site, '017c40bd')   # ldr s1,[x0,#0x7c]
    start = here(); emit(ADRP(16, here(), FS_STATE_OFF))
    for w in drs_controller: emit(w)
    fs_put(site, [B(site, start)], 'dynamic resolution level: 0 (full resolution), or with state +0x40 (DM_DYNAMIC_RESOLUTION) a climb on the load measured at each level')
    ctx.capabilities.add('DYNAMIC_RESOLUTION')
    ctx.render_scale = 2   # DM_RENDER_SCALE up to 2.0 of level 0 (0.75 of the output: 1.333 is native); the stub holds it at 1.0 of the output
    output_4k(ctx)

# 3840x2160: a render scale above 1.333 (state +0x10, DM_RENDER_SCALE) makes the output the game's own 3840x2160 instead of 2560x1440 docked, and the level stub above draws
# the 3D views at 1080 x the render scale lines of it (2.0: 3840x2160, native). Four places size the output for it, each x 1.5 then (a multiple of 4), and one budget x 2.25:
# - the frame buffer: main+0x1087f40 (v131072) keeps the output at +0x10 of the graphics system and maps it to the game's list of seven (640x480 .. 3840x2160). The game's
#   init (main+0x1087b70) sets it from its start parameters ([x1], 2560x1440 docked), before its call; on a change of mode or tier main+0x6502e0 sets it again from the mode's
#   size (w21 x w22), after the pick at main+0x112c5e0 that holds it to the list (3840x2160 docked at tier 0, 2560x1440 at tier 1). x16, x17, s0 and s1 are free at both calls
# - the window textures: the graphics init (main+0x346be50) hands the window its size from the config ([x19+0x9c], 2560x1440 docked) as floats (ldp s0, s1 at +0x3fc); x9,
#   s2 and s3 are free there. Without it the frame is 3840x2160 and every present crops a 2560x1440 window texture
# - the render-target memory: the per-frame render targets (main+0x3ed00) take blocks from a budget the app object holds at +0x5b8 (0x13100000, 305 MB) in blocks of +0x5c0
#   (64 MB). A 3840x2160 frame needs 2.25 times the 2560x1440 one: without it the allocation fails at the first frame of play and the game reads a null block (main+0x404fc).
#   The app's graphics init (main+0x36ae9d0) copies both into the allocator's setup after +0x88 (ldr x21, [x19, #0x590]); x9, x10, s0..s2 are free there
# The 3D size: the view-size update (main+0x31d3200) and the level's scale both scale by 1440 / the output's height, so the level stub writes 1.5 x 0.75 x the render scale.
# The upscaler (roulibr) has no 3840x2160 variant; at 2.0 it has nothing to upscale. 1.333 or less keeps every size the game's own
above_4_3 = (0xbd401200, 0x1e2d1001, 0x1e210800, 0x1e2e1001, 0x1e212000)   # ldr s0,[x16,#0x10]; s0 = it x 0.75; fcmp s0, 1.0
init_stub = (0xb9000a68,) + above_4_3 + (0x5400018d, 0x1e2f1000,              # the replaced str w8, [x19, #8]; b.le the call (1.333 or less, or NaN); s0 = 1.5
             0x29404430, 0x1e220201, 0x1e200821, 0x1e380030, 0x121e7610,      # width x 1.5, a multiple of 4
             0x1e220221, 0x1e200821, 0x1e380031, 0x121e7631, 0x29004430)      # height x 1.5, a multiple of 4; back to the parameters
mode_stub = above_4_3 + (0x5400014d, 0x1e2f1000,                              # b.le the replaced ldp; s0 = 1.5
             0x1e2202a1, 0x1e200821, 0x1e380035, 0x121e76b5,                  # width x 1.5, a multiple of 4
             0x1e2202c1, 0x1e200821, 0x1e380036, 0x121e76d6, 0x297f27a8)      # height x 1.5, a multiple of 4; the replaced ldp w8, w9, [x29, #-8]
window_stub = (0x2d538660, 0xbd401122, 0x1e2d1003, 0x1e230842, 0x1e2e1003, 0x1e232040, 0x5400018d, 0x1e2f1002,   # the size [x19+0x9c] (ints in s0, s1); above 1.333: x 1.5
               0x7e21d803, 0x1e220863, 0x1e390069, 0x121e7529, 0x1e270120,                                       # width x 1.5, a multiple of 4
               0x7e21d823, 0x1e220863, 0x1e390069, 0x121e7529, 0x1e270121)                                       # height x 1.5, a multiple of 4
budget_stub = (0xbd401120, 0x1e2d1001, 0x1e210800, 0x1e2e1001, 0x1e212000, 0x540001cd, 0x1e205000,              # above 1.333: x 2.25
               0xf942de6a, 0x9e230142, 0x1e200842, 0x9e39004a, 0x9270bd4a, 0xf902de6a,                           # the budget (+0x5b8) x 2.25, a multiple of 64 KB
               0xf942e26a, 0x9e230142, 0x1e200842, 0x9e39004a, 0x9270bd4a, 0xf902e26a,                           # the block size (+0x5c0) x 2.25, a multiple of 64 KB
               0xf942ca75)                                                                                       # the replaced ldr x21, [x19, #0x590]
def output_4k(ctx):
    names = ('output size init', 'output size', 'window size', 'graphics init')
    init, fn, win, gfx = (ctx.others.get(k) for k in names)
    if None in (init, fn, win, gfx): return   # not all found in this build (a red line from the locator): the game's own sizes, 1.333 at most
    why = ' x 1.5 when the render scale is above 1.333 (state +0x10, DM_RENDER_SCALE): the game\'s 3840x2160 docked'
    for site, orig, reg, words, what in ((init + 0xf4, '680a00b9', 16, init_stub, 'output size at start: the parameters\' size, before the call at +0xf8' + why),
                                         (fn + 0x300, 'a8277f29', 16, mode_stub, 'output size on a mode change: the mode\'s size, after the pick at +0x2fc' + why),
                                         (win + 0x3fc, '6086532d', 9, window_stub, 'window textures\' size' + why),
                                         (gfx + 0x88, '75ca42f9', 9, budget_stub, 'render-target memory budget and block size x 2.25 when the render scale is above 1.333')):
        ctx.site(site, orig)
        start = ctx.here()
        if words is init_stub: ctx.emit(words[0]); words = words[1:]   # the replaced store first: the state page's adrp after it
        ctx.emit(ADRP(reg, ctx.here(), ctx.state))
        for w in words: ctx.emit(w)
        ctx.emit(B(ctx.here(), site + 4))
        ctx.put(site, [B(site, start)], what)

MATCH_FRAME_STEP = {'main': [
    {'name': 'frame ratio from the host\'s step', 'size': 0x9b4, 'fingerprints': ['6ae2aff5344d14568e4dbaf93455e2d8278384bf892ed6c6eca87fef98fbcd1d'], 'stub': frame_step,   # v131072 main+0x6e6be0
     'others': {'dynamic resolution': {'size': 0x294, 'fingerprints': ['7af6ceb0aedf8e0c17dc08ce17fcd6a7cba6d394a0d09e3b504c6d668dc418e7']},   # v131072 main+0x8065e0
                'dynamic resolution update': {'size': 0xb24, 'fingerprints': ['5a423697838976baf286cffa5e2ceef0d4e037abbcd9d371e90e9e702d61b3d4']},   # main+0x805aa0
                'output size init': {'size': 0x2fc, 'fingerprints': ['f102ed9f80a4fc711650f4a86d11fe36602ecb330f422af96a4bad212dab5382']},   # main+0x1087b70
                'output size': {'size': 0x554, 'fingerprints': ['c0cab0dbf6f0644635315a356f8fd23a4701515c4ab4775d52191835b1a12e3c']},   # main+0x6502e0
                'window size': {'size': 0x6f0, 'fingerprints': ['db697988f57115f6aa10a89a8b6b765629fc12a5be32a02c196dc2c1329ab095']},   # main+0x346be50
                'graphics init': {'size': 0x858, 'fingerprints': ['83be7b818af92490839ec68669d6ee56d396e6bf4028c4c5a022e403e50d53ac']}}},   # main+0x36ae9d0
]}

BINDING = [
    {'what': 'frame ratio from the host\'s step', 'now': 'game', 'goal': 'game', 'purpose': 'frame rate',
     'why': "the game's own frame-ratio division, found by its function's fingerprint on any build that keeps the function",
     'checked': 'v131072 main: one function with this fingerprint, the site and its free registers read by disassembly (2026-11-08)'},
    {'what': 'dynamic resolution: full size, or a climb on the load measured at each level', 'now': 'game', 'goal': 'game', 'purpose': 'rendering',
     'why': "the game's own dynamic-resolution level function and update, found by their functions' fingerprints on any build that keeps both; switchable (DM_DYNAMIC_RESOLUTION)",
     'checked': 'v131072 main: one function with each fingerprint (the level function a leaf, x2..x11 / x16 / x17 / s0..s2 free), its callers, the update and the controller fields read by disassembly, the fields read in play (2026-11-08); the climb run natively against a model of the update and in play to full size (2026-11-09)'},
]
