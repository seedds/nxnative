"""Experimental fractional timing adapted from upstream PR1543.
State +0x40: simulation FPS override (zero retains native step); +0x38: elapsed seconds.
The native rendering controller is unchanged. Short forest gameplay is validated;
movie/audio transitions and whole-game high-rate play remain open (see README.md).
"""
from nso_asm import ADRP, B


def real_speed(ctx):
    ctx.fractional = 1
    here, emit = ctx.here, ctx.emit
    ctx.site(0x6e6cc4, '2818201e')
    start = here()
    emit(0x1e201828)                 # fdiv s8,s1,s0: native fallback
    emit(ADRP(16, here(), ctx.state))
    emit(0x52800051)                 # mov w17,#2: fractional contract
    emit(0xb9003e11)                 # str w17,[x16,#0x3c]
    emit(0xb9404211)                 # ldr w17,[x16,#0x40]: validated positive float bits or zero
    emit(0x34000071)                 # cbz w17,+12: native fallback, preserves NZCV
    emit(0x1e270220)                 # fmov s0,w17
    emit(0x1e201828)                 # fdiv s8,s1,s0: requested simulation step
    emit(0xbd403a00)                 # ldr s0,[x16,#0x38]: elapsed seconds
    emit(0x1e210800)                 # fmul s0,s0,s1
    emit(0x1e204908)                 # fmax s8,s8,s0: correct late frames
    emit(B(here(), 0x6e6cc8))
    ctx.put(0x6e6cc4, [B(0x6e6cc4, start)], 'EXPERIMENT: fractional simulation step without modifying native rendering budget')

FRAME_STEP = {'e0f3ccfaf50065ae4ea01eb5aeb13b1a': real_speed}

BINDING = [
    {'what': 'frame step from the measured frame time', 'now': 'build', 'goal': 'game', 'purpose': 'frame rate',
     'why': "this game's own frame loop; the frame-step state page is general",
     'blocked': 'frame-step stubs have no function-matched form yet'},
]
