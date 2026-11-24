"""Opt-in NSO patch for Witcher 3 region 0100BFE00E9CA000, 3.7 (v327680).

Use as patches.py in that release's provisional profile. Its fill site and
NVN command-buffer lookup are bound to the main NSO build ID below.
"""
from nso_asm import ADDI, ADRP, BL, BLR, BR, LDP, LDP_POST, LDRX, STP, STP_PRE

BUILD = '4ffb62f1cd9e17f87e4e019731bfb0c4'
FILL_CALL = 0x1C50EF4


def histogram_clear_before_fill(ctx):
    # The original fill (vtable +0x68) is at FILL_CALL. The same
    # function waits after filling at 0x1C50FCC using NVN barrier 0x21.
    ctx.site(0x1C50ED4, '288f01d0081940f900555af9080040f9083540f9e1031b2ae2030032e303003200013fd6')
    ctx.site(0x1C50F08, '489c01d0086543f9')
    ctx.site(0x1C50F98, '09e386522900a0720a5e8452096969b8290500512821aa9b096c9e52c90ea072086969f800010191489c01d0088542f92104805200013fd6')
    device_state = ctx.address(0x1C50F08)
    barrier = ctx.address(0x1C50FC0)
    start = ctx.here()
    emit = ctx.emit

    emit(STP_PRE(0, 1, 31, -0x30))
    emit(STP(2, 3, 31, 0x10))
    emit(STP(8, 30, 31, 0x20))

    # The same current-command-buffer lookup the game uses after the fill.
    emit(ADRP(8, ctx.here(), device_state))
    emit(LDRX(8, 8, (device_state & 0xFFF) // 8))
    for word in (
        0x5286E309,  # mov w9, #0x3718
        0x72A00029,  # movk w9, #1, lsl #16
        0x52845E0A,  # mov w10, #0x22f0
        0xB8696909,  # ldr w9, [x8, x9]
        0x51000529,  # sub w9, w9, #1
        0x9BAA2128,  # umaddl x8, w9, w10, x8
        0x529E6C09,  # mov w9, #0xf360
        0x72A00EC9,  # movk w9, #0x76, lsl #16
        0xF8696908,  # ldr x8, [x8, x9]
    ):
        emit(word)
    emit(ADDI(0, 8, 0x40))
    emit(ADRP(8, ctx.here(), barrier))
    emit(LDRX(8, 8, (barrier & 0xFFF) // 8))
    emit(0x52800421)  # mov w1, #0x21
    emit(BLR(8))

    emit(LDP(8, 30, 31, 0x20))
    emit(LDP(2, 3, 31, 0x10))
    emit(LDP_POST(0, 1, 31, 0x30))
    emit(BR(8))  # Original fill call with its arguments and return address.
    ctx.put(FILL_CALL, [BL(FILL_CALL, start)], 'wait for the luminance histogram clear before dispatching its fill')


STUBS = {BUILD: histogram_clear_before_fill}
