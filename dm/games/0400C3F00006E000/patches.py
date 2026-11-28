# Mario Kart World (0400C3F00006E000): the rewriter's patches of this game's own code. dm/tools/patch_nso.py loads every profile's patches.py;
# MATCH_FRAME_STEP as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
import struct
from nso_asm import ADRP, B

# The framework's frame interval. Once a frame the function at main+0x3a9e28 (v0; main+0x69cbb0 on 1.8.0; x19 the framework) picks the interval in vsyncs at [x19+0x214]: its frame
# mode at [x19+0x210] (+0xa0; +0xa4 on 1.8.0) is 1 = interval 1, 2 = interval 2 (the game's 30 fps mode), 0 = chosen from the frame's CPU time alone ([x19+0x128], the time to the end of the calc call; the whole
# frame's is [x19+0xe8]), so a frame that waits on the GPU keeps interval 1. The interval is added to the vsync counter the game steps by (the global at main+0x33e6ae8) and set
# as the present interval (1.8.0: main+0x37c8438). On a Mac whose frames take longer than a vsync the game played in slow motion (seen by the user, 2026-11-08). The stub hands the interval the host's step
# instead, at most 2 (the game's own intervals), and counts the vsyncs stepped; the game's 30 fps mode stays its own and is published as a step the game fixes (state +0x24), so the
# host holds those frames 2 vsyncs. Until the host has decided a step, and at fps_target 0, the game's own choice runs. x8, x16 and x17 are free at the site (x8 is loaded there;
# the function uses neither x16 nor x17). Lichii (a GPL emulator; only the facts were taken) found the mode load (its commit 8f70204825: ldr w8,[x19,#0x210], 2 = 30 fps,
# CPU time at +0x128, whole frame at +0xe8). 1.8.0's function is one word shorter (0x28c) with its own fingerprint, and the site is 4 bytes on (the mode load at +0xa4, the
# interval store at +0x140, 0x9c after it as on v0; found as the only such pair in its main, no v0 main being at hand to map from); branches reach the site itself (+0x264,
# +0x278) and the store (+0x214, +0x230), none the instruction after the site, and it uses neither x16 nor x17. The rule could not move off the fingerprints: of its calls,
# nn::vi::QueryDisplayHdrStatus and QueryDisplayHdrPaperWhite have two callers each (so no rule anchored on an import) and GetSystemTick / LockMutex hundreds
def frame_step(ctx, at=0xa0):
    here, emit, fs_site, fs_put, FS_STATE_OFF, base = ctx.here, ctx.emit, ctx.site, ctx.put, ctx.state, ctx.base
    site, interval = base + at, base + at + 0x9c
    ctx.step_max = 2   # the stub caps the step at the game's own intervals: the host holds frames at most 2 vsyncs, or hold 3 played at 2/3 speed and the debt it paid back was lost
    fs_site(site, '681242b9')       # ldr w8,[x19,#0x210]
    fs_site(interval, '681602b9')   # str w8,[x19,#0x214]: the interval, then the frame goes on
    start = here(); emit(ADRP(16, here(), FS_STATE_OFF))
    for w in (0xb9400211,   # ldr w17,[x16]          target fps (0 = off)
              0x34000211,   # cbz w17,orig (+16)
              0xb9421268,   # ldr w8,[x19,#0x210]    the game's frame mode
              0x7100091f,   # cmp w8,#2
              0x54000100,   # b.eq fixed (+8)        its 30 fps mode
              0xb900261f,   # str wzr,[x16,#0x24]    no step the game fixes
              0xb9402211,   # ldr w17,[x16,#0x20]    step in vsyncs
              0x34000151,   # cbz w17,orig (+10)     not decided yet
              0x52800048,   # mov w8,#2
              0x71000a3f,   # cmp w17,#2
              0x1a889228,   # csel w8,w17,w8,ls      at most 2
              0x14000002,   # b count (+2)
              0xb9002608,   # fixed: str w8,[x16,#0x24]   the host holds 2 vsyncs
              0xf9401a11,   # count: ldr x17,[x16,#0x30]
              0x8b284231,   # add x17,x17,w8,uxtw
              0xf9001a11):  # str x17,[x16,#0x30]    vsyncs stepped
        emit(w)
    emit(B(here(), interval))
    emit(0xb9421268); emit(B(here(), site + 4))   # orig: ldr w8,[x19,#0x210]; back
    fs_put(site, [B(site, start)], 'frame interval: the host\'s step, at most 2, unless the game\'s 30 fps mode is set (state page target fps; 0 = the game\'s choice by CPU time)')

def screen_shape(ctx):
    """Screen shape (DM_SCREEN_ASPECT, the launcher's Picture shape; Tears of the Kingdom 1.4.3's method, its patches.py screen_shape): a screen taller than 16:9 (the iPhone
    Duo's, about 1.43:1) gets a 3D picture of its own shape and the 2D layer in a 16:9 band across the middle. 1.8.0 (v917504) main only, at its own addresses. State +0x48 /
    +0x4c output / layout height, +0x54 size-site mask (DM_SCREEN_ASPECT_SITES), +0x58 the screen's ratio (0 = off), +0x5c k = (16/9) / ratio, +0x60 tolerance, +0xfc / +0x100
    the last widened fovys (loader.c); +0x120 / +0x124 / +0x130.. counters, logged by hle_nvn.c as "screen shape"."""
    here, emit, fs_site, fs_put, FS_STATE_OFF = ctx.here, ctx.emit, ctx.site, ctx.put, ctx.state
    ctx.capabilities.add('SCREEN_ASPECT')   # the launcher's Picture shape (settings.tsv fixups:SCREEN_ASPECT)
    # Output size: the 1920x1080 / 1280x720 pairs (movk xN,#1080 or #720,lsl #32) take the host's height while their bit of +0x54 is set (x16 free at each: checked)
    OUT, LAY = 0xb9404a10, 0xb9404e10
    def taller_bit(site, reg, bit, load, why):
        orig = 0xf2c00000 | (0x438 if load == OUT else 0x2d0) << 5 | reg
        fs_site(site, struct.pack('<I', orig).hex())
        start = here(); emit(orig); emit(ADRP(16, here(), FS_STATE_OFF))
        emit(0xb9405610); emit(0x36000000 | bit << 19 | 5 << 5 | 16)   # ldr w16,[x16,#0x54]; tbz w16,#bit,back (+5)
        emit(ADRP(16, here(), FS_STATE_OFF))
        for w in (load, 0x34000050, 0xb3607e00 | reg): emit(w)         # ldr w16,[x16,#off]; cbz w16,+8; bfi xN,x16,#32,#32
        emit(B(here(), site + 4))
        fs_put(site, [B(site, start)], why)
    for bit, (site, reg, load) in enumerate(((0x40e17c, 9, OUT), (0x697fc4, 9, OUT), (0xb87bc8, 8, OUT), (0xd37518, 8, OUT), (0xd37524, 8, OUT),
                                             (0x697f6c, 8, LAY), (0xb87ba0, 8, LAY), (0xd374f8, 8, LAY), (0xdcabe8, 10, LAY), (0xde4f30, 11, LAY), (0xdeee24, 9, LAY),
                                             (0xeea284, 8, LAY), (0x1680f8c, 8, LAY), (0x1ec2a38, 0, LAY), (0x21dc660, 8, LAY), (0x265790c, 23, LAY), (0x2657be0, 8, LAY))):
        taller_bit(site, reg, bit, load, f'screen shape site {bit}: {"1920x1080" if load == OUT else "1280x720"} → the host\'s height')
    # The display mode table (main+0xb87b94..: 1280x720, 1920x1080, 2560x1440, 3840x2160, the Switch 2's output sizes; docked is 3840x2160): after its three pairs are built
    # (main+0xb87ba8, movk x10,#1440), each takes the screen's shape: 3840 wide twice the host's 1920-wide height, 2560 wide 4/3 of it, 1280 wide the layout height (state
    # +0x48 / +0x4c; 0 = off). Assembled with clang; x16 / x17 free (stores follow, then a load chain). Also the other 3840x2160 pair (main+0xdce854, x10): twice the height
    RES = (0xF2C0B40A, 'adrp', 0xB9404A11, 0x34000151, 0x531F7A30, 0xB3607E09, 0x531E7630, 0x52800071, 0x1AD10A10, 0xB3607E0A, 'adrp', 0xB9404E10, 0xB3607E08, 'back')
    site = 0xb87ba8
    fs_site(site, struct.pack('<I', 0xF2C0B40A).hex())
    start = here()
    for w in RES:
        if w == 'adrp': emit(ADRP(16, here(), FS_STATE_OFF))
        elif w == 'back': emit(B(here(), site + 4))
        else: emit(w)
    fs_put(site, [B(site, start)], 'screen shape: the display mode table\'s 1280x720 / 2560x1440 / 3840x2160 take the screen\'s shape')
    site = 0xdce854
    fs_site(site, struct.pack('<I', 0xf2c10e0a).hex())   # movk x10,#0x870,lsl #32
    start = here(); emit(0xf2c10e0a); emit(ADRP(16, here(), FS_STATE_OFF))
    for w in (0xB9404A11, 0x34000071, 0x531F7A30, 0xB3607E0A): emit(w)   # ldr w17,[x16,#0x48]; cbz w17,back (+3); lsl w16,w17,#1; bfi x10,x16,#32,#32
    emit(B(here(), site + 4))
    fs_put(site, [B(site, start)], 'screen shape: a 3840x2160 pair takes twice the host\'s height')
    # The window crop (main+0x69d110: nvnWindowSetCrop's x, y, width, height from the frame object's +0xac): a 16:9 crop (the game's render size, 2560x1440 docked on the
    # Mac) was presented over the taller window, stretched. Its height takes the screen's shape, width x (host height / 1920). Assembled with clang; x16 / x17 free up to the
    # blr, whose callee needs no flags
    CROP = (0x29569283, 'adrp', 0xB9404A11, 0x34000171, 0x52800130, 0x1B107C70, 0x531C6C91, 0x6B11021F, 0x540000C1, 'adrp', 0xB9404A11, 0x1B117C70, 0x5280F011, 0x1AD10A04, 'back')
    site = 0x69d114
    fs_site(site, struct.pack('<I', 0x29569283).hex())
    start = here()
    for w in CROP:
        if w == 'adrp': emit(ADRP(16, here(), FS_STATE_OFF))
        elif w == 'back': emit(B(here(), site + 4))
        else: emit(w)
    fs_put(site, [B(site, start)], 'screen shape: a 16:9 window crop takes the screen\'s shape')
    # The render size (main+0x471938: the packed width / height at the frame object's +0x10, stored at +0x228 and set as the window crop; 2560x1440 docked on the Mac,
    # found by DM_PROBE_WATCH on the crop): a 16:9 size takes the screen's shape, width x (host height / 1920), before both uses. Assembled with clang; x16 / x17 free and
    # no flags read up to the branch back
    # Also the render scale (state +0x10, DM_RENDER_SCALE 0.5 to 1.0, the launcher's Render scale): both sizes x the scale, multiples of 8, so the picture the game
    # renders and the window crop that shows it scaled up both follow (heat on the phone). x9, x11, x12 (written after the site), x16 / x17 and s16 / s17 (unused in the
    # function) free
    ctx.render_scale = 1
    RS = (0xd360fd49, 0x2a0a03eb, 'adrp', 0xb9404a11, 0x34000111, 0x5280012c, 0x1b0c7d6c, 0x6b09119f, 0x54000081, 0x1b117d69, 0x5280f00c, 0x1acc0929, 0xbd401210, 0x1e2e1011, 0x1e312200, 0x5400016a, 0x1e202208, 0x5400012d, 0x1e230171, 0x1e300a31, 0x1e39022b, 0x121d716b, 0x1e230131, 0x1e300a31, 0x1e390229, 0x121d7129, 0x2a0b03ea, 0xb3607d2a, 0xd360fd49, 'back')
    site = 0x471938
    fs_site(site, struct.pack('<I', 0xD360FD49).hex())   # lsr x9,x10,#32
    start = here()
    for w in RS:
        if w == 'adrp': emit(ADRP(16, here(), FS_STATE_OFF))
        elif w == 'back': emit(B(here(), site + 4))
        else: emit(w)
    fs_put(site, [B(site, start)], 'screen shape: a 16:9 render size takes the screen\'s shape, and the render size takes the render scale')
    # The scene's frame buffer (main+0x474440, its sizes set from a physical size x1 (floats) and a virtual one x2 (ints), copied to the object's +0x20..): the race scene's
    # target stays 2560x1440 (1920x1080 handheld) while its viewport comes in the taller layout (1280x896), so the viewport became 0,-352 2560x1792 on the 1440 target (found by
    # DM_VP_TRACE, the viewport code main+0x78bec, and DM_PROBE_WATCH on the frame buffer): the scene was cut at the top and drawn 1.24 times too tall. A virtual 1280x720 over a
    # 16:9 physical size takes the layout height (state +0x4c), stored in the caller's size too, so the viewport fills the 16:9 target, which the picture's composite stretches to
    # the screen's shape. Assembled with clang; a leaf: x9 / x10 / x16 / x17 and s16 / s17 free, flags set again before they are read
    FBV = ('adrp', 0xB9404E10, 0x340001D0, 0xF9400051, 0xD280A009, 0xF2C05A09, 0xEB09023F, 0x54000121, 0x2D404430, 0x1E390209, 0x1E39022A, 0x531C6D4A, 0x0B090D29,
           0x6B0A013F, 0x54000041, 0xB9000450, 0xF943D008, 'back')
    site = 0x474440
    fs_site(site, struct.pack('<I', 0xF943D008).hex())   # ldr x8,[x0,#0x7a0]
    start = here()
    for w in FBV:
        if w == 'adrp': emit(ADRP(16, here(), FS_STATE_OFF))
        elif w == 'back': emit(B(here(), site + 4))
        else: emit(w)
    fs_put(site, [B(site, start)], 'screen shape: a 16:9 frame buffer\'s 1280x720 virtual size takes the layout height, so the taller layout\'s viewports fill it')
    # The cameras' aspect: every store of a projection's aspect (+0xb0, with its changed flag +8) near 16:9, and the aspect argument of sead::PerspectiveProjection's set()
    # and constructor, take the screen's ratio (Ocarina of Time's wide_view method, as Tears of the Kingdom's): (bits - bits(16/9 - 0.0098)) >> 18 == 0. Counted per site
    def cnt(off): return (0xb9400211 | (off // 4) << 10, 0x11000631, 0xb9000211 | (off // 4) << 10)
    def aspect_site(k, site, kind, n, b, why):
        orig = {'store': 0xbd00b000 | b << 5 | n, 'arg': 0x1e204068}[kind]
        apply = 0x1e270228 if kind == 'arg' else 0xb900b011 | b << 5
        fs_site(site, struct.pack('<I', orig).hex())
        start = here(); emit(ADRP(16, here(), FS_STATE_OFF))
        for w in cnt(0x130 + 4 * k): emit(w)
        for w in (0x1e260011 | n << 5, 0x52800010 | 0x4e39 << 5, 0x72a00010 | 0x3fe2 << 5, 0x4b100231, 0x53127e31, 0x35000011 | 9 << 5): emit(w)
        emit(ADRP(16, here(), FS_STATE_OFF))
        for w in (0xb9405a11, 0x34000011 | 9 << 5, apply) + cnt(0x124): emit(w)
        emit(B(here(), site + 4))
        emit(ADRP(16, here(), FS_STATE_OFF)); emit(0x1e260011 | n << 5); emit(0xb9012a11)
        emit(orig); emit(B(here(), site + 4))
        fs_put(site, [B(site, start)], f'screen shape: a 16:9 projection aspect set here takes the screen\'s ratio (site {k}: {why})')
    for k, (site, kind, n, b, why) in enumerate((
            (0x474080, 'store', 0, 25, 'view update, from camera parameters +0x218'), (0x5f5ff4, 'store', 0, 24, 'from a virtual getter'),
            (0xdefb28, 'store', 8, 22, 'a constructor\'s aspect'), (0x8a6d8, 'store', 0, 8, 'a viewport\'s ratio'), (0x8a788, 'store', 0, 11, 'a viewport\'s ratio'),
            (0xdefdd8, 'store', 0, 13, 'a viewport\'s ratio'), (0xd2fcac, 'arg', 3, 0, 'sead::PerspectiveProjection::set'),
            (0xd2e5b4, 'arg', 3, 0, 'sead::PerspectiveProjection constructor'))):
        aspect_site(k, site, kind, n, b, why)
    # The field of view: Breath of the Wild's / Tears of the Kingdom's stubs, the same code here (setFovy_, the constructor, set()); tanf / atanf through 1.8.0's PLT
    TANF, ATANF = 0x2badea0, 0x2badfb0
    FOV_BODY = (None, 0xbd405a01, 0x1e202028, 0x540003e0, 'ratio', 0x1e213842, 0x1e20c042, 0xbd406203, 0x1e232040, 0x5400032a,
                0xb940a269, 0x9104020a, 0x5280080b, 0xb840454c, 0x6b09019f, 0x54000260, 0x7100056b, 0x54ffff81,
                'half_to_s0', 'tanf', None, 0xbd405e01, 0x1e210800, 'atanf', 's0_to_half', 0x1e202800, 0xbd00a260,
                None, 0xb940fe0b, 0x1200156c, 0x1100056b, 0xb900fe0b, 0x9104020a, 0xbc2c5940, 'half_to_s0')
    def fov_stub(site, orig, aspect_word, half_to_s0, s0_to_half):
        fs_site(site, orig)
        start = here()
        for w in FOV_BODY:
            if w is None: emit(ADRP(16, here(), FS_STATE_OFF))
            elif w == 'tanf': emit(B(here(), TANF) | 0x80000000)
            elif w == 'atanf': emit(B(here(), ATANF) | 0x80000000)
            else: emit({'ratio': aspect_word, 'half_to_s0': half_to_s0, 's0_to_half': s0_to_half}.get(w, w))
        emit(B(here(), site + 4))
        fs_put(site, [B(site, start)], 'sead::PerspectiveProjection fovy widened for a view of the screen\'s ratio (more above and below, 16:9 sides), stored widened')
    fov_stub(0x25aabd4, '0041201e', 0xbd40b262, 0x1e204100, 0x1e204008)   # setFovy_: ratio at [x19,#0xb0], half in s8
    fov_stub(0xd2e624, '0041201e', 0xbd40b262, 0x1e204100, 0x1e204008)    # constructor: aspect stored at +0xb0 before, half in s8
    fov_stub(0xd2fcbc, '2041201e', 0x1e204102, 0x1e204120, 0x1e204009)    # set(): ratio still in s8, half in s9
    # Cameras whose aspect becomes the screen's ratio after their fovy was set (the race camera: its aspect comes from the viewport ratio stores, 16:9's m00 0.8829 became
    # 1.0988 with m11 kept at 1.5697, so the sides were cut and the view stretched): where the projection matrix is built (the five inlined copies and doUpdateMatrix,
    # ldp tan, aspect at +0xac), an aspect of the screen's ratio whose fovy is not one of the widened ones has its tan x k for the matrix. Assembled with clang; x16 / x17 and
    # s16 / s17 free and no flags read after each site (checked). Counts matrices at state +0x120 ("built") and the widened ones at +0x124
    LATE = {
        'x0': (0x2D558801, 0xD503201F, 0xB9412211, 0x11000631, 0xB9012211, 0xBD405A10, 0x1E202208, 0x54000280, 0x1E303851, 0x1E20C231, 0xBD406210, 0x1E302220, 0x540001EA, 0xBD40A010, 0x91040210, 0x52800811, 0xBC404611, 0x1E302220, 0x54000120, 0x71000631, 0x54FFFF81, 0xD503201F, 0xBD405E10, 0x1E300821, 0xB9412611, 0x11000631, 0xB9012611, 0xD503201F),
        'x1': (0x2D558821, 0xD503201F, 0xB9412211, 0x11000631, 0xB9012211, 0xBD405A10, 0x1E202208, 0x54000280, 0x1E303851, 0x1E20C231, 0xBD406210, 0x1E302220, 0x540001EA, 0xBD40A030, 0x91040210, 0x52800811, 0xBC404611, 0x1E302220, 0x54000120, 0x71000631, 0x54FFFF81, 0xD503201F, 0xBD405E10, 0x1E300821, 0xB9412611, 0x11000631, 0xB9012611, 0xD503201F),
        'x21': (0x2D558AA1, 0xD503201F, 0xB9412211, 0x11000631, 0xB9012211, 0xBD405A10, 0x1E202208, 0x54000280, 0x1E303851, 0x1E20C231, 0xBD406210, 0x1E302220, 0x540001EA, 0xBD40A2B0, 0x91040210, 0x52800811, 0xBC404611, 0x1E302220, 0x54000120, 0x71000631, 0x54FFFF81, 0xD503201F, 0xBD405E10, 0x1E300821, 0xB9412611, 0x11000631, 0xB9012611, 0xD503201F),
        'x19': (0x2D558E62, 0xD503201F, 0xB9412211, 0x11000631, 0xB9012211, 0xBD405A10, 0x1E202208, 0x54000280, 0x1E303871, 0x1E20C231, 0xBD406210, 0x1E302220, 0x540001EA, 0xBD40A270, 0x91040210, 0x52800811, 0xBC404611, 0x1E302220, 0x54000120, 0x71000631, 0x54FFFF81, 0xD503201F, 0xBD405E10, 0x1E300842, 0xB9412611, 0x11000631, 0xB9012611, 0xD503201F),
    }
    for site, b in ((0x75b34, 'x0'), (0x2e6508, 'x1'), (0x5b5560, 'x21'), (0x5b57e8, 'x21'), (0xb3e81c, 'x19'), (0x25aac34, 'x0')):
        words = LATE[b]
        fs_site(site, struct.pack('<I', words[0]).hex())
        start = here()
        for i, w in enumerate(words):
            if i in (1, 21): emit(ADRP(16, here(), FS_STATE_OFF))
            elif i == len(words) - 1: emit(B(here(), site + 4))
            else: emit(w)
        fs_put(site, [B(site, start)], 'screen shape: a projection of the screen\'s ratio whose fovy was not widened draws with tan x k (16:9\'s sides, more above and below)')

    # The 2D layer: the UI renderer's stored ortho copied into each draw's uniform block (main+0x17ed900, Tears of the Kingdom's leaf byte for byte) and the text
    # composites' flipped ortho before the quad's matrix (main+0x1822e50). With the 16:9 layout (DM_SCREEN_UI=band: layout height 720, state +0x4c) the game fits its
    # 2D to the screen's height, an ortho of the screen's ratio (|m11| / m00 = ratio, 1028x720 wide at 1.43) whose sides fell off the screen: both rows are divided by
    # k, the 2D fitting the width in a 16:9 band across the middle. With the taller layout (896) the game's own 2D fills the screen and the stub leaves it. Assembled
    # with clang; s4..s7, v6, w16 / w17 free, as the 16:9-only stub before it (checked)
    UI_BAND = (0x5E040404, 0x5E0C0425, 'adrp', 0xB9404E11, 0x710B423F, 0x540002A1, 0xBD405A07, 0x1E2020E8, 0x54000240, 0x1E2418A6, 0x1E20C0C6, 0x1E2738C6,
               0x1E20C0C6, 0x529AE151, 0x72A78471, 0x1E270227, 0x1E2720C0, 0x5400012A, 0xBD405E06, 0x1E2E1007, 0x1E2618E6, 0x4F869000, 0x4F869021, 0xB9412611,
               0x11000631, 0xB9012611, 'orig', 'back')
    def ui_band(site, orig, why):
        fs_site(site, struct.pack('<I', orig).hex())
        start = here()
        for w in UI_BAND:
            if w == 'adrp': emit(ADRP(16, here(), FS_STATE_OFF))
            elif w == 'back': emit(B(here(), site + 4))
            elif w == 'orig': emit(orig)
            else: emit(w)
        fs_put(site, [B(site, start)], why)
    ui_band(0x17ed908, 0xad000420, 'screen shape: the 2D layer\'s 16:9 ortho matrix copied as a 16:9 band in the middle of the taller picture')
    ui_band(0x1822e50, 0xf9401069, 'screen shape: the text composites\' 16:9 ortho (before the quad\'s matrix) as a 16:9 band in the middle')
    # The HUD at the taller screen's edges (duo_anchor.c, Donkey Kong Bananza's rules: Breath of the Wild 1.9.0's for the newer nn::ui2d, parameters at state
    # +0x80..+0x98 from loader.c, which moves by the screen's own layout height even with the 2D's 16:9 layout), called once a pane's global matrix is stored in the base
    # matrix calculation (main+0x2f6520, the same library as Bananza's main+0x2f8420): with the band stubs above the 2D keeps its proportions and its edge widgets reach
    # the screen's edges. The words: `clang -target arm64-apple-macos -O2 -ffreestanding -fno-builtin -fno-stack-protector -fno-jump-tables
    # -fno-asynchronous-unwind-tables -c duo_anchor.c`, its __text as 32-bit words (no relocations)
    ANCHOR = 'bd4000201e2f10021e2208001e20200854000041d65f03c0bd4070012d4a10031e2108631e20c066bd4084031e2308851e2388841e2020a81e254c85bd4010241e2208821e2220c0540002abbd407c021e20c04452a84e081e2701061e262080540001e5bd408c041e20c0861e2701071e2720c05290000872a889481e2701061e2644a8540000cbbd4018261e2f10071e2708c61e2620a054000ba9f9400c08b4fffba8bd4008211e2f10021e220823bd408c011e2140221e2020281e214c421e232040540001c5b940b009528be98a72ae884a6b0a013f540001c152a872c91e2701231e23204054fff9643942d0097101b93f5400016117ffffc752a880e91e2701231e23204054fff86c1400000e52a872c91e2701231e23204054fff7c4bd408d031e20c06352a880e91e2701241e24204052a84e091e2701221e22d46054fff6aa913e0029528be98a72ac290a528c8e4b72aca96baa0003ec2956398d6b0a01bf7a4b01c054fff560f9400d8cb5ffff6c528a3d8a72a7b70a1e270142aa0803eabd407143bd4081441e2408841f0310631e22206054fff3e4f9400d4ab5ffff2ad280000af86a692beb08017f54fff3209100214af120015f54ffff61f9400d08b5ffff081e2140021e2020281e22cc001e202820bd008c00f868692aeb00015f54fff18091002108f120011f54ffff61b94f782892401d0af82a792011000508b90f7828d65f03c0f940140891008009eb09011f54ffefe11e20206854ffefa052a890e81e2701061e2620a0540000a51e2010061f0614001e25180014000002bd40042091022008bd4080056e0c04654d4081056e1c04854f8090a33d802003b940b008528bea0972ad2e096b09011f54ffed013942d00871018d1f54ffeca13942d4087101d11f54ffec41fc4740036e0360036e0404236e1c04434f8090603d801c00d65f03c0'
    anchor_code = here()
    for i in range(0, len(ANCHOR), 8): emit(int(ANCHOR[i:i + 8], 16))
    # The inline calculation: at its store (main+0x2f6688, stp q6,q4,[x19,#0x70]), not at the mov w22,#1 after it, which the up-to-date path (flags bit 6, main+0x2f65a0)
    # also branches to: there a matrix that was not recomputed would be moved again every frame. w8 (the flags' bit 1, which cbz tests at +0x2f6690) is rebuilt after
    # the call; no other call-clobbered register is live after the site (read to the function's end); the frame keeps x30
    site = 0x2f6688
    fs_site(site, '669203ad')   # stp q6,q4,[x19,#0x70]
    start = here(); emit(0xad039266)
    emit(0xaa1303e0); emit(ADRP(1, here(), FS_STATE_OFF)); emit(0x91020021)   # mov x0,x19; x1 = state + 0x80
    emit(B(here(), anchor_code) | 0x80000000)                                  # bl duo_calc
    emit(0x39416268); emit(0x121f0108); emit(B(here(), site + 4))              # ldrb w8,[x19,#0x58]; and w8,w8,#2; back
    fs_put(site, [B(site, start)], 'Pane matrix calculation: a HUD widget at the edge moved to the taller screen\'s edge, full-screen backgrounds stretched (duo_anchor.c)')
    # The common path: most panes have their global matrix computed by main+0x17f89f0, called at main+0x2f65c4; the same call right after it, where the flags are loaded
    # again (Bananza's main+0x2f84d4)
    site = 0x2f65c8
    fs_site(site, '68624139')   # ldrb w8,[x19,#0x58]
    start = here()
    emit(0xaa1303e0); emit(ADRP(1, here(), FS_STATE_OFF)); emit(0x91020021)   # mov x0,x19; x1 = state + 0x80
    emit(B(here(), anchor_code) | 0x80000000)                                  # bl duo_calc
    emit(0x39416268); emit(B(here(), site + 4))                                # ldrb w8,[x19,#0x58]; back
    fs_put(site, [B(site, start)], 'Pane matrix calculation (the common path): a HUD widget at the edge moved to the taller screen\'s edge (duo_anchor.c)')
    # Full-screen draws with no pane (an identity model matrix: backdrops, fades, a scene's picture): rows 0 and 1 of the model matrix scaled by k as the draw info's model
    # matrix is copied into the uniform block (main+0x17ed938; Bananza's main+0x857cb4, Tears of the Kingdom's main+0x10cc970), so with the band's ortho the quad fills
    # the screen's height, its sides cut evenly (nothing stretched). A leaf: v4..v7, x16 / x17 free
    FULLQUAD = (2415919120, 3175112196, 505421960, 1409287232, 1384572145, 1923567729, 505872935, 506335238, 1577321477, 505821349, 505462949, 505880736, 1409286954, 1577845797, 505821349, 505462949, 505880736, 1409286794, 1577845765, 505462949, 505880736, 1409286666, 1577321509, 505462949, 505880736, 1409286538, 506204166, 1578894341, 505462949, 505815200, 1409286378, 1578894373, 505462949, 505815200, 1409286250, 1334087680, 1334087713, 2902459424)
    site = 0x17ed938
    fs_site(site, '200400ad')   # stp q0,q1,[x1]
    start = here(); emit(ADRP(16, here(), FS_STATE_OFF))
    for w in FULLQUAD[1:]: emit(w)
    emit(B(here(), site + 4))
    fs_put(site, [B(site, start)], 'screen shape: a full-screen draw with no pane (identity model) scaled up evenly to fill the taller screen, its sides cut')

def frame_step_180(ctx):
    frame_step(ctx, 0xa4)
    if ctx.base == 0x69cbb0: screen_shape(ctx)   # 1.8.0's own addresses

MATCH_FRAME_STEP = {'main': [
    {'name': 'frame interval from the host\'s step', 'size': 0x290, 'fingerprints': ['fe68735d459ac00d848aa10c5da10964193abc0c950c35cafe5203606de3ea29'], 'stub': frame_step},   # v0 main+0x3a9e28
    {'name': 'frame interval from the host\'s step', 'size': 0x28c, 'fingerprints': ['f5ff6fbab4ddd6eb8be409727bbc0cbf68ef791aa143a1043253cdbb771cc365'],
     'stub': frame_step_180},   # 1.8.0 (v917504) main+0x69cbb0; with the screen shape
]}

BINDING = [
    {'what': 'frame interval from the host\'s step', 'now': 'game', 'goal': 'game', 'purpose': 'frame rate',
     'why': "the game's own interval choice, found by its function's fingerprint on any build that keeps the function",
     'checked': 'v0 and 1.8.0 mains: one function with each fingerprint, the sites and the free registers read by disassembly (2026-11-08, 1.8.0 2026-11-12)'},
]
