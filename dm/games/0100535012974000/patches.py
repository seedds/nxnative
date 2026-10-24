# Hades (0100535012974000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's build;
# CUTS, OBS and FRAME_STEP as described there ("Registered patches"). Hades has no frame step: its entry in FRAME_STEP only uses the state page for the output size stub
from nso_asm import ADRP, B
# Hades sets its window size once at startup (main+0x2492b0: 1280x720 into the window settings at main+0x8ccd90, +0x8 and +0x130, docked and handheld alike), and The Forge
# makes the swap chain and the render targets from it, ignoring the Vulkan surface's extent. The stub replaces the movk of that pair: x9 = the host's output size (state page +0x40,
# DM_OUTPUT_SIZE; 0 = the game's 1280x720). At 2560x1440 the game renders the whole frame at that size (not an upscale)
def output_size(ctx):
    here, emit, fs_site, fs_put, FS_STATE_OFF = ctx.here, ctx.emit, ctx.site, ctx.put, ctx.state
    fs_site(0x2492e0, '095ac0f2')
    start = here(); emit(ADRP(9, here(), FS_STATE_OFF))
    for w in (0xf9402129,   # ldr x9,[x9,#0x40]      the host's size
              0xb5000069,   # cbnz x9,+3
              0xd280a009,   # mov x9,#0x500          the game's 1280x720
              0xf2c05a09):  # movk x9,#0x2d0,lsl #32
        emit(w)
    emit(B(here(), 0x2492e4))
    fs_put(0x2492e0, [B(0x2492e0, start)], 'window size = the host\'s output size (state page +0x40; 0 = 1280x720)')
    ctx.keys.update(OUTPUT_W='0x500', OUTPUT_H='0x2d0')

FRAME_STEP = {'d41357a3da356b09fa400b90f303b564': output_size}
