# The Legend of Zelda: Ocarina of Time (0400486024DEC000): the rewriter's patches of this game's own code. dm/tools/patch_nso.py loads every profile's patches.py;
# MATCH_FRAME_STEP as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
from nso_asm import ADRP, B

# The game's frame ratio. Once a frame the function at main+0x6e6be0 (v131072, ModuleSystem::moduleCalc_ by the name of its job) divides its base rate (60.0, or 30.0 while
# bit 0 of [x21+0x24f] is set) by the int frame rate at [x21+0x2c8] (fdiv s8,s1,s0 at +0xe4) and hands the result to the time object (main+0x3b4e300: ratio at +0x10, 1 / ratio
# at +0x14, ratio / 60 at +0x18, the step in seconds, round(60 / ratio) at +0x1c), which the game steps by; the frame's own time never enters it, so a frame that takes two
# vsyncs at rate 60 plays at half speed. The stub gives the ratio the host's step instead: step × base / 60, and counts the vsyncs stepped. Until the host has decided a step,
# and at fps_target 0, the division runs. s0 and x16 / x17 are free there (s0 is written again on every path before it is read; the function uses neither x16 nor x17); s1 keeps
# the base rate and x21 the framework.
# Why × base / 60 and not the step alone (PR #1996 read base 30 as a cutscene rate and took the step, twice the speed there): the ratio's unit is 1/60 s whatever the base
# (the time object's +0x18 is ratio × 1/60, the constant at main+0x3e4066c). Base 30 is the framework's half rate: bit 0 of its +0x24f is that mode and bit 1 flips each pass of
# its frame function (main+0x346c680); with both set the draw and the present return at once (main+0x2cb020, main+0x40cd20) while the calc tree runs (main+0x2cabd0). So it
# calcs twice a drawn frame, this function among them, and each calc takes half the frame's step: 30 / 30 = 1 twice a 30 fps frame on the console, step / 2 twice here. The
# vsyncs are counted once a drawn frame, at the calc that is drawn (bit 1 clear): counted at both, the host's game time read twice the real time in that mode.
# The branch targets are computed from the words emitted, so a word added to the stub cannot leave one pointing into its middle (dsi-hub's HookLink met that on an iPhone,
# a fault on the first frame, when a block was added between main's fixed offsets and their target).
# Lichii (a GPL emulator; only the fact was taken) found the division (its notes: main+0x6E6CC4, 60 / [x21+0x2C8])
def frame_step(ctx):
    here, emit, fs_site, fs_put, FS_STATE_OFF, base = ctx.here, ctx.emit, ctx.site, ctx.put, ctx.state, ctx.base
    site = base + 0xe4
    fs_site(site, '2818201e')   # fdiv s8,s1,s0
    start = here()
    ORIG, COUNTED, n = 18, 13, [0]   # the original division is word 18 of the stub, the ratio starts at word 13 (both checked below); n counts the words emitted
    def put(w): emit(w); n[0] += 1
    def cbz_w17_orig(): return 0x34000000 | (((ORIG - n[0]) & 0x7FFFF) << 5) | 17   # from this word to word ORIG
    put(ADRP(16, here(), FS_STATE_OFF))
    put(0xb9400211)             # ldr w17,[x16]          target fps (0 = off)
    put(cbz_w17_orig())         # cbz w17,orig
    put(0xb9402211)             # ldr w17,[x16,#0x20]    step in vsyncs
    put(cbz_w17_orig())         # cbz w17,orig           not decided yet
    put(0x1e230220)             # ucvtf s0,w17
    put(0x39493eb0)             # ldrb w16,[x21,#0x24f]  the framework's half-rate bits
    put(0x37000000 | 1 << 19 | ((COUNTED - n[0]) & 0x3FFF) << 5 | 16)   # tbnz w16,#1,ratio: the calc of a half-rate frame that is not drawn; its vsyncs are counted at the drawn one
    put(ADRP(16, here(), FS_STATE_OFF))
    for w in (0xf9401a10,   # ldr x16,[x16,#0x30]
              0x8b110210):  # add x16,x16,x17
        put(w)
    put(ADRP(17, here(), FS_STATE_OFF))
    put(0xf9001a30)             # str x16,[x17,#0x30]    vsyncs stepped
    assert n[0] == COUNTED, f'frame_step: the ratio starts at word {n[0]}, the tbnz goes to word {COUNTED}'
    for w in (0x1e210808,   # fmul s8,s0,s1          step × base rate
              0x52a84e10,   # mov w16,#0x42700000    60.0
              0x1e270200,   # fmov s0,w16
              0x1e201908):  # fdiv s8,s8,s0          / 60
        put(w)
    put(B(here(), site + 4))
    assert n[0] == ORIG, f'frame_step: the original division is word {n[0]}, the cbz branches go to word {ORIG}'
    emit(0x1e201828); emit(B(here(), site + 4))   # orig: fdiv s8,s1,s0; back
    fs_put(site, [B(site, start)], 'frame ratio: the host\'s step × base rate / 60 instead of base rate / the game\'s frame rate (state page target fps; 0 = the game\'s ratio)')
    dynamic_resolution(ctx)
    half_rate(ctx)

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

# The framework's half rate, asked for by the host. Some of this game counts calcs, not time: the timeline that fires its subtitles (PlaySubtitleWithVoice trigger events) and
# its movie player, which takes one picture a calc (the limit at the player's +0x20 is 1 for the creation argument the game passes, main+0x3b5c55c). The frame ratio above
# moves what reads the time object by real time, so below 60 fps the two fall apart: on an iPhone at 33 fps with the player let take two pictures a calc, the opening's
# picture kept time and "However, there was one particular boy..." came up 98 s into it, over the nightmare's castle gate; at 53 fps the narration was over by 61 s, over
# Link asleep. The game's own answer to a frame of two vsyncs is its half rate: two calcs a drawn frame (bit 0 of the framework's +0x24f; frame_step's note), 60 calcs a second
# at 30 fps. It asks for it once a calc from bit 0 of [ModuleSystem+0xa30]: 2 (enter) or 1 (leave) into the framework's +0x24e (main+0x650ea4..0x650eb4, v131072; the frame
# function main+0x346c680 acts on it at the end of a pass, and while the mode is on the request is written only at the calc that is drawn). The stub ORs the host's ask into that
# bit: state +0x44, 1 while the host holds 2 vsyncs or more under DM_FPS_HALF_RATE=1 (hle_nvn.c fstep_tick). x16 and x17 are free there (the function uses x16 only as a
# local temporary far from the site, x17 nowhere).
def half_rate(ctx):
    here, emit, fs_site, fs_put, FS_STATE_OFF = ctx.here, ctx.emit, ctx.site, ctx.put, ctx.state
    f = ctx.others.get('half rate request')
    if f is None: return   # not found in this build (a red line from the locator): the game's own choice
    site = f + 0x1a4
    fs_site(site, '68324ab9')   # ldr w8,[x19,#0xa30]
    start = here()
    emit(0xb94a3268)                        # ldr w8,[x19,#0xa30]   the game's own flags
    emit(ADRP(16, here(), FS_STATE_OFF))
    emit(0xb9404610)                        # ldr w16,[x16,#0x44]   the host's ask, 0 or 1
    emit(0x2a100108)                        # orr w8,w8,w16
    emit(B(here(), site + 4))
    fs_put(site, [B(site, start)], 'half rate request: the game\'s own flag or the host\'s ask (state +0x44, DM_FPS_HALF_RATE)')
    ctx.capabilities.add('HALF_RATE')

MATCH_FRAME_STEP = {'main': [
    {'name': 'frame ratio from the host\'s step', 'size': 0x9b4, 'fingerprints': ['6ae2aff5344d14568e4dbaf93455e2d8278384bf892ed6c6eca87fef98fbcd1d'], 'stub': frame_step,   # v131072 main+0x6e6be0
     'others': {'dynamic resolution': {'size': 0x294, 'fingerprints': ['7af6ceb0aedf8e0c17dc08ce17fcd6a7cba6d394a0d09e3b504c6d668dc418e7']},   # v131072 main+0x8065e0
                'dynamic resolution update': {'size': 0xb24, 'fingerprints': ['5a423697838976baf286cffa5e2ceef0d4e037abbcd9d371e90e9e702d61b3d4']},   # main+0x805aa0
                'half rate request': {'size': 0x938, 'fingerprints': ['cf27987b41456695ce0447d7b711f9a4f2d6424d4f3c04744252ca2cfb47dc2c']}}},   # main+0x650d00
]}

BINDING = [
    {'what': 'half rate request: the game\'s own flag or the host\'s ask', 'now': 'game', 'goal': 'game', 'purpose': 'frame rate',
     'why': "the game's own half-rate request, found by its function's fingerprint on any build that keeps the function; switchable (DM_FPS_HALF_RATE)",
     'checked': 'v131072 main: one function with this fingerprint, the site, the request byte and the frame function that acts on it read by disassembly; run on an iPhone Duo, two calcs a drawn frame at a hold of 2 (2026-11-09)'},
    {'what': 'frame ratio from the host\'s step', 'now': 'game', 'goal': 'game', 'purpose': 'frame rate',
     'why': "the game's own frame-ratio division, found by its function's fingerprint on any build that keeps the function",
     'checked': 'v131072 main: one function with this fingerprint, the site and its free registers read by disassembly (2026-11-08)'},
    {'what': 'dynamic resolution: full size, or a climb on the load measured at each level', 'now': 'game', 'goal': 'game', 'purpose': 'rendering',
     'why': "the game's own dynamic-resolution level function and update, found by their functions' fingerprints on any build that keeps both; switchable (DM_DYNAMIC_RESOLUTION)",
     'checked': 'v131072 main: one function with each fingerprint (the level function a leaf, x2..x11 / x16 / x17 / s0..s2 free), its callers, the update and the controller fields read by disassembly, the fields read in play (2026-11-08); the climb run natively against a model of the update and in play to full size (2026-11-09)'},
]
