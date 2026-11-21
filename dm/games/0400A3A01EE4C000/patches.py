# Pokemon Pokopia (0400A3A01EE4C000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO build
# ID (FRAME_STEP as described there, "Registered patches"). The game has no frame step stubs: FRAME_STEP only gives its main the state page that the screen shape stubs read
# (DM_SCREEN_ASPECT, loader.c); with DM_SCREEN_ASPECT unset the page's fields stay zero and every stub runs the original instructions.
from nso_asm import ADRP, B


# ---- v655360: a screen taller than 16:9 (DM_SCREEN_ASPECT from 1.3 to 1.77, such as a MacBook's 16:10 or 2560x1664) ----
# The output keeps its width (1920) and takes the height H that gives the screen's ratio (loader.c: state +0x48 u32, +0x64 f32; 1.54 -> 1248). The 3D cameras keep their
# horizontal angle and show more above and below (Vert+); the UI keeps its 1920x1080 canvas, 1:1 and centred, with the world visible above and below it. The sites are the
# functions a v327680 wide-screen patch hooks (LNPANDY/nxnative 37b34ed), found again in v655360 at the same offsets inside them:
#   render heights: main+0x11299c0 / 0x11299d0 (1080, or the dynamic resolution heights at main+0x28496e0) return H / 1080 of it
#   render targets (main+0x179e0a0, descriptor x1 +0 / +4) and textures (main+0x114b9dc, descriptor x2 +0x14 / +0x18) of a 16:9 size take H / 1080 of their height,
#     except while the album makes its photo textures (main+0x10fd8e0, state +0xac)
#   the renderer's 1080.0 constants next to its 1920.0 ones (viewports, crops, size checks): H
#   cameras (main+0x1f12114, +0x148 fovy, +0x14c ratio, +0x158 flags): a camera of exactly 16:9 takes the screen's ratio with its last bit flipped, which marks it,
#     and the tan of the half angle of a marked camera, or of one at the screen's ratio by itself, is multiplied by (16/9) / ratio (state +0x5c). The UI layers'
#     cameras (main+0x1f528d0, built from a canvas size, in both of its branches) that come out at the screen's ratio are marked there with bit 1 flipped and left alone: scaling them
#     shrank the title screen by 1080 / 1248, and leaving every unmarked camera alone left characters off the ground
#   a world point to the 1920x1080 canvas (main+0x10fc06c): its y lands where the taller camera draws it
#   HUD panes (main+0x1f71de0): a full-screen picture (+0x112 anchor 1 or 3, +0xf2 height >= 1080, under a root) stretches to H. Moving that patch's list of edge widgets
#     moved button labels away from their buttons, so nothing moves yet
# DM_SCREEN_ASPECT_SITES (state +0x54, every bit when unset) turns groups off for trials: bit 0 cameras, 1 HUD stretch, 2 world point y, 3 renderer 1080.0, 4 render sizes.
# The stubs were assembled with clang -c, `nop` where the ADRP of the state page and the branch back go; the registers each one uses are free at its site.
_t_hget = ['adrp9', 0xb940552a, 0xb9404929, 0x34000109, 0x362000ea, 0x1b097c00, 0x1110dc00, 0x5280870a, 0x1aca0800, 0x11000400, 0x121f7800, 0xd65f03c0]
# ldr w10,[x9,#0x54]; ldr w9,[x9,#0x48]; cbz w9,ret; tbz w10,#4,ret; w0 = w0 x H / 1080 rounded up to even; ret   (a leaf)
_t_taller = [0x5280020c, 0x1b0c7d4c, 0x5280012d, 0x1b0d7d2d, 0x6b0d019f, 0x54000101, 0x1b0b7d4c, 0x1110dd8c, 0x5280870d, 0x1acd098c, 0x1100058c, 0x121f798c]
# w9 width, w10 height, w11 H: when w10 x 16 == w9 x 9, w12 = w10 x H / 1080 rounded up to even
_t_stubs = {
    'rt': (0x179e0c8, '371040b9', 'render target descriptor: a 16:9 size takes the screen\'s height (bit 4)', [
        0xb9401037,   # orig: ldr w23,[x1,#0x10]
        'adrp9', 0xb940492b,   # ldr w11,[x9,#0x48]   H (0 = 16:9)
        0x3400028b,   # cbz w11, done
        0xb940ad2c, 0x3500024c,   # ldr w12,[x9,#0xac]; cbnz w12, done   the album makes its photos
        0xb940552c, 0x3620020c,   # ldr w12,[x9,#0x54]; tbz w12,#4, done
        0x29402829, 0x340001ca,   # ldp w9,w10,[x1]; cbz w10, done
        *_t_taller,
        0xb900042c,   # str w12,[x1,#4]
    ]),
    'win': (0x114ba00, '210040f9', 'texture descriptor: a 16:9 size takes the screen\'s height (bit 4)', [
        0xf9400021,   # orig: ldr x1,[x1]
        'adrp9', 0xb940492b, 0x3400024b,   # ldr w11,[x9,#0x48]; cbz w11, done
        0xb940552c, 0x3620020c,   # ldr w12,[x9,#0x54]; tbz w12,#4, done
        0x2942a849, 0x340001ca,   # ldp w9,w10,[x2,#0x14]; cbz w10, done
        *_t_taller,
        0xb900184c,   # str w12,[x2,#0x18]
    ]),
    'album_on': (0x10fd904, 'f30300aa', 'the album\'s photo textures keep their size (state +0xac set)', [
        0xaa0003f3, 'adrp16', 0x52800031, 0xb900ae11,   # orig: mov x19,x0; mov w17,#1; str w17,[x16,#0xac]   (x16 not yet set in the function)
    ]),
    'album_off': (0x10fdb70, 'e0020012', 'the album\'s photo textures keep their size (state +0xac cleared)', [
        0x120002e0, 'adrp16', 0xb900ae1f,   # orig: and w0,w23,#1 (its one return path); str wzr,[x16,#0xac]
    ]),
    'cam': (0x1f1212c, '08604539', 'camera projection: a 16:9 camera takes the screen\'s ratio, marked (bit 0)', [
        'adrp16',
        0xb9405a11, 0x34000151,   # ldr w17,[x16,#0x58]; cbz w17, orig   the screen's ratio (0 = 16:9)
        0xb9405608, 0x36000108,   # ldr w8,[x16,#0x54]; tbz w8,#0, orig
        0xb9414c08,   # ldr w8,[x0,#0x14c]   the camera's ratio
        0x5291c730, 0x72a7fc70,   # 16/9
        0x6b10011f, 0x54000061,   # cmp w8,w16; b.ne orig
        0x52000231,   # eor w17,w17,#1   the mark
        0xb9014c11,   # str w17,[x0,#0x14c]
        0x39456008,   # orig: ldrb w8,[x0,#0x158]
    ]),
    'ui_mark': (0x1f52958, '604e01bd', 'a UI layer camera built at the screen\'s ratio by itself is marked (bit 1 of its ratio flipped) so its angle is left alone', [
        'adrp16', 0xb9405a11, 0x340000d1,   # ldr w17,[x16,#0x58]; cbz w17, orig
        0x1e260010, 0x6b11021f, 0x54000061,   # fmov w16,s0; cmp w16,w17; b.ne orig   ratio = canvas w / h
        0x521f0210, 0x1e270200,   # eor w16,w16,#2; fmov s0,w16
        0xbd014e60,   # orig: str s0,[x19,#0x14c]
    ]),
    'ui_mark_fixed': (0x1f52a98, '614e01bd', 'a UI layer camera of the builder\'s fixed-angle branch at the screen\'s ratio is marked as well', [
        'adrp16', 0xb9405a11, 0x340000d1,   # ldr w17,[x16,#0x58]; cbz w17, orig
        0x1e260030, 0x6b11021f, 0x54000061,   # fmov w16,s1; cmp w16,w17; b.ne orig
        0x521f0210, 0x1e270201,   # eor w16,w16,#2; fmov s1,w16
        0xbd014e61,   # orig: str s1,[x19,#0x14c]
    ]),
    'tan_a': (0x1f12164, '01102e1e', 'camera projection: the half angle tan of a 3D camera at the screen\'s ratio grows by (16/9) / ratio (Vert+)', [
        'adrp16', 0xb9405611, 0x36000171,   # ldr w17,[x16,#0x54]; tbz w17,#0, orig   bit 0: cameras
        0xb9405a11, 0x34000131,   # ldr w17,[x16,#0x58]; cbz w17, orig
        0xbd405e01,   # ldr s1,[x16,#0x5c]   k
        0xb9414e70, 0x6b11021f, 0x54000080,   # ldr w16,[x19,#0x14c]; cmp w16,w17; b.eq scale   at the screen's ratio by itself
        0x52000231, 0x6b11021f, 0x54000041,   # eor w17,w17,#1; cmp w16,w17; b.ne orig   or marked by the entry stub; a UI camera (bit 1) is neither
        0x1e210800,   # scale: fmul s0,s0,s1
        0x1e2e1001,   # orig: fmov s1,#1.0
    ]),
    'tan_b': (0x1f12248, '01102e1e', 'camera projection (its other branch): the half angle tan of a 3D camera grows by (16/9) / ratio', [
        'adrp16', 0xb9405611, 0x36000171, 0xb9405a11, 0x34000131, 0xbd405e01, 0xb9414e70, 0x6b11021f, 0x54000080, 0x52000231, 0x6b11021f, 0x54000041, 0x1e210800, 0x1e2e1001,
    ]),
    'tan_d': (0x1f121c8, '0008281e', 'camera projection by distance: the half angle tan of a 3D camera grows by (16/9) / ratio', [
        'adrp16', 0xb9405611, 0x36000171, 0xb9405a11, 0x34000131, 0xbd405e01, 0xb9414e70, 0x6b11021f, 0x54000080, 0x52000231, 0x6b11021f, 0x54000041, 0x1e210800,
        0x1e280800,   # orig: fmul s0,s0,s8
    ]),
    'hud': (0x1f721d0, '091940f9', 'full-screen pictures stretch to the screen\'s height (bit 1)', [
        'adrp16',
        0xb9405611, 0xb9404a10,   # ldr w17,[x16,#0x54]; ldr w16,[x16,#0x48]
        0x340002b0, 0x36080291,   # cbz w16, orig; tbz w17,#1, orig
        0x39444a6a, 0x7100055f, 0x54000060, 0x71000d5f, 0x540001e1,   # the anchor byte +0x112: 1 or 3
        0x79c1e66a, 0x7110e15f, 0x5400018b,   # ldrsh w10,[x19,#0xf2]; cmp w10,#1080; b.lt orig
        0xf9406a6b, 0xb400014b, 0x3944496a, 0x3500010a,   # a parent, whose anchor byte is 0
        0x1e220206, 0x5280870a, 0x1e220147, 0x1e2718c6,   # s6 = H / 1080
        0x3dc00523, 0x4f869063, 0x3d800523,   # the global matrix's y axis x s6
        0xf9401909,   # orig: ldr x9,[x8,#0x30]
    ]),
}
# s0 = (y + 1) / 2, s2 = 1080.0, then y = 1080 - s0 x 1080. With H: y = (H / 2 + 540) - s0 x H. Replaces two instructions and returns after both (bit 2)
_t_anchor_y = ['adrp8', 0xb9404909, 0x34000189, 0xb940550a, 0x3610014a, 0x1e230123, 0x1e230800, 0x1e2c1004, 0x1e240864, 0x52a880e9, 0x1e270125, 0x1e252884, 0x1e203880,
               0x14000003, 0x1e220800, 0x1e203840]
_t_float_sites = [(0x113ec8c, 9), (0x113ef44, 9), (0x10fe4e8, 9), (0x11082c0, 8), (0x10ee1a8, 10), (0x10ee390, 9)]   # movz w<n>,#0x4487,lsl #16: 1080.0
_t_get_sites = [(0x11299c4, 'render height (1080)'), (0x11299ec, 'render height of a dynamic resolution level (the table)'), (0x11299f4, 'render height of a level out of range (1080)')]


def _t_emit(ctx, words, back):
    for w in words:
        if isinstance(w, str): ctx.emit(ADRP(int(w[4:]), ctx.here(), ctx.state))
        else: ctx.emit(w)
    if back is not None: ctx.emit(B(ctx.here(), back))


def screen_shape_v655360(ctx):
    for name, (site, orig, why, words) in _t_stubs.items():
        ctx.site(site, orig)
        start = ctx.here(); _t_emit(ctx, words, site + 4)
        ctx.put(site, [B(site, start)], f'screen shape, taller: {why} (DM_SCREEN_ASPECT)')
    ctx.site(0x10fc11c, '0008221e')
    start = ctx.here(); _t_emit(ctx, _t_anchor_y, 0x10fc124)
    ctx.put(0x10fc11c, [B(0x10fc11c, start)], 'screen shape, taller: a world point lands where the taller camera draws it, in the canvas\'s coordinates (bit 2, DM_SCREEN_ASPECT)')
    for site, n in _t_float_sites:
        orig = 0x52a890e0 | n
        ctx.site(site, orig.to_bytes(4, 'little').hex())
        start = ctx.here()
        ctx.emit(ADRP(n, ctx.here(), ctx.state)); ctx.emit(0xb9405400 | n << 5 | n); ctx.emit(0x36180080 | n)   # ldr w<n>,[x<n>,#0x54]; tbz w<n>,#3, orig
        ctx.emit(ADRP(n, ctx.here(), ctx.state)); ctx.emit(0xb9406400 | n << 5 | n); ctx.emit(0x35000040 | n)   # ldr w<n>,[x<n>,#0x64]   H as a float; cbnz w<n>, back
        ctx.emit(orig); ctx.emit(B(ctx.here(), site + 4))
        ctx.put(site, [B(site, start)], 'screen shape, taller: the renderer\'s 1080.0 becomes the output height (bit 3, DM_SCREEN_ASPECT, state +0x64)')
    for site, why in _t_get_sites:
        ctx.site(site, 'c0035fd6')
        start = ctx.here(); _t_emit(ctx, _t_hget, None)
        ctx.put(site, [B(site, start)], f'screen shape, taller: {why} takes the screen\'s height (bit 4, DM_SCREEN_ASPECT, state +0x48)')


FRAME_STEP = {
    '76a489f3bd3642fa5e18bd83d32c98ba': screen_shape_v655360,   # v655360 main: taller than 16:9
}

BINDING = [
    {'what': 'screen shape taller than 16:9 (v655360): the render heights, the 16:9 render targets and textures, the renderer\'s 1080.0 constants, the 3D cameras\' vertical angle and the full-screen HUD pictures (DM_SCREEN_ASPECT)', 'now': 'build', 'goal': 'game', 'purpose': 'rendering',
     'why': "the output size is the host's, but where KTGL takes its sizes and its cameras' angle is this build's code",
     'blocked': 'the stubs have no function-matched form yet'},
]
