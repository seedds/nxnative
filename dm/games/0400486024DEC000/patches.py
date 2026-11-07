# The Legend of Zelda: Ocarina of Time, Switch 2 edition (0400486024DEC000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's
# patches.py and applies those of a module's NSO build ID; CUTS, OBS and FRAME_STEP as described there ("Registered patches").
from nso_asm import ADRP, B

# Frame step (update v131072, main e0f3ccfa…): the game runs at 60 fps on the Switch 2, one game tick (1/60 s) per frame, and never changes its present interval: the frame-rate
# tick of its sead framework (vtable +0x128, main+0x4dbd60) returns at once because the framework's frame-rate mode ((fw+0x24f) & 3) is 1, so the interval controller
# embedded at fw+0x270 (main+0x3b4ef60, the same design as Breath of the Wild's) never runs. On a Mac that holds about 39 fps the game therefore plays in slow motion.
# The logic does follow the interval when one is set: with the controller forced to interval 2 / 30 fps, Link's run from the bed to the door took 23 presented frames instead
# of 59 (DM_NVN_REGLUM per-frame luminance, 2026-11-06, M1 Ultra), two ticks a frame, as Breath of the Wild's VFR does. So the host's frame step goes in at the controller:
#   · main+0x4dbd6c / +0x4dbd7c (the tick): with a target fps the mode check is skipped and the interval controller runs (never the VRR frame-rate controller, +0x232 / +0x231);
#   · main+0x3b4ef90 / +0x3b4ef9c (the controller's paused flag +0x60 and "no frame time yet", fw+0x140 < 1): skipped with a target, so the controller reaches its output every frame;
#   · main+0x3b4f054 (the controller's output: w21 = the interval it chose, stored at ctrl+0x48, fps = 60 / w21 at +0x58, then setFrameRate main+0x4dc0f0 → the swapchain's
#     setPresentInterval main+0xf49970 → nvnWindowSetPresentInterval, and setWaitVsyncInterval main+0x346c5c4, fps at fw+0xfc): w21 = the host's step in whole vsyncs
#     (state +0x20) once the host has decided one; until then the controller's own choice (its first frames measure the CPU and GPU time of the previous frame).
# State page target fps 0 (DM_FPS_TARGET=0): every stub runs the original instructions, the game keeps its fixed 60 fps interval 1 as shipped. x16 / x17 are free at all
# five sites (neither function uses them); the controller's sites keep the flags (adrp / ldr / cbz / mov do not set them: +0x3b4ef9c's b.lt reads cmp x8,#1 from before).
def frame_step(ctx):
    here, emit, fs_site, fs_put, FS_STATE_OFF = ctx.here, ctx.emit, ctx.site, ctx.put, ctx.state
    # the frame-rate tick (vtable +0x128): mode check and controller choice
    fs_site(0x4dbd6c, '60010054'); fs_site(0x4dbd7c, 'a1000054')
    start = here(); emit(ADRP(16, here(), FS_STATE_OFF))
    for w in (0xb9400210,   # ldr w16,[x16]          target fps
              0x34000090,   # cbz w16,orig (+4)
              0xaa0003e1,   # mov x1,x0
              0x9109c000):  # add x0,x0,#0x270       the interval controller
        emit(w)
    emit(B(here(), 0x3b4ef60))                                      # run it (the tail call at main+0x4dbd94)
    emit(0x54000160)                                                # orig: b.eq +11 → the ret at main+0x4dbd98 (below)
    emit(B(here(), 0x4dbd70))                                       # mode != 1: the original choice
    for _ in range(9): emit(0xd503201f)
    emit(B(here(), 0x4dbd98))                                       # the ret
    fs_put(0x4dbd6c, [B(0x4dbd6c, start)], 'frame-rate tick: with a target fps the frame-rate mode check is skipped and the interval controller runs')
    # the interval controller: paused flag and "no frame time yet"
    fs_site(0x3b4ef90, '28070037'); fs_site(0x3b4ef9c, 'cb060054')
    start = here(); emit(ADRP(16, here(), FS_STATE_OFF))
    for w in (0xb9400210,   # ldr w16,[x16]
              0x35000050):  # cbnz w16,on (+2)
        emit(w)
    emit(0x37000048)                                                # orig: tbnz w8,#0,paused (+2)
    emit(B(here(), 0x3b4ef94))                                      # on / not paused: continue
    emit(B(here(), 0x3b4f074))                                      # paused: the original target
    fs_put(0x3b4ef90, [B(0x3b4ef90, start)], 'interval controller: its paused flag (+0x60) is ignored with a target fps')
    start = here(); emit(ADRP(16, here(), FS_STATE_OFF))
    for w in (0xb9400210,   # ldr w16,[x16]
              0x35000050):  # cbnz w16,on (+2)
        emit(w)
    emit(0x5400004b)                                                # orig: b.lt skip (+2)   (flags: cmp x8,#1 at main+0x3b4ef98)
    emit(B(here(), 0x3b4efa0))                                      # on / a frame time exists: continue
    emit(B(here(), 0x3b4f074))                                      # skip: the original target
    fs_put(0x3b4ef9c, [B(0x3b4ef9c, start)], 'interval controller: runs before the first frame time too with a target fps')
    # the controller's output: the host's step
    fs_site(0x3b4f054, '88078052')
    start = here(); emit(ADRP(16, here(), FS_STATE_OFF))
    for w in (0xb9400211,   # ldr w17,[x16]          target fps (0 = off)
              0x340000f1,   # cbz w17,orig (+7)
              0xb9402211,   # ldr w17,[x16,#0x20]    step in vsyncs
              0x340000b1,   # cbz w17,orig (+5)      not decided yet: the controller's own interval
              0x2a1103f5,   # mov w21,w17
              0xf9401a11,   # ldr x17,[x16,#0x30]
              0x8b354231,   # add x17,x17,w21,uxtw
              0xf9001a11,   # str x17,[x16,#0x30]    vsyncs stepped
              0x52800788):  # orig: mov w8,#0x3c
        emit(w)
    emit(B(here(), 0x3b4f058))
    fs_put(0x3b4f054, [B(0x3b4f054, start)], "interval controller output: the host's step in whole vsyncs (state page target fps; 0 = the controller's own)")

FRAME_STEP = {'e0f3ccfaf50065ae4ea01eb5aeb13b1a': frame_step}

# How general each patch is and could be (dm/tools/docs_patches.py "Binding", listed in docs/agents/game-patches.md)
BINDING = [
    {'what': 'frame step in whole vsyncs', 'now': 'build', 'goal': 'game', 'purpose': 'frame rate',
     'why': "the framework's interval controller and frame-rate tick",
     'blocked': 'frame-step stubs have no function-matched form yet'},
]
