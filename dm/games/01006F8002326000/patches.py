# Animal Crossing: New Horizons (01006F8002326000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO build
# ID (FRAME_STEP as described there, "Registered patches"). The game has no frame step stubs: FRAME_STEP only gives its main the state page that the screen shape stubs read
# (DM_SCREEN_ASPECT, as for Breath of the Wild); with DM_SCREEN_ASPECT unset the page stays zero and every stub runs the original instructions.
import hashlib
import struct
from pathlib import Path
from nso_asm import ADRP, B, LDP_POST, STP_PRE, STRX

# Screen shape (DM_SCREEN_ASPECT, loader.c): a screen taller than 16:9 keeps the picture's width (1920) and takes the height of the screen's ratio (state +0x48, 1344 at 1.43), the
# layout space 1280 x 2/3 of it (+0x4c, 896; +0x50 the same as a float). The game's platform config (vtable main+0x47fa0e0, no RTTI) hands its sizes out three ways:
#   main+0x5c9b44 (vtable main+0x47f9c08) fills the graphics create arg: +0x60 the display size (the table main+0x4076728 of 960x540 .. 3840x2160 by the mode at [main+0x512e2e0]
#     when [main+0x5131900] is set, else 1920x1080), +0x20 1280x720
#   main+0x5cdce0 (slot 7) a static display size main+0x4b87524 (the same choice), main+0x5cdd88 (slot 8) a static 1280.0 x 720.0 (main+0x4b87538)
#   main+0x5cdcb8 (slot 3) another arg's +0x14: 1920x1080
# Each stub is one bit of the conversion site mask (+0x54, DM_SCREEN_ASPECT_SITES, all on by default); a height of 0 (16:9) leaves the original value. x16 / x17 are free at every site
# ==== BEGIN GENERATED VIEW CODE (gen_view_stubs.py; do not edit) ====
# Generated from view.s by dm/tools/gen_view_stubs.py; do not edit.
source_sha256 = '83e09ee895319ca0a39eadbbbc2fb4c202800ce62d38955b0e82f6a9fe76afdd'
words = [
    0x2d558801, 0x9000000f, 0x910001ef, 0xb94059ef, 0x3400016f, 0x1c002386,
    0x1e262040, 0x54000101, 0x1e2701e7, 0x1e2718c6, 0x1e2e1003, 0x1e2348c6,
    0x1e260821, 0x1e2040e2, 0x1e2c1003, 0x14000000, 0x39402008, 0x350004a8,
    0x9000000f, 0x910001ef, 0xb94059ef, 0x3400042f, 0xadbe07e0, 0xad010fe2,
    0xd53b420f, 0xf81f0fef, 0xf940000f, 0x90000010, 0x91000210, 0xeb1001ff,
    0x54000281, 0xbd40b000, 0x1c002021, 0x1e212000, 0x54000201, 0x9000000f,
    0x910001ef, 0xb94059ef, 0x3400004f, 0x1e2701e0, 0xbd400c01, 0xbd402002,
    0x1e211841, 0x1e20c021, 0x1e203821, 0x1e20c021, 0x1c001ec2, 0x1e220802,
    0x1e222020, 0x1a9f97e8, 0xf84107ef, 0xd51b420f, 0xad410fe2, 0xacc207e0,
    0xd65f03c0, 0x91002100, 0xa9bf7be8, 0x94000003, 0xa8c17be8, 0xd65f03c0,
    0x2a1f03e8, 0x9000000f, 0x910001ef, 0xb94059f0, 0x34000690, 0xb9403c10,
    0x35000650, 0xb9403810, 0x52b7f00e, 0x6b0e021f, 0x540005c1, 0xb9402810,
    0x180005ce, 0x6b0e021f, 0x54000541, 0xb9402c10, 0x1800056e, 0x6b0e021f,
    0x540004c1, 0xb9400810, 0xb940180e, 0x2a0e0210, 0x35000450, 0xbd400000,
    0xbd401401, 0x1c000402, 0x1c001963, 0x1c0019a4, 0x1e223802, 0x1e20c042,
    0x1e242040, 0x54000189, 0x1e202008, 0x540002ed, 0x1c0002e2, 0x1e222000,
    0x54000288, 0x1e230842, 0x1e223822, 0x1e20c042, 0x1e242040, 0x540001e8,
    0x1e231820, 0xb94059f0, 0x34000050, 0x1e270203, 0x1c000160, 0x1c0016c2,
    0x1e231842, 0x1e2e1004, 0x1e245842, 0x1e220800, 0x1e230801, 0xbd000000,
    0xbd001401, 0x52800028, 0xd65f03c0, 0x3fc5d182, 0xbf80068e, 0xc0000347,
    0x1e221803, 0x1e241822, 0xa9bf43ef, 0x9000000f, 0x910001ef, 0xb94059ef,
    0x340002cf, 0xb9404e70, 0x35000290, 0xadbe9be5, 0x3d800be7, 0xbd402665,
    0xbd401266, 0x1e2618a5, 0x1e20c0a5, 0xf940626f, 0x2d451de6, 0x1e2718c6,
    0x1e2618a5, 0x1e2e1006, 0x1e2558c6, 0x1e260842, 0x1e2e1006, 0x1e2518c5,
    0x1e2658a5, 0x1e250863, 0x3dc00be7, 0xacc19be5, 0xa8c143ef, 0x14000000,
    0x1e221922, 0xa9bf43ef, 0x9000000f, 0x910001ef, 0xb94059ef, 0x340002cf,
    0xb9404eb0, 0x35000290, 0xadbe97e4, 0x3d800be6, 0xbd4026a4, 0xbd4012a5,
    0x1e251884, 0x1e20c084, 0xf94062af, 0x2d4519e5, 0x1e2618a5, 0x1e251884,
    0x1e2e1005, 0x1e2458a5, 0x1e250863, 0x1e2e1005, 0x1e2418a4, 0x1e255884,
    0x1e240842, 0x3dc00be6, 0xacc197e4, 0xa8c143ef, 0x14000000, 0xf81f0fef,
    0x9000000f, 0x910001ef, 0xb94059ef, 0x3400028f, 0xadbf0fe2, 0x1e2701e2,
    0x1c000ce1, 0x1e221820, 0x1e211841, 0x1e2e1002, 0x1c000ca3, 0x1e230800,
    0x1e224800, 0x1e230821, 0x1e224821, 0x1e200908, 0x1e20098c, 0x1e210929,
    0x1e2109ad, 0xbd404be2, 0x1e210842, 0xbd004be2, 0xacc10fe2, 0xf84107ef,
    0x1e293940, 0x14000000, 0xf81e0fef, 0x3d8007e7, 0x9000000f, 0x910001ef,
    0xb94059ef, 0x3400022f, 0xadbf0be1, 0x1e2701e2, 0x1c000967, 0x1e2218e0,
    0x1e271841, 0x1c000947, 0x1e270800, 0x1e270821, 0x1e2e1007, 0x1e274800,
    0x1e274821, 0x1e20094a, 0x1e20096b, 0x1e210908, 0x1e210929, 0xacc10be1,
    0x3dc007e7, 0xf84207ef, 0xbd400280, 0xd65f03c0, 0xb9400008, 0xf81f0fef,
    0x9000000f, 0x910001ef, 0xb94059ef, 0x3400004f, 0x531f7908, 0xf84107ef,
    0x14000000, 0xbd40ac01, 0x9000000f, 0x910001ef, 0xb94059ef, 0x3400014f,
    0x1c000566, 0xbd40b007, 0x1e2720c0, 0x540000c1, 0x1e2701e7, 0x1e2718c6,
    0x1e2e1010, 0x1e3048c6, 0x1e260821, 0x14000000, 0x2d558c02, 0x9000000f,
    0x910001ef, 0xb94059ef, 0x3400014f, 0x1c000386, 0x1e2320c0, 0x540000e1,
    0x1e2701e7, 0x1e2718c6, 0x1e2e1010, 0x1e3048c6, 0x1e260842, 0x1e2040e3,
    0x14000000, 0xf81f0fef, 0x9000000f, 0x910001ef, 0xb94059ef, 0x340000ef,
    0x1e200802, 0x1f010822, 0x1f0d09a2, 0x1e21c042, 0x1e290840, 0x14000006,
    0x1e212000, 0x1e21cc00, 0x1e2021a0, 0x1e20cda0, 0x1e290800, 0xf84107ef,
    0x14000000, 0x3fe38e39, 0x44a00000, 0x3f8ccccd, 0x3727c5ac, 0xa9bb2be9,
    0xa90133eb, 0xa9023bed, 0xa90343ef, 0xf90023f1, 0xd53b4211, 0x9000000f,
    0x910001ef, 0xb94059f0, 0x340003d0, 0xb94002a9, 0x1800058a, 0x6b0a013f,
    0x54000341, 0xb9400ea9, 0x5286d78a, 0x6b0a013f, 0x540000e0, 0x52856c8a,
    0x6b0a013f, 0x54000261, 0x5800038d, 0xd2819b8e, 0x14000003, 0x580002ed,
    0xd2816b8e, 0xaa1503ea, 0x5800030b, 0x5800032c, 0x38401548, 0xca08016b,
    0x9b0c7d6b, 0x51000529, 0x35ffff89, 0xeb0d017f, 0x54000081, 0xb82e6abf,
    0x52800030, 0xb900a1f0, 0xd51b4211, 0xf94023f1, 0xa94343ef, 0xa9423bed,
    0xa94133eb, 0xa8c52be9, 0x79400ea8, 0x14000000, 0x7f41e3bf, 0x1e453d08,
    0xfd75c925, 0xb7299d38, 0x84222325, 0xcbf29ce4, 0x000001b3, 0x00000100,
    0x54594c46, 0xa9be43ef, 0xa9017bf1, 0xd2800411, 0x94000015, 0xa9417bf1,
    0xa8c243ef, 0xd102c3ff, 0x14000000, 0xa9bd3fee, 0xa90147f0, 0xa9027bed,
    0xd53b420d, 0xf940000e, 0x90000010, 0x91000210, 0xeb1001df, 0x54000061,
    0xd2800511, 0x94000006, 0xd51b420d, 0xa9427bed, 0xa94147f0, 0xa8c33fee,
    0xd65f03c0, 0x9000000f, 0x910001ef, 0xf94055f0, 0x91000610, 0xf90055f0,
    0x8b1101ef, 0xc89ffdf0, 0xd65f03c0, 0xf94055f0, 0x2a1f03e8, 0xf94059f1,
    0xb40000b1, 0xcb110211, 0xf1000a3f, 0x54000048, 0x52800028, 0xd65f03c0,
    0xa9bf7be0, 0x91004000, 0x94000003, 0xa8c17be0, 0xd65f03c0, 0xa9bf7bef,
    0x97fffeb0, 0x34000248, 0x1cfff261, 0xb94059f0, 0x34000050, 0x1e270201,
    0x1cfff1e2, 0x1e222020, 0x5400016a, 0x97ffffe8, 0x34000128, 0x1e221821,
    0x1e2e1002, 0x1e213841, 0xbd401803, 0xbd408604, 0x1e241821, 0x1e212861,
    0xbd001801, 0xa8c17bef, 0xd65f03c0, 0x2d411905, 0xa9be3fee, 0xa90147f0,
    0xadbe9fe0, 0x3d800bf0, 0xd53b4211, 0x9000000f, 0x910001ef, 0xb94059f0,
    0x340002d0, 0xb9404e70, 0x35000290, 0xbd401267, 0xbd402670, 0x1e271a10,
    0x1cffee27, 0x1e271a10, 0x1e2e1007, 0x1e3038f0, 0xbd402a67, 0x1e2020e8,
    0x5400014d, 0x1e3038e0, 0x1e20c000, 0x1cffed70, 0x1e302000, 0x540000a8,
    0x1e2c1010, 0x1e2608e7, 0x1e3008e7, 0x1e273842, 0xd51b4211, 0x3dc00bf0,
    0xacc19fe0, 0xa94147f0, 0xa8c23fee, 0x14000000, 0xa9bf7be1, 0x94000000,
    0xf94003e2, 0xaa1303e0, 0xaa1403e1, 0x94000004, 0xf94007fe, 0x910043ff,
    0xd65f03c0, 0xf81f0ffe, 0x2a1f03e3, 0x94000205, 0xf84107fe, 0x35001708,
    0x9000000f, 0x910001ef, 0xb94059f0, 0x34001690, 0xb94059f0, 0x34001650,
    0xf9405810, 0x9257fa10, 0x5800160e, 0xeb0e021f, 0x54001780, 0x580015ee,
    0xeb0e021f, 0x54001720, 0x39416c10, 0x12000e0e, 0x710021df, 0x540014c8,
    0x12000610, 0x35001490, 0xb940b010, 0x18001d2e, 0x6b0e021f, 0x54000400,
    0x18001cee, 0x6b0e021f, 0x540003a0, 0x18001cae, 0x6b0e021f, 0x54000340,
    0x18001c6e, 0x6b0e021f, 0x540002e0, 0x18001c2e, 0x6b0e021f, 0x54000280,
    0x18001bee, 0x6b0e021f, 0x54000220, 0xf9405810, 0x58001b8e, 0xeb0e021f,
    0x54000160, 0x58001b6e, 0xeb0e021f, 0x54000080, 0x58001b4e, 0xeb0e021f,
    0x540010e1, 0x79417010, 0x7100c21f, 0x54001081, 0x14000003, 0x3942e010,
    0x35001030, 0x2a1f03eb, 0x2d4a0400, 0x1e202008, 0x54000fad, 0x1e202028,
    0x54000f6d, 0xf9406030, 0xb4000f30, 0x2d450e02, 0x1e222000, 0x54000ec1,
    0x1e232020, 0x54000e81, 0xbd421044, 0xbd421445, 0x1e202080, 0x54000e01,
    0x1e2120a0, 0x54000dc1, 0xb9404c30, 0x35000d90, 0xbd402422, 0xbd401023,
    0x1e202068, 0x54000d0d, 0x1e231842, 0xbd4059e4, 0x1e243845, 0x1e20c0a5,
    0x1cffdf86, 0x1e2408c6, 0x1e2620a0, 0x54000c08, 0xf9406031, 0x2d451223,
    0x1e241863, 0x1e231842, 0x1e202048, 0x54000b4d, 0x1e2e1003, 0x1e232040,
    0x5400028c, 0x54000aca, 0x1e221863, 0x1e230824, 0xbd021444, 0x1e2c1005,
    0x1e213884, 0x1e250884, 0xbd402826, 0x1e2108c6, 0x1e2308c6, 0x1f0510c4,
    0xbd421c45, 0x1e252884, 0xbd021c44, 0x3500090b, 0xd2801c90, 0xd2800209,
    0xd280040a, 0x14000022, 0x350001cb, 0x1e204043, 0x1e230804, 0xbd021044,
    0x1e203884, 0x1e2c1005, 0x1e250884, 0xbd421845, 0x1e2438a5, 0xbd021845,
    0xd2801c10, 0xd2800409, 0xd280020a, 0x14000014, 0xbd405000, 0xbd421044,
    0x1e242000, 0x54000641, 0x1e2e1003, 0x1e233842, 0xbd402a23, 0x1e230842,
    0x1e2c1003, 0x1e230842, 0x1e222884, 0xbd021044, 0x3942c810, 0x7101321f,
    0x540004e1, 0xbd421844, 0x1e223884, 0xbd021844, 0x14000023, 0x5280006e,
    0xd280310d, 0x3943c40c, 0x38ed6831, 0x6b0c023f, 0x54000322, 0xbc706840,
    0x8b0a0211, 0xbc716841, 0x1e203821, 0x1e2c1002, 0x1e204064, 0x1e2e1005,
    0x1e253884, 0x1e220884, 0x1e210884, 0x1e243800, 0xbc306840, 0x8b090211,
    0xbc716840, 0x1e243800, 0xbc316840, 0x8b0a0211, 0xbc716840, 0x1e242800,
    0xbc316840, 0x9100c211, 0xbc716840, 0x1e242800, 0xbc316840, 0x910005ad,
    0x91010210, 0x710005ce, 0x54fffc41, 0xd65f03c0, 0x5f4c5f50, 0x00003030,
    0x5f525f50, 0x00003030, 0x69435f50, 0x656c6372, 0x6e495f4e, 0x5f74754f,
    0x746f6f52, 0x656e6150, 0x44f00000, 0x44870000, 0x41f00000, 0x41f00000,
    0xf9400c11, 0xb4fffe11, 0xf9405a30, 0x58fffe6e, 0xeb0e021f, 0x54fffd81,
    0xb940ba30, 0x3942d40d, 0x53103dad, 0x52860bee, 0x2a0d01ce, 0x6b0e021f,
    0x54fffca1, 0xf9400e31, 0xb4fffc71, 0xf9405a30, 0x58fffd0e, 0xeb0e021f,
    0x54fffbe1, 0xb940ba30, 0x3942d40d, 0x53185dad, 0x5280060e, 0x2a0d01ce,
    0x6b0e021f, 0x54fffb01, 0xf9402a30, 0x58fffc6e, 0xeb0e021f, 0x54fffa81,
    0xf9400e31, 0xb4fffa51, 0xf9405a30, 0x58fffb2e, 0xeb0e021f, 0x54fff9c1,
    0x3942e230, 0x35fff990, 0xf9402a30, 0x58fffaae, 0xeb0e021f, 0x54fff901,
    0xf9406031, 0xb4fff8d1, 0xf9401630, 0xeb0e021f, 0x54fff861, 0xbd405401,
    0xbd402e22, 0x1e222020, 0x54fff7e1, 0xbd421442, 0x1e222020, 0x54fff781,
    0x5280002b, 0x17ffff4d, 0x47425f50, 0x67425f50, 0x6f445f50, 0x6f435f50,
    0x6c415f50, 0x74535f50, 0x65545f50, 0x30305f78, 0x61425f50, 0x305f6573,
    0x61465f50, 0x305f6564, 0x2a1f03e8, 0xf940580e, 0x58000b4f, 0xeb0f01df,
    0x54000421, 0xb940b80e, 0x5286060f, 0x6b0f01df, 0x540003a1, 0xf940280e,
    0x58000acf, 0xeb0f01df, 0x54000321, 0xf9400c11, 0xb40002f1, 0xf9402a2e,
    0x58000a4f, 0xeb0f01df, 0x54000261, 0xf9401411, 0x9100800e, 0xeb0e023f,
    0x540001e0, 0xb40001d1, 0xd1002231, 0xf9405a2e, 0x5800088f, 0xeb0f01df,
    0x54000121, 0xb940ba2e, 0x5286060f, 0x6b0f01df, 0x540000a1, 0xf9402a2e,
    0x5800084f, 0xeb0f01df, 0x1a9f17e8, 0xd65f03c0, 0xa9bd7be8, 0xa9013fee,
    0xa90247f0, 0xd53b4210, 0x9000000f, 0x910001ef, 0xb94059f1, 0x34000091,
    0x97ffffd2, 0x34000048, 0x32000063, 0xd51b4210, 0xa94247f0, 0xa9413fee,
    0xa8c37be8, 0xfc1b0fe8, 0x14000000, 0xa9bc7be8, 0xa9013fee, 0xa90247f0,
    0xf9001bed, 0xadbf07e0, 0xd53b420d, 0x9000000f, 0x910001ef, 0xb94059f1,
    0x34000291, 0x97ffffbf, 0x34000248, 0x9000000f, 0x910001ef, 0xb94059f0,
    0x340001d0, 0xb94059f0, 0x34000190, 0x1cffbf40, 0x1e270201, 0x1e202020,
    0x5400010a, 0x97fffe52, 0x350000c8, 0x1e211800, 0x1e2e1001, 0x1e213800,
    0x1c000281, 0x1f018c03, 0xd51b420d, 0xacc107e0, 0xf9401bed, 0xa94247f0,
    0xa9413fee, 0xa8c47be8, 0x53067d4a, 0x14000000, 0x6e495f4e, 0x5f74754f,
    0x6c415f4e, 0x5f6e6769, 0x41f00000, 0x41f00000, 0x44f00000, 0x44870000,
    0x44e24000, 0x42c80000, 0x44070000, 0xa9bf7be0, 0x94000004, 0xa8c17be0,
    0x39442808, 0x14000000, 0xa9bc37e8, 0xa9013fee, 0xa90247f0, 0xa9037be0,
    0xd53b420d, 0xf940000e, 0x9000000f, 0x910001ef, 0xeb0f01df, 0x54000ba1,
    0xf940580e, 0x58000c2f, 0xeb0f01df, 0x54000b21, 0xb940b80e, 0x5286260f,
    0x6b0f01df, 0x54000aa1, 0xf940280e, 0x58000cef, 0xeb0f01df, 0x54000a21,
    0xf9400c11, 0xb40009f1, 0xf9405a2e, 0x58000aaf, 0xeb0f01df, 0x54000961,
    0xb940ba2e, 0x5286260f, 0x6b0f01df, 0x540008e1, 0xf9402a2e, 0x58000b6f,
    0xeb0f01df, 0x54000861, 0xf9400e31, 0xb4000831, 0xf9405a2e, 0x5800092f,
    0xeb0f01df, 0x540007a1, 0xb940ba2e, 0x5286260f, 0x6b0f01df, 0x54000721,
    0xf9402a2e, 0x580009af, 0xeb0f01df, 0x540006a1, 0xf9400e31, 0xb4000671,
    0xf9405a2e, 0x580007af, 0xeb0f01df, 0x54000120, 0x5800078f, 0xeb0f01df,
    0x54000581, 0x3942e22e, 0x3500054e, 0xf9400e31, 0xb4000511, 0x14000009,
    0x3942e22e, 0x350004ae, 0xf9400e31, 0xb4000471, 0xf9405a2e, 0x5800066f,
    0xeb0f01df, 0x54000260, 0xf9405a2e, 0x580005af, 0xeb0f01df, 0x54000361,
    0xb940ba2e, 0x5280060f, 0x6b0f01df, 0x540002e1, 0xf9402a2e, 0x580005ef,
    0xeb0f01df, 0x54000261, 0xf9400e31, 0xb4000231, 0xf9405a2e, 0x5800042f,
    0xeb0f01df, 0x540001a1, 0xf9402a2e, 0x5800046f, 0xeb0f01df, 0x54000121,
    0x9000000f, 0x910001ef, 0xf94055f0, 0xb40000b0, 0xc8dffe10, 0x91000610,
    0x9100e1ef, 0xc89ffdf0, 0xd51b420d, 0xa9437be0, 0xa94247f0, 0xa9413fee,
    0xa8c437e8, 0xd65f03c0, 0x61425f57, 0x5f4c6573, 0x63535f4e, 0x5f656c61,
    0x61565f4e, 0x5f64696c, 0x74425f4c, 0x30305f6e, 0x74425f4c, 0x31305f6e,
    0x6f4c5f4e, 0x305f706f, 0x6f525f4e, 0x0000746f, 0x43d20000, 0x42f00000,
    0x41f00000, 0x42200000, 0x41200000, 0x42d80000, 0x41f00000, 0x41f00000,
    0xa9bf7be1, 0x94000000, 0xf94003e2, 0xd1010042, 0xaa1303e0, 0xaa1403e1,
    0x52800023, 0x94000003, 0xa8c17be1, 0xd65f03c0, 0x9000000f, 0x910001ef,
    0xb94059ee, 0x3400324e, 0xb94059ee, 0x3400320e, 0x1e2701c0, 0x1cffa881,
    0x1e212000, 0x54003180, 0xf9406030, 0xb4003150, 0xf9401a11, 0xb4003111,
    0xb940022e, 0x1800312d, 0x6b0d01df, 0x54000560, 0x180030ed, 0x6b0d01df,
    0x54000081, 0x3940122e, 0x340004ce, 0x1400017e, 0xf940022e, 0x5800302d,
    0xeb0d01df, 0x54000121, 0xb9400a2e, 0x18002fed, 0x6b0d01df, 0x54002ec1,
    0x79401a2e, 0x710195df, 0x54000340, 0x14000172, 0x58002f2d, 0xeb0d01df,
    0x54000121, 0xb9400a2e, 0x18002eed, 0x6b0d01df, 0x54002d61, 0x79401a2e,
    0x7101d1df, 0x540001e0, 0x14000167, 0x58002e2d, 0xeb0d01df, 0x54002c81,
    0xf940062e, 0x58002ded, 0xeb0d01df, 0x54002c01, 0xb940122e, 0x18002dad,
    0x6b0d01df, 0x54002b81, 0x3940522e, 0x35002b4e, 0xf940160e, 0x58002d0d,
    0xeb0d01df, 0x54002a81, 0xf940580a, 0x10002ce9, 0x5280024b, 0xa8c1352c,
    0x8a0d014e, 0xeb0c01df, 0x54000080, 0x5100056b, 0x35ffff6b, 0x1400014a,
    0xb9404c2e, 0x3500290e, 0xb940482e, 0x52b7f00d, 0x6b0d01df, 0x54002881,
    0xf940202e, 0x920079ce, 0xb500282e, 0xbd401020, 0xbd402421, 0x1e202008,
    0x540027ad, 0x1e202028, 0x5400276d, 0x1e201822, 0xbd4059e3, 0x1e233842,
    0x1e20c042, 0x1cff9e3e, 0x1e3e0863, 0x1e232040, 0x54002668, 0xd280000b,
    0xf86b684c, 0x9100402a, 0xf86b694d, 0xeb0d019f, 0x540025a1, 0x9100216b,
    0xf101017f, 0x54ffff21, 0xd280000b, 0x91010049, 0x9102002a, 0xf86b692c,
    0xf86b694d, 0xeb0d019f, 0x54002461, 0x9100216b, 0xf100c17f, 0x54ffff41,
    0xbd404442, 0x1e202048, 0x540023a1, 0xbd405042, 0x1e202048, 0x54002341,
    0xbd406042, 0x1e202048, 0x540022e1, 0xbd406442, 0x1e202048, 0x54002281,
    0xbd404044, 0xbd405445, 0xbd404c46, 0xbd405c47, 0xbd406c5d, 0x1e202088,
    0x540021a0, 0x1e2020a8, 0x54002160, 0x1e2023a8, 0x5400212a, 0x1e2143bd,
    0x1e201bba, 0x1e211bbb, 0x2d456618, 0x1e2c101f, 0x1e3f0b18, 0x1e3f0b39,
    0xbd40183c, 0x1e3a0b9c, 0xbd40283d, 0x1e3b0bbd, 0x1e3c3b54, 0x1e383a94,
    0x1e3c2b55, 0x1e383ab5, 0x1e3d3b76, 0x1e393ad6, 0x1e3d2b77, 0x1e393af7,
    0x1e2703ff, 0x1e3f4a94, 0x1e3f4ab5, 0x1e3f4ad6, 0x1e3f4af7, 0x2d4a0400,
    0xbd421842, 0xbd421c43, 0x35000103, 0xbd42105c, 0xbd42145d, 0x1e202380,
    0x54001d21, 0x1e2123a0, 0x54001ce1, 0x14000024, 0xbd42105c, 0xbd42145d,
    0x1e2c101f, 0x1e3f081e, 0x1e3e2380, 0x54001c01, 0x1e3f083e, 0x1e3e23a0,
    0x54001ba1, 0x1e202048, 0x54001b61, 0x1e202068, 0x54001b21, 0x39416c0e,
    0x120005cd, 0x71000dbf, 0x54001aa0, 0x1e2703e2, 0x3500008d, 0x1e3f0802,
    0x1e214042, 0x14000004, 0x710009bf, 0x54000041, 0x1e214002, 0x53020dcd,
    0x71000dbf, 0x54001940, 0x1e2703e3, 0x3500006d, 0x1e3f0823, 0x14000004,
    0x710009bf, 0x54000041, 0x1e204023, 0x1f021890, 0x1e202840, 0x1f001891,
    0x1e315a1c, 0x1e314a11, 0x1e204390, 0x1f031cb2, 0x1e213861, 0x1f011cb3,
    0x1e335a5c, 0x1e334a53, 0x1e204392, 0x1c00195f, 0x1e20c21c, 0x1e3f2380,
    0x54001662, 0x1e20c23c, 0x1e3f2380, 0x54001602, 0x1e20c25c, 0x1e3f2380,
    0x540015a2, 0x1e20c27c, 0x1e3f2380, 0x54001542, 0x1e352a9c, 0x1e372add,
    0x1e3d2b9c, 0x1e3f2380, 0x540014a2, 0x1cff8bfe, 0x1e3e0b1c, 0x1e3e0b3d,
    0x1e21431e, 0x1e3c2bde, 0x1e3e2200, 0x5400006c, 0x1e343a14, 0x14000002,
    0x1e204214, 0x1e3c3b1e, 0x1e3e2220, 0x5400006b, 0x1e352a35, 0x14000002,
    0x1e204235, 0x1e21433e, 0x1e3d2bde, 0x1e3e2240, 0x5400006c, 0x1e363a56,
    0x14000002, 0x1e204256, 0x1e3d3b3e, 0x1e3e2260, 0x5400006b, 0x1e372a77,
    0x14000002, 0x1e204277, 0x1e302280, 0x540000e1, 0x1e3122a0, 0x540000a1,
    0x1e3222c0, 0x54000061, 0x1e3322e0, 0x54001000, 0x35000543, 0xbd405000,
    0x1e240800, 0x1e260011, 0x37f80091, 0x1e204280, 0x1e343aa1, 0x14000003,
    0x1e2042a0, 0x1e353a81, 0x1e263800, 0x1e241800, 0x1e241821, 0xbd405402,
    0x1e250842, 0x1e260051, 0x37f80091, 0x1e2042e2, 0x1e363ae3, 0x14000003,
    0x1e2042c2, 0x1e373ac3, 0x1e273842, 0x1e251842, 0x1e251863, 0x1e20c01c,
    0x1e3f2380, 0x54000c82, 0x1e20c03c, 0x1e3f2380, 0x54000c22, 0x1e20c05c,
    0x1e3f2380, 0x54000bc2, 0x1e20c07c, 0x1e3f2380, 0x54000b62, 0xbd021041,
    0xbd021443, 0xbd021840, 0xbd021c42, 0x14000056, 0x1e303a20, 0x1e323a61,
    0x1e202008, 0x540003cd, 0x1e202028, 0x5400038d, 0x1e343aa2, 0x1e363ae3,
    0x1e201842, 0x1e211863, 0x1e3038c0, 0x1e3238e1, 0x1f025000, 0x1f035821,
    0x1e220884, 0x1e2308a5, 0x1e20c09c, 0x1e3f2380, 0x54000862, 0x1e20c0bc,
    0x1e3f2380, 0x54000802, 0x1e20c01c, 0x1e3f2380, 0x540007a2, 0x1e20c03c,
    0x1e3f2380, 0x54000742, 0xbd004044, 0xbd005445, 0xbd004c40, 0xbd005c41,
    0x14000035, 0x1e343aa0, 0x1e363ae1, 0x1e20c082, 0x1e20c0a3, 0x1e221800,
    0x1e231821, 0x1e2c101c, 0x1e3c0800, 0x1e3c0821, 0xb9405011, 0x36f80051,
    0x1e214000, 0xb9405411, 0x36f80051, 0x1e214021, 0x1e352a82, 0x1e372ac3,
    0x1e3c0842, 0x1e3c0863, 0x39416c0e, 0x120005cd, 0x340000ed, 0x1e20401d,
    0x710009bf, 0x54000041, 0x1e2143bd, 0x1e3d089d, 0x1e3d3842, 0x53020dcd,
    0x340000ed, 0x1e21403d, 0x710009bf, 0x54000041, 0x1e2143bd, 0x1e3d08bd,
    0x1e3d3863, 0x1e20c01c, 0x1e3f2380, 0x540001c2, 0x1e20c03c, 0x1e3f2380,
    0x54000162, 0x1e20c05c, 0x1e3f2380, 0x54000102, 0x1e20c07c, 0x1e3f2380,
    0x540000a2, 0xbd021040, 0xbd021441, 0xbd004c42, 0xbd005c43, 0x52800028,
    0xd65f03c0, 0x2a1f03e8, 0xd65f03c0, 0xd503201f, 0x65706957, 0x65646146,
    0x65646956, 0x6d61676f, 0x64614665, 0x7069574c, 0x6d695465, 0x69644565,
    0x756f6c43, 0x6c734964, 0x57646e61, 0x44657069, 0x6d616572, 0x44f00000,
    0x44870000, 0x7f800000, 0x61425f50, 0x005f6573, 0xffffffff, 0x00ffffff,
    0x61425f50, 0x005f6b63, 0xffffffff, 0x00ffffff, 0x61465f50, 0x005f6564,
    0xffffffff, 0x00ffffff, 0x67425f50, 0x0000005f, 0xffffffff, 0x000000ff,
    0x47425f50, 0x0000005f, 0xffffffff, 0x000000ff, 0x69575f50, 0x00006570,
    0xffffffff, 0x0000ffff, 0x754f5f50, 0x00000074, 0xffffffff, 0x000000ff,
    0x70555f50, 0x0000005f, 0xffffffff, 0x000000ff, 0x6f445f50, 0x005f6e77,
    0xffffffff, 0x00ffffff, 0x61505f50, 0x72657474, 0xffffffff, 0xffffffff,
    0x5f4c5f50, 0x00000000, 0xffffffff, 0x00000000, 0x5f525f50, 0x00000000,
    0xffffffff, 0x00000000, 0x5f545f50, 0x00000000, 0xffffffff, 0x00000000,
    0x5f425f50, 0x00000000, 0xffffffff, 0x00000000, 0x4e4c5f50, 0x616d726f,
    0xffffffff, 0xffffffff, 0x4e525f50, 0x616d726f, 0xffffffff, 0xffffffff,
    0x4e545f50, 0x616d726f, 0xffffffff, 0xffffffff, 0x4e425f50, 0x616d726f,
    0xffffffff, 0xffffffff,
]
symbols = {'_dm_view_area': 824, '_dm_view_backdrop': 1852, '_dm_view_backdrop_call': 1816, '_dm_view_cache': 64, '_dm_view_capture': 600, '_dm_view_create': 716, '_dm_view_footer_match': 2936, '_dm_view_footer_recalc': 3088, '_dm_view_footer_y': 3156, '_dm_view_keyboard_frame': 1396, '_dm_view_keyboard_window': 3368, '_dm_view_keyboard_window_call': 3348, '_dm_view_layout': 1172, '_dm_view_matrix': 0, '_dm_view_near_xy': 1024, '_dm_view_near_y': 964, '_dm_view_panel_projection': 1580, '_dm_view_scissor': 480, '_dm_view_scissor_origin': 1668, '_dm_view_sphere': 1084, '_dm_view_touch_page': 1428, '_dm_view_transition': 3928, '_dm_view_transition_fast_call': 3888, '_dm_view_ui_override': 220, '_dm_view_ui_projection': 1560, '_dm_view_units': 928}
relocations = [(4, 3, '_dm_view_state'), (8, 4, '_dm_view_state'), (60, 2, '_dm_return_037e5978'), (72, 3, '_dm_view_state'), (76, 4, '_dm_view_state'), (108, 3, '_dm_code_04a65608'), (112, 4, '_dm_code_04a65608'), (140, 3, '_dm_view_state'), (144, 4, '_dm_view_state'), (244, 3, '_dm_view_state'), (248, 4, '_dm_view_state'), (492, 3, '_dm_view_state'), (496, 4, '_dm_view_state'), (596, 2, '_dm_return_03a308f0'), (608, 3, '_dm_view_state'), (612, 4, '_dm_view_state'), (712, 2, '_dm_return_03a2eac0'), (720, 3, '_dm_view_state'), (724, 4, '_dm_view_state'), (820, 2, '_dm_return_013a29a8'), (832, 3, '_dm_view_state'), (836, 4, '_dm_view_state'), (936, 3, '_dm_view_state'), (940, 4, '_dm_view_state'), (960, 2, '_dm_return_01462bb8'), (968, 3, '_dm_view_state'), (972, 4, '_dm_view_state'), (1020, 2, '_dm_return_037e5a50'), (1028, 3, '_dm_view_state'), (1032, 4, '_dm_view_state'), (1080, 2, '_dm_return_037e5a80'), (1088, 3, '_dm_view_state'), (1092, 4, '_dm_view_state'), (1152, 2, '_dm_return_013a37c8'), (1196, 3, '_dm_view_state'), (1200, 4, '_dm_view_state'), (1356, 2, '_dm_return_00268ee4'), (1424, 2, '_dm_return_03268180'), (1448, 3, '_dm_code_049adc80'), (1452, 4, '_dm_code_049adc80'), (1492, 3, '_dm_view_state'), (1496, 4, '_dm_view_state'), (1692, 3, '_dm_view_state'), (1696, 4, '_dm_view_state'), (1812, 2, '_dm_return_03a30988'), (1820, 2, '_dm_code_00264a10'), (1872, 3, '_dm_view_state'), (1876, 4, '_dm_view_state'), (3104, 3, '_dm_view_state'), (3108, 4, '_dm_view_state'), (3152, 2, '_dm_return_0027d4c4'), (3180, 3, '_dm_view_state'), (3184, 4, '_dm_view_state'), (3204, 3, '_dm_view_state'), (3208, 4, '_dm_view_state'), (3300, 2, '_dm_return_0027d7c0'), (3364, 2, '_dm_return_0029a18c'), (3392, 3, '_dm_code_0474de58'), (3396, 4, '_dm_code_0474de58'), (3744, 3, '_dm_view_state'), (3748, 4, '_dm_view_state'), (3892, 2, '_dm_code_00265220'), (3928, 3, '_dm_view_state'), (3932, 4, '_dm_view_state')]
# ==== END GENERATED VIEW CODE ====

def relocated_words(words, relocations, symbols, address, state):
    """Resolve the adapter's ARM64 relocations; _dm_view_state becomes the frame step page."""
    result = list(words)
    for offset, kind, name in relocations:
        assert offset % 4 == 0 and 0 <= offset < 4 * len(result), 'adapter relocation outside code'
        pc = address + offset; word = result[offset // 4]
        if name == '_dm_view_state': target = state
        elif name.startswith('_dm_return_'): target = int(name[len('_dm_return_'):], 16)
        elif name.startswith('_dm_code_'): target = int(name[len('_dm_code_'):], 16)
        elif name in symbols: target = address + symbols[name]
        else: raise ValueError('Unknown adapter relocation: ' + name)
        if kind == 2:
            assert word & 0x7c000000 == 0x14000000
            result[offset//4] = (word & 0xfc000000) | (B(pc, target) & 0x3ffffff)
        elif kind == 3:
            assert word & 0x9f000000 == 0x90000000
            result[offset//4] = ADRP(word & 31, pc, target)
        elif kind == 4:
            assert word & 0xffc00000 == 0x91000000
            result[offset//4] = (word & ~(0xfff << 10)) | ((target & 0xfff) << 10)
        else: raise ValueError('Unknown adapter relocation kind')
    return result


def _bl(frm, to): return 0x94000000 | (((to - frm) >> 2) & 0x3FFFFFF)


# Adapter sites, keyed by the 3.0.3 main build ID. 0x265140 is absent: the
# chained bit 6 stub owns that site (one rewrite site per address).
_view_sites = [
    (0x4a65660, "68597e0300000000"),
    (0x474de58 + 21*8, struct.pack("<Q", 0x29a17c).hex()),
    (0x269134, "000d40bd602a00bd001140bd08710091682603a9602e00bd"),
]
_view_hooks = [(0x283e9c,"e184ff97","_dm_view_transition_fast_call",True),
        (0x29a188,"08284439","_dm_view_keyboard_window_call",False),
        (0x27d4c0,"e80f1bfc","_dm_view_footer_recalc",False),
        (0x27d7bc,"4a7d0653","_dm_view_footer_y",False),
        (0x326817c,"ffc302d1","_dm_view_keyboard_frame",False),
        (0x2af1258,"c0035fd6","_dm_view_touch_page",False),
        (0x3a30984,"0519412d","_dm_view_scissor_origin",False),
        (0x284048,"7282ff97","_dm_view_backdrop_call",True),
        (0x2840e4,"4b82ff97","_dm_view_backdrop_call",True),
        (0x268ee0,"a80e4079","_dm_view_layout",False),
        (0x5ce924,"00210091","_dm_view_ui_override",False),
        (0x37e5974,"0188552d","_dm_view_matrix",False),
        (0x3a308e8,"0318221e2218241e","_dm_view_scissor",False),
        (0x3a2eabc,"2219221e","_dm_view_capture",False),
        (0x13a29a4,"4039291e","_dm_view_create",False),
        (0x1126144,"800240bd","_dm_view_area",True),
        (0x1125fe4,"800240bd","_dm_view_area",True),
        (0x1462bb4,"080040b9","_dm_view_units",False),
        (0x37e5a4c,"01ac40bd","_dm_view_near_y",False),
        (0x37e5a7c,"028c552d","_dm_view_near_xy",False),
        (0x13a37b4,"0020211e00cc211ea021201ea0cd201e0008291e","_dm_view_sphere",False),
]
_view_caches = [(0x15083c,20,8,"88224039"),
        (0x2b8888,0,8,"08204039"),
        (0x5ce93c,19,8,"68224039"),
        (0x31c3074,20,8,"88224039"),
        (0x3268228,21,8,"a8224039"),
        (0x37b0eec,19,8,"68224039"),
        (0x37c1b0c,25,8,"28234039"),
        (0x37e5654,0,8,"08204039"),
        (0x38189d8,3,8,"68204039"),
        (0x3818a34,22,8,"c8224039"),
        (0x3818c70,24,8,"08234039"),
        (0x3819a08,24,8,"08234039"),
        (0x384225c,23,8,"e8224039"),
        (0x38423d8,23,8,"e8224039"),
        (0x386a888,21,8,"a8224039"),
        (0x3883a94,20,8,"88224039"),
        (0x389ebc0,21,8,"a8224039"),
        (0x3908dc8,19,8,"68224039"),
        (0x39af314,20,9,"89224039"),
        (0x39af334,20,9,"89224039"),
        (0x3a77ac4,27,8,"68234039"),
        (0x3a789d4,24,8,"08234039"),
        (0x3ab6100,21,8,"a8224039"),
        (0x3ab7190,24,8,"08234039"),
        (0x3abb650,21,8,"a8224039"),
        (0x3abc700,24,8,"08234039"),
        (0x3b2235c,26,8,"48234039"),
        (0x3b55694,20,8,"88224039"),
        (0x3db2bf0,27,8,"68234039"),
]


_size_sites = [
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
# -fno-jump-tables -fno-asynchronous-unwind-tables; the words of its __text, duo_calc at _duo_calc). nn::ui2d::Pane::Calculate (main+0x27d4c0, vtable main+0x474d1f8 slot 19)
# calls CalculateGlobalMatrix (main+0x27d74c) when the pane or a parent changed and returns to main+0x27d570, before the children's Calculate (vtable +0x98): the stub there
# calls duo_calc(pane, state + 0x80, nn::ui2d::Pane::DrawSelf) while the output's height is set (bit 9 of the sites). x0..x18 and x30 are dead there (the function reloads
# x30 from its frame), d8 is callee-saved
_duo_anchor = 'bd4000211e20202854000041d65f03c0f9400c08b4ffffc852800609aa0803eaf9400d4bb400008baa0b03ea7100052954ffff81bd40554052a88f491e2701221e2220001e2e10001e2f10021e20ac40bd4070022d4a14031e2208641e2288621e2020881e244c44bd4084031e2308a21e2388a51e2020481e224ca5f94014099100800abd4010221e2208021e222080540003cbbd4014261e2608061e2620a05400034bbd4018261e2608061e2620a0540002c8bd407c061e20c0c652a8440b1e2701671e2708071e2720c0540001e5bd408c061e20c0d01e27220054000165eb0a013f54fff941f9400008f9405508eb02011f54fff8c0bd400420bd4080011e2108001400001ceb0a013f540004c1bd4014261e2608061e2620a05400044b52a886891e2701261e260806bd4004271e3010101f0740c61e2620a054000345bd408c061e20c0d052a844091e2701311e3108111e31220054000265f9400009f9405529eb02013f540001e0bd4080001e2008e0bd008000bd4004201e200860bd008400bd400420bd4088011e210800bd008800bd4004201e2008c0bd008c00d65f03c01e222080540000abbd4014231e2308031e2320a054fff2aaf9400009f940552952a8500a1e2701431e230803eb02013f540000811e2320801e2354a854fff1641e2008215280060914000004aa0a03e87100052954000440f9400d0ab400034abd4071042d4a19051e2408a71e2488a41e2020e81e274c84bd4085051e2508c71e2588c51e2020e81e274ca51e222080540000abbd4014261e2608061e2620a054fffd6af9400108f9405508eb02011f54ffedc11e2320801e2354a854fffc8417ffff6a2d460d021e2308631f020c421e2a10031e23204054ffec8cbd407c021e20c04252a879081e2701031e2308031e23204052a870e81e27010252a86cc81e2701031e22ac621e220802bd408c001e20c0031e22206054ffea8b1e2140221e2020081e22cc211e212800bd008c00d65f03c0'
_duo_calc = 0x0
_pane_draw_self = 0x27e2c8   # nn::ui2d::Pane::DrawSelf (vtable main+0x474d1f8 slot 21, empty): the game's null panes, alignments, captures and parts keep it
_tanf_plt, _atanf_plt = 0x3f03330, 0x3f03360   # main's PLT stubs of the SDK's tanf, atanf


def _bl(frm, to): return 0x94000000 | (((to - frm) >> 2) & 0x3FFFFFF)


def screen_shape(ctx):
    here, emit, site, put, STATE = ctx.here, ctx.emit, ctx.site, ctx.put, ctx.state

    # view.s must match the generated words. The adapter shares the frame step
    # state page: no VIEW table, no VIEW_STATE_OFF, no host-managed state.
    assert hashlib.sha256(Path(__file__).with_name('view.s').read_bytes()).hexdigest() == source_sha256, \
        'regenerate view stubs after editing view.s'
    view_base = here()
    for w in relocated_words(words, relocations, symbols, view_base, STATE):
        emit(w)
    for at, ws, why in _size_sites:
        site(at, struct.pack('<I', ws[-1]).hex())
        start = here(); emit(ADRP(16, here(), STATE))
        for w in ws: emit(w)
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
    for at, orig, ws in ((0x31cee60, 0xf2e890f8, (0x1e230230, 0x1e260211, 0xb3607e38)),   # movk x24, #0x4487, lsl #48; ucvtf s16, w17; fmov w17, s16; bfi x24, x17, #32, #32
                            (0x31cee9c, 0xf2c08708, (0xb3607e28,))):                        # movk x8, #0x438, lsl #32; bfi x8, x17, #32, #32
        site(at, struct.pack('<I', orig).hex())
        start = here(); emit(orig)
        emit(ADRP(16, here(), STATE))
        n = 3 + len(ws)   # from the tbz to the branch back (the cbz: n - 2)
        emit(0xb9405611)                                          # ldr w17, [x16, #0x54]   sites
        emit(0x36400011 | (n << 5))                               # tbz w17, #8, back
        emit(0xb9404a11)                                          # ldr w17, [x16, #0x48]   the output's height (0 = 16:9)
        emit(0x34000011 | ((n - 2) << 5))                         # cbz w17, back
        for w in ws: emit(w)
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
        emit(_bl(here(), _tanf_plt))
        emit(ADRP(16, here(), STATE))
        emit(0xbd405e01)   # ldr s1, [x16, #0x5c]   k
        emit(0x1e210800)   # fmul s0, s0, s1
        emit(_bl(here(), _atanf_plt))
        emit(0x1e202800)   # fadd s0, s0, s0        the widened angle
        emit(0xf84107e0)   # ldr x0, [sp], #16
        emit(B(here(), at + 4))
        put(at, [B(at, start)], 'field camera: its vertical angle widened so the taller screen keeps 16:9\'s sides (bit 5)')
    code = here()
    for i in range(0, len(_duo_anchor), 8): emit(int(_duo_anchor[i:i + 8], 16))
    site(0x27d570, '68624139')
    start = here(); emit(ADRP(16, here(), STATE))
    emit(0xb9405611)   # ldr w17, [x16, #0x54]   sites
    emit(0x36480111)   # tbz w17, #9, orig (+8)
    emit(0xb9404a11)   # ldr w17, [x16, #0x48]   the output's height (0 = 16:9)
    emit(0x340000d1)   # cbz w17, orig (+6)
    emit(0xaa1303e0)   # mov x0, x19             the pane
    emit(0x91020201)   # add x1, x16, #0x80      the parameters
    emit(ADRP(2, here(), _pane_draw_self)); emit(0x91000042 | ((_pane_draw_self & 0xFFF) << 10))   # add x2, x2, #lo12: nn::ui2d::Pane::DrawSelf
    emit(_bl(here(), code + _duo_calc))
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
    emit(_bl(here(), _tanf_plt))
    emit(ADRP(16, here(), STATE))
    emit(0xbd405e01)   # ldr s1, [x16, #0x5c]   k
    emit(0x1e210800)   # fmul s0, s0, s1
    emit(_bl(here(), _atanf_plt))
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
    emit(0x34000591)   # cbz w17, ret (+44): the y row is untouched at 16:9
    emit(0xb9405611)   # ldr w17, [x16, #0x54]
    emit(0x36300551)   # tbz w17, #6, ret (+42): bit 6 off
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
    emit(0x5400036c)   # b.gt ret (+27): not a 16:9 projection, as it is
    emit(0xbd408600)   # ldr s0, [x16, #0x84]
    for i in (1, 4, 5, 6, 7, 9, 13):
        o = (0x10 + 4 * i) // 4
        emit(0xbd400001 | (o << 10))   # ldr s1, [x0, #0x10 + 4 i]
        emit(0x1e201821)               # fdiv s1, s1, s0
        emit(0xbd000001 | (o << 10))   # str s1, [x0, #0x10 + 4 i]
    # The adapter's keyboard dock follows the upstream y-row division. Its
    # Lscreen_projection rewrites m0/m5, so the 16:9 test above MUST run first.
    emit(0xf81f0fe0)                      # str x0,[sp,#-16]!  save DrawInfo
    emit(ADRP(15, here(), STATE))         # x15 = state for the adapter
    bl_at = here()
    emit(_bl(bl_at, view_base + symbols['_dm_view_ui_projection']))
    emit(0xf84107e0)                      # ldr x0,[sp],#16
    emit(0xd65f03c0)                      # ret
    put(0x265140, [B(0x265140, start)], 'nn::ui2d::DrawInfo::SetProjectionMtx: y row divided by the stretch, then the keyboard UI dock (bit 6, one chained stub)')


    # Adapter site assertions, hooks and caches. 0x27d4c0 (Pane::Calculate entry)
    # and the upstream 0x27d570 (after CalculateGlobalMatrix) coexist.
    for off, orig in _view_sites:
        site(off, orig)
    for off, orig, sym, link in _view_hooks:
        site(off, orig)
        put(off, [B(off, view_base + symbols[sym]) | (0x80000000 if link else 0)],
            'adaptive-view ' + sym)
    for off, src, dst, orig in _view_caches:
        site(off, orig)
        body = here()
        for w in (STP_PRE(0,30,31,-16), STP_PRE(8,9,31,-16), STP_PRE(15,16,31,-16),
                  0xaa0003e0 | (src << 16)): emit(w)
        emit(B(here(), view_base + symbols['_dm_view_cache']) | 0x80000000)
        emit(STRX(8, 31, 2 if dst == 8 else 3))
        emit(LDP_POST(15,16,31,16)); emit(LDP_POST(8,9,31,16))
        emit(LDP_POST(0,30,31,16))
        emit(B(here(), off + 4))
        put(off, [B(off, body)], 'refresh cached projection after a view target change')


FRAME_STEP = {
    'ff1d1c05670db6021c85b624a710b963': screen_shape,   # 3.0.3 (v2228224) main
}

# How general each patch is and could be (dm/tools/docs_patches.py "Binding", listed in docs/agents/game-patches.md): what, its binding now, the loosest it could have (goal),
# purpose, why; blocked or kept: why the gap stays open
BINDING = [
    {'what': "screen shape: the sizes, the view, the layouts' projection and their edge widgets (DM_SCREEN_ASPECT)", 'now': 'build', 'goal': 'game', 'purpose': 'rendering',
     'why': "the output size is the host's, but where the game takes its sizes, its cameras and its layouts are this build's code",
     'blocked': 'the stubs have no function-matched form yet'},
    {'what': "adaptive view: cached projections, scissors, captures, field ranges and UI docking (DM_SCREEN_ASPECT)", 'now': 'build', 'goal': 'game', 'purpose': 'rendering',
     'why': "the state page is the host's, but every adapter site is this build's code",
     'blocked': 'the stubs have no function-matched form yet'},
]
