# The Legend of Zelda: Ocarina of Time (0400486024DEC000): the rewriter's patches of this game's own code. dm/tools/patch_nso.py loads every profile's patches.py;
# MATCH_FRAME_STEP as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
from nso_asm import ADDI, ADRP, B

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
    screen_shape(ctx)

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

# Screen shape (game.tsv screen_aspect any; the loader writes state +0xa0 the ratio, 0 = 16:9, +0xa4 the width factor, +0xa8 the height factor: loader.c screen_any_write).
# The game's output size, the picture its upscaler writes and the window's crop (2048x1152 docked, 1536x864 or 1920x1080 handheld on v131072), is set in one place: the setter
# main+0x1087f40 (display settings x0, x1 -> {w, h}) stores it at +0x10, raises bit 21 of +0x8 when it changed (main+0x6502e0 then hands it to the window's setCrop, main+0xe9a860,
# which nvnWindowSetCrop takes at each present) and classifies it (+0x18: 640x480 .. 3840x2160, 7 for any other size, as 2048x1152 already is). Its two callers are the settings'
# init (main+0x1087b70, the size from its config struct) and the mode change (main+0x6502e0: a level's size × its scale, capped by a table). The dynamic resolution scales from the
# output height by one factor for both axes (main+0x31d3f64..), so its sizes follow the output's shape. The stub scales the requested size by the factors (the width for a wider
# screen, the height for a taller one); when that is more than 1.5 times the requested size's pixels (beyond 2.67:1 or 1.19:1, a portrait window) it scales both sides down to
# 1.5 times, so the pixel count stays in bounds whatever the shape. Each side is rounded to a multiple of 8, into state +0xe0 (scratch, never the caller's struct: the init passes
# its config), and the setter runs on it. It also copies the camera's multipliers for that shape (state +0xc4 / +0xc8, from the loader) to +0xb8 / +0xbc (0 at 16:9): the camera
# (camera_projection) follows the size the game renders at, not the shape the host asks for, as the game sets its size only at its init, at a mode change and when shape_change
# asks it to. A leaf: x9 / x10 / x16 / s16..s19 are free (the setter loads w8..w11 before it reads them; s16..s19 are caller-saved), x1 an argument it only reads
size_stub = (   # size_stub.s, assembled with clang; x16 the state page (the ADRP before it)
    0xbd40a210, 0x1e202208, 0x54000061, 0xf9005e1f, 0x1400001b,   # ratio (+0xa0); 0 (16:9): no camera multipliers (+0xb8 = 0), the setter as it is
    0x29402829, 0x1e230132, 0x1e230153, 0x2d58c610, 0x2d174610, 0x2d54c610,   # w, h; the camera's multipliers to +0xb8 / +0xbc; the size factors
    0x1e300a50, 0x1e310a71,                           # w × width factor, h × height factor
    0x1e330a52, 0x1e2f1013, 0x1e330a52, 0x1e310a13, 0x1e322260, 0x540000ad,   # more than 1.5 × the requested pixels?
    0x1e331a52, 0x1e21c252, 0x1e320a10, 0x1e320a31,   # both sides × sqrt(limit / pixels)
    0x1e390209, 0x11001129, 0x121d7129, 0x1e39022a, 0x1100114a, 0x121d714a,   # each to a multiple of 8
    0x291c2a09, 0x91038201)                           # into state +0xe0; x1 -> it
def screen_shape(ctx):
    here, emit, fs_site, fs_put, FS_STATE_OFF = ctx.here, ctx.emit, ctx.site, ctx.put, ctx.state
    site = ctx.others.get('output size')
    if site is None: return   # not found in this build (a red line from the locator): 16:9
    fs_site(site, '08244229')   # ldp w8,w9,[x0,#0x10]
    start = here(); emit(ADRP(16, here(), FS_STATE_OFF))
    for w in size_stub: emit(w)
    emit(0x29422408); emit(B(here(), site + 4))   # ldp w8,w9,[x0,#0x10]; back
    fs_put(site, [B(site, start)], 'output size: the requested size times the screen shape\'s width and height factors (state +0xa4 / +0xa8; +0xa0 0 = 16:9, as it is)')
    window_size(ctx)
    camera_projection(ctx)
    hud(ctx)

# The window textures. The game creates its three window textures at 2560x1440 (the window object's +0x8 / +0xc floats; defaults main+0xecbc80), and a crop larger than them
# (2048x1576 at 1.3:1, 2688x1152 at 21:9) cannot be presented. The game re-creates its window and textures from +0x8 / +0xc when +0xc8 is 1: main+0x527fc0, which the
# per-frame texture acquire (main+0x527f20, main+0x346c9b0) tail-calls and which returns at once while +0xc8 is 0. The stub at the entry of setCrop (main+0xe9a860, a leaf:
# win x0, x w1, y w2, w w3, h w4) raises +0x8 / +0xc to hold the crop when it does not fit and sets +0xc8, so the next frame's acquire re-creates the textures at that size. It
# only grows them: a later, smaller crop is presented from the larger textures without another re-creation
crop_stub = (   # crop.s, assembled with clang
    0x2d414410, 0x0b030029, 0x0b04004a, 0x1e230132, 0x1e230153,   # the window's size; the crop's right and bottom edges
    0x1e306a52, 0x1e316a73, 0x1e302240, 0x54000061, 0x1e312260, 0x54000080,   # the larger of each; both the same: as it is
    0x2d014c12, 0x52800029, 0x39032009)               # the window's new size; +0xc8 = 1: re-create at the next acquire
def window_size(ctx):
    here, emit, fs_site, fs_put = ctx.here, ctx.emit, ctx.site, ctx.put
    site = ctx.others.get('window crop')
    if site is None: return   # not found in this build: crops beyond 2560x1440 are not presented
    fs_site(site, '28008052')   # mov w8,#1
    start = here()
    for w in crop_stub: emit(w)
    emit(0x52800028); emit(B(here(), site + 4))   # mov w8,#1; back
    fs_put(site, [B(site, start)], 'window crop: the window textures grow to hold a crop beyond their size (+0x8 / +0xc, re-created at the next acquire through +0xc8)')

# The camera's projection. Each frame the game finds the camera named "Main" and hands its view and its sead::PerspectiveProjection (x2: near +0x98, far +0x9c, fovy +0xa0,
# aspect +0xb0, the cached matrix at +0xc) to main+0x33a8c0, which copies the projection's matrix into the render camera (x19 +0x148, row major; main+0x33ab7c..0x33abac) and
# stores near / far / fovy / aspect beside it (+0x1c8..+0x1d4), then derives the frustum from the rows it holds in v4..v18. The projection's aspect is 16:9 whatever the output's
# shape (sead::PerspectiveProjection::set, tanf and the 16/9 literals do not run in play: found with DM_PROBE_FINDMAT and a write watch on the render camera's matrix). After
# the last row is stored the stub multiplies row 0 (x) and row 1 (y) by the camera's multipliers, in memory and in v4 / v5, and the aspect at +0x1d4 by y's over x's. The loader
# chooses them (loader.c screen_any_write): landscape wider than 16:9 keeps the vertical angle and shows more at the sides; landscape between 1:1 and 16:9 keeps the horizontal
# angle and shows more above and below; portrait takes the scale of the same screen turned landscape (its width gets what that one's height gets), so turning a phone or a window
# keeps the characters' size and shows another part of the world. They are those the output size was made with (state +0xb8 / +0xbc, copied by size_stub; 0 = 16:9), so the
# projection always matches the picture the game renders. The fcmp at main+0x33ab78
# is still live (b.eq at +0x33abb0): the stub tests with cbz and keeps the flags. w9 (reloaded at +0x33abbc), v6 and v29 (written before they are read) are free
proj_stub = (   # proj.s, assembled with clang; x16 the state page (the ADRP before it)
    0xb940ba09, 0x340001c9,                           # the camera's x multiplier (+0xb8); 0 (16:9): as it is
    0xbd40ba06, 0xbd41d67d, 0x1e261bbd, 0xbd40be06, 0x1e260bbd, 0xbd01d67d,   # aspect ÷ x multiplier × y multiplier
    0x4e0404c6, 0x6e26dca5, 0x3d800685,               # row 1 × y multiplier
    0xbd40ba06, 0x4e0404c6, 0x6e26dc84, 0x3d800284)   # row 0 × x multiplier
def camera_projection(ctx):
    here, emit, fs_site, fs_put, FS_STATE_OFF = ctx.here, ctx.emit, ctx.site, ctx.put, ctx.state
    fn = ctx.others.get('camera copy')
    if fn is None: return   # not found in this build (a red line from the locator): the projection stays 16:9
    site = fn + 0x2ec
    fs_site(site, '920e803d')   # str q18,[x20,#0x30]: the matrix's last row
    start = here(); emit(0x3d800e92); emit(ADRP(16, here(), FS_STATE_OFF))
    for w in proj_stub: emit(w)
    emit(B(here(), site + 4))
    fs_put(site, [B(site, start)], 'camera projection: matrix rows x / y times the camera multipliers the output size was made with, the aspect beside it (state +0xb8 / +0xbc; 0 = 16:9)')
    shape_change(ctx)

# A shape asked for while playing (a window resized): the display settings' update main+0x6502e0 (x0 the settings, the same object as the size setter's) recomputes the output size
# and hands it to the size setter (main+0x650600) when bit 17 (0x20000) of its flags (+0x8) is set, and clears the bit after; the mode change path sets it (main+0x6506a4). The
# stub, after the update loads its flags (ldp w11,w9,[x0,#8] at +0x18), compares the loader's change count (state +0xac) with the count last applied (+0xc0) and, when they
# differ, records it and sets bit 17 in w11, which the update stores back (main+0x650348) and tests (main+0x65036c): the size, and with it the crop, the targets and the camera
# (camera_projection), follow the new shape at the next update. w10 and w12 are written right after (+0x1c, +0x20); the flags are set again by the tst at +0x34
trig_stub = (   # trig.s, assembled with clang; x16 the state page (the ADRP before it)
    0xb940ae0a, 0xb940c20c, 0x6b0c015f, 0x54000060,  # the change count (+0xac) against the one applied (+0xc0); the same: as it is
    0xb900c20a, 0x320f016b)                           # recorded; flags bit 17: recompute the output size
def shape_change(ctx):
    here, emit, fs_site, fs_put, FS_STATE_OFF = ctx.here, ctx.emit, ctx.site, ctx.put, ctx.state
    fn = ctx.others.get('display update')
    if fn is None: return   # not found in this build: the shape follows only at start and at a mode change
    site = fn + 0x18
    fs_site(site, '0b244129')   # ldp w11,w9,[x0,#8]
    start = here(); emit(0x2941240b); emit(ADRP(16, here(), FS_STATE_OFF))
    for w in trig_stub: emit(w)
    emit(B(here(), site + 4))
    fs_put(site, [B(site, start)], 'screen shape change: the display update recomputes the output size when the loader\'s change count (state +0xac) moved since the last (+0xc0)')

# The HUD and menus (eui, Nintendo's newer UI: layouts UI/Layout2Archive/*.blarc.zs). Every layout file holds its screen size, 3840x2160 (LayoutResData +0x200, file offset 0x2f8), and
# the game reads it in place through one getter (main+0x583fd0: [x0] + 0x200). A screen's init (main+0x773b90, a virtual) builds its orthographic projection from it (+-w/2,
# +-h/2, main+0x773f28) and keeps it in the screen's uniform block (+0x1b8: view-projection +8 and its y-flipped copy +0x48, column major, the size twice at +0x88, main+0x7741e0);
# each frame the screen's draw copies the block's 0x90 bytes into the frame's ring (main+0x2d2ba0, its memcpy at +0x48), so the HUD drew in a 16:9 space stretched over any
# output. The stub after that memcpy scales the copy's x / y rows by the HUD's multipliers and divides the size by them: the space takes the output's shape at the HUD's pixel
# size, at the next frame after a shape change and without touching the screen's block. A capture's block (main+0x154b660) holds its texture size beside the screen's and
# is left as it is. The HUD's multipliers are the camera's (state +0xb8 / +0xbc, those the output size was made with) divided by the largest of them and 1: in landscape the
# camera's, so the HUD keeps the size it has at 16:9 of the same height (wider) or width (taller); upright, the 16:9 space still fits the width, so nothing is cut off. The
# layouts place everything about the screen's centre (eui does not anchor panes to the screen's edges: a layout made 4752 wide in a copy of its file drew round hearts at
# their 16:9 place), so the HUD keeps its 16:9 arrangement in the middle; moving the edge groups (60x80 nulls at the space's edges, as PlayerStatus_00's N_ScaleL_00 at
# (-1920, 1078)) and stretching full-screen panes (3840x2160 at the centre: fades, menu backgrounds) is open (game note, Screen shape)
hud_upload = (   # hud.s, assembled with clang; x0 the copy (memcpy's result), x20 the block, x16 the state page (the ADRP before it)
    0xa948aa89, 0xeb0a013f, 0x54000441, 0xb4000429, 0xb940ba09, 0x340003e9,   # the size twice (a screen's own projection) and a shape other than 16:9
    0x2d574610, 0x1e316a12, 0x1e2e1013, 0x1e336a52, 0x1e321a10, 0x1e321a31,   # the camera's multipliers / max(mx, my, 1)
    0x4f03f614, 0x6e040614, 0x6e0c0634,                                       # (mx', my', 1, 1)
    0xad405815, 0x6e34deb5, 0x6e34ded6, 0xad005815, 0xad415815, 0x6e34deb5, 0x6e34ded6, 0xad015815,   # both matrices' columns: rows x / y scaled
    0xad425815, 0x6e34deb5, 0x6e34ded6, 0xad025815, 0xad435815, 0x6e34deb5, 0x6e34ded6, 0xad035815,
    0x2d504c12, 0x1e301a52, 0x1e311a73, 0x2d104c12, 0x2d114c12)               # the size / the multipliers, both copies
# The game's other conversions between the layout space and the screen (a pane's rect in screen coordinates, a 3D position's place on the HUD) read the same getter: the
# stub at its entry returns the size the HUD draws with (state +0xd0, scratch), except to the screen's init (its two calls, main+0x773ee0 / +0x773f24), whose projection the
# upload stub widens each frame. A leaf: x8..x10, x16 and s16..s21 are free
layout_size = (   # lsize.s, assembled with clang; x16 the state page, x10 the init's first return address (the ADRP / ADD before it)
    0xf9400008, 0x91080100, 0xb940ba09, 0x34000229,                           # the layout's size; 16:9: as it is
    0xeb0a03df, 0x540001e0, 0x9101114a, 0xeb0a03df, 0x54000180,               # the screen's init: as it is
    0x2d404410, 0x2d574e12, 0x1e336a54, 0x1e2e1015, 0x1e356a94, 0x1e341a52, 0x1e341a73,   # the HUD's multipliers
    0x1e321a10, 0x1e331a31, 0x91034200, 0x2d004410, 0xd65f03c0)               # the size / them, into state +0xd0; x0 -> it
def hud(ctx):
    here, emit, fs_site, fs_put, FS_STATE_OFF = ctx.here, ctx.emit, ctx.site, ctx.put, ctx.state
    fn = ctx.others.get('hud upload')
    if fn is not None:   # not found in this build (a red line from the locator): the HUD stays 16:9, stretched over the output
        site = fn + 0x4c
        fs_site(site, 'e00315aa')   # mov x0,x21 (after the memcpy)
        start = here(); emit(ADRP(16, here(), FS_STATE_OFF))
        for w in hud_upload: emit(w)
        emit(0xaa1503e0); emit(B(here(), site + 4))   # mov x0,x21; back
        fs_put(site, [B(site, start)], 'HUD projection: the screen\'s uniform block copy takes the output\'s shape at the HUD\'s pixel size (state +0xb8 / +0xbc; 0 = 16:9)')
    fn, init = ctx.others.get('layout size'), ctx.others.get('screen init')
    if fn is None or init is None: return   # the game's own layout size for its conversions
    fs_site(fn, '080040f9')   # ldr x8,[x0]
    start = here(); emit(ADRP(16, here(), FS_STATE_OFF)); emit(ADRP(10, here(), init + 0x354)); emit(ADDI(10, 10, (init + 0x354) & 0xfff))
    for w in layout_size: emit(w)
    fs_put(fn, [B(fn, start)], 'layout size: the size the HUD draws with for the game\'s conversions to and from the screen (state +0xd0), the screen\'s init as it is')

MATCH_FRAME_STEP = {'main': [
    {'name': 'frame ratio from the host\'s step', 'size': 0x9b4, 'fingerprints': ['6ae2aff5344d14568e4dbaf93455e2d8278384bf892ed6c6eca87fef98fbcd1d'], 'stub': frame_step,   # v131072 main+0x6e6be0
     'others': {'dynamic resolution': {'size': 0x294, 'fingerprints': ['7af6ceb0aedf8e0c17dc08ce17fcd6a7cba6d394a0d09e3b504c6d668dc418e7']},   # v131072 main+0x8065e0
                'dynamic resolution update': {'size': 0xb24, 'fingerprints': ['5a423697838976baf286cffa5e2ceef0d4e037abbcd9d371e90e9e702d61b3d4']},   # main+0x805aa0
                'output size': {'size': 0xd8, 'fingerprints': ['9e24f74affcfca6fb386818db0e5a9d4c9e3c57aee7b35c5afff3abed93a58a4']},   # main+0x1087f40
                'camera copy': {'size': 0x1648, 'fingerprints': ['66a50abf2237dee694c40d6a2371071d62e91379d40b643869d680f597bfab02']},   # main+0x33a8c0
                'display update': {'size': 0x554, 'fingerprints': ['c0cab0dbf6f0644635315a356f8fd23a4701515c4ab4775d52191835b1a12e3c']},   # main+0x6502e0
                'window crop': {'size': 0x14, 'fingerprints': ['a6116ba396321af2dacdfc1b236bbf1103285a96b92db1c2c6cc3f1aed190f6d']},   # main+0xe9a860
                'hud upload': {'size': 0x70, 'fingerprints': ['7cf489e6e9cc18b56c67f82ba4ca22e55abbd592d21309873d6803d19e0e7872']},   # main+0x2d2ba0
                'screen init': {'size': 0x62c, 'fingerprints': ['103de33fb929cda858c2fbd57b726e6e1bca705bad66de872701819fbf5c6089']},   # main+0x773b90
                'layout size': {'size': 0xc, 'fingerprints': ['d451096b59a2483a7e026bd2764333eecd505ea3aad2b3f8d902aa20ba5b02f7']}}},   # main+0x583fd0
]}

BINDING = [
    {'what': 'frame ratio from the host\'s step', 'now': 'game', 'goal': 'game', 'purpose': 'frame rate',
     'why': "the game's own frame-ratio division, found by its function's fingerprint on any build that keeps the function",
     'checked': 'v131072 main: one function with this fingerprint, the site and its free registers read by disassembly (2026-11-08)'},
    {'what': 'dynamic resolution: full size, or a climb on the load measured at each level', 'now': 'game', 'goal': 'game', 'purpose': 'rendering',
     'why': "the game's own dynamic-resolution level function and update, found by their functions' fingerprints on any build that keeps both; switchable (DM_DYNAMIC_RESOLUTION)",
     'checked': 'v131072 main: one function with each fingerprint (the level function a leaf, x2..x11 / x16 / x17 / s0..s2 free), its callers, the update and the controller fields read by disassembly, the fields read in play (2026-11-08); the climb run natively against a model of the update and in play to full size (2026-11-09)'},
    {'what': 'screen shape: the output size, the window textures, the camera projection and a live shape change (screen_aspect any)', 'now': 'game', 'goal': 'game', 'purpose': 'rendering',
     'why': "the game's own size setter, setCrop, camera copy and display update, found by their functions' fingerprints on any build that keeps them; switchable (DM_SCREEN_ASPECT)",
     'checked': 'v131072 main: one function with each fingerprint, the sites and their free registers read by disassembly, the camera matrix and its writer found in play with DM_PROBE_FINDMAT and the write watch, the stubs run natively (test_oot_aspect.py) and on the Mac at six window shapes resized live (2026-11-09)'},
    {'what': "HUD: the screens' orthographic projection and the layout size take the output's shape", 'now': 'game', 'goal': 'game', 'purpose': 'rendering',
     'why': "the screen draw's uniform block copy and the layout-size getter, found by their functions' fingerprints with the screen init beside them",
     'checked': 'v131072 main: one function with each fingerprint, the block layout and its per-frame copy read by disassembly, the 3840x2160 size read in play with probes and changed in a copy of a layout file, the stubs run natively (test_oot_aspect.py) and on the Mac at six shapes, the pause menu and a cutscene (2026-11-09)'},
]
