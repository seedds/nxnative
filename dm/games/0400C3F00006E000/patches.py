# Mario Kart World (0400C3F00006E000): the rewriter's patches of this game's own code. dm/tools/patch_nso.py loads every profile's patches.py;
# MATCH_FRAME_STEP as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
from nso_asm import ADRP, B

# The framework's frame interval. Once a frame the function at main+0x3a9e28 (v0; main+0x69cbb0 on 1.8.0; x19 the framework) picks the interval in vsyncs at [x19+0x214]: its frame
# mode at [x19+0x210] (+0xa0; +0xa4 on 1.8.0) is 1 = interval 1, 2 = interval 2 (the game's 30 fps mode), 0 = chosen from the frame's CPU time alone ([x19+0x128], the time to the end of the calc call; the whole
# frame's is [x19+0xe8]), so a frame that waits on the GPU keeps interval 1. The interval is added to the vsync counter the game steps by (the global at main+0x33e6ae8) and set
# as the present interval (1.8.0: main+0x37c8438). On a Mac whose frames take longer than a vsync the game played in slow motion (seen by the user, 2026-11-08). The stub hands the interval the host's step
# instead, at most 2 (the game's own intervals), and counts the vsyncs stepped; the game's 30 fps mode stays its own and is published as a step the game fixes (state +0x24), so the
# host holds those frames 2 vsyncs. Until the host has decided a step, and at fps_target 0, the game's own choice runs. x8, x16 and x17 are free at the site (x8 is loaded there;
# the function uses neither x16 nor x17). Lichii (a GPL emulator; only the facts were taken) found the mode load (its commit 8f70204825: ldr w8,[x19,#0x210], 2 = 30 fps,
# CPU time at +0x128, whole frame at +0xe8). 1.8.0's function is one word shorter (0x28c) with its own fingerprint, and the site is 4 bytes on (the mode load at +0xa4, the
# interval store at +0x140, 0x9c after it as on v0; found as the only such pair in its main, no v0 main being at hand to map from); branches reach the site itself (+0x264,
# +0x278) and the store (+0x214, +0x230), none the instruction after the site, and it uses neither x16 nor x17. The rule could not move off the fingerprints: of its calls,
# nn::vi::QueryDisplayHdrStatus and QueryDisplayHdrPaperWhite have two callers each (so no rule anchored on an import) and GetSystemTick / LockMutex hundreds
def frame_step(ctx, at=0xa0):
    here, emit, fs_site, fs_put, FS_STATE_OFF, base = ctx.here, ctx.emit, ctx.site, ctx.put, ctx.state, ctx.base
    site, interval = base + at, base + at + 0x9c
    ctx.step_max = 2   # the stub caps the step at the game's own intervals: the host holds frames at most 2 vsyncs, or hold 3 played at 2/3 speed and the debt it paid back was lost
    fs_site(site, '681242b9')       # ldr w8,[x19,#0x210]
    fs_site(interval, '681602b9')   # str w8,[x19,#0x214]: the interval, then the frame goes on
    start = here(); emit(ADRP(16, here(), FS_STATE_OFF))
    for w in (0xb9400211,   # ldr w17,[x16]          target fps (0 = off)
              0x34000211,   # cbz w17,orig (+16)
              0xb9421268,   # ldr w8,[x19,#0x210]    the game's frame mode
              0x7100091f,   # cmp w8,#2
              0x54000100,   # b.eq fixed (+8)        its 30 fps mode
              0xb900261f,   # str wzr,[x16,#0x24]    no step the game fixes
              0xb9402211,   # ldr w17,[x16,#0x20]    step in vsyncs
              0x34000151,   # cbz w17,orig (+10)     not decided yet
              0x52800048,   # mov w8,#2
              0x71000a3f,   # cmp w17,#2
              0x1a889228,   # csel w8,w17,w8,ls      at most 2
              0x14000002,   # b count (+2)
              0xb9002608,   # fixed: str w8,[x16,#0x24]   the host holds 2 vsyncs
              0xf9401a11,   # count: ldr x17,[x16,#0x30]
              0x8b284231,   # add x17,x17,w8,uxtw
              0xf9001a11):  # str x17,[x16,#0x30]    vsyncs stepped
        emit(w)
    emit(B(here(), interval))
    emit(0xb9421268); emit(B(here(), site + 4))   # orig: ldr w8,[x19,#0x210]; back
    fs_put(site, [B(site, start)], 'frame interval: the host\'s step, at most 2, unless the game\'s 30 fps mode is set (state page target fps; 0 = the game\'s choice by CPU time)')

MATCH_FRAME_STEP = {'main': [
    {'name': 'frame interval from the host\'s step', 'size': 0x290, 'fingerprints': ['fe68735d459ac00d848aa10c5da10964193abc0c950c35cafe5203606de3ea29'], 'stub': frame_step},   # v0 main+0x3a9e28
    {'name': 'frame interval from the host\'s step', 'size': 0x28c, 'fingerprints': ['f5ff6fbab4ddd6eb8be409727bbc0cbf68ef791aa143a1043253cdbb771cc365'],
     'stub': lambda ctx: frame_step(ctx, 0xa4)},   # 1.8.0 (v917504) main+0x69cbb0
]}

BINDING = [
    {'what': 'frame interval from the host\'s step', 'now': 'game', 'goal': 'game', 'purpose': 'frame rate',
     'why': "the game's own interval choice, found by its function's fingerprint on any build that keeps the function",
     'checked': 'v0 and 1.8.0 mains: one function with each fingerprint, the sites and the free registers read by disassembly (2026-11-08, 1.8.0 2026-11-12)'},
]
