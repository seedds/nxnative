# Donkey Kong Bananza (040074A01BF12000): the rewriter's patches of this game's own code (dm/tools/patch_nso.py loads every profile's patches.py).
import struct
from nso_asm import ADRP, B

def screen_shape(ctx):
    """Screen shape (DM_SCREEN_ASPECT, the launcher's Picture shape; Mario Kart World 1.8.0's method, its patches.py screen_shape): a screen taller than 16:9 (the iPhone
    Duo's, about 1.43:1) gets a picture of its own shape. v458752 main only, at its own addresses. Registered as this build's FRAME_STEP for the state page alone: there is no
    frame interval stub, so with the profile's fps_target 0 the game keeps its own pacing. State +0x48 / +0x4c output / layout height, +0x54 size-site mask
    (DM_SCREEN_ASPECT_SITES), +0x58 the screen's ratio (0 = off), +0x5c k = (16/9) / ratio, +0x60 tolerance, +0xfc / +0x100 the last widened fovys; +0x120 / +0x124 counters"""
    here, emit, fs_site, fs_put, FS_STATE_OFF = ctx.here, ctx.emit, ctx.site, ctx.put, ctx.state
    ctx.capabilities.add('SCREEN_ASPECT')   # the launcher's Picture shape (settings.tsv fixups:SCREEN_ASPECT)
    # Output and layout sizes: the 1920x1080 / 1280x720 pairs (mov xN,#1920 or #1280; movk xN,#1080 or #720,lsl #32) take the host's height while their bit of +0x54 is set.
    # x16 / x17 and the flags free after each site (to the next call or return: checked by disassembly)
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
    # The output sizes (1920x1080) also take the render scale (state +0x10 below 1.0): width and height x scale, multiples of 8, so in handheld the game's picture, its
    # crop and its 3D views follow the docked getters below. s16 / s17 free as well (a call or a return follows each site)
    OUT_SCALED = (0x90000010, 0xB9405610, 'tbz', 0x90000010, 0xB9404A11, 0x34000271, 'bfi_h', 0xBD401210, 0x1E2E1011, 0x1E312200, 0x540001CA, 0x1E202208, 0x5400018D,
                  0x1E230231, 0x1E300A31, 0x1E390231, 0x121D7231, 'bfi_h', 0x5280F011, 0x1E230231, 0x1E300A31, 0x1E390231, 0x121D7231, 'bfi_w')
    def out_scaled(site, reg, bit, why):
        orig = 0xf2c00000 | 0x438 << 5 | reg
        fs_site(site, struct.pack('<I', orig).hex())
        start = here(); emit(orig)
        for w in OUT_SCALED:
            if w == 0x90000010: emit(ADRP(16, here(), FS_STATE_OFF))
            elif w == 'tbz': emit(0x360002D0 | bit << 19)
            elif w == 'bfi_h': emit(0xB3607E20 | reg)
            elif w == 'bfi_w': emit(0xB3407E20 | reg)
            else: emit(w)
        emit(B(here(), site + 4))
        fs_put(site, [B(site, start)], why)
    for bit, (site, reg, load) in enumerate(((0x20b750, 8, OUT), (0x486f54, 8, OUT), (0x5399b4, 8, OUT), (0x5494a0, 9, OUT), (0x549a44, 9, OUT), (0x804384, 9, OUT),
                                             (0x45f0e4, 10, LAY), (0x479c48, 9, LAY), (0x47fbc0, 8, LAY), (0x5399cc, 8, LAY), (0x539cb4, 10, LAY), (0x544b20, 8, LAY),
                                             (0x71114c, 10, LAY), (0x1639d94, 23, LAY), (0x163a064, 8, LAY), (0x194ca1c, 8, LAY))):
        if load == OUT: out_scaled(site, reg, bit, f'screen shape site {bit}: 1920x1080 → the host\'s height, both x the render scale')
        else: taller_bit(site, reg, bit, load, f'screen shape site {bit}: 1280x720 → the host\'s height')
    # The docked screen size: main+0x5bf770 / +0x5bf780 return 2880 / 1620 (leaf getters; six callers, among them the display setup at main+0x5399a4 beside output site 2;
    # the renderer sizes its targets from them, the 3D views at half: 1440x1008 at 1.43). The height takes the screen's shape, 1.5 x the host's 1920-wide height (2016 at
    # 1.43), and both take the render scale (state +0x10, DM_RENDER_SCALE 0.5 to 1.0, multiples of 8), so the 3D draws fewer pixels and the picture is scaled up when shown
    # (the open world needed 29-33 ms of GPU a frame on an A20 Pro at thermal state 2, past the game's 30 fps there). Assembled with clang; leaves ending in ret, x16 / x17 free
    ctx.render_scale = 1
    for site, orig, words, why in (
            (0x5bf770, '00688152', (0x52816800, 0xBD401200, 0x1E2E1001, 0x1E212000, 0x540000EA, 0x1E202008, 0x540000AD, 0x1E230001, 0x1E210800, 0x1E390000, 0x121D7000,
                                    0xD65F03C0), 'the docked screen width (2880) takes the render scale'),
            (0x5bf780, '80ca8052', (0x5280CA80, 0xB9404A11, 0x34000051, 0x0B510620, 0xBD401200, 0x1E2E1001, 0x1E212000, 0x540000EA, 0x1E202008, 0x540000AD, 0x1E230001,
                                    0x1E210800, 0x1E390000, 0x121D7000, 0xD65F03C0), 'the docked screen height (1620) takes the screen\'s shape and the render scale')):
        fs_site(site, orig)
        start = here(); emit(ADRP(16, here(), FS_STATE_OFF))
        for w in words: emit(w)
        fs_put(site, [B(site, start)], 'screen shape: ' + why)
    # The window crop (main+0x204368: nvnWindowSetCrop's x, y, width, height from the frame object's +0xac, Mario Kart World's code byte for byte): a 16:9 crop takes the
    # screen's shape, width x (host height / 1920). Mario Kart World's stub; x16 / x17 free up to the blr, whose callee needs no flags
    CROP = (0x29569283, 'adrp', 0xB9404A11, 0x34000171, 0x52800130, 0x1B107C70, 0x531C6C91, 0x6B11021F, 0x540000C1, 'adrp', 0xB9404A11, 0x1B117C70, 0x5280F011, 0x1AD10A04, 'back')
    site = 0x20436c
    fs_site(site, struct.pack('<I', 0x29569283).hex())
    start = here()
    for w in CROP:
        if w == 'adrp': emit(ADRP(16, here(), FS_STATE_OFF))
        elif w == 'back': emit(B(here(), site + 4))
        else: emit(w)
    fs_put(site, [B(site, start)], 'screen shape: a 16:9 window crop takes the screen\'s shape')
    # The cameras: sead::PerspectiveProjection's set() takes the screen's ratio for a 16:9 aspect argument (main+0x158d3a4, fmov s8,s3), and the fovy is widened for a
    # view of the screen's ratio (16:9's sides, more above and below) in setFovy_ (main+0x158d348) and set() (main+0x158d3b8): Mario Kart World's stubs, the same code here
    def cnt(off): return (0xb9400211 | (off // 4) << 10, 0x11000631, 0xb9000211 | (off // 4) << 10)
    def aspect_arg(k, site, why):   # fmov s8,s3: a 16:9 aspect argument takes the screen's ratio (counted at +0x130 + 4k, applied at +0x124, the other aspects' bits at +0x128)
        fs_site(site, '6840201e')
        start = here(); emit(ADRP(16, here(), FS_STATE_OFF))
        for w in cnt(0x130 + 4 * k): emit(w)
        for w in (0x1e260071, 0x52800010 | 0x4e39 << 5, 0x72a00010 | 0x3fe2 << 5, 0x4b100231, 0x53127e31, 0x35000011 | 9 << 5): emit(w)   # bits of s3 within 16:9's window
        emit(ADRP(16, here(), FS_STATE_OFF))
        for w in (0xb9405a11, 0x34000011 | 9 << 5, 0x1e270228) + cnt(0x124): emit(w)   # ldr w17,[x16,#0x58]; cbz; fmov s8,w17 (the ratio)
        emit(B(here(), site + 4))
        emit(ADRP(16, here(), FS_STATE_OFF)); emit(0x1e260071); emit(0xb9012a11)
        emit(0x1e204068); emit(B(here(), site + 4))
        fs_put(site, [B(site, start)], f'screen shape: a 16:9 aspect given to {why} takes the screen\'s ratio')
    aspect_arg(0, 0x158d3a4, 'sead::PerspectiveProjection::set')
    aspect_arg(1, 0x6bca90, 'the sead::PerspectiveProjection constructor (main+0x6bca70; the gameplay camera, fovy 45 degrees, is made there and never set again)')
    TANF, ATANF = 0x1b7acd0, 0x1b7ae80   # tanf / atanf through v458752's PLT (resolved from its JMPREL)
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
    fov_stub(0x158d348, '0041201e', 0xbd40b262, 0x1e204100, 0x1e204008)   # setFovy_: ratio at [x19,#0xb0], half in s8
    fov_stub(0x158d3b8, '2041201e', 0x1e204102, 0x1e204120, 0x1e204009)   # set(): ratio still in s8, half in s9
    fov_stub(0x6bcb0c, '0041201e', 0xbd40b262, 0x1e204100, 0x1e204008)    # constructor: aspect stored at +0xb0 before, half in s8

    # The game camera's aspect (main+0x8790c, str s0,[x21,#0xb0]: the view update sets the fovy through setFovy_ at main+0x878e8 first, then this aspect, the viewport's
    # 16:9, so setFovy_'s widening never saw the screen's ratio and the view stayed 16:9, stretched to the taller picture: m00 1.3580 / m11 2.4142 at both shapes, from the
    # uniform dumps). A 16:9 aspect takes the screen's ratio and setFovy_ runs again with the camera's fovy, so its stub widens it and the sin / cos / tan agree (Mario Kart
    # World's race camera widened only the matrix, late, and its distant objects flickered). Assembled with clang; after the site the code reloads what it uses (x8 / x9
    # from x19, nothing call-clobbered read on the way to its next call, checked by disassembly), x16 / x17 / x9 / x10 free, and setFovy_ keeps x19..x28
    ASP = ('adrp', 0xB9405A11, 0x34000191, 0x1E260009, 0x5289C72A, 0x72A7FC4A, 0x4B0A0129, 0x53127D29, 0x350000C9, 0xB900B2B1, 0xBD40A2A0, 0xAA1503E0, 'bl_setfovy',
           0x14000002, 0xBD00B2A0, 'back')
    site = 0x8790c
    fs_site(site, 'a0b200bd')   # str s0,[x21,#0xb0]
    start = here()
    for w in ASP:
        if w == 'adrp': emit(ADRP(16, here(), FS_STATE_OFF))
        elif w == 'bl_setfovy': emit(B(here(), 0x158d324) | 0x80000000)
        elif w == 'back': emit(B(here(), site + 4))
        else: emit(w)
    fs_put(site, [B(site, start)], 'screen shape: the game camera\'s 16:9 aspect takes the screen\'s ratio and its fovy is set again, widened')

    # An inlined set() (main+0x39bc84..: the projection at x19+0xb0, its fovy +0x150, sin / cos / tan +0x154.., aspect +0x160 from s9, the source camera's aspect
    # loaded at main+0x39bc8c and stored last, at main+0x39bce0): the gameplay cameras' 45 degree views (tan 0.4142, aspect 16/9, found by DM_PROBE_WATCH on one's aspect)
    # are set here every frame. At the half angle (main+0x39bca4, fmul s8,s2,s0) a 16:9 aspect in s9 widens it (atan(tan x k)) and takes the screen's ratio, so the fovy,
    # sin, cos, tan and aspect the code goes on to store agree. Assembled with clang; s2 / s8 / s9 are what the code after uses (s2 stored next, s9 only by the aspect
    # store), it calls sinf next so nothing else call-clobbered is live, x9 / x10 / x16 / x17 free; the frame keeps x30 (the function calls)
    # With DM_SCREEN_FOV=off (state +0x60 negative) only the aspect takes the ratio: 16:9's vertical angle, the sides cut (Mario Kart World's "closer" view)
    def inlined_set(site, words, where):
        fs_site(site, struct.pack('<I', words[0]).hex())
        start = here()
        for w in words:
            if w == 'adrp': emit(ADRP(16, here(), FS_STATE_OFF))
            elif w == 'tanf': emit(B(here(), TANF) | 0x80000000)
            elif w == 'atanf': emit(B(here(), ATANF) | 0x80000000)
            elif w == 'back': emit(B(here(), site + 4))
            else: emit(w)
        fs_put(site, [B(site, start)], f'screen shape: an inlined sead::PerspectiveProjection set() of a 16:9 aspect is widened and takes the screen\'s ratio ({where})')
    inlined_set(0x39bca4, (0x1E200848, 'adrp', 0xB9405A11, 0x340002D1, 0x1E260129, 0x5289C72A, 0x72A7FC4A, 0x4B0A0129, 0x53127D29, 0x35000209, 0xB9406209, 0x37F80149, 0x1E204100, 'tanf', 'adrp', 0xBD405E01, 0x1E210800, 'atanf', 0x1E204008, 0x1E202802, 'adrp', 0xBD405A09, 0xB9412611, 0x11000631, 0xB9012611, 'back'),
                'fovy s2, half s8, aspect s9')
    # The second (main+0x4db8b4, fmul s8,s10,s13: fovy s10 from [x20,#0xb4] x the scale, half s8, aspect s12 from [x19,#0x100]; stored at +0xa0..+0xb0 and again at +0x1d8 /
    # +0x1e0): the view the frame is drawn with (its aspect stayed 16/9 with the first one widened, DM_PROBE_WATCH). s8 / s10 / s12 are what the code after uses; sinf next
    inlined_set(0x4db8b4, (0x1E2D0948, 'adrp', 0xB9405A11, 0x340002D1, 0x1E260189, 0x5289C72A, 0x72A7FC4A, 0x4B0A0129, 0x53127D29, 0x35000209, 0xB9406209, 0x37F80149, 0x1E204100, 'tanf', 'adrp', 0xBD405E01, 0x1E210800, 'atanf', 0x1E204008, 0x1E20280A, 'adrp', 0xBD405A0C, 0xB9412611, 0x11000631, 0xB9012611, 'back'),
                'fovy s10, half s8, aspect s12')

    # The 2D layer: drawn with a 16:9 projection over the taller picture, stretched 1.244 tall (the hearts 207x45 at 16:9, 207x56 at 1.43, with the layout sites on or off).
    # The UI renderer's stored ortho copied into each draw's uniform block (main+0x857c84, Tears of the Kingdom's and Mario Kart World's leaf byte for byte) and the text
    # composites' flipped ortho before the quad's matrix (main+0x88eeb8, Mario Kart World's code byte for byte): a 16:9 one (|m11| / m00 = 16/9) has row 1 divided by k, the
    # 2D fitting the width in a 16:9 band across the middle. Tears of the Kingdom's stub; s4..s7, v6, w16 / w17 free at both (checked there, the same code here)
    UI_BAND = (0x5e040404, 0x5e0c0425, 0x1e2418a6, 0x1e20c0c6, 0x1e2600d1, 0x5289c730, 0x72a7fc50, 0x4b100231, 0x53127e31, 0x35000171, 'adrp',
               0xb9405a11, 0x34000111, 0xbd405e06, 0x1e2e1007, 0x1e2618e6, 0x4f869021, 0xb9412611, 0x11000631, 0xb9012611, 'orig', 'back')
    def ui_band(site, orig, why):
        fs_site(site, struct.pack('<I', orig).hex())
        start = here()
        for w in UI_BAND:
            if w == 'adrp': emit(ADRP(16, here(), FS_STATE_OFF))
            elif w == 'back': emit(B(here(), site + 4))
            elif w == 'orig': emit(orig)
            else: emit(w)
        fs_put(site, [B(site, start)], why)
    ui_band(0x857c8c, 0xad000420, 'screen shape: the 2D layer\'s 16:9 ortho matrix copied as a 16:9 band in the middle of the taller picture')
    ui_band(0x88eeb8, 0xf9401069, 'screen shape: the text composites\' 16:9 ortho (before the quad\'s matrix) as a 16:9 band in the middle')

FRAME_STEP = {'e25c90c1c136638885659fd55bf8a03b': screen_shape}   # v458752 main: the state page for the screen shape (no frame interval stub)
