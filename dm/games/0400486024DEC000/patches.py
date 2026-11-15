# The Legend of Zelda: Ocarina of Time (0400486024DEC000): the rewriter's patches of this game's own code. dm/tools/patch_nso.py loads every profile's patches.py;
# MATCH_FRAME_STEP as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
import struct

from nso_asm import ADRP, ADDI, B

ASPECT_STATE = 0x280   # +0x40 is this build's DM_DYNAMIC_RESOLUTION switch; keep host-owned values separate
ASPECT_ACK = 0x284
PROJECTION_ACK = 0x288  # one last-applied aspect per camera site, for safe live changes and rotation

def _f32(value): return struct.unpack('<I', struct.pack('<f', value))[0]
def _movz(wd, imm): return 0x52800000 | ((imm & 0xffff) << 5) | wd
def _movk16(wd, imm): return 0x72a00000 | ((imm & 0xffff) << 5) | wd
def _fmov_sw(sd, wn): return 0x1e270000 | (wn << 5) | sd
def _fsub(sd, sn, sm): return 0x1e203800 | (sm << 16) | (sn << 5) | sd
def _fabs(sd, sn): return 0x1e20c000 | (sn << 5) | sd
def _fcmp(sn, sm): return 0x1e202000 | (sm << 16) | (sn << 5)
def _ldr_w(wt, xn, off=0): return 0xb9400000 | ((off // 4) << 10) | (xn << 5) | wt
def _ldr_s(st, xn, off=0): return 0xbd400000 | ((off // 4) << 10) | (xn << 5) | st
def _ldar_w(wt, xn): return 0x88dffc00 | (xn << 5) | wt
def _stlr_w(wt, xn): return 0x889ffc00 | (xn << 5) | wt
def _str_s(st, xn, off): return 0xbd000000 | ((off // 4) << 10) | (xn << 5) | st
def _cbz_w(wt, frm, to): return 0x34000000 | ((((to - frm) >> 2) & 0x7ffff) << 5) | wt
def _b_cond(frm, to, cond): return 0x54000000 | ((((to - frm) >> 2) & 0x7ffff) << 5) | cond

def _load_f32(words, reg, value):
    bits = _f32(value)
    words.extend((_movz(16, bits), _movk16(16, bits >> 16), _fmov_sw(reg, 16)))

def _state_addr(ctx, words, reg, off):
    pc = ctx.here() + len(words) * 4
    words.extend((ADRP(reg, pc, ctx.state + off), ADDI(reg, reg, (ctx.state + off) & 0xfff)))

def _append_stub(ctx, site, original, words, reason):
    start = ctx.here()
    for w in words: ctx.emit(w)
    ctx.emit(B(ctx.here(), site + 4 * len(original)))
    ctx.put(site, [B(site, start)] + [0xd503201f] * (len(original) - 1), reason)

def _projection(ctx, site, original, base, aspect_reg, projection_ack):
    # Preserve non-16:9 projections. Track each camera separately because mAspect is written back and is also used by culling.
    ctx.site(site, struct.pack('<I', original).hex())
    w = [original]
    _load_f32(w, 16, 16.0 / 9.0)
    w.extend((_fsub(17, aspect_reg, 16), _fabs(17, 17)))
    _load_f32(w, 16, 0.01)
    w.append(_fcmp(17, 16))
    stock = len(w); w.append(0)
    _state_addr(ctx, w, 17, projection_ack)
    w.append(_ldar_w(16, 17))
    no_ack = len(w); w.append(0)
    w.append(_fmov_sw(16, 16))
    w.extend((_fsub(17, aspect_reg, 16), _fabs(17, 17)))
    _load_f32(w, 16, 0.01)
    w.append(_fcmp(17, 16))
    stale = len(w); w.append(0)
    apply = len(w)
    _state_addr(ctx, w, 17, ASPECT_STATE)
    w.append(_ldar_w(16, 17))
    disabled = len(w); w.append(0)
    w.append(_fmov_sw(aspect_reg, 16))
    w.append(_str_s(aspect_reg, base, 176))
    _state_addr(ctx, w, 17, ASPECT_ACK)
    w.append(_stlr_w(16, 17))
    _state_addr(ctx, w, 17, projection_ack)
    w.append(_stlr_w(16, 17))
    skip_restore = len(w); w.append(0)
    disabled_at = len(w)
    _load_f32(w, aspect_reg, 16.0 / 9.0)
    w.append(_str_s(aspect_reg, base, 176))
    w.append(0x2a1f03f0)  # acknowledge disabled widescreen as 0
    _state_addr(ctx, w, 17, ASPECT_ACK)
    w.append(_stlr_w(16, 17))
    _state_addr(ctx, w, 17, projection_ack)
    w.append(_stlr_w(16, 17))
    end = len(w)
    w[stock] = _b_cond(ctx.here() + stock * 4, ctx.here() + apply * 4, 3)  # LO: stock aspect
    w[no_ack] = _cbz_w(16, ctx.here() + no_ack * 4, ctx.here() + end * 4)
    w[stale] = _b_cond(ctx.here() + stale * 4, ctx.here() + end * 4, 2)  # HS: neither stock nor this camera's previous aspect
    w[disabled] = _cbz_w(16, ctx.here() + disabled * 4, ctx.here() + disabled_at * 4)
    w[skip_restore] = B(ctx.here() + skip_restore * 4, ctx.here() + end * 4)
    _append_stub(ctx, site, [original], w, 'camera projection: use drawable aspect for 16:9 views; restore stock ratio when disabled')

def _camera_aspect(ctx):
    if getattr(ctx, 'build_id', None) != 'e0f3ccfaf50065ae4ea01eb5aeb13b1a': return  # fixed sites belong only to the registered v131072 build
    # Adapted from the exact-build analysis in craftycorvid/oot-mod (MIT), which targets this same 0400486024DEC000 main build.
    # The four sites load sead::PerspectiveProjection::mAspect at +0xac; writeback at +0xb0 keeps culling in step with the widened view.
    for i, (site, original, base, areg) in enumerate(((0x3485cc0, 0x2d558400, 0, 1),
                                       (0x004f1e8, 0x2d558ea0, 21, 3),
                                       (0x023b9a8, 0x2d558c00, 0, 3),
                                       (0x033b82c, 0x2d558440, 2, 1))):
        _projection(ctx, site, original, base, areg, PROJECTION_ACK + i * 4)

    # The render-view's width/height constant is also consumed by shaders and view-dependent effects.
    site = 0x025d76c; orig = [0x1e2202a1, 0x1e221821]
    ctx.site(site, struct.pack('<II', *orig).hex())
    w = list(orig); _load_f32(w, 16, 16.0 / 9.0); w.extend((_fsub(17, 1, 16), _fabs(17, 17)))
    _load_f32(w, 16, 0.01); w.append(_fcmp(17, 16))
    skip_view = len(w); w.append(0)
    _state_addr(ctx, w, 17, ASPECT_STATE); w.append(_ldar_w(16, 17))
    skip_off = len(w); w.append(0); w.append(_fmov_sw(1, 16)); end = len(w)
    w[skip_view] = _b_cond(ctx.here() + skip_view * 4, ctx.here() + end * 4, 2)  # preserve non-16:9 views
    w[skip_off] = _cbz_w(16, ctx.here() + skip_off * 4, ctx.here() + end * 4)
    _append_stub(ctx, site, orig, w, 'render-view aspect: match the widened camera on the output view')

    # Full-screen 2D canvases use the game's 3840x2160 orthographic projection. Squeeze that layout before the stretched present.
    site = 0x0774368; orig = 0xad401041
    ctx.site(site, struct.pack('<I', orig).hex())
    w = [orig, 0x1e260030, _movz(17, _f32(2 / 3840)), _movk16(17, _f32(2 / 3840) >> 16), 0x6b11021f]
    ne_x = len(w); w.append(0)
    w.extend((0x0e0c3c00 | (4 << 5) | 16, 0x12007800 | (16 << 5) | 16,
              _movz(17, _f32(2 / 2160)), _movk16(17, _f32(2 / 2160) >> 16), 0x6b11021f))
    ne_y = len(w); w.append(0)
    w.extend((0xd1000000 | (0x1b8 << 10) | 17, 0xf9400000 | (17 << 5) | 16))  # screen = canvas - 0x1b8
    bg_branches = []
    for vt in (0x4bed098, 0x4be4fb0, 0x4be7b30, 0x4be61c8):
        pc = ctx.here() + len(w) * 4
        w.extend((ADRP(17, pc, vt), ADDI(17, 17, vt & 0xfff), 0xeb00001f | (17 << 16) | (16 << 5), 0x54000000))
        bg_branches.append(len(w) - 1)
    _state_addr(ctx, w, 17, ASPECT_STATE); w.append(_ldar_w(16, 17))
    zero = len(w); w.append(0)
    bits = _f32(16.0 / 9.0)
    w.extend((_movz(17, bits), _movk16(17, bits >> 16), _fmov_sw(17, 17), _fmov_sw(18, 16),
              0x1e201800 | (18 << 16) | (17 << 5) | 18, 0x1e260000 | (18 << 5) | 16,
              0x4e040c00 | (16 << 5) | 17, 0x6e20dc00 | (17 << 16) | (1 << 5) | 1))
    end = len(w)
    w[ne_x] = _b_cond(ctx.here() + ne_x * 4, ctx.here() + end * 4, 1)
    w[ne_y] = _b_cond(ctx.here() + ne_y * 4, ctx.here() + end * 4, 1)
    w[zero] = _cbz_w(16, ctx.here() + zero * 4, ctx.here() + end * 4)
    for at in bg_branches: w[at] = _b_cond(ctx.here() + at * 4, ctx.here() + end * 4, 0)
    _append_stub(ctx, site, [orig], w, 'full-screen 2D layout: keep the HUD at 16:9 when the picture fills a wider drawable')

    # Pre-rendered movies remain centred at 16:9 instead of being stretched with the world image.
    site = 0x3b5c758; orig = 0xa941a269
    ctx.site(site, struct.pack('<I', orig).hex())
    # Keep the viewport pointer out of x16: the aspect/state helpers use x16 as scratch.
    w = [0xd1000000 | (40 << 10) | (29 << 5) | 14, _ldr_s(22, 14, 8), _ldr_s(23, 14, 16),
         0x1e203800 | (22 << 16) | (23 << 5) | 18]
    _state_addr(ctx, w, 17, ASPECT_STATE); w.append(_ldar_w(16, 17))
    no_wide = len(w); w.append(0)
    w.extend((_fmov_sw(19, 16),))
    _load_f32(w, 20, 16.0 / 9.0)
    w.extend((0x1e201800 | (19 << 16) | (20 << 5) | 20,))  # s20 = (16:9) / target
    _load_f32(w, 21, 1.0)
    w.extend((_fsub(20, 21, 20),))
    _load_f32(w, 21, 0.5)
    w.extend((0x1e200800 | (21 << 16) | (20 << 5) | 20,  # margin = (1 - (16:9)/target) / 2
              0x1e200800 | (20 << 16) | (18 << 5) | 18,  # delta = viewport width * margin
              0x1e202800 | (18 << 16) | (22 << 5) | 22, 0x1e203800 | (18 << 16) | (23 << 5) | 23,
              _str_s(22, 14, 8), _str_s(23, 14, 16)))
    end = len(w); w[no_wide] = _cbz_w(16, ctx.here() + no_wide * 4, ctx.here() + end * 4)
    w.append(orig)
    _append_stub(ctx, site, [orig], w, 'movie viewport: letterbox pre-rendered movies within the centred 16:9 HUD')

    # NPC speech bubbles use 1920x1080 layout units; compensate their x anchor for the HUD's horizontal squeeze.
    site = 0x020c2b0; orig = 0x52a89e08
    ctx.site(site, struct.pack('<I', orig).hex())
    w = []; _state_addr(ctx, w, 17, ASPECT_STATE); w.append(_ldar_w(16, 17))
    no_wide = len(w); w.append(0)
    w.append(_fmov_sw(16, 16)); _load_f32(w, 17, 1080.0)
    w.extend((0x1e200800 | (17 << 16) | (16 << 5) | 16, 0x1e260000 | (16 << 5) | 8))
    end = len(w); w[no_wide] = _cbz_w(16, ctx.here() + no_wide * 4, ctx.here() + end * 4)
    w.append(orig)
    _append_stub(ctx, site, [orig], w, 'speech bubble anchor: keep the bubble over the speaker when the HUD is centred')

    ctx.capabilities.add('CAMERA_ASPECT')

# The game's frame ratio. Once a frame the function at main+0x6e6be0 (v131072) divides its base rate (60.0, or 30.0 while bit 0 of [x21+0x24f] is set) by the int frame rate at
# [x21+0x2c8] (fdiv s8,s1,s0 at +0xe4) and hands the result to the time object (main+0x3b4e300: ratio at +0x10, 1 / ratio at +0x14, round(60 / ratio) at +0x1c), which the game
# steps by; the frame's own time never enters it, so a frame that takes two vsyncs at rate 60 plays at half speed. The stub gives the ratio the host's step instead: step × base / 60
# (the division's value when the host keeps frames 60 / rate vsyncs), and counts the vsyncs stepped. Until the host has decided a step, and at fps_target 0, the division runs.
# s0 and x16 / x17 are free there (s0 is written again on every path before it is read; the function uses neither x16 nor x17); s1 keeps the base rate.
# Lichii (a GPL emulator; only the fact was taken) found the division (its notes: main+0x6E6CC4, 60 / [x21+0x2C8])
def frame_step(ctx):
    here, emit, fs_site, fs_put, FS_STATE_OFF, base = ctx.here, ctx.emit, ctx.site, ctx.put, ctx.state, ctx.base
    site = (base or 0x6e6be0) + 0xe4  # exact v131072 function entry when the registered rule has no matched base
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

FRAME_STEP = {'e0f3ccfaf50065ae4ea01eb5aeb13b1a': frame_step}

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
    exact_oot = getattr(ctx, 'build_id', None) == 'e0f3ccfaf50065ae4ea01eb5aeb13b1a'
    if site is None and exact_oot: site = 0x8065e0
    if site is None or (ctx.others.get('dynamic resolution update') is None and not exact_oot):
        _camera_aspect(ctx)
        return   # not both found in this build (a red line from the locator): the game's own dynamic resolution
    fs_site(site, '017c40bd')   # ldr s1,[x0,#0x7c]
    start = here(); emit(ADRP(16, here(), FS_STATE_OFF))
    for w in drs_controller: emit(w)
    fs_put(site, [B(site, start)], 'dynamic resolution level: 0 (full resolution), or with state +0x40 (DM_DYNAMIC_RESOLUTION) a climb on the load measured at each level')
    ctx.capabilities.add('DYNAMIC_RESOLUTION')
    _camera_aspect(ctx)

MATCH_FRAME_STEP = {'main': [
    {'name': 'frame ratio from the host\'s step', 'size': 0x9b4, 'fingerprints': ['6ae2aff5344d14568e4dbaf93455e2d8278384bf892ed6c6eca87fef98fbcd1d'], 'stub': frame_step,   # v131072 main+0x6e6be0
     'others': {'dynamic resolution': {'size': 0x294, 'fingerprints': ['7af6ceb0aedf8e0c17dc08ce17fcd6a7cba6d394a0d09e3b504c6d668dc418e7']},   # v131072 main+0x8065e0
                'dynamic resolution update': {'size': 0xb24, 'fingerprints': ['5a423697838976baf286cffa5e2ceef0d4e037abbcd9d371e90e9e702d61b3d4']}}},   # main+0x805aa0
]}

BINDING = [
    {'what': 'frame ratio from the host\'s step', 'now': 'game', 'goal': 'game', 'purpose': 'frame rate',
     'why': "the game's own frame-ratio division, found by its function's fingerprint on any build that keeps the function",
     'checked': 'v131072 main: one function with this fingerprint, the site and its free registers read by disassembly (2026-11-08)'},
    {'what': 'dynamic resolution: full size, or a climb on the load measured at each level', 'now': 'game', 'goal': 'game', 'purpose': 'rendering',
     'why': "the game's own dynamic-resolution level function and update, found by their functions' fingerprints on any build that keeps both; switchable (DM_DYNAMIC_RESOLUTION)",
     'checked': 'v131072 main: one function with each fingerprint (the level function a leaf, x2..x11 / x16 / x17 / s0..s2 free), its callers, the update and the controller fields read by disassembly, the fields read in play (2026-11-08); the climb run natively against a model of the update and in play to full size (2026-11-09)'},
    {'what': 'native landscape camera, full-screen HUD and movie layout', 'now': 'build', 'goal': 'game', 'purpose': 'rendering',
     'why': 'the exact-build projection, render-view and layout sites plus the host drawable aspect; only Ocarina v131072 has reviewed fixed addresses',
     'checked': 'user reports fullscreen and map transitions work on iPhone 18 Pro Max; 30 fps is not stable, and HUD, cutscene framing, rotation and touch mapping remain unverified'},
]
