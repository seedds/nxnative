# Pokémon Pokopia (0400A3A01EE4C000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO build
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
    # The album (main+0x100dea0 makes its textures: two 1920x1080 at +0xd8 / +0xe0, 25 480x270 photo thumbnails, two 320x180, ten 640x360) fills them with saved
    # photos as pixels of those widths: widened, the title screen's two photos came out sheared into repeated strips. State +0xac is set from that function's start
    # to its end, and the render target stub leaves sizes as they are while it is set
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
    # A point in the world onto the canvas (main+0x100c60c, the anchors of in-world prompts and labels): the clip-space x maps to 0 .. 1920, which spans the screen
    # only at 16:9; with the camera as wide as the screen the canvas x is x_ndc x width / 2 + 960. A leaf: x9, s3 and s4 are dead here
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
    # Screen-space rects (main+0xdea5xx: a rect's size at +0x18 and its centre at +0x48, in the canvas's 1920x1080, scaled into the screen's pixels by width / 1920 and
    # height / 1080). The door wipe draws its house-shaped mask (a square around the player) and four full-canvas fills with it. With the canvas scale (main+0xdea66c is
    # one of the calls above) the square stays square but the rects were placed from the screen's left edge, so the wipe's black stopped 1920 pixels in and the house sat
    # left of the player; the map's icons (the map beside the menu outdoors, placed in the canvas's top-left coordinates by main+0x9f52b0) too. A centre x moves by
    # width / 2 - 960 x scale (the canvas in the middle of the screen), and a rect of the whole canvas spans the whole width, as the wipe's fills must. x16, x17, s3 and s4 are free at both sites
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
    'hud': (0x1e82690, 'full-screen pictures stretch to the screen\'s width; the gameplay HUD\'s listed panes move out by the extra half-width; the in-world button diamond takes an Xbox controller\'s layout for other controllers\' prompts', [
        'adrp16',
        # The in-world button diamond (the four face buttons on a cloud beside a prompt such as "A Leave": the button to press pops out of a Switch controller's diamond into the prompt's
        # bubble), in three versions by the button that pops out (A right, Y left, X top). With an Xbox controller's labels (state +0xb0, DM_PAD_MAP=label) each takes an Xbox
        # controller's places on its own cloud: A bottom, B right, X left, Y top, the popped button's bubble beside it. Each listed pane's global translation, as just computed, moves by
        # a fixed step (layout pixels, y up; the diamond's arm is 36), so the game's animations stay; pad_uv points window parts at other places of the UI sheet (their materials'
        # texture coordinates: top left, top right, bottom left, bottom right at +0x08), so that each cloud is drawn without the bubble or the arm it carries for the Switch's places
        # and the A and Y prompts' pill windows draw the bubble (the A prompt's bubble's own pixels: its left end on its cloud is lost to the cloud's mirrored lobe, so it is the right
        # end mirrored). The cloud's mirrored lobe is the Xbox icon sets' (button_icons.tsv). The text and bubble of the X prompt turn to the left by their origin byte (+0x10c, 0x01 left,
        # 0x21 right, as the Y prompt's), the text also by its lines' origin and alignment (+0x206 0x21, +0x207 3: right): the layout keeps them, so the stub writes them each time.
        # Only the prompts' own panes (their parent is one of the three roots): the pick-up prompt ("Y Leppa Berries", no cloud) has a text of the same name, which moved up out of its pill
        0xb940b20a,   # ldr w10, [x16, #0xb0]     1 when the prompts follow an Xbox controller's layout (loader.c)
        0x34001bea,   # cbz w10, keep
        0xf9406a6b,   # ldr x11, [x19, #0xd0]     its parent: one of the three prompts (other layouts reuse these names: the pick-up prompt's text is 8e0aa2dc too)
        0xb4001bab,   # cbz x11, keep
        0xb940c16b,   # ldr w11, [x11, #0xc0]
        0x100005f1,   # adr x17, pad_roots
        0xb840462a,   # 0: ldr w10, [x17], #4
        0x34001b2a,   # cbz w10, keep
        0x6b0b015f,   # cmp w10, w11
        0x54ffffa1,   # b.ne 0b
        0xb940c26a,   # ldr w10, [x19, #0xc0]     the pane's name hash
        0x100005b1,   # adr x17, pad_list
        0xb841062b,   # 1: ldr w11, [x17], #16
        0x3400026b,   # cbz w11, uv               not listed
        0x6b0a017f,   # cmp w11, w10
        0x54ffffa1,   # b.ne 1b
        0xbc5f4223,   # ldur s3, [x17, #-12]      the pane's move, x (layout pixels)
        0xbd403126,   # ldr s6, [x9, #0x30]       the global x just computed
        0x1e262863,   # fadd s3, s3, s6
        0xbd003123,   # str s3, [x9, #0x30]
        0xbc5f8223,   # ldur s3, [x17, #-8]       y (up)
        0xbd403526,   # ldr s6, [x9, #0x34]
        0x1e262863,   # fadd s3, s3, s6
        0xbd003523,   # str s3, [x9, #0x34]
        0xb85fc22b,   # ldur w11, [x17, #-4]      its origin byte (+0x10c), if it changes
        0x340000eb,   # cbz w11, uv
        0x3904326b,   # strb w11, [x19, #0x10c]
        0x53087d6b,   # lsr w11, w11, #8          a text's own: its lines' origin (+0x206) and alignment (+0x207)
        0x3400008b,   # cbz w11, uv
        0x39081a6b,   # strb w11, [x19, #0x206]
        0x53087d6b,   # lsr w11, w11, #8
        0x39081e6b,   # strb w11, [x19, #0x207]
        0x10000e31,   # uv: adr x17, pad_uv       window parts drawn from another place of the 4096x2048 UI sheet
        0xb841862b,   # 2: ldr w11, [x17], #24
        0x340017cb,   # cbz w11, keep
        0x6b0a017f,   # cmp w11, w10
        0x54ffffa1,   # b.ne 2b
        0xb89ec22b,   # ldursw x11, [x17, #-20]   the part (+0x190 + 8 * i)
        0xf86b6a6b,   # ldr x11, [x19, x11]
        0xb4ffff4b,   # cbz x11, 2b
        0xf940316b,   # ldr x11, [x11, #0x60]     its material
        0xb4ffff0b,   # cbz x11, 2b
        0xbc5f0223,   # ldur s3, [x17, #-16]      u0
        0xbc5f4226,   # ldur s6, [x17, #-12]      v0
        0x2d011963,   # stp s3, s6, [x11, #0x8]   top left
        0xbc5f8223,   # ldur s3, [x17, #-8]       u1
        0x2d021963,   # stp s3, s6, [x11, #0x10]  top right
        0xbc5fc226,   # ldur s6, [x17, #-4]       v1
        0x2d041963,   # stp s3, s6, [x11, #0x20]  bottom right
        0xbc5f0223,   # ldur s3, [x17, #-16]
        0x2d031963,   # stp s3, s6, [x11, #0x18]  bottom left
        0x17ffffee,   # b 2b
        0x0bd9224c, 0x0149e464, 0x6c6531a3, 0x00000000,   # pad_roots: the prompts' roots (A, Y, X), the parents of every pane listed
        # pad_list: A pops out on the right (root 0bd9224c), the layout of every A prompt: Y top, X left, B right; the popped A, its text and its bubble go to the bottom, 12 lower than the arm so the bubble clears B
        0x7708cd35, 0xc2100000, 0xc2400000, 0x00000000,   # the bubble (pad_bubble: the root's own pill window, drawn with the bubble's pixels) (-36, -48)
        0x23d9ab35, 0xc2100000, 0xc2400000, 0x00000000,   # the popped button's ring (-36, -48)
        0xfb32aa8d, 0xc2100000, 0xc2400000, 0x00000000,   # its icon (-36, -48)
        0x205d6ff4, 0xc2100000, 0xc2400000, 0x00000000,   # its text (-36, -48)
        0x8022474d, 0x42100000, 0x42100000, 0x00000000,   # Y left -> top (+36, +36)
        0x919f40cc, 0xc2100000, 0xc2100000, 0x00000000,   # X top -> left (-36, -36)
        0x125cb1b6, 0x42100000, 0x42100000, 0x00000000,   # B bottom -> right (+36, +36)
        # pad_list: Y pops out on the left (root 0149e464): X left, B right, A bottom; the popped Y, its text and its bubble (both left of it) go to the top, 12 higher than the arm so the bubble clears X
        0xf4e08936, 0x42100000, 0x42400000, 0x00000000,   # the bubble (pad_bubble: the root's pill window, drawn with the bubble's pixels) (+36, +48)
        0x44b4296c, 0x42100000, 0x42400000, 0x00000000,   # the popped button's ring (+36, +48)
        0x9b580f75, 0x42100000, 0x42400000, 0x00000000,   # its icon (+36, +48)
        0x8e0aa2dc, 0x42100000, 0x42400000, 0x00000000,   # its text (+36, +48)
        0x563122eb, 0xc2100000, 0xc2100000, 0x00000000,   # X top -> left (-36, -36)
        0xd6ee93d5, 0x42100000, 0x42100000, 0x00000000,   # B bottom -> right (+36, +36)
        0xe86b8d54, 0xc2100000, 0xc2100000, 0x00000000,   # A right -> bottom (-36, -36)
        # pad_list: X pops out on top (root 6c6531a3): Y top, B right, A bottom; the popped X goes to the left, its bubble and text turned to its left (origin +0x10c 0x21, as the Y prompt's)
        0x1ac3050a, 0xc2100000, 0xc2100000, 0x00000000,   # the popped button's ring (-36, -36)
        0x9f568096, 0xc2100000, 0xc2100000, 0x00000000,   # its icon (-36, -36)
        0xc978c0bd, 0xc2d80000, 0xc2100000, 0x00032121,   # its text, right-aligned: its right end 36 left of the ring (-108, -36, origin 0x32121)
        0x043d9bd0, 0x41e00000, 0xc2100000, 0x00000021,   # the bubble: right end 32 right of the ring (+28, -36, origin 0x21)
        0x72b84537, 0x41a00000, 0xc2100000, 0x00000021,   # its glow: right end 28 right of the ring (+20, -36, origin 0x21)
        0x09460b8b, 0x42100000, 0x42100000, 0x00000000,   # Y left -> top (+36, +36)
        0xacfd6f73, 0xc2100000, 0xc2100000, 0x00000000,   # A right -> bottom (-36, -36)
        0x9b8075f4, 0x42100000, 0x42100000, 0x00000000,   # B bottom -> right (+36, +36)
        0x00000000,
        # pad_uv: hash, part offset, u0, v0, u1, v1 (fractions of 4096 and 2048)
        0x759a506f, 0x000001b0, 0x3f1e2000, 0x3f28c000, 0x3f1e3000, 0x3f28e000,   # A's cloud window: its bubble's middle drawn transparent: 2530,1350 -> 2531,1351
        0x759a506f, 0x000001c8, 0x3f1e2000, 0x3f28c000, 0x3f1e3000, 0x3f28e000,   # and its right end: 2530,1350 -> 2531,1351
        0x5b879957, 0x00000198, 0x3f1e2000, 0x3f28c000, 0x3f1e3000, 0x3f28e000,   # Y's cloud window (A's mirrored): its bubble's left end: 2530,1350 -> 2531,1351
        0x5b879957, 0x000001b0, 0x3f1e2000, 0x3f28c000, 0x3f1e3000, 0x3f28e000,   # and its middle: 2530,1350 -> 2531,1351
        0xe9819928, 0x00000198, 0x3f1e0000, 0x3f288000, 0x3f26c000, 0x3f3a0000,   # X's cloud (an arm up to the bubble): A's cloud instead: 2528,1348 -> 2668,1488
        0xe9819928, 0x000001b0, 0x3f1e2000, 0x3f28c000, 0x3f1e3000, 0x3f28e000,   # its arm: 2530,1350 -> 2531,1351
        0xe9819928, 0x000001c8, 0x3f1e2000, 0x3f28c000, 0x3f1e3000, 0x3f28e000,   # its arm's end: 2530,1350 -> 2531,1351
        0x7708cd35, 0x00000198, 0x3f294000, 0x3f2d2000, 0x3f273000, 0x3f356000,   # A's pill: left end: the right end mirrored: 2708,1385 -> 2675,1451
        0x7708cd35, 0x000001b0, 0x3f26d000, 0x3f2d2000, 0x3f26f000, 0x3f356000,   # A's pill: middle: 2669,1385 -> 2671,1451
        0x7708cd35, 0x000001c8, 0x3f273000, 0x3f2d2000, 0x3f294000, 0x3f356000,   # A's pill: right end: 2675,1385 -> 2708,1451
        0xf4e08936, 0x00000198, 0x3f294000, 0x3f2d2000, 0x3f273000, 0x3f356000,   # Y's pill: left end: the right end mirrored: 2708,1385 -> 2675,1451
        0xf4e08936, 0x000001b0, 0x3f26d000, 0x3f2d2000, 0x3f26f000, 0x3f356000,   # Y's pill: middle: 2669,1385 -> 2671,1451
        0xf4e08936, 0x000001c8, 0x3f273000, 0x3f2d2000, 0x3f294000, 0x3f356000,   # Y's pill: right end: 2675,1385 -> 2708,1451
        0x00000000,
        0xd503201f,   # keep: nop
        0xb940a210,   # ldr w16, [x16, #0xa0]   the output width (0 = 16:9)
        0x340004f0,   # cbz w16, orig
        # A picture or window pane (type +0x112 1 or 3) at least 1920 wide whose parent is a null pane (type 0): its x axis widens by the output width / 1920, so
        # fades, the loading screen's bands and dimmers cover the whole screen (the title screen's art stretches with them) (a child of a stretched pane follows it, and a stretched pane's own children are not
        # stretched again, their parent being a picture)
        0x39444a6a,   # ldrb w10, [x19, #0x112]
        0x7100055f,   # cmp w10, #1
        0x54000060,   # b.eq size
        0x71000d5f,   # cmp w10, #3
        0x54000261,   # b.ne listed
        0x79c1e26a,   # size: ldrsh w10, [x19, #0xf0]
        0x711e015f,   # cmp w10, #1920
        0x5400020b,   # b.lt listed
        0xd503201f,   # nop (a height test here kept the loading screen's 1920x340 band and its black bands 16:9 wide)
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
        0x1400000b,   # b over the list (to the branch back)
        # the gameplay HUD layout's panes that stand in the edge band (|x| >= 600 of 960) as direct children of its 1920x1080 canvas: the button guide (2074df4f, 432x310 at
        # -725,-385), the tool wheel (1569295d, 376x316 at 763,-443), and the others of that layout found with them (top left d81f3cb5 392x180, e75fc20a 346x98; left
        # 31d72b49 512x512, 78801aae 494x356; right f13b1a7c / 0623e2bd / 1b0caafe 540x56). The same rule on every layout pulled the main menu's pieces apart
        0xd81f3cb5, 0xf13b1a7c, 0x0623e2bd, 0x1b0caafe, 0x1569295d, 0x31d72b49, 0xe75fc20a, 0x2074df4f, 0x78801aae, 0,
    ]),
    # The in-world prompt's pill windows (7708cd35 under the A prompt's root, which the layout keeps transparent; f4e08936 under the Y prompt's, a soft glow behind its bubble): with
    # an Xbox controller's layout (state +0xb0) each is the bubble beside the popped button (the hud stub moves them and points them at the bubble's pixels), since the bubble the game
    # draws is one window with the cloud: the A prompt's is drawn with the prompt's alpha, and both get the bubble's height and ends. main+0x1e82c00 stores a pane's alpha with its
    # parent's (0 skips its matrix and its drawing), and main+0x1e82dfc, main+0x1e82eac and main+0x1e830ac store it again on the other update paths; at each, x10 and x16 are free
    # (written next or unused)
    'pad_bubble': (0x1e82c00, 'the in-world prompt\'s pill windows are its bubble for an Xbox controller\'s layout', [
        'adrp16',
        0xb940b20a,   # ldr w10, [x16, #0xb0]     1 when the prompts follow an Xbox controller's layout (loader.c)
        0x340003ca,   # cbz w10, orig
        0xf9406a6a,   # ldr x10, [x19, #0xd0]     the parent: the A or Y prompt (the pick-up prompt's pill has the same name)
        0xb400038a,   # cbz x10, orig
        0xb940c14a,   # ldr w10, [x10, #0xc0]
        0x52844990,   # mov w16, #0x224c
        0x72a17b30,   # movk w16, #0x0bd9, lsl #16
        0x6b10015f,   # cmp w10, w16              the A prompt
        0x54000121,   # b.ne 1f
        0xb940c26a,   # ldr w10, [x19, #0xc0]     the pane's name hash
        0x5299a6b0,   # mov w16, #0xcd35
        0x72aee110,   # movk w16, #0x7708, lsl #16
        0x6b10015f,   # cmp w10, w16              its pill
        0x54000241,   # b.ne orig
        0xf9406a6a,   # ldr x10, [x19, #0xd0]
        0x39444d48,   # ldrb w8, [x10, #0x113]    drawn with the prompt's alpha (its own is 0: the layout keeps it transparent)
        0x1400000a,   # b 2f
        0x529c8c90,   # 1: mov w16, #0xe464
        0x72a02930,   # movk w16, #0x0149, lsl #16
        0x6b10015f,   # cmp w10, w16              the Y prompt
        0x54000161,   # b.ne orig
        0xb940c26a,   # ldr w10, [x19, #0xc0]
        0x529126d0,   # mov w16, #0x8936
        0x72be9c10,   # movk w16, #0xf4e0, lsl #16
        0x6b10015f,   # cmp w10, w16              its pill (drawn by the layout: a soft glow)
        0x540000c1,   # b.ne orig
        0x5280080a,   # 2: mov w10, #64           the height of the game's bubble (52 is less), the ends 32 wide (read by its next layout)
        0x7901e66a,   # strh w10, [x19, #0xf2]
        0x52a8400a,   # mov w10, #0x42000000      32.0
        0xb901866a,   # str w10, [x19, #0x184]    left frame
        0xb9018a6a,   # str w10, [x19, #0x188]    right frame
        0x39044e68,   # orig: strb w8, [x19, #0x113]
    ]),
    'pad_bubble2': (0x1e82dfc, 'the same, on another update path', [
        'adrp16',
        0xb940b20a,   # ldr w10, [x16, #0xb0]     1 when the prompts follow an Xbox controller's layout (loader.c)
        0x340003ca,   # cbz w10, orig
        0xf9406a6a,   # ldr x10, [x19, #0xd0]     the parent: the A or Y prompt (the pick-up prompt's pill has the same name)
        0xb400038a,   # cbz x10, orig
        0xb940c14a,   # ldr w10, [x10, #0xc0]
        0x52844990,   # mov w16, #0x224c
        0x72a17b30,   # movk w16, #0x0bd9, lsl #16
        0x6b10015f,   # cmp w10, w16              the A prompt
        0x54000121,   # b.ne 1f
        0xb940c26a,   # ldr w10, [x19, #0xc0]     the pane's name hash
        0x5299a6b0,   # mov w16, #0xcd35
        0x72aee110,   # movk w16, #0x7708, lsl #16
        0x6b10015f,   # cmp w10, w16              its pill
        0x54000241,   # b.ne orig
        0xf9406a6a,   # ldr x10, [x19, #0xd0]
        0x39444d48,   # ldrb w8, [x10, #0x113]    drawn with the prompt's alpha (its own is 0: the layout keeps it transparent)
        0x1400000a,   # b 2f
        0x529c8c90,   # 1: mov w16, #0xe464
        0x72a02930,   # movk w16, #0x0149, lsl #16
        0x6b10015f,   # cmp w10, w16              the Y prompt
        0x54000161,   # b.ne orig
        0xb940c26a,   # ldr w10, [x19, #0xc0]
        0x529126d0,   # mov w16, #0x8936
        0x72be9c10,   # movk w16, #0xf4e0, lsl #16
        0x6b10015f,   # cmp w10, w16              its pill (drawn by the layout: a soft glow)
        0x540000c1,   # b.ne orig
        0x5280080a,   # 2: mov w10, #64           the height of the game's bubble (52 is less), the ends 32 wide (read by its next layout)
        0x7901e66a,   # strh w10, [x19, #0xf2]
        0x52a8400a,   # mov w10, #0x42000000      32.0
        0xb901866a,   # str w10, [x19, #0x184]    left frame
        0xb9018a6a,   # str w10, [x19, #0x188]    right frame
        0x39044e68,   # orig: strb w8, [x19, #0x113]
    ]),
    'pad_bubble3': (0x1e82eac, 'the same, on another update path', [
        'adrp16',
        0xb940b20a,   # ldr w10, [x16, #0xb0]     1 when the prompts follow an Xbox controller's layout (loader.c)
        0x340003ca,   # cbz w10, orig
        0xf9406a6a,   # ldr x10, [x19, #0xd0]     the parent: the A or Y prompt (the pick-up prompt's pill has the same name)
        0xb400038a,   # cbz x10, orig
        0xb940c14a,   # ldr w10, [x10, #0xc0]
        0x52844990,   # mov w16, #0x224c
        0x72a17b30,   # movk w16, #0x0bd9, lsl #16
        0x6b10015f,   # cmp w10, w16              the A prompt
        0x54000121,   # b.ne 1f
        0xb940c26a,   # ldr w10, [x19, #0xc0]     the pane's name hash
        0x5299a6b0,   # mov w16, #0xcd35
        0x72aee110,   # movk w16, #0x7708, lsl #16
        0x6b10015f,   # cmp w10, w16              its pill
        0x54000241,   # b.ne orig
        0xf9406a6a,   # ldr x10, [x19, #0xd0]
        0x39444d48,   # ldrb w8, [x10, #0x113]    drawn with the prompt's alpha (its own is 0: the layout keeps it transparent)
        0x1400000a,   # b 2f
        0x529c8c90,   # 1: mov w16, #0xe464
        0x72a02930,   # movk w16, #0x0149, lsl #16
        0x6b10015f,   # cmp w10, w16              the Y prompt
        0x54000161,   # b.ne orig
        0xb940c26a,   # ldr w10, [x19, #0xc0]
        0x529126d0,   # mov w16, #0x8936
        0x72be9c10,   # movk w16, #0xf4e0, lsl #16
        0x6b10015f,   # cmp w10, w16              its pill (drawn by the layout: a soft glow)
        0x540000c1,   # b.ne orig
        0x5280080a,   # 2: mov w10, #64           the height of the game's bubble (52 is less), the ends 32 wide (read by its next layout)
        0x7901e66a,   # strh w10, [x19, #0xf2]
        0x52a8400a,   # mov w10, #0x42000000      32.0
        0xb901866a,   # str w10, [x19, #0x184]    left frame
        0xb9018a6a,   # str w10, [x19, #0x188]    right frame
        0x39044e68,   # orig: strb w8, [x19, #0x113]
    ]),
    'pad_bubble4': (0x1e830ac, 'the same, on another update path', [
        'adrp16',
        0xb940b20a,   # ldr w10, [x16, #0xb0]     1 when the prompts follow an Xbox controller's layout (loader.c)
        0x340003ca,   # cbz w10, orig
        0xf9406a6a,   # ldr x10, [x19, #0xd0]     the parent: the A or Y prompt (the pick-up prompt's pill has the same name)
        0xb400038a,   # cbz x10, orig
        0xb940c14a,   # ldr w10, [x10, #0xc0]
        0x52844990,   # mov w16, #0x224c
        0x72a17b30,   # movk w16, #0x0bd9, lsl #16
        0x6b10015f,   # cmp w10, w16              the A prompt
        0x54000121,   # b.ne 1f
        0xb940c26a,   # ldr w10, [x19, #0xc0]     the pane's name hash
        0x5299a6b0,   # mov w16, #0xcd35
        0x72aee110,   # movk w16, #0x7708, lsl #16
        0x6b10015f,   # cmp w10, w16              its pill
        0x54000241,   # b.ne orig
        0xf9406a6a,   # ldr x10, [x19, #0xd0]
        0x39444d48,   # ldrb w8, [x10, #0x113]    drawn with the prompt's alpha (its own is 0: the layout keeps it transparent)
        0x1400000a,   # b 2f
        0x529c8c90,   # 1: mov w16, #0xe464
        0x72a02930,   # movk w16, #0x0149, lsl #16
        0x6b10015f,   # cmp w10, w16              the Y prompt
        0x54000161,   # b.ne orig
        0xb940c26a,   # ldr w10, [x19, #0xc0]
        0x529126d0,   # mov w16, #0x8936
        0x72be9c10,   # movk w16, #0xf4e0, lsl #16
        0x6b10015f,   # cmp w10, w16              its pill (drawn by the layout: a soft glow)
        0x540000c1,   # b.ne orig
        0x5280080a,   # 2: mov w10, #64           the height of the game's bubble (52 is less), the ends 32 wide (read by its next layout)
        0x7901e66a,   # strh w10, [x19, #0xf2]
        0x52a8400a,   # mov w10, #0x42000000      32.0
        0xb901866a,   # str w10, [x19, #0x184]    left frame
        0xb9018a6a,   # str w10, [x19, #0x188]    right frame
        0x39044e68,   # orig: strb w8, [x19, #0x113]
    ]),
    # The same pills' width, as their window is laid out (main+0x1e905f8 loads the pane's width, from which the window places its ends and stretches its middle): the game's bubble
    # is 12 longer than the pill it sizes with it (the A prompt's 180 with the pill's 168), so the width read there grows by 12, which keeps the pill's pop animation (it grows from
    # less), its centre and its length for every text. The pane's own width is not changed, since what places the pane uses it
    'pad_width': (0x1e905f8, 'the in-world prompt\'s pill windows are as long as the game\'s bubble for an Xbox controller\'s layout', [
        'adrp16',
        0xb940b210,   # ldr w16, [x16, #0xb0]     1 when the prompts follow an Xbox controller's layout (loader.c)
        0x79c1e00d,   # ldrsh w13, [x0, #0xf0]    the original instruction: the window's width
        0x34000310,   # cbz w16, back
        0xf9406810,   # ldr x16, [x0, #0xd0]      the parent: the A or Y prompt
        0xb40002d0,   # cbz x16, back
        0xb940c210,   # ldr w16, [x16, #0xc0]
        0x52844991,   # mov w17, #0x224c
        0x72a17b31,   # movk w17, #0x0bd9, lsl #16
        0x6b11021f,   # cmp w16, w17              the A prompt
        0x540000e1,   # b.ne 1f
        0xb940c010,   # ldr w16, [x0, #0xc0]      the pane's name hash
        0x5299a6b1,   # mov w17, #0xcd35
        0x72aee111,   # movk w17, #0x7708, lsl #16
        0x6b11021f,   # cmp w16, w17              its pill
        0x54000160,   # b.eq 3f
        0x1400000b,   # b back
        0x529c8c91,   # 1: mov w17, #0xe464
        0x72a02931,   # movk w17, #0x0149, lsl #16
        0x6b11021f,   # cmp w16, w17              the Y prompt
        0x540000e1,   # b.ne back
        0xb940c010,   # ldr w16, [x0, #0xc0]
        0x529126d1,   # mov w17, #0x8936
        0x72be9c11,   # movk w17, #0xf4e0, lsl #16
        0x6b11021f,   # cmp w16, w17              its pill
        0x54000041,   # b.ne back
        0x110031ad,   # 3: add w13, w13, #12      the game's bubble is 12 longer than the pill the game sizes with it (x16 and x17 are written before they are read in this function)
    ]),
}
_get = [   # at a width getter's ret: w0 x the output width / 1920, rounded up to even
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
# The renderer's own 1920.0 (mov wN, #0x44f00000): the ground's depth and G-buffer passes take their viewport as the dynamic resolution scale x 1920.0 x 1080.0
# (main+0x104f364 and main+0x104f61c; at 0.9, 1728x972 while every other pass had 2304x972, so outdoors the terrain stopped at the 16:9 edge), main+0xfff1c4 and
# main+0xfff450 the same for another pass pair, main+0xd3aac and main+0x100edd4 write 1920.0 x 1080.0 into a pass's constants, main+0x1018bac / main+0x1018c08 an
# object's screen size (+0x4d0, compared and set). Each loads the output width as a float (state +0xa8) into the same register, or keeps 1920.0 when it is 0.
# The 1920.0 that the HUD code divides the screen width by (its scale, min(width / 1920, height / 1080)) stays
_float_sites = [(0x104f364, 8), (0x104f61c, 8), (0xfff1c4, 8), (0xfff450, 8), (0xd3aac, 9), (0x100edd4, 9), (0x1018bac, 8), (0x1018c08, 8)]
_get_sites = [(0x103a1c4, 'render width (1920)'), (0x103a1ec, 'render width of a dynamic resolution level (the table)'), (0x103a1f4, 'render width of a level out of range (1920)')]

# The UI's own sizes. KTGL's text and its map widgets take a screen scale, the screen's width / 1920 (main+0xd3ca0 returns the screen object's width, +0x48), assuming the
# screen is 16:9 like the 1920x1080 canvas the layouts are drawn in. On a wider screen the canvas stays 1920 wide in the middle (the HUD's scale is min(width / 1920,
# height / 1080)), so that scale came out 2560 / 1920 = 1.33 at 64:27: the letter spacing (FontSpace, main+0x9c15c8 stores it times that scale at text +0x1f4: -5 became -6.67)
# and the glyphs were squeezed while measured texts grew, so every button bar's pills widened and their texts slid to the right; the map's icons spread out from its centre.
# Every call of main+0xd3ca0 that makes that scale takes the canvas's width instead, min(width, height x 16 / 9): the same as the screen's on a 16:9 or taller screen.
# Two calls are left alone: main+0x100c8ec builds the UI's mapping onto the whole screen (2 / width), and main+0x101f88c sets a GPU viewport of the screen's width
# times a render scale; both must stay the screen's. The screen-space rects of main+0xdea5xx (the door wipe) take the canvas scale too, placed by 'rect_x' / 'rect_w'
_canvas_sites = [0x86bf88, 0x9bc69c, 0x9bc8d0, 0x9bced4, 0x9c0ee0, 0x9c1630, 0x9c1658, 0x9c29c4, 0x9c29ec, 0x9c3344, 0x9c44e0, 0x9c4500, 0x9c4b08, 0x9c4b30,
                 0x9f3ff4, 0x9f4050, 0x9f407c, 0x9f4190, 0x9f41bc, 0x9f41e4, 0xdbf458, 0xdda9e4, 0xddab50, 0xde5064, 0xdea66c, 0xe0c3e4, 0xe8d438, 0xe8d62c,
                 0xe8e7a8, 0xe8ef98, 0xe8f320, 0xe8fab0, 0xe8fd20, 0xe902b0, 0xeb5ce0, 0xf0f45c, 0xf10074]
_canvas = [   # after adrp x8, main+0x3451000 (the screen object's pointer at +0x350); x8..x10 are free at a call
    0xf941a908,   # ldr x8, [x8, #0x350]
    0xb4000128,   # cbz x8, none
    0xb9404900,   # ldr w0, [x8, #0x48]    the screen's width
    0xb9404d09,   # ldr w9, [x8, #0x4c]    its height
    0x531c6d29,   # lsl w9, w9, #4
    0x5280012a,   # mov w10, #9
    0x1aca0929,   # udiv w9, w9, w10       height x 16 / 9
    0x6b09001f,   # cmp w0, w9
    0x1a808120,   # csel w0, w9, w0, hi    the smaller
    0xd65f03c0,   # ret
    0x5280f000,   # none: mov w0, #1920
    0xd65f03c0,   # ret
]
def _bl(frm, to):
    off = to - frm; assert -(1 << 27) <= off < (1 << 27) and off % 4 == 0
    return 0x94000000 | ((off >> 2) & 0x3FFFFFF)

_orig = {'anchor': '2108221e', 'rect_w': '4a00381e', 'rect_x': '0109211e', 'rt': '371040b9', 'win': '210040f9', 'cam': '08604539', 'album_on': 'f30300aa', 'album_off': 'e003132a', 'hud': '091940f9', 'pad_bubble': '684e0439', 'pad_bubble2': '684e0439', 'pad_bubble3': '684e0439', 'pad_bubble4': '684e0439', 'pad_width': '0de0c179'}   # each site's original instruction

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
        tag = 'button layout: ' if name.startswith('pad_') else 'screen shape: '
        ctx.put(site, [B(site, start)], tag + why + (' (DM_PAD_MAP=label, state +0xb0)' if name.startswith('pad_') else f' (DM_SCREEN_ASPECT, state +{_wide_w:#x} / +{_wide_a:#x})'))
    for site, n in _float_sites:
        orig = (0xd2800000 if site == 0x1018c08 else 0x52800000) | 0x44f0 << 5 | 1 << 21 | n   # mov wN / xN, #0x44f00000
        ctx.site(site, orig.to_bytes(4, 'little').hex())
        start = ctx.here(); ctx.emit(ADRP(n, ctx.here(), ctx.state))
        ctx.emit(0xb940a800 | n << 5 | n)   # ldr wN, [xN, #0xa8]   the output width as a float (0 = 16:9)
        ctx.emit(0x35000040 | n)            # cbnz wN, back
        ctx.emit(orig)
        ctx.emit(B(ctx.here(), site + 4))
        ctx.put(site, [B(site, start)], 'screen shape: the renderer\'s 1920.0 becomes the output width (DM_SCREEN_ASPECT, state +0xa8)')
    for site, why in _get_sites:
        ctx.site(site, 'c0035fd6')   # ret
        start = ctx.here(); _emit(ctx, _get)
        ctx.put(site, [B(site, start)], f'screen shape: {why} takes the screen\'s width (DM_SCREEN_ASPECT, state +{_wide_w:#x})')
    start = ctx.here(); ctx.emit(ADRP(8, ctx.here(), 0x3451000))
    for w in _canvas: ctx.emit(w)
    for site in _canvas_sites:
        ctx.site(site, _bl(site, 0xd3ca0).to_bytes(4, 'little').hex())
        ctx.put(site, [_bl(site, start)], 'screen shape: the UI\'s text and map scale takes the 16:9 canvas\'s width, min(width, height x 16 / 9) (DM_SCREEN_ASPECT)')

FRAME_STEP = {
    '816c5358e8178d802fcec4a7ce5a741a': screen_shape_v327680,   # v327680 main
}

BINDING = [
    {'what': 'screen shape wider than 16:9: the render sizes, the swap chain, the renderer\'s 1920.0 constants, the cameras\' ratio and the gameplay HUD at the edges (DM_SCREEN_ASPECT)', 'now': 'build', 'goal': 'game', 'purpose': 'rendering',
     'why': "the output size is the host's, but where KTGL takes its sizes and its cameras' ratio is this build's code",
     'blocked': 'the stubs have no function-matched form yet'},
]
