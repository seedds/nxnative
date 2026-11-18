# Pokemon Pokopia (0400A3A01EE4C000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO build
# ID (FRAME_STEP as described there, "Registered patches"). The game has no frame step stubs: FRAME_STEP only gives its main the state page that the screen shape stubs read
# (DM_SCREEN_ASPECT wider than 16:9, loader.c); with DM_SCREEN_ASPECT unset the page stays zero and every stub runs the original instructions.
from nso_asm import ADRP, B

# Screen shape, wider than 16:9 (DM_SCREEN_ASPECT, loader.c): the output keeps its height (1080) and takes the width of the screen's ratio (state +0xa0 u32, 2560 at 64:27; 0 = 16:9),
# and the cameras take that ratio (+0xa4, the float bits of width / 1080). KTGL (v327680 main) takes its sizes from four places, each widened by the output width / 1920 when it is 16:9:
#   main+0x16ae780 makes every render target from a descriptor (+0 width, +4 height): the scene's targets and their 1/2, 1/4, 1/8 .. chains (1920x1080 .. 80x45)
#   main+0x105c0bc makes a texture from a descriptor (+0x14 width, +0x18 height): the swap chain's three 1920x1080 textures (from main+0x105bb24)
#   main+0x103a1c0 / 0x103a1d0 the render width: the dynamic resolution table main+0x2756530 (34 widths, 960 .. 1920 in steps of 32, 2560, 3840, 7680; heights at main+0x27565b8,
#     540 .. 1080, 1440, 2160, 4320), 1920 out of range; the frame's viewports and the window crop follow it (with the targets alone the scene filled 1920 of the 2560 and the
#     rest of the targets showed other passes' contents)
#   main+0x1e225d4 builds a camera's projection (camera +0x140 near, +0x144 far, +0x148 fovy, +0x14c aspect, +0x158 flags; setter main+0x1e22828): every 3D camera has 16:9's
#     ratio, so a camera of exactly 16/9 takes the screen's; the vertical angle stays and the sides widen (Hor+). The ratio is stored back, so culling and shadows that read
#     +0x14c see the view as drawn
# The HUD at the screen's edges: every KTGL layout is a 1920x1080 canvas centred on the screen, and its panes are placed from the centre (each pane's anchor byte
# +0x10d is 0x11), so a wider screen leaves them in the middle 16:9 band. main+0x1e822a0 computes a pane's global matrix (pane +0xc0 name hash, +0xd0 parent, +0xd8 its
# layout, +0xe0 global matrix, +0xe8 local matrix, translations at +0x30, centre-origin layout pixels, +0xf0 / +0xf2 i16 size); at main+0x1e82690 the global matrix is
# stored and x8 is the parent's, x9 the pane's. A listed pane's global x moves out by (output width - 1920) / 2 towards its side; its children follow, being computed
# from it, and the translation-only update path (main+0x1e823e8) keeps the shift, as it adds deltas to the stored matrix. Registers there: x10 / x11 / x16 / x17 and
# s3 / s6 / s7 are free (x10 and the vector registers are written before they are read again)
# Registers: at main+0x16ae7a8 and main+0x105c0e0 x9..x13 are not yet set (only x0..x5 carry arguments); at main+0x1e225ec w8 is loaded by the original instruction, which runs
# last, and x16 / x17 are free; the width getters are leaves that return in w0
_wide_w, _wide_a = 0xa0, 0xa4   # state page offsets (loader.c)
_widen = [   # w9 width, w10 height, w11 the output width: w9 x w11 / 1920, rounded up to even, when w9 x 9 == w10 x 16
    0x5280020c,   # mov w12, #16
    0x1b0c7d4c,   # mul w12, w10, w12
    0x5280012d,   # mov w13, #9
    0x1b0d7d2d,   # mul w13, w9, w13
    0x6b0d019f,   # cmp w12, w13
    0x54000101,   # b.ne done (+8)
    0x1b0b7d2c,   # mul w12, w9, w11
    0x111dfd8c,   # add w12, w12, #1919
    0x5280f00d,   # mov w13, #1920
    0x1acd098c,   # udiv w12, w12, w13
    0x1100058c,   # add w12, w12, #1
    0x121f798c,   # and w12, w12, #~1
]
_stubs = {
    'rt': (0x16ae7a8, 'render target descriptor: a 16:9 size takes the screen\'s width', [
        0xb9401037,   # orig: ldr w23, [x1, #0x10]
        'adrp9',
        0xb940a12b,   # ldr w11, [x9, #0xa0]   the output width (0 = 16:9)
        0x3400024b,   # cbz w11, done
        0xb940ad2c,   # ldr w12, [x9, #0xac]   set while the album makes its photo textures ('album_on' / 'album_off')
        0x3500020c,   # cbnz w12, done
        0x29402829,   # ldp w9, w10, [x1]
        0x340001ca,   # cbz w10, done
        *_widen,
        0xb900002c,   # str w12, [x1]
    ]),
    'win': (0x105c0e0, 'texture descriptor (the swap chain): a 16:9 size takes the screen\'s width', [
        0xf9400021,   # orig: ldr x1, [x1]
        'adrp9',
        0xb940a12b,   # ldr w11, [x9, #0xa0]
        0x3400020b,   # cbz w11, done
        0x2942a849,   # ldp w9, w10, [x2, #0x14]
        0x340001ca,   # cbz w10, done
        *_widen,
        0xb900144c,   # str w12, [x2, #0x14]
    ]),
    'album_on': (0x100dec4, 'the album\'s photo textures keep their size (state +0xac set)', [
        0xaa0003f3,   # orig: mov x19, x0
        'adrp9',
        0x5280002a,   # mov w10, #1
        0xb900ad2a,   # str w10, [x9, #0xac]
    ]),
    'album_off': (0x100e478, 'the album\'s photo textures keep their size (state +0xac cleared)', [
        0x2a1303e0,   # orig: mov w0, w19
        'adrp8',
        0xb900ad1f,   # str wzr, [x8, #0xac]
    ]),
    'cam': (0x1e225ec, 'camera projection: a 16:9 camera takes the screen\'s ratio (Hor+)', [
        'adrp16',
        0xb940a611,   # ldr w17, [x16, #0xa4]  the screen's ratio (0 = 16:9)
        0x340000f1,   # cbz w17, orig
        0xb9414c08,   # ldr w8, [x0, #0x14c]   the camera's ratio
        0x5291c730,   # mov w16, #0x8e39
        0x72a7fc70,   # movk w16, #0x3fe3, lsl #16   16/9
        0x6b10011f,   # cmp w8, w16
        0x54000041,   # b.ne orig
        0xb9014c11,   # str w17, [x0, #0x14c]
        0x39456008,   # orig: ldrb w8, [x0, #0x158]
    ]),
    'anchor': (0x100c6b4, 'a point in the world lands where the wider camera draws it, in the canvas\'s coordinates', [
        'adrp9',
        0xb940a129,   # ldr w9, [x9, #0xa0]
        0x34000129,   # cbz w9, orig
        0x1e230123,   # ucvtf s3, w9
        0x1e2c1004,   # fmov s4, #0.5
        0x1e243821,   # fsub s1, s1, s4     (x_ndc + 1) / 2 - 0.5
        0x1e230821,   # fmul s1, s1, s3
        0x52a88e09,   # mov w9, #0x44700000  960.0
        0x1e270124,   # fmov s4, w9
        0x1e242821,   # fadd s1, s1, s4
        0x14000002,   # b done
        0x1e220821,   # orig: fmul s1, s1, s2   x 1920.0
    ]),
    'rect_w': (0xdea6ac, 'a full-canvas screen rect spans the screen\'s width (the door wipe\'s black)', [
        'adrp16',
        0xb940a211,   # ldr w17, [x16, #0xa0]   the output width (0 = 16:9)
        0x34000111,   # cbz w17, orig
        0xb9401a70,   # ldr w16, [x19, #0x18]   the rect's width in the canvas
        0x711e021f,   # cmp w16, #1920
        0x540000a1,   # b.ne orig
        0xb9401e70,   # ldr w16, [x19, #0x1c]   its height
        0x7110e21f,   # cmp w16, #1080
        0x54000041,   # b.ne orig
        0x1e230222,   # ucvtf s2, w17          the screen's width
        0x1e38004a,   # orig: fcvtzs w10, s2
    ]),
    'rect_x': (0xdea6bc, 'a screen rect\'s centre is placed in the canvas in the middle of the screen', [
        0x1e210901,   # orig: fmul s1, s8, s1   x scale
        'adrp16',
        0xb940a211,   # ldr w17, [x16, #0xa0]
        0x34000111,   # cbz w17, done
        0x1e230223,   # ucvtf s3, w17
        0x1e2c1004,   # fmov s4, #0.5
        0x1e240863,   # fmul s3, s3, s4        width / 2
        0x52a88e10,   # mov w16, #0x44700000   960.0
        0x1e270204,   # fmov s4, w16
        0x1f088c83,   # fmsub s3, s4, s8, s3   width / 2 - 960 x scale
        0x1e232821,   # fadd s1, s1, s3
    ]),
    'hud': (0x1e82690, 'full-screen pictures stretch to the screen\'s width; the gameplay HUD\'s listed panes move out by the extra half-width', [
        'adrp16',
        0xb940a210,   # ldr w16, [x16, #0xa0]   the output width (0 = 16:9)
        0x340004f0,   # cbz w16, orig
        0x39444a6a,   # ldrb w10, [x19, #0x112]
        0x7100055f,   # cmp w10, #1
        0x54000060,   # b.eq size
        0x71000d5f,   # cmp w10, #3
        0x54000261,   # b.ne listed
        0x79c1e26a,   # size: ldrsh w10, [x19, #0xf0]
        0x711e015f,   # cmp w10, #1920
        0x5400020b,   # b.lt listed
        0xd503201f,   # nop
        0xd503201f,   # nop
        0xd503201f,   # nop
        0xf9406a6b,   # ldr x11, [x19, #0xd0]   the parent
        0xb400016b,   # cbz x11, listed
        0x3944496a,   # ldrb w10, [x11, #0x112]
        0x3500012a,   # cbnz w10, listed
        0x1e220206,   # scvtf s6, w16
        0x5280f00a,   # mov w10, #1920
        0x1e220147,   # scvtf s7, w10
        0x1e2718c6,   # fdiv s6, s6, s7
        0x3dc00123,   # ldr q3, [x9]            the global matrix's x axis
        0x4f869063,   # fmul v3.4s, v3.4s, v6.s[0]
        0x3d800123,   # str q3, [x9]
        0x14000010,   # b orig
        0xb940c26a,   # listed: ldr w10, [x19, #0xc0]   the pane's name hash
        0x10000211,   # adr x17, list
        0xb840462b,   # 1: ldr w11, [x17], #4
        0x3400018b,   # cbz w11, orig           not listed
        0x6b0a017f,   # cmp w11, w10
        0x54ffffa1,   # b.ne 1b
        0xbd403123,   # ldr s3, [x9, #0x30]     the global x the matrix calc just stored
        0x511e0210,   # sub w16, w16, #1920
        0x13017e10,   # asr w16, w16, #1        the extra half-width
        0x1e220206,   # scvtf s6, w16
        0x1e2140c7,   # fneg s7, s6
        0x1e202068,   # fcmp s3, #0.0
        0x1e264ce6,   # fcsel s6, s7, s6, mi    towards the pane's side
        0x1e262863,   # fadd s3, s3, s6
        0xbd003123,   # str s3, [x9, #0x30]
        0xf9401909,   # orig: ldr x9, [x8, #0x30]
        0x1400000b,   # b over the list
        0xd81f3cb5, 0xf13b1a7c, 0x0623e2bd, 0x1b0caafe, 0x1569295d, 0x31d72b49, 0xe75fc20a, 0x2074df4f, 0x78801aae, 0,
    ]),
}
_get = [
    'adrp9',
    0xb940a129,   # ldr w9, [x9, #0xa0]
    0x340000e9,   # cbz w9, ret
    0x1b097c00,   # mul w0, w0, w9
    0x111dfc00,   # add w0, w0, #1919
    0x5280f00a,   # mov w10, #1920
    0x1aca0800,   # udiv w0, w0, w10
    0x11000400,   # add w0, w0, #1
    0x121f7800,   # and w0, w0, #~1
    0xd65f03c0,   # ret
]
_float_sites = [(0x104f364, 8), (0x104f61c, 8), (0xfff1c4, 8), (0xfff450, 8), (0xd3aac, 9), (0x100edd4, 9), (0x1018bac, 8), (0x1018c08, 8)]
_get_sites = [(0x103a1c4, 'render width (1920)'), (0x103a1ec, 'render width of a dynamic resolution level (the table)'), (0x103a1f4, 'render width of a level out of range (1920)')]
_canvas_sites = [0x86bf88, 0x9bc69c, 0x9bc8d0, 0x9bced4, 0x9c0ee0, 0x9c1630, 0x9c1658, 0x9c29c4, 0x9c29ec, 0x9c3344, 0x9c44e0, 0x9c4500, 0x9c4b08, 0x9c4b30,
                 0x9f3ff4, 0x9f4050, 0x9f407c, 0x9f4190, 0x9f41bc, 0x9f41e4, 0xdbf458, 0xdda9e4, 0xddab50, 0xde5064, 0xdea66c, 0xe0c3e4, 0xe8d438, 0xe8d62c,
                 0xe8e7a8, 0xe8ef98, 0xe8f320, 0xe8fab0, 0xe8fd20, 0xe902b0, 0xeb5ce0, 0xf0f45c, 0xf10074]
_canvas = [
    0xf941a908,   # ldr x8, [x8, #0x350]
    0xb4000128,   # cbz x8, none
    0xb9404900,   # ldr w0, [x8, #0x48]
    0xb9404d09,   # ldr w9, [x8, #0x4c]
    0x531c6d29,   # lsl w9, w9, #4
    0x5280012a,   # mov w10, #9
    0x1aca0929,   # udiv w9, w9, w10
    0x6b09001f,   # cmp w0, w9
    0x1a808120,   # csel w0, w9, w0, hi
    0xd65f03c0,   # ret
    0x5280f000,   # none: mov w0, #1920
    0xd65f03c0,   # ret
]


def _bl(frm, to):
    off = to - frm; assert -(1 << 27) <= off < (1 << 27) and off % 4 == 0
    return 0x94000000 | ((off >> 2) & 0x3FFFFFF)


_orig = {'anchor': '2108221e', 'rect_w': '4a00381e', 'rect_x': '0109211e', 'rt': '371040b9', 'win': '210040f9', 'cam': '08604539', 'album_on': 'f30300aa', 'album_off': 'e003132a', 'hud': '091940f9'}


def _emit(ctx, words):
    for w in words:
        if w == 'adrp9': ctx.emit(ADRP(9, ctx.here(), ctx.state))
        elif w == 'adrp16': ctx.emit(ADRP(16, ctx.here(), ctx.state))
        elif w == 'adrp8': ctx.emit(ADRP(8, ctx.here(), ctx.state))
        else: ctx.emit(w)


def screen_shape_v327680(ctx):
    for name, (site, why, words) in _stubs.items():
        ctx.site(site, _orig[name])
        start = ctx.here(); _emit(ctx, words); ctx.emit(B(ctx.here(), site + 4))
        ctx.put(site, [B(site, start)], 'screen shape: ' + why + f' (DM_SCREEN_ASPECT, state +{_wide_w:#x} / +{_wide_a:#x})')
    for site, n in _float_sites:
        orig = (0xd2800000 if site == 0x1018c08 else 0x52800000) | 0x44f0 << 5 | 1 << 21 | n
        ctx.site(site, orig.to_bytes(4, 'little').hex())
        start = ctx.here(); ctx.emit(ADRP(n, ctx.here(), ctx.state))
        ctx.emit(0xb940a800 | n << 5 | n)
        ctx.emit(0x35000040 | n)
        ctx.emit(orig)
        ctx.emit(B(ctx.here(), site + 4))
        ctx.put(site, [B(site, start)], 'screen shape: the renderer\'s 1920.0 becomes the output width (DM_SCREEN_ASPECT, state +0xa8)')
    for site, why in _get_sites:
        ctx.site(site, 'c0035fd6')
        start = ctx.here(); _emit(ctx, _get)
        ctx.put(site, [B(site, start)], f'screen shape: {why} takes the screen\'s width (DM_SCREEN_ASPECT, state +{_wide_w:#x})')
    start = ctx.here(); ctx.emit(ADRP(8, ctx.here(), 0x3451000))
    for w in _canvas: ctx.emit(w)
    for site in _canvas_sites:
        ctx.site(site, _bl(site, 0xd3ca0).to_bytes(4, 'little').hex())
        ctx.put(site, [_bl(site, start)], 'screen shape: the UI\'s text and map scale takes the 16:9 canvas\'s width, min(width, height x 16 / 9) (DM_SCREEN_ASPECT)')
    ctx.capabilities.add('WIDE_ASPECT')


FRAME_STEP = {
    '816c5358e8178d802fcec4a7ce5a741a': screen_shape_v327680,   # v327680 main
}

BINDING = [
    {'what': 'screen shape wider than 16:9: the render sizes, the swap chain, the renderer\'s 1920.0 constants, the cameras\' ratio and the gameplay HUD at the edges (DM_SCREEN_ASPECT)', 'now': 'build', 'goal': 'game', 'purpose': 'rendering',
     'why': "the output size is the host's, but where KTGL takes its sizes and its cameras' ratio is this build's code",
     'blocked': 'the stubs have no function-matched form yet'},
]
