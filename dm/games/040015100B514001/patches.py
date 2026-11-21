# Super Mario Bros. Wonder, Nintendo Switch 2 Edition (040015100B514001): the rewriter's patches of this game's own code. dm/tools/patch_nso.py loads every profile's
# patches.py; MATCH_FRAME_STEP as described in docs/reference/rewriter.md ("Frame-step stubs placed by their function"). Patches of Nintendo SDK module builds are in
# dm/tools/sdk_patches.py
from nso_asm import ADRP, B

# The framework's interval controller. The game's sead GameFrameworkNx calc (main+0x1bb9b0) runs the controller at framework+0x258 once a frame (main+0x1bbee0; x20 the
# controller, x19 the framework). From the last frame's time ([framework+0x130] ticks, over the tick rate at main+0x3b73b80, times 60) plus a bias ([controller+8], float) it
# counts the vsyncs the frame needed, trunc(x) + 1, clamped to [[controller+0], [controller+4]] (the game's own intervals), steps that up after overrunning frames and down
# again slowly, and ends at main+0x1bbfa4 with the interval in w10: it stores it at [controller+0x40], the frame rate 60 / interval at [controller+0x50] and [controller+0x54],
# tells the framework the rate (main+0x1bc190) and sets the present interval from [controller+0x40] (setWaitVsyncInterval, main+0x1bc1d0; the NVN setter's return address
# main+0x1bc1f0 is what the host logs as "the game sets present interval"). Right after the controller the calc stores 60 / [controller+0x50] as a float at main+0x3aac460:
# the game's time step in vsyncs, the interval itself (main+0x1bb9d0..0x1bb9f0).
# On the Mac the game stayed at interval 2, 30 fps, whatever the GPU did: 7.5 ms of GPU time a frame in handheld mode (2026-11-17), none dropped, the interval set 1 -> 2 once at
# the start and never back. Presenting every frame (DM_PRESENT_INTERVAL=display) played the animations at double speed, so the time step follows the interval, as on the
# Switch. The stub hands the controller's result the interval the host's step decides, within the game's own range, and counts the vsyncs stepped, so the interval, the frame
# rate, the present interval and the time step all follow it. Until the host has decided a step, and at fps_target 0, the game's own choice runs. The site is the load of
# [controller+0x28] after `mov w8,#60`, which every path that reaches the store passes (nothing branches to it or after it); w8 (60) is kept for the sdiv, w9 is loaded again
# by the original instruction, x16 and x17 are not used in the function. The stub reads the state page: +0 the target fps (0 = off), +0x20 the host's step, +0x24 (the step
# the game fixes: cleared, this game has none here), +0x30 the vsyncs stepped. Not covered: the controller's early exits ([framework+0x22c] mode 1; [framework+0x212] or
# +0x213 set, which tail-call main+0x22718b0) and its fixed mode ([controller+0x58] bit 0, which sets no interval), so a scene in those modes runs the game's own interval.
def frame_step(ctx):
    here, emit, fs_site, fs_put, FS_STATE_OFF, base = ctx.here, ctx.emit, ctx.site, ctx.put, ctx.state, ctx.base
    site = base + 0xc8   # main+0x1bbfa8
    ctx.step_max = 2   # the game's own intervals are 1 and 2: the host holds frames at most 2 vsyncs
    fs_site(site, '892a40b9')   # ldr w9,[x20,#0x28]: the controller's lowest frame rate, then the interval is stored and used
    start = here(); emit(ADRP(16, here(), FS_STATE_OFF))
    for w in (0xb9400211,   # ldr w17,[x16]           target fps (0 = off)
              0x340001b1,   # cbz w17,orig (+13)
              0xb9402211,   # ldr w17,[x16,#0x20]     the host's step in vsyncs
              0x34000171,   # cbz w17,orig (+11)      not decided yet
              0xb9400689,   # ldr w9,[x20,#4]         the game's largest interval
              0x6b09023f,   # cmp w17,w9
              0x1a89922a,   # csel w10,w17,w9,ls      at most that
              0xb9400289,   # ldr w9,[x20]            the game's smallest interval
              0x6b09015f,   # cmp w10,w9
              0x1a8a312a,   # csel w10,w9,w10,lo      at least that
              0xb900261f,   # str wzr,[x16,#0x24]     no step the game fixes
              0xf9401a11,   # ldr x17,[x16,#0x30]
              0x8b2a4231,   # add x17,x17,w10,uxtw
              0xf9001a11,   # str x17,[x16,#0x30]     vsyncs stepped
              0xb9402a89):  # orig: ldr w9,[x20,#0x28]
        emit(w)
    emit(B(here(), site + 4))
    fs_put(site, [B(site, start)], 'frame interval: the host\'s step within the game\'s own range, in place of the controller\'s choice (state page target fps; 0 = the game\'s choice)')

MATCH_FRAME_STEP = {'main': [
    {'name': 'frame interval from the host\'s step', 'size': 0x2ac, 'fingerprints': ['34d436f2a51948a7f85e5462ab45e250cae1587bfd7b42ed62d2d834ebf4403f'], 'stub': frame_step},   # main+0x1bbee0
]}

BINDING = [
    {'what': 'frame interval from the host\'s step', 'now': 'game', 'goal': 'game', 'purpose': 'frame rate',
     'why': "the game's interval controller, found by its function's fingerprint on any build that keeps the function",
     'checked': 'v0 main: one function with this size and fingerprint, the site, its free registers and the lack of branches into it read by disassembly (2026-11-18)'},
]
