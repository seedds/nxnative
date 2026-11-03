# Animal Crossing: New Horizons (01006F8002326000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO build
# ID (FRAME_STEP as described there, "Registered patches"). The game has no frame step stubs: FRAME_STEP only gives its main the state page that the screen shape stubs read
# (DM_SCREEN_ASPECT, as for Breath of the Wild); with DM_SCREEN_ASPECT unset the page stays zero and every stub runs the original instructions.
import struct
from nso_asm import ADRP, B

# Screen shape (DM_SCREEN_ASPECT, loader.c): a screen taller than 16:9 keeps the picture's width (1920) and takes the height of the screen's ratio (state +0x48, 1344 at 1.43), the
# layout space 1280 x 2/3 of it (+0x4c, 896; +0x50 the same as a float). The game's platform config (vtable main+0x47fa0e0, no RTTI) hands its sizes out three ways:
#   main+0x5c9b44 (vtable main+0x47f9c08) fills the graphics create arg: +0x60 the display size (the table main+0x4076728 of 960x540 .. 3840x2160 by the mode at [main+0x512e2e0]
#     when [main+0x5131900] is set, else 1920x1080), +0x20 1280x720
#   main+0x5cdce0 (slot 7) a static display size main+0x4b87524 (the same choice), main+0x5cdd88 (slot 8) a static 1280.0 x 720.0 (main+0x4b87538)
#   main+0x5cdcb8 (slot 3) another arg's +0x14: 1920x1080
# Each stub is one bit of the conversion site mask (+0x54, DM_SCREEN_ASPECT_SITES, all on by default); a height of 0 (16:9) leaves the original value. x16 / x17 are free at every site
SIZE_SITES = [
    # (site, the stub's words after ADRP x16 (state), the last one the site's original instruction, reason)
    (0x5c9b80, [
        0xb9405611,   # ldr w17, [x16, #0x54]   sites
        0x360000d1,   # tbz w17, #0, orig
        0xb9404a11,   # ldr w17, [x16, #0x48]   the output's height (0 = 16:9)
        0x34000091,   # cbz w17, orig
        0x1b117d28,   # mul w8, w9, w17         height = width x the output's height / 1920 (each mode of the table: 960 → 672, 1280 → 896, 1920 → 1344)
        0x5280f011,   # mov w17, #1920
        0x1ad10908,   # udiv w8, w8, w17
        0x290c2029,   # orig: stp w9, w8, [x1, #0x60]
    ], 'graphics create arg +0x60: the display size in the screen\'s shape (bit 0)'),
    (0x5c9b94, [
        0xb9405611,   # ldr w17, [x16, #0x54]
        0x36080091,   # tbz w17, #1, orig
        0xb9404e11,   # ldr w17, [x16, #0x4c]   the layout height (0 = 16:9)
        0x34000051,   # cbz w17, orig
        0xb3607e28,   # bfi x8, x17, #32, #32   1280 x the layout height
        0xf9001028,   # orig: str x8, [x1, #0x20]
    ], 'graphics create arg +0x20: 1280 x the layout height (bit 1)'),
    (0x5cdd74, [
        0xb9405611,   # ldr w17, [x16, #0x54]
        0x361000d1,   # tbz w17, #2, orig
        0xb9404a11,   # ldr w17, [x16, #0x48]
        0x34000091,   # cbz w17, orig
        0x1b117d28,   # mul w8, w9, w17
        0x5280f011,   # mov w17, #1920
        0x1ad10908,   # udiv w8, w8, w17
        0x29002149,   # orig: stp w9, w8, [x10]
    ], 'platform config slot 7: the static display size in the screen\'s shape (bit 2)'),
    (0x5cddd4, [
        0xb9405611,   # ldr w17, [x16, #0x54]
        0x361800b1,   # tbz w17, #3, orig
        0xb9404e11,   # ldr w17, [x16, #0x4c]
        0x34000071,   # cbz w17, orig
        0xb9405211,   # ldr w17, [x16, #0x50]   the layout height as a float
        0xb3607e29,   # bfi x9, x17, #32, #32   1280.0 x the layout height
        0xf9029d09,   # orig: str x9, [x8, #0x538]
    ], 'platform config slot 8: the static layout size 1280.0 x the layout height (bit 3)'),
    (0x5cdcc8, [
        0xb9405611,   # ldr w17, [x16, #0x54]
        0x36200091,   # tbz w17, #4, orig
        0xb9404a11,   # ldr w17, [x16, #0x48]
        0x34000051,   # cbz w17, orig
        0xb3607e28,   # bfi x8, x17, #32, #32   1920 x the output's height
        0xf8014028,   # orig: stur x8, [x1, #0x14]
    ], 'platform config slot 3: the arg\'s 1920x1080 in the screen\'s shape (bit 4)'),
]

# The HUD at the taller screen's edges: duo_anchor.c, compiled by clang to position-independent code without data (-O2 -ffreestanding -fno-builtin -fno-stack-protector
# -fno-jump-tables -fno-asynchronous-unwind-tables; the words of its __text, duo_calc at DUO_CALC). nn::ui2d::Pane::Calculate (main+0x27d4c0, vtable main+0x474d1f8 slot 19)
# calls CalculateGlobalMatrix (main+0x27d74c) when the pane or a parent changed and returns to main+0x27d570, before the children's Calculate (vtable +0x98): the stub there
# calls duo_calc(pane, state + 0x80, nn::ui2d::Pane::DrawSelf) while the output's height is set (bit 9 of the sites). x0..x18 and x30 are dead there (the function reloads
# x30 from its frame), d8 is callee-saved
DUO_ANCHOR = 'bd4000211e20202854000041d65f03c0f9400c08b4ffffc852800609aa0803eaf9400d4bb400008baa0b03ea7100052954ffff81bd40554052a88f491e2701221e2220001e2e10001e2f10021e20ac40bd4070022d4a14031e2208641e2288621e2020881e244c44bd4084031e2308a21e2388a51e2020481e224ca5f94014099100800abd4010221e2208021e222080540003cbbd4014261e2608061e2620a05400034bbd4018261e2608061e2620a0540002c8bd407c061e20c0c652a8440b1e2701671e2708071e2720c0540001e5bd408c061e20c0d01e27220054000165eb0a013f54fff941f9400008f9405508eb02011f54fff8c0bd400420bd4080011e2108001400001ceb0a013f540004c1bd4014261e2608061e2620a05400044b52a886891e2701261e260806bd4004271e3010101f0740c61e2620a054000345bd408c061e20c0d052a844091e2701311e3108111e31220054000265f9400009f9405529eb02013f540001e0bd4080001e2008e0bd008000bd4004201e200860bd008400bd400420bd4088011e210800bd008800bd4004201e2008c0bd008c00d65f03c01e222080540000abbd4014231e2308031e2320a054fff2aaf9400009f940552952a8500a1e2701431e230803eb02013f540000811e2320801e2354a854fff1641e2008215280060914000004aa0a03e87100052954000440f9400d0ab400034abd4071042d4a19051e2408a71e2488a41e2020e81e274c84bd4085051e2508c71e2588c51e2020e81e274ca51e222080540000abbd4014261e2608061e2620a054fffd6af9400108f9405508eb02011f54ffedc11e2320801e2354a854fffc8417ffff6a2d460d021e2308631f020c421e2a10031e23204054ffec8cbd407c021e20c04252a879081e2701031e2308031e23204052a870e81e27010252a86cc81e2701031e22ac621e220802bd408c001e20c0031e22206054ffea8b1e2140221e2020081e22cc211e212800bd008c00d65f03c0'
DUO_CALC = 0x0
PANE_DRAW_SELF = 0x27e2c8   # nn::ui2d::Pane::DrawSelf (vtable main+0x474d1f8 slot 21, empty): the game's null panes, alignments, captures and parts keep it
TANF, ATANF = 0x3f03330, 0x3f03360   # main's PLT stubs of the SDK's tanf, atanf


def BL(frm, to): return 0x94000000 | (((to - frm) >> 2) & 0x3FFFFFF)


def screen_shape(ctx):
    here, emit, site, put, STATE = ctx.here, ctx.emit, ctx.site, ctx.put, ctx.state
    for at, words, why in SIZE_SITES:
        site(at, struct.pack('<I', words[-1]).hex())
        start = here(); emit(ADRP(16, here(), STATE))
        for w in words: emit(w)
        emit(B(here(), at + 4))
        put(at, [B(at, start)], why)
    # Every frame main+0x1527ebc picks the resolution table's mode (3, 1920x1080, in the console mode; 1, 1280x720, handheld) into [main+0x512e2e0] / +0x2e4, divides the
    # entry by the framebuffer's size ([main+0x59ea928] +0x3a8 / +0x3ac) into the 3D views' render scale (+0x4c8 / +0x4cc), and sets the display buffer's crop
    # ([main+0x59e77d8] +0x50, +0x4c..+0x58) to the entry: the presented picture was cropped back to the top 1920x1080 of the 1920x1344 window. The two loads of the
    # entry (main+0x1527f00, main+0x1527f2c) take the height of the screen's shape, the width x the output's height / 1920 in integers (1920 → 1344 exactly; a float
    # product truncated to 1343). Bit 7 of the sites. s4.. and x16 / x17 are free there
    for at, orig, cvt, back in ((0x1527f00, 0x2d400500, 0x1e380010, 0x1e220201),    # ldp s0, s1, [x8]; fcvtzs w16, s0; scvtf s1, w16
                                (0x1527f2c, 0x2d400d02, 0x1e380050, 0x1e220203)):   # ldp s2, s3, [x8]; fcvtzs w16, s2; scvtf s3, w16
        site(at, struct.pack('<I', orig).hex())
        start = here(); emit(orig)   # the original load
        emit(ADRP(16, here(), STATE))
        for w in (0xb9405611,   # ldr w17, [x16, #0x54]   sites
                  0x36380111,   # tbz w17, #7, back (+8)
                  0xb9404a11,   # ldr w17, [x16, #0x48]   the output's height (0 = 16:9)
                  0x340000d1,   # cbz w17, back (+6)
                  cvt,          # fcvtzs w16, <width>
                  0x1b117e10,   # mul w16, w16, w17
                  0x5280f011,   # mov w17, #1920
                  0x1ad10a10,   # udiv w16, w16, w17
                  back):        # scvtf <height>, w16
            emit(w)
        emit(B(here(), at + 4))
        put(at, [B(at, start)], 'resolution table entry of the frame (crop, render scale): the height of the screen\'s shape (bit 7)')
    # The layouts' DrawInfo ([+0x60] of the renderer at main+0x31cee50) takes the framebuffer's size twice, 1920.0 x 1080.0 at +0x108 (main+0x31cee58..60) and 1920 x 1080
    # at +0x138 (main+0x31cee98..9c), which nn::ui2d's scissors and captures map the screen with: both take the output's height. Each stub runs the site's original movk
    # first. v16 is dead there (a call follows with x0 only), x16 / x17 free. Bit 8 of the sites
    for at, orig, words in ((0x31cee60, 0xf2e890f8, (0x1e230230, 0x1e260211, 0xb3607e38)),   # movk x24, #0x4487, lsl #48; ucvtf s16, w17; fmov w17, s16; bfi x24, x17, #32, #32
                            (0x31cee9c, 0xf2c08708, (0xb3607e28,))):                        # movk x8, #0x438, lsl #32; bfi x8, x17, #32, #32
        site(at, struct.pack('<I', orig).hex())
        start = here(); emit(orig)
        emit(ADRP(16, here(), STATE))
        n = 3 + len(words)   # from the tbz to the branch back (the cbz: n - 2)
        emit(0xb9405611)                                          # ldr w17, [x16, #0x54]   sites
        emit(0x36400011 | (n << 5))                               # tbz w17, #8, back
        emit(0xb9404a11)                                          # ldr w17, [x16, #0x48]   the output's height (0 = 16:9)
        emit(0x34000011 | ((n - 2) << 5))                         # cbz w17, back
        for w in words: emit(w)
        emit(B(here(), at + 4))
        put(at, [B(at, start)], 'layouts\' DrawInfo: the framebuffer\'s size in the screen\'s shape (bit 8)')
    # The scene camera's vertical angle: the field camera (main+0x7a8xxx) keeps it in radians at +0x2cc (0.384, 22 degrees, on the island) and reads it twice, for its
    # sead::PerspectiveProjection ([+0x80] of the view, main+0x7a895c: fovy +0xa0 and the half angle's sin / cos / tan) and for main+0x151424c (main+0x7a89a8), the views
    # that follow it. Their aspect already follows the taller viewport (1.43), so the picture kept the vertical angle and lost a fifth of each side (Mac, an island
    # save: the tent 1.23 times as wide as at 16:9). Both loads widen it to 2 atan(tan(fovy / 2) k), k = (16/9) / the screen's ratio (state +0x5c), so the sides are
    # 16:9's and the taller screen shows more above and below; every view and its culling take the wider angle from there, and the camera writes +0x2cc afresh, so it
    # never widens twice. The second site's x0 is main+0x151424c's argument: saved around the calls. Bit 5 of the sites
    for at in (0x7a895c, 0x7a89a8):
        site(at, '80ce42bd')
        start = here(); emit(0xbd42ce80)   # orig: ldr s0, [x20, #0x2cc]
        emit(ADRP(16, here(), STATE))
        emit(0xb9405611)   # ldr w17, [x16, #0x54]   sites
        emit(0x362801b1)   # tbz w17, #5, back (+13)
        emit(0xb9404a11)   # ldr w17, [x16, #0x48]   the output's height (0 = 16:9)
        emit(0x34000171)   # cbz w17, back (+11)
        emit(0xf81f0fe0)   # str x0, [sp, #-16]!
        emit(0x1e2c1001)   # fmov s1, #0.5
        emit(0x1e210800)   # fmul s0, s0, s1
        emit(BL(here(), TANF))
        emit(ADRP(16, here(), STATE))
        emit(0xbd405e01)   # ldr s1, [x16, #0x5c]   k
        emit(0x1e210800)   # fmul s0, s0, s1
        emit(BL(here(), ATANF))
        emit(0x1e202800)   # fadd s0, s0, s0        the widened angle
        emit(0xf84107e0)   # ldr x0, [sp], #16
        emit(B(here(), at + 4))
        put(at, [B(at, start)], 'field camera: its vertical angle widened so the taller screen keeps 16:9\'s sides (bit 5)')
    code = here()
    for i in range(0, len(DUO_ANCHOR), 8): emit(int(DUO_ANCHOR[i:i + 8], 16))
    site(0x27d570, '68624139')
    start = here(); emit(ADRP(16, here(), STATE))
    emit(0xb9405611)   # ldr w17, [x16, #0x54]   sites
    emit(0x36480111)   # tbz w17, #9, orig (+8)
    emit(0xb9404a11)   # ldr w17, [x16, #0x48]   the output's height (0 = 16:9)
    emit(0x340000d1)   # cbz w17, orig (+6)
    emit(0xaa1303e0)   # mov x0, x19             the pane
    emit(0x91020201)   # add x1, x16, #0x80      the parameters
    emit(ADRP(2, here(), PANE_DRAW_SELF)); emit(0x91000042 | ((PANE_DRAW_SELF & 0xFFF) << 10))   # add x2, x2, #lo12: nn::ui2d::Pane::DrawSelf
    emit(BL(here(), code + DUO_CALC))
    emit(0x39416268)   # orig: ldrb w8, [x19, #0x58]
    emit(B(here(), 0x27d574))
    put(0x27d570, [B(0x27d570, start)], 'nn::ui2d::Pane::Calculate after the global matrix: duo_calc (the HUD at the taller screen\'s edges, full-screen backgrounds stretched)')
    # The field of view: the game camera (main+0x386a6a0, the scene camera's update) copies its sead::PerspectiveProjection ([camera +0x80]: near +0x98, far +0x9c,
    # fovy +0xa0, its half angle's sin / cos / tan +0xa4..+0xac, aspect +0xb0) into its parameters ([camera +0x1f8] + 0x108), sets near, far and the vertical angle
    # there from the parameters (+0xe8, +0xec, +0xf0 in degrees x pi/180) and copies them back. A taller picture of the same vertical angle shows less at the sides:
    # the angle is widened where it is computed (main+0x386a7a0: fmul s8, s0, s9, the half angle; s9 = 0.5) to 2 atan(tan(fovy / 2) k), k = (16/9) / the screen's
    # ratio (state +0x5c), so the sides stay those of 16:9 and the taller screen shows more above and below. The degrees come from the parameters every time, so it
    # never widens twice, and everything after it (sin / cos / tan, the copy back, the matrix, culling) takes the wider angle. Bit 5 of the sites. Caller-saved
    # registers are dead there but s0 (the game calls sinf right after)
    site(0x386a7a0, '0808291e')
    start = here(); emit(ADRP(16, here(), STATE))
    emit(0xb9404a11)   # ldr w17, [x16, #0x48]   the output's height (0 = 16:9)
    emit(0x34000171)   # cbz w17, orig (+11)
    emit(0xb9405611)   # ldr w17, [x16, #0x54]   sites
    emit(0x36280131)   # tbz w17, #5, orig (+9)
    emit(0x1e2c1002)   # fmov s2, #0.5
    emit(0x1e220800)   # fmul s0, s0, s2
    emit(BL(here(), TANF))
    emit(ADRP(16, here(), STATE))
    emit(0xbd405e01)   # ldr s1, [x16, #0x5c]   k
    emit(0x1e210800)   # fmul s0, s0, s1
    emit(BL(here(), ATANF))
    emit(0x1e202800)   # fadd s0, s0, s0        the widened angle
    emit(0x1e290808)   # orig: fmul s8, s0, s9
    emit(B(here(), 0x386a7a4))
    put(0x386a7a0, [B(0x386a7a0, start)], 'game camera: the vertical angle widened so the taller screen keeps 16:9\'s sides (bit 5)')
    # The layouts' projection: nn::ui2d::DrawInfo::SetProjectionMtx (main+0x265100, 13 callers) copies the game's projection of a layout's rectangle (1920x1080 or
    # 1280x720) into DrawInfo +0x10 (4x4). On the taller screen it would stretch every layout to it; its y row is divided by the vertical stretch (layout height / 720,
    # state +0x84) before the return, so layouts keep their shape, centred, with room above and below (the edges and backgrounds: duo_calc). Indices 1, 4, 5, 6, 7, 9
    # and 13 are the y row in either storage order (row-major 4..7, column-major 1, 5, 9, 13); the others of the union are zero in any projection without rotation.
    # Only a 16:9 projection is divided (|m5| / |m0| within 5 % of 16/9: the layouts' perspective camera, m0 1.545, m5 2.747, fovy 40 degrees): the game copies a
    # DrawInfo's projection into another, and the title's layouts came out divided twice (0.65 of their height); another shape (a square render target) is left
    # alone. Bit 6 of the sites. At the return s0..s4 are free
    site(0x265140, 'c0035fd6')
    start = here(); emit(ADRP(16, here(), STATE))
    emit(0xb9404a11)   # ldr w17, [x16, #0x48]   the output's height (0 = 16:9)
    emit(0x34000511)   # cbz w17, ret (+41)
    emit(0xb9405611)   # ldr w17, [x16, #0x54]
    emit(0x363004d1)   # tbz w17, #6, ret (+39)
    emit(0xbd401001)   # ldr s1, [x0, #0x10]
    emit(0x1e20c021)   # fabs s1, s1
    emit(0xbd402402)   # ldr s2, [x0, #0x24]
    emit(0x1e20c042)   # fabs s2, s2
    emit(0x5291c731)   # mov w17, #0x8e39
    emit(0x72a7fc71)   # movk w17, #0x3fe3, lsl #16
    emit(0x1e270223)   # fmov s3, w17
    emit(0x1e230823)   # fmul s3, s1, s3
    emit(0x7ea3d443)   # fabd s3, s2, s3
    emit(0x529999b1)   # mov w17, #0xcccd
    emit(0x72a7a991)   # movk w17, #0x3d4c, lsl #16
    emit(0x1e270224)   # fmov s4, w17
    emit(0x1e240824)   # fmul s4, s1, s4
    emit(0x1e242060)   # fcmp s3, s4
    emit(0x540002ec)   # b.gt ret (+23)   not 16:9: as it is
    emit(0xbd408600)   # ldr s0, [x16, #0x84]
    for i in (1, 4, 5, 6, 7, 9, 13):
        o = (0x10 + 4 * i) // 4
        emit(0xbd400001 | (o << 10))   # ldr s1, [x0, #0x10 + 4 i]
        emit(0x1e201821)               # fdiv s1, s1, s0
        emit(0xbd000001 | (o << 10))   # str s1, [x0, #0x10 + 4 i]
    emit(0xd65f03c0)   # ret
    put(0x265140, [B(0x265140, start)], 'nn::ui2d::DrawInfo::SetProjectionMtx: the y row divided by the vertical stretch, layouts in their own shape (bit 6)')



FRAME_STEP = {
    'ff1d1c05670db6021c85b624a710b963': screen_shape,   # 3.0.3 (v2228224) main
}

# How general each patch is and could be (dm/tools/docs_patches.py "Binding", listed in docs/agents/game-patches.md): what, its binding now, the loosest it could have (goal),
# purpose, why; blocked or kept: why the gap stays open
BINDING = [
    {'what': "screen shape: the sizes, the view, the layouts' projection and their edge widgets (DM_SCREEN_ASPECT)", 'now': 'build', 'goal': 'game', 'purpose': 'rendering',
     'why': "the output size is the host's, but where the game takes its sizes, its cameras and its layouts are this build's code",
     'blocked': 'the stubs have no function-matched form yet'},
]
