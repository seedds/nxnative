"""Ocarina v131072's build-checked real-time speed."""
from nso_asm import ADRP, B


# Real-time speed. The frame loop (main+0x6e6be0) computes each frame's step as a constant over its target frame rate and never reads the clock: s1 is 60, or 30 in one mode of the game
# (the fcsel at main+0x6e6cc0), s0 the target, so one frame is one step however long it took and everything runs slowly below the target. Found by gunvalk (zortos293/Lichii PR #216).
# The stub keeps that division and takes the larger of it and s1 x the measured frame time (state +0x38, float seconds, hle_nvn.c): 0 there leaves the game's own step, and the step is
# never below it. It marks state +0x3c so the host measures only for a main that reads the value. x16, x17 and s0 are not read again before they are written after the site
def real_speed(ctx):
    here, emit = ctx.here, ctx.emit
    ctx.site(0x6e6cc4, '2818201e')
    start = here()
    emit(0x1e201828)                       # fdiv s8,s1,s0        the game's own step
    emit(ADRP(16, here(), ctx.state))
    for w in (0x52800031,   # mov w17,#1
              0xb9003e11,   # str w17,[x16,#0x3c]  this main reads the frame time
              0xbd403a00,   # ldr s0,[x16,#0x38]   seconds; 0 = at the target
              0x1e210800,   # fmul s0,s0,s1
              0x1e204908):  # fmax s8,s8,s0
        emit(w)
    emit(B(here(), 0x6e6cc8))
    ctx.put(0x6e6cc4, [B(0x6e6cc4, start)], 'frame step from the measured frame time, never below the game\'s own (state +0x38)')


FRAME_STEP = {'e0f3ccfaf50065ae4ea01eb5aeb13b1a': real_speed}   # 1.1.1 (v131072), the main of v0 too

BINDING = [
    {'what': 'frame step from the measured frame time', 'now': 'build', 'goal': 'game', 'purpose': 'frame rate',
     'why': "this game's own frame loop; the frame-step state page is general",
     'blocked': 'frame-step stubs have no function-matched form yet'},
]
