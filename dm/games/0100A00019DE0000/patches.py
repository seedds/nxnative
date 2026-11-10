# Hades II (0100A00019DE0000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py;
# MATCH_FRAME_STEP as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
from nso_asm import ADRP, B

# Render size. The app's init (the function that also runs the main loop) sets the size of everything the game draws once, right after nn::vi::CreateLayer: mov x8,#0x500;
# movk x8,#0x2d0,lsl #32 (1280 | 720 << 32) at +0x190, stored to the app settings (+8) and the window rectangle; the swapchain, every screen-sized target and the HUD's layout
# follow it (1080p: 45 targets at 1920x1080 and 16 at 960x540 where 720p has 1280x720 and 640x360). The game asks no service for a size (no vi or oe resolution import), so
# only its code can change it, and it is the same 1280x720 docked and handheld. The stub sizes it by the host's render scale (state +0x10, DM_RENDER_SCALE, written by the
# loader), each side rounded to a multiple of 4 (every 0.05 step gives whole sizes: 64 x 36 pixels); 1.0, or no scale written, keeps the game's constant. Up to 3.0 (3840x2160).
# x16 / x17 and s16..s18 are free there: the function uses none of them, and s0 / s1 are written again before they are read.
# padsiwono found the site and ran 1600x900 to 3840x2160 on update 1.0.131248 (issue #772)
def render_size(ctx):
    here, emit, fs_site, fs_put, base = ctx.here, ctx.emit, ctx.site, ctx.put, ctx.base
    site = base + 0x190
    fs_site(site, '08a080d2085ac0f2')   # mov x8,#0x500; movk x8,#0x2d0,lsl #32
    start = here(); emit(ADRP(16, here(), ctx.state))
    for w in (0xbd401210,   # ldr s16,[x16,#0x10]      render scale
              0x1e2e1011,   # fmov s17,#1.0
              0x1e312200,   # fcmp s16,s17
              0x540001e0,   # b.eq orig                1.0: the game's size
              0x1e202208,   # fcmp s16,#0.0
              0x540001ad,   # b.le orig                none written
              0x52802811,   # mov w17,#320
              0x1e220231,   # scvtf s17,w17
              0x1e300a31,   # fmul s17,s17,s16
              0x1e200231,   # fcvtns w17,s17           width / 4
              0x52801690,   # mov w16,#180
              0x1e220212,   # scvtf s18,w16
              0x1e300a52,   # fmul s18,s18,s16
              0x1e200250,   # fcvtns w16,s18           height / 4
              0x531e7631,   # lsl w17,w17,#2
              0x531e7610,   # lsl w16,w16,#2
              0xaa108228):  # orr x8,x17,x16,lsl #32   width | height << 32
        emit(w)
    emit(B(here(), site + 8))
    emit(0xd280a008); emit(0xf2c05a08); emit(B(here(), site + 8))   # orig: mov x8,#0x500; movk x8,#0x2d0,lsl #32; back
    fs_put(site, [B(site, start)], 'render size: 1280x720 times the host\'s render scale (state +0x10; 1.0 = the game\'s 1280x720)')
    ctx.render_scale = 3   # the largest scale the stub takes (3840x2160)

# The init function, 848 bytes, the size at +0x190 in both. The two builds differ in one global's member offset (0xb40 / 0xb10), so each has its own fingerprint.
MATCH_FRAME_STEP = {'main': [
    {'name': 'render size from the host\'s render scale', 'size': 848, 'fingerprints': ['ec42a9cf460848924228caf4dd72a5dc2494e80898977a3252fc1a6603f4cd77'], 'stub': render_size},   # v0 main+0x2a4410
    {'name': 'render size from the host\'s render scale', 'size': 848, 'fingerprints': ['5dcdac610fafc7847ca606727f1439a0be24e1e69c64338abbe1922d5bfb21cd'], 'stub': render_size},   # 1.0.141667 (v655360) main+0x2a5790
]}

# How general each patch is and could be (dm/tools/docs_patches.py "Binding", listed in docs/agents/game-patches.md): what, its binding now, the loosest it could have (goal),
# purpose, why; blocked or kept: why the gap stays open (cannot close yet, or left open on purpose); checked: what was checked on game files (else a judgement)
BINDING = [
    {'what': 'render size: 1280x720 times the host\'s render scale', 'now': 'game', 'goal': 'game', 'purpose': 'rendering',
     'why': "the game's own size constant in its init, found by the function's fingerprint on any build that keeps the function; the game asks no service for a size, and no other game in "
            "the repository runs Supergiant's NVN engine (Hades is Vulkan)",
     'checked': 'v0 and 1.0.141667 mains: one function with each fingerprint, the constant unique in main, the site and its free registers read by disassembly (2026-11-10)'},
]
