# The Legend of Zelda: Ocarina of Time (0400486024DEC000): the rewriter's patches of this game's own code. dm/tools/patch_nso.py loads every profile's patches.py;
# MATCH_FRAME_STEP as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
import struct
from nso_asm import ADRP, B

_aspect_state = 0xa4    # loader.c writes float ow / 1080.0f here when DM_SCREEN_ASPECT >= 1.8 (0 = 16:9)
_aspect_ack = 0x284
_projection_ack = 0x288   # one last-applied aspect per camera site


def _f32(value): return struct.unpack('<I', struct.pack('<f', value))[0]
def _addi(rd, rn, imm): return 0x91000000 | ((imm & 0xfff) << 10) | (rn << 5) | rd
def _movz(wd, imm): return 0x52800000 | ((imm & 0xffff) << 5) | wd
def _movk16(wd, imm): return 0x72a00000 | ((imm & 0xffff) << 5) | wd
def _fmov_sw(sd, wn): return 0x1e270000 | (wn << 5) | sd
def _fsub(sd, sn, sm): return 0x1e203800 | (sm << 16) | (sn << 5) | sd
def _fabs(sd, sn): return 0x1e20c000 | (sn << 5) | sd
def _fcmp(sn, sm): return 0x1e202000 | (sm << 16) | (sn << 5)
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
    words.extend((ADRP(reg, pc, ctx.state + off), _addi(reg, reg, (ctx.state + off) & 0xfff)))


def _append_stub(ctx, site, original, words, reason):
    start = ctx.here()
    for w in words: ctx.emit(w)
    ctx.emit(B(ctx.here(), site + 4 * len(original)))
    ctx.put(site, [B(site, start)] + [0xd503201f] * (len(original) - 1), reason)


def _projection(ctx, site, original, base, aspect_reg, projection_ack):
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
    _state_addr(ctx, w, 17, _aspect_state)
    w.append(_ldar_w(16, 17))
    disabled = len(w); w.append(0)
    w.append(_fmov_sw(aspect_reg, 16))
    w.append(_str_s(aspect_reg, base, 176))
    _state_addr(ctx, w, 17, _aspect_ack)
    w.append(_stlr_w(16, 17))
    _state_addr(ctx, w, 17, projection_ack)
    w.append(_stlr_w(16, 17))
    skip_restore = len(w); w.append(0)
    disabled_at = len(w)
    _load_f32(w, aspect_reg, 16.0 / 9.0)
    w.append(_str_s(aspect_reg, base, 176))
    w.append(0x2a1f03f0)
    _state_addr(ctx, w, 17, _aspect_ack)
    w.append(_stlr_w(16, 17))
    _state_addr(ctx, w, 17, projection_ack)
    w.append(_stlr_w(16, 17))
    end = len(w)
    w[stock] = _b_cond(ctx.here() + stock * 4, ctx.here() + apply * 4, 3)
    w[no_ack] = _cbz_w(16, ctx.here() + no_ack * 4, ctx.here() + end * 4)
    w[stale] = _b_cond(ctx.here() + stale * 4, ctx.here() + end * 4, 2)
    w[disabled] = _cbz_w(16, ctx.here() + disabled * 4, ctx.here() + disabled_at * 4)
    w[skip_restore] = B(ctx.here() + skip_restore * 4, ctx.here() + end * 4)
    _append_stub(ctx, site, [original], w, 'camera projection: use drawable aspect for 16:9 views, restore stock ratio when disabled')


def _camera_aspect(ctx):
    # Adapted from craftycorvid/oot-mod (MIT) for 0400486024DEC000 v131072 main.
    for i, (site, original, base, areg) in enumerate(((0x3485cc0, 0x2d558400, 0, 1),
                                                      (0x004f1e8, 0x2d558ea0, 21, 3),
                                                      (0x023b9a8, 0x2d558c00, 0, 3),
                                                      (0x033b82c, 0x2d558440, 2, 1))):
        _projection(ctx, site, original, base, areg, _projection_ack + i * 4)

    site = 0x025d76c; orig = [0x1e2202a1, 0x1e221821]
    ctx.site(site, struct.pack('<II', *orig).hex())
    w = list(orig); _load_f32(w, 16, 16.0 / 9.0); w.extend((_fsub(17, 1, 16), _fabs(17, 17)))
    _load_f32(w, 16, 0.01); w.append(_fcmp(17, 16))
    skip_view = len(w); w.append(0)
    _state_addr(ctx, w, 17, _aspect_state); w.append(_ldar_w(16, 17))
    skip_off = len(w); w.append(0); w.append(_fmov_sw(1, 16)); end = len(w)
    w[skip_view] = _b_cond(ctx.here() + skip_view * 4, ctx.here() + end * 4, 2)
    w[skip_off] = _cbz_w(16, ctx.here() + skip_off * 4, ctx.here() + end * 4)
    _append_stub(ctx, site, orig, w, 'render-view aspect: match the widened camera on the output view')

    site = 0x0774368; orig_w = 0xad401041
    ctx.site(site, struct.pack('<I', orig_w).hex())
    w = [orig_w, 0x1e260030, _movz(17, _f32(2 / 3840)), _movk16(17, _f32(2 / 3840) >> 16), 0x6b11021f]
    ne_x = len(w); w.append(0)
    w.extend((0x0e0c3c00 | (4 << 5) | 16, 0x12007800 | (16 << 5) | 16,
              _movz(17, _f32(2 / 2160)), _movk16(17, _f32(2 / 2160) >> 16), 0x6b11021f))
    ne_y = len(w); w.append(0)
    w.extend((0xd1000000 | (0x1b8 << 10) | 17, 0xf9400000 | (17 << 5) | 16))
    bg_branches = []
    for vt in (0x4bed098, 0x4be4fb0, 0x4be7b30, 0x4be61c8):
        pc = ctx.here() + len(w) * 4
        w.extend((ADRP(17, pc, vt), _addi(17, 17, vt & 0xfff), 0xeb00001f | (17 << 16) | (16 << 5), 0x54000000))
        bg_branches.append(len(w) - 1)
    _state_addr(ctx, w, 17, _aspect_state); w.append(_ldar_w(16, 17))
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
    _append_stub(ctx, site, [orig_w], w, 'full-screen 2D layout: keep the HUD at 16:9 when the picture fills a wider drawable')

    site = 0x3b5c758; orig_w = 0xa941a269
    ctx.site(site, struct.pack('<I', orig_w).hex())
    w = [0xd1000000 | (40 << 10) | (29 << 5) | 14, _ldr_s(22, 14, 8), _ldr_s(23, 14, 16),
         0x1e203800 | (22 << 16) | (23 << 5) | 18]
    _state_addr(ctx, w, 17, _aspect_state); w.append(_ldar_w(16, 17))
    no_wide = len(w); w.append(0)
    w.extend((_fmov_sw(19, 16),))
    _load_f32(w, 20, 16.0 / 9.0)
    w.extend((0x1e201800 | (19 << 16) | (20 << 5) | 20,))
    _load_f32(w, 21, 1.0)
    w.extend((_fsub(20, 21, 20),))
    _load_f32(w, 21, 0.5)
    w.extend((0x1e200800 | (21 << 16) | (20 << 5) | 20,
              0x1e200800 | (20 << 16) | (18 << 5) | 18,
              0x1e202800 | (18 << 16) | (22 << 5) | 22, 0x1e203800 | (18 << 16) | (23 << 5) | 23,
              _str_s(22, 14, 8), _str_s(23, 14, 16)))
    end = len(w); w[no_wide] = _cbz_w(16, ctx.here() + no_wide * 4, ctx.here() + end * 4)
    w.append(orig_w)
    _append_stub(ctx, site, [orig_w], w, 'movie viewport: letterbox pre-rendered movies within the centred 16:9 HUD')

    site = 0x020c2b0; orig_w = 0x52a89e08
    ctx.site(site, struct.pack('<I', orig_w).hex())
    w = []; _state_addr(ctx, w, 17, _aspect_state); w.append(_ldar_w(16, 17))
    no_wide = len(w); w.append(0)
    w.append(_fmov_sw(16, 16)); _load_f32(w, 17, 1080.0)
    w.extend((0x1e200800 | (17 << 16) | (16 << 5) | 16, 0x1e260000 | (16 << 5) | 8))
    end = len(w); w[no_wide] = _cbz_w(16, ctx.here() + no_wide * 4, ctx.here() + end * 4)
    w.append(orig_w)
    _append_stub(ctx, site, [orig_w], w, 'speech bubble anchor: keep the bubble over the speaker when the HUD is centred')

    ctx.capabilities.add('ANAMORPHIC_ASPECT')


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
# Priority; on v131072 levels 0..9 at 0.75..0.333 of 2048x1152, 1536x864 to 683x384).
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


def frame_step_v131072(ctx):
    ctx.base = 0x6e6be0
    ctx.others = {'dynamic resolution': 0x8065e0, 'dynamic resolution update': 0x805aa0}
    frame_step(ctx)
    _camera_aspect(ctx)


FRAME_STEP = {
    'e0f3ccfaf50065ae4ea01eb5aeb13b1a': frame_step_v131072,   # v131072 main
}

MATCH_FRAME_STEP = {'main': [
    {'name': 'frame ratio from the host\'s step', 'size': 0x9b4, 'fingerprints': ['6ae2aff5344d14568e4dbaf93455e2d8278384bf892ed6c6eca87fef98fbcd1d'], 'stub': frame_step,   # v131072 main+0x6e6be0
     'others': {'dynamic resolution': {'size': 0x294, 'fingerprints': ['7af6ceb0aedf8e0c17dc08ce17fcd6a7cba6d394a0d09e3b504c6d668dc418e7']},   # v131072 main+0x8065e0
                'dynamic resolution update': {'size': 0xb24, 'fingerprints': ['5a423697838976baf286cffa5e2ceef0d4e037abbcd9d371e90e9e702d61b3d4']}}},   # main+0x805aa0
]}

# The movie player's pictures per frame. Once a frame the player's update (main+0x3b5ca60 on v131072) adds the frame's time (the time object's +0x18, the frame ratio / 60 s, times
# 60) to a count at +0x8, takes its whole pictures (the movie's rate at the player's +0x324; the title's and the opening's are 60 fps) and advances that many (main+0x3b61ea0 pops them from its
# queue and shows the last), but never more than the player's +0x20 (csel at main+0x3b5cb2c), and the count keeps what was left. The constructor (main+0x3b5cec0) sets +0x20 to
# 1, or to 2 when its last argument is 2 or more (cmp w27, #2; cinc w8, w28, ge at +0x1c4, w28 = 1), and the queue's size at +0x50 to twice that; the game's one caller passes 1
# (main+0x3b5c55c). So a frame of two vsyncs shows one picture where the movie had two, the picture falls behind for good (the count only grows), and the movie's sound, a stream of
# the sound engine on real time, runs ahead and ends before the picture: at a 30 fps frame step a movie played at exactly 0.500 of its speed (58.00 s of movie in 115.95 s) with
# the count at 1,350 pictures after 45 s. The console holds one vsync a frame in its movies, so the limit never shows there; here the frame step hands the game steps of 2 and 3.
# The cut gives the constructor's own value for 2 (+0x20 = 2, +0x50 = 4), so a frame of two vsyncs shows every second picture and a movie that fell behind catches up at two
# pictures a frame: at the 30 fps frame step 1.000 (95.00 s in 94.97 s).
movie_pictures = 'movie pictures a frame: up to 2, the constructor\'s value for an argument of 2 or more, whatever its caller passes (1)'
# The update's cap, 3. The constructor's limit also sizes the decoder's pipeline: the picture pool is twice it (video-frame-count, registered from a four-slot array at the player's
# +0x168, which the teardown frees slot by slot) and the inputs in flight are as many as it (their buffers two slots at +0x188 and +0x190: main+0x3b5ff30 takes slot 0 for an id of
# 2 or more). So 3 cannot be the limit: with the game's pool of 6 the registration wrote over the two input buffers (the game stopped at the movie's start with a copy to a null
# pointer in sdk), and with the pool kept at 4 the third input reused slot 0 while in flight (a VideoToolbox decode error after 150 pictures of the title movie). The frame step's
# largest hold is 3 vsyncs, and with a cap of 2 each such frame left the picture one behind for as long as the step stayed at 2. The update reads the limit only for its cap
# (ldr w9, [x0, #0x20] at +0xc0, before the csel); the cut puts 3 there and leaves the limit, the pool and the inputs at the constructor's 2. The advance pops up to that many from
# its own queue and stops when the queue is empty, so a frame takes a third picture only when the decoder has one.
movie_cap = 'movie update cap: 3 pictures a frame, the frame step\'s largest hold, in place of the limit (2), which also sizes the decoder pipeline'
# The movie's frame, which its event follows. The event that plays a movie (its update main+0x3b3e9a0 on v131072) reads the movie's frame (its vtable's +0x78, main+0x3b3ec10: the
# movie object's int at +0x2f8) and moves its timeline up to it when the timeline is behind (main+0x122d520), then measures the movie's sound stream against it (main+0x3b3ea50).
# The opening's timeline carries its subtitles, the narrator's voice lines (EventFlowMsg/Dm_Kokiri_0000, played with them), its rumble and the title logo
# (EventOpenOpeningTitle). The movie's draw (main+0x3b5c710, and main+0x3b5c870 for its first draw) adds 1 to that frame each frame a picture is on screen (ldr, add #1,
# str at +0xb4 and +0x98), so the frame counts frames drawn, not pictures shown: the same while the player takes one picture a frame, half of them when it takes two. With the
# cut above, a frame of two vsyncs showed the picture in real time but the subtitles and voice at half speed, and the logo, due after the movie's end, never showed. The cut
# sets the frame to the pictures the player has taken (the player's +0xc: its update adds what it advances each frame; reset when a movie starts, as +0x2f8 is), so the
# timeline follows the picture whatever the frame takes. The player is the object at +0x10, already read by both draws (main+0x3b5ce80 on it) just before.
movie_frame = 'movie frame: the pictures the player has taken (its +0xc), not 1 more for each frame drawn'
MATCH_CUTS = {'main': [
    {'name': movie_pictures, 'size': 828, 'fingerprints': ['f61ad468b6b4e1d574fe8678f5a12dfc60e1aab71be7375142a4b48b0d73ce19'],   # v131072 main+0x3b5cec0
     'cuts': [(452, '88b79c1a', '48008052', movie_pictures + ' (cinc w8, w28, ge → mov w8, #2; as v131072 main+0x3b5d084)')]},
    {'name': movie_cap, 'size': 368, 'fingerprints': ['6a29b27559214b3b89cc31a5b85f14bda4a55685f1ae4fe486d53eda66855867'],   # v131072 main+0x3b5ca60, the update
     'cuts': [(192, '092040b9', '69008052', movie_cap + ' (ldr w9, [x0, #0x20] → mov w9, #3; as v131072 main+0x3b5cb20)')]},
    {'name': movie_frame, 'functions': [
        {'size': 344, 'fingerprints': ['cc1ca04feb1ca4e553a6c7a4f36f99b13f56977185eea8ef5cd8dd3dbbdcdbcd'],   # v131072 main+0x3b5c710, the draw
         'cuts': [(180, '88fa42b908050011', '880a40f9080d40b9', movie_frame + ' (ldr w8, [x20, #0x2f8]; add w8, w8, #1 → ldr x8, [x20, #0x10]; ldr w8, [x8, #0xc]; as v131072 main+0x3b5c7c4)')]},
        {'size': 196, 'fingerprints': ['8fe123ca6fcd016cd9c718e5e66035c807b27f63408861235765e47b4ec34da3'],   # v131072 main+0x3b5c870, the first draw
         'cuts': [(152, '88fa42b908050011', '880a40f9080d40b9', movie_frame + ' (the same; as v131072 main+0x3b5c908)')]},
    ]},
]}

BINDING = [
    {'what': 'movie pictures a frame: up to 2', 'now': 'game', 'goal': 'game', 'purpose': 'frame rate',
     'why': "the game's own movie player constructor, found by its function's fingerprint on any build that keeps the function; no other game has it: the fingerprint matches no function in the mains of Donkey Kong Bananza, Splatoon Raiders, Pokopia, Mario Tennis Fever, Switch Sports, Yoshi and the Mysterious Book, Mario Golf, Xenoblade Chronicles 2 or Super Mario 3D All-Stars, and Breath of the Wild's and Tears of the Kingdom's movies go through subsdk0's movie::Decoder, not a player in main",
     'checked': 'v131072 main: one function with this fingerprint, the constructor, its one caller, the update and the advance read by disassembly; the limit and the count read in play with DM_PROBE, the opening movie timed at a 30 fps frame step without and with the cut (2026-11-12)'},
    {'what': 'movie update cap: 3 pictures a frame', 'now': 'game', 'goal': 'game', 'purpose': 'frame rate',
     'why': "the game's own movie player update, found by its function's fingerprint on any build that keeps the function; it goes with the player's two-picture cut and has no wider binding",
     'checked': 'v131072 main: one function with this fingerprint, the cap\'s only read of the limit, the pool and input slots the limit sizes and the advance read by disassembly; the opening with frames forced to three vsyncs at cap 2, limit 3 with pool 4 (decode error) and cap 3 (2026-11-16)'},
    {'what': 'movie frame: the pictures the player has taken', 'now': 'game', 'goal': 'game', 'purpose': 'frame rate',
     'why': "the game's own movie draw functions, found by their functions' fingerprints on any build that keeps them; it is the counter the same game's player keeps, so it goes with the player's two-picture cut and has no wider binding",
     'checked': 'v131072 main: one function with each fingerprint, each with one caller, the event update that reads the frame (vtable +0x78), the timeline and sound syncs and the player\'s +0xc read by disassembly; the opening timed with frames forced to two vsyncs (2026-11-16)'},
    {'what': 'frame ratio from the host\'s step', 'now': 'game', 'goal': 'game', 'purpose': 'frame rate',
     'why': "the game's own frame-ratio division, found by its function's fingerprint on any build that keeps the function",
     'checked': 'v131072 main: one function with this fingerprint, the site and its free registers read by disassembly (2026-11-08)'},
    {'what': 'dynamic resolution: full size, or a climb on the load measured at each level', 'now': 'game', 'goal': 'game', 'purpose': 'rendering',
     'why': "the game's own dynamic-resolution level function and update, found by their functions' fingerprints on any build that keeps both; switchable (DM_DYNAMIC_RESOLUTION)",
     'checked': 'v131072 main: one function with each fingerprint (the level function a leaf, x2..x11 / x16 / x17 / s0..s2 free), its callers, the update and the controller fields read by disassembly, the fields read in play (2026-11-08); the climb run natively against a model of the update and in play to full size (2026-11-09)'},
    {'what': 'screen shape wider than 16:9: Hor+ camera projections, render-view aspect, centered 16:9 2D layouts and letterboxed movies (DM_SCREEN_ASPECT)', 'now': 'build', 'goal': 'game', 'purpose': 'rendering',
     'why': "the output aspect is the host's, but the projection, render-view and layout sites are v131072 main's fixed offsets",
     'blocked': 'the stubs have no function-matched form yet'},
]
