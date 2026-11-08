# Hades II (0100A00019DE0000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO build ID;
# FRAME_STEP as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
from nso_asm import ADRP, B, LDRX, NOP

# Render size. Supergiant's engine sizes the swapchain and every screen-sized target from the app settings' width and height. On the Switch the app's init (main+0x2a4d20 in
# 1.0.131248 = v196608, the function that also runs the main loop) writes them as one constant right after nn::vi::CreateLayer: mov x8,#0x500; movk x8,#0x2d0,lsl #32
# (1280 | 720 << 32) at main+0x2a4eb0, stored to settings +8 and to the window's rectangle {0, 0, w, h}. Next to it the init stores a pointer to the PC version's resolution
# table (main+0x6c837c: 1280x720, 1920x1080, 2560x1440, 3840x2160), which nothing reads. The stub takes the size from the state page (+0x38: u32 width, u32 height; DM_RENDER_SIZE,
# written by the loader) and keeps 1280x720 when it is 0. x8 is written by the stub before its only read. Measured headless on the Mac (M5 Max): at 1600x900, 1920x1080, 2560x1440
# and 3840x2160 the swapchain and the 45 full-size and 16 half-size targets of a frame follow the size, the HUD is laid out for it, and the game runs the opening into Erebus
# without a fault. PR #485 reports v0 crashing at 1600x900 (main+0x48e5a4) with the same two instructions patched at main+0x2a6080; that did not happen on this build
def frame_step_v196608(ctx):
    ctx.site(0x2a4eb0, '08a080d2085ac0f2')
    start = ctx.here(); ctx.emit(ADRP(8, ctx.here(), ctx.state)); ctx.emit(LDRX(8, 8, 0x38 // 8))   # ldr x8,[state,#0x38]  width | height << 32
    for w in (0xb5000068,   # cbnz x8,back (+3)
              0xd280a008,   # mov x8,#0x500          1280 (the original)
              0xf2c05a08):  # movk x8,#0x2d0,lsl #32 720
        ctx.emit(w)
    ctx.emit(B(ctx.here(), 0x2a4eb8))
    ctx.put(0x2a4eb0, [B(0x2a4eb0, start), NOP], 'app settings width and height: the state page\'s render size (DM_RENDER_SIZE), 1280x720 when it is 0')
    ctx.capabilities.add('RENDER_SIZE')

FRAME_STEP = {'e4f736641889b26f3194847d2b9dce57': frame_step_v196608}

# How general each patch is and could be (dm/tools/docs_patches.py "Binding", listed in docs/agents/game-patches.md): what, its binding now, the loosest it could have (goal),
# purpose, why; blocked or kept: why the gap stays open (cannot close yet, or left open on purpose); checked: what was checked on game files (else a judgement)
BINDING = [
    {'what': 'render size: the app settings\' width and height from the state page', 'now': 'build', 'goal': 'game', 'purpose': 'rendering',
     'why': "the game's own init constant; the same two instructions sit at main+0x2a6080 in v0, so a function-matched rule could carry it over the updates",
     'kept': 'tested on 1.0.131248 only; PR #485 saw v0 crash at 1600x900 with the same change, so each build needs its own run first',
     'checked': '1.0.131248 headless at 1600x900, 1920x1080, 2560x1440 and 3840x2160 (M5 Max, macOS 26.6.2)'},
]
