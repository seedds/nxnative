"""Simulation step for the v131072 Ocarina of Time main build."""
from nso_asm import ADRP, B


def frame_step(ctx):
    # The game update at 0x6e6be0 reads the framework's integer FPS at +0x2c8,
    # converts it at 0x6e6ca8, then passes 60/FPS to its time manager
    # (0x3b4e300). The latter writes ratio, reciprocal and seconds at +0x10.
    # Keep the original conversion and the game's 30/60 numerator selection.
    # Use the host's whole-vsync step, as for BOTW/TOTK, when enabled.
    site = 0x6e6c98
    ctx.site(site, 'a0ca42bd')
    start = ctx.here()
    words = (
        0xa9be47f0,  # stp x16,x17,[sp,#-32]!
        0xf9000bef,  # str x15,[sp,#16]
        0,           # adrp x16,state
        0xb9400211,  # ldr w17,[x16]: target (zero disables)
        0x34000271,  # cbz w17,original
        0xb9402211,  # ldr w17,[x16,#0x20]: host step
        0x35000111,  # cbnz w17,ready
        0xb9400211,  # first sample: load the target's float bits
        0x52a83e0f,  # mov w15,#0x41f00000 (30.0f)
        0x4a0f0231,  # eor w17,w17,w15: compare without changing NZCV
        0x34000071,  # cbz w17,initial30
        0x52800031,  # mov w17,#1
        0x14000002,  # b ready
        0x52800051,  # initial30: mov w17,#2
        0x5280078f,  # mov w15,#60
        0x1ad109ef,  # udiv w15,w15,w17
        0x1e2701e0,  # fmov s0,w15: integer bits for original scvtf
        0xf9401a0f,  # ldr x15,[x16,#0x30]
        0x8b3141ef,  # add x15,x15,w17,uxtw
        0xf9001a0f,  # str x15,[x16,#0x30]: stepped vsyncs
        0xf9400bef,  # ldr x15,[sp,#16]
        0xa8c247f0,  # ldp x16,x17,[sp],#32
        0,           # return
        0xbd42caa0,  # original: ldr s0,[x21,#0x2c8]
        0xf9400bef,
        0xa8c247f0,
        0,           # return
    )
    for i, word in enumerate(words):
        if i == 2:
            word = ADRP(16, ctx.here(), ctx.state)
        elif i in (22, 26):
            word = B(ctx.here(), site + 4)
        ctx.emit(word)
    ctx.put(site, [B(site, start)], 'simulation FPS follows the host step; target zero retains the original framework FPS')


FRAME_STEP = {'e0f3ccfaf50065ae4ea01eb5aeb13b1a': frame_step}

BINDING = [
    {'what': 'simulation step in whole vsyncs', 'now': 'build', 'goal': 'game', 'purpose': 'frame rate',
     'why': "the game's framework FPS load feeds its simulation time manager",
     'blocked': 'frame-step stubs have no function-matched form yet',
     'checked': 'v131072 main and ARM64 execution of the emitted stub'},
]
