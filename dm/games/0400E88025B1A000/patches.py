# StarFox (0400E88025B1A000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO build ID;
# FRAME_STEP and CAPABILITIES as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
from nso_asm import ADRP, B

# Resolution scale (state +0xa0, a float the loader writes from DM_RESOLUTION_SCALE; 0 = the game's own). The renderer (main+0xb447300) sizes its scene each frame as its target
# (1920x1080 at graphics scale 0) times a scale it reads from its settings at main+0xb4498a8: render.resolution_scale_console docked (0.5) or render.resolution_scale_handheld
# (0.83), and runs DLSS at that size, as anti-aliasing; its own upscaler (NDSR) then takes the picture to the target. The stub replaces the value read, the load at main+0xb4498ac,
# when the state holds one: the rest (a near-zero value falls back to render.resolution_scale, the minimum with the benchmark scale, GameShare's scales) runs as before. x9, s1
# and the flags are dead there: main+0xb4498b4, +0xb4498b8 and +0xb4498bc set them. With 0 every instruction the game runs is its own
def frame_step(ctx):
    here, emit, site, put, state = ctx.here, ctx.emit, ctx.site, ctx.put, ctx.state
    site(0xb4498ac, 'e0a343bd')                          # ldr s0,[sp,#0x3a0]: the scale read from the settings
    ctx.area(0xb4498ac)                                  # main's text is 189 MB: the back patch area reaches the site
    start = here()
    emit(0xbd43a3e0)                                     # ldr s0,[sp,#0x3a0]   (original)
    emit(ADRP(9, here(), state))
    for w in (0xbd40a121,                                # ldr s1,[x9,#0xa0]    the host's scale
              0x1e202028,                                # fcmp s1,#0.0
              0x5400004d,                                # b.le done (+8)       0: the game's own
              0x1e204020):                               # fmov s0,s1
        emit(w)
    emit(B(here(), 0xb4498b0))                           # done
    put(0xb4498ac, [B(0xb4498ac, start)], 'scene resolution scale from the host (DM_RESOLUTION_SCALE; 0 = the game\'s own)')

FRAME_STEP = {'a7730d0f74d168d90422354c8317f76f': frame_step}
# The launcher offers DM_RESOLUTION_SCALE (dm/games/settings.tsv) only for this build
CAPABILITIES = {'a7730d0f74d168d90422354c8317f76f': ('RESOLUTION_SCALE',)}

# How general each patch is and could be (dm/tools/docs_patches.py "Binding", listed in docs/agents/game-patches.md): what, its binding now, the loosest it could have (goal),
# purpose, why; blocked or kept: why the gap stays open (cannot close yet, or left open on purpose); checked: what was checked on game files (else a judgement)
BINDING = [
    {'what': 'scene resolution scale', 'now': 'build', 'goal': 'game', 'purpose': 'rendering',
     'why': "the game's own settings read in its renderer",
     'blocked': 'frame-step stubs have no function-matched form yet'},
]
