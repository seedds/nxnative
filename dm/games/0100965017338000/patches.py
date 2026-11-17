# Super Mario Party Jamboree (0100965017338000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a
# module's NSO build ID; CUTS, OBS and FRAME_STEP as described there ("Registered patches").
#
# The 3D render size is pinned, so the island map's picture stops going soft and crisp again (docs/reference/frame-rate.md, "A dynamic resolution whose frame span does not follow
# its render size"). NintendoWare Bezel Engine 2.9 keeps its dynamic resolution in one object of the renderer module (bex::gfx::RendererModule +0x148, reached through g_Module,
# main+0x1bfd890; its getters are RendererModule::IsDynamicResolutionEnabled, main+0x560e0 = the byte at +0x1b8, and RendererModule::GetDynamicResolutionScale, main+0x560fc =
# the controller's +0x68):
#   - main+0x795a0 is the controller's per-frame update, called from the renderer's pass (main+0x534a0..) with the frame's time and the frame's budget; it computes
#     scale x sqrt((budget - (t1 - t0)) / t0) and stores the result at +0x68 (main+0x79758), clamped to [min_scale, 1.0]. min_scale is +0x18, 0.5, set by the constructor
#     (main+0x7953c: mov w8,#0x3f000000; str w8,[x0,#0x18]).
#   - main+0x797ac turns the scale into a render size for the size it is given (an IntSize): height = 900 x scale rounded down to a multiple of 8 (the w2 argument), width kept in
#     proportion; the renderer's pass (main+0x534a0) then resizes the scene's render targets to that size. 0.5 gives 448 = 900 x 0.5 rounded down to 8, the 796x448 the user saw,
#     and 1.0 gives the 1600x900 base.
# The size does not shorten the frame (the 1920x1080 composite and UI passes are fixed), so the controller steps down for a frame it cannot shorten and back up: measured
# 1600x900 -> 796x448 and back at 57-60 fps either way (the profile README's Frame rate section; DM_NVN_TSDBG=1).
# The cut sends the store at main+0x79758 through a stub that writes 1.0 instead, unless the host's DM_DYNAMIC_RESOLUTION (state page +0x40) asks for the game's own controller;
# the build declares the DYNAMIC_RESOLUTION capability, so the launcher's "Dynamic resolution" setting is the same switch (off by default: pinned). FRAME_STEP is used for its
# state page only -- no step site is installed and the profile has no fps_target, so every field the host writes there is 0 and its pacing is the game's own.
import struct
from nso_asm import ADRP, B
CUTS = {}

# main+0x79758 is 0xbd006a60 (str s0,[x19,#0x68]); the stub repeats it, then the store of +0x6c that follows, and returns to main+0x79760. x19 is the controller, w8 is 1 at
# this point on every path that reaches the store, and x16 is free (the function uses x0-x2, x8-x12, x19, x20, s0-s9 only); the branch back is at the end of the stub.
def pin_render_size(ctx):
    here, emit, site, put = ctx.here, ctx.emit, ctx.site, ctx.put
    site(0x79758, '606a00bd')
    start = here()
    emit(ADRP(16, here(), ctx.state))
    for w in (0xb9404210,   # ldr w16,[x16,#0x40]   DM_DYNAMIC_RESOLUTION (1: the game's own controller)
              0x35000050,   # cbnz w16,keep (+8)
              0x1e2e1000,   # fmov s0,#1.0          0: full size
              0xbd006a60,   # keep: str s0,[x19,#0x68]
              0x3901b268):  # strb w8,[x19,#0x6c]
        emit(w)
    emit(B(here(), 0x79760))
    put(0x79758, [B(0x79758, start)], 'render size pinned: the dynamic-resolution controller\'s scale is 1.0 (main+0x79758), unless DM_DYNAMIC_RESOLUTION asks for the game\'s own')
    ctx.capabilities.add('DYNAMIC_RESOLUTION')

FRAME_STEP = {'d70ce1726c979e45ed4200e9c2b23b2a': pin_render_size}

# How general each patch is and could be (dm/tools/docs_patches.py "Binding", listed in docs/agents/game-patches.md): what, its binding now, the loosest it could have (goal),
# purpose, why; blocked or kept: why the gap stays open (cannot close yet, or left open on purpose); checked: what was checked on game files (else a judgement)
BINDING = [
    {'what': 'dynamic resolution pinned to full size', 'now': 'build', 'goal': 'game', 'purpose': 'rendering',
     'why': "Bezel Engine 2.9's own controller (bex::gfx::RendererModule's scale and its per-frame update), found through the engine's dynamic symbols in this main; the code is the "
            'engine\'s, but the offsets are this build\'s',
     'kept': 'a function-matched rule would cover other builds (the update and the store\'s function are distinct), but only one build is registered and no second build exists to '
             'check its fingerprint against'},
]
