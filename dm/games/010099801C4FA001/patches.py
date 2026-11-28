# METAL GEAR SOLID: Peace Walker - Master Collection Version (Program 1, 010099801C4FA001): the rewriter's patches of this game's own builds.
# dm/tools/patch_nso.py loads every profile's patches.py; MATCH_STUBS as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py.
from nso_asm import B, BL, LDP, LDP_POST, LDRX, MOVX

# The renderer manager drives a dedicated RenderThread (core 2, priority 58) with two manual-clear nn::os events:
#   +0x470 (kick): initialized 0; kick_render (main+0x8ba40 on 1.3.0) signals it; RenderThread (main+0x8bca0) waits and clears it.
#   +0x480 (done): initialized 1; RenderThread signals it after EndFrame (main+0x8bd04); wait_render (main+0x8bbe4) waits and clears it.
#
# Two data races in kick_render / wait_render break on Apple Silicon:
# 1. Premature kick before set_resolve: kick_render signals +0x470 at +0x1c..+0x20, then takes ~90 instructions before calling set_resolve
#    (main+0x9dcc1c at +0x18c) to populate the non-double-buffered MSAA resolve target at [renderer + 0x1210..0x1224], which EndFrame (main+0x9dc694)
#    clears to NULL after every frame. When RenderThread on another core reaches main+0x9d9f08 before set_resolve runs, it dereferences NULL
#    in main+0xf28c (SIGSEGV at addr=0x20). Core preemption does not prevent this because producer (core 0) and RenderThread (core 2) run on different cores.
# 2. Stale done event at boot: +0x480 starts signaled (1), and startup calls kick_render twice without an intervening wait_render (main+0x1f4ae8,
#    then main+0x1f4858). The first wait_render (main+0x1f4864) consumes the stale signal immediately, shifting +0x480 one frame ahead of RenderThread
#    so main+0x1f5e28 returns while RenderThread is still inside frame 4; main thread then enters BeginFrame/EndFrame (main+0x1f863c..+0x1f866c)
#    concurrently with RenderThread, hitting NVN command-buffer re-entry ([RED] BeginRecording re-entered) and concurrent WindowAcquireTexture.
#
# Root-cause synchronization fix (three sites, all position-independent):
# Maintain the invariant "+0x480 == 1 iff RenderThread is idle":
# - wait_render (+0x60..+0x68): return after WaitEvent(+0x480) without clearing +0x480, so repeated waits stay idempotent and +0x480 reflects idle state.
# - kick_render (+0x18): wait for +0x480 (RenderThread idle), clear +0x480 (RenderThread busy), store the do_render flag ([mgr + 0x454]),
#   and skip the premature SignalEvent(+0x470) at +0x1c..+0x20.
# - kick_render (+0x190, after set_resolve and the cbz bypass): signal +0x470 through callee-saved x23 (mgr + 0x180, so [x23, #0x2f0] == mgr + 0x470)
#   so RenderThread only wakes after set_resolve has published [renderer + 0x1210..0x1224].


def render_thread_sync(ctx):
    wait_fn = ctx.others.get('wait_render')
    render_fn = ctx.others.get('render_thread')
    sig_ev = ctx.others.get('nn_signal_event')
    wait_ev = ctx.others.get('nn_wait_event')
    clr_ev = ctx.others.get('nn_clear_event')
    if None in (wait_fn, render_fn, sig_ev, wait_ev, clr_ev):
        return
    kick = ctx.base
    ctx.site(kick + 0x18, '01501139')                       # strb w1, [x0, #0x454]
    ctx.site(kick + 0x190, 'f44f43a9')                      # ldp x20, x19, [sp, #0x30]
    ctx.site(wait_fn + 0x60, 'e00313aaf44f41a9fd7bc2a8')    # mov x0, x19; ldp x20, x19, [sp, #0x10]; ldp x29, x30, [sp], #0x20

    begin = ctx.here()
    ctx.emit(MOVX(20, 1))                                   # save do_render flag (x20 is callee-saved, dead until kick + 0x2c)
    ctx.emit(LDRX(0, 19, 0x480 // 8))                       # ldr x0, [x19, #0x480] (done EventType*)
    ctx.emit(BL(ctx.here(), wait_ev))                       # wait until RenderThread is idle (+0x480 == 1)
    ctx.emit(LDRX(0, 19, 0x480 // 8))                       # ldr x0, [x19, #0x480]
    ctx.emit(BL(ctx.here(), clr_ev))                        # mark RenderThread busy (+0x480 = 0)
    ctx.emit(0x39115274)                                    # strb w20, [x19, #0x454] (original instruction while RenderThread is idle)
    ctx.emit(B(ctx.here(), kick + 0x24))                    # skip original add x0, x0, #0x470; bl SignalEvent

    end = ctx.here()
    ctx.emit(LDRX(0, 23, 0x2f0 // 8))                       # ldr x0, [x23, #0x2f0] (x23 == mgr + 0x180 -> kick EventType* at +0x470)
    ctx.emit(BL(ctx.here(), sig_ev))                        # wake RenderThread after set_resolve has published resolve target
    ctx.emit(LDP(20, 19, 31, 0x30))                         # ldp x20, x19, [sp, #0x30] (original instruction)
    ctx.emit(B(ctx.here(), kick + 0x194))

    ctx.put(kick + 0x18, [B(kick + 0x18, begin)], 'kick_render: wait+clear done event (+0x480) before writing frame state, defer kick (+0x470)')
    ctx.put(kick + 0x190, [B(kick + 0x190, end)], 'kick_render: signal RenderThread (+0x470) after set_resolve')
    ctx.put(wait_fn + 0x60, [LDP(20, 19, 31, 0x10), LDP_POST(29, 30, 31, 0x20), 0xD65F03C0],
            'wait_render: keep done event (+0x480) signaled while RenderThread is idle')


MATCH_STUBS = {'main': [
    {
        'name': 'render-thread synchronization',
        'size': 0x1a4,
        'fingerprints': ['2784f9a010822523817f5275051321a25e2c39726a8eb29234607c718a4ad0bf'],
        'stub': render_thread_sync,
        'others': {
            'wait_render': {'size': 0x70, 'fingerprints': ['056a63c16f229c5deb43f2d94e33129a54436e7c7caed748206575279030bc08']},
            'render_thread': {'size': 0xa4, 'fingerprints': ['25ba2e18e9ae036f75d547cd9c5abdbff1b808f9fd1f1b18ab733112274587a2']},
            'nn_signal_event': {'size': 0x4, 'fingerprints': ['bfe9eaa0c2306b5d6587fd01d26d9973b0ea716c9d567f46c5a561bebe4b4556']},
            'nn_wait_event': {'size': 0x4, 'fingerprints': ['31dfee1fbfc85e7c39007e4fe42e9f5421efbf4f37a1bf5afcac7902077e04fd']},
            'nn_clear_event': {'size': 0x4, 'fingerprints': ['955a92d0347d4ef7fc50ddcf472976c4985cc68888fee32c659c0a11f15f8cde']},
        },
    },
]}

# How general each patch is and could be (dm/tools/docs_patches.py "Binding", listed in docs/agents/game-patches.md): what, its binding now, the loosest it could have (goal),
# purpose, why; blocked or kept: why the gap stays open (cannot close yet, or left open on purpose); checked: what was checked on game files (else a judgement)
BINDING = [
    {'what': 'render-thread synchronization (defer kick past set_resolve, track idle state in done event)', 'now': 'game', 'goal': 'game', 'purpose': 'race',
     'checked': '1.3.0 (v131072) main bca1a793…: complete kick_render/wait_render/RenderThread disassembly, DM_PROBE happens-before trace, and 10/10 CPU-loaded Mac runs across DM_PREEMPT=0 and DM_PREEMPT=1 (2026-11-23)',
     'why': "two data races in the game's own render-thread handshake (premature SignalEvent before set_resolve and unwaited startup kick leaving the done event stale), not a host or SDK bug; function-matched (MATCH_STUBS) across updates"},
]
