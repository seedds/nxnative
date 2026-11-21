# Star Fox (0400E88025B1A000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py;
# MATCH_FRAME_STEP as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
from nso_asm import ADRP, B

# Render scale. The render function (v131072 main+0xb447300, 0x744c bytes) sizes the 3D views by a scale of the window: the mode's override, render.resolution_scale_console
# ("Resolution scale override when in console mode, when nonzero (Nintendo only)"; render.resolution_scale_handheld in handheld mode), when it is nonzero, else
# render.resolution_scale, in s10 there. It reads the override into [sp+0x3a0] through the cvar getter and loads it with ldr s0,[sp,#0x3a0] (+0x25ac); the game caps the result
# at 1.0, the window's size. The shipped settings set the console override to 0.5 (README.md, The picture). The stub loads the override as the game does, then multiplies the
# scale in force (the override, or s10 when the override is 0) by the host's render scale (state +0x10, DM_RENDER_SCALE, written by the loader): 1.0, or none written, keeps
# the game's own scale. Up to 2.0, which takes the console override's 0.5 to the window's size.
# The three instructions after the site write s2, s1 and x9 before any read (fmov s2,w24; fabs s1,s0; ldr x9,[sp,#0x330]), so the stub uses them; s10 is only read there, and
# the function itself uses x16, x17, v16 and v17. No branch lands on the site. The fingerprint covers the whole function and its callees, so a build that matches has this code.
def render_scale(ctx):
    here, emit, fs_site, fs_put = ctx.here, ctx.emit, ctx.site, ctx.put
    site = ctx.base + 0x25ac   # ldr s0,[sp,#0x3a0] in the render function
    fs_site(site, 'e0a343bd')
    ctx.area(site)   # main's text is longer than a branch reaches: the area that reaches the site
    start = here()
    emit(0xbd43a3e0)    # ldr s0,[sp,#0x3a0]       the mode's override, as the game loads it
    emit(ADRP(9, here(), ctx.state))
    for w in (0xbd401121,   # ldr s1,[x9,#0x10]        render scale
              0x1e2e1002,   # fmov s2,#1.0
              0x1e222020,   # fcmp s1,s2
              0x540000e0,   # b.eq back                1.0: the game's own
              0x1e202028,   # fcmp s1,#0.0
              0x540000ad,   # b.le back                none written
              0x1e202008,   # fcmp s0,#0.0
              0x54000041,   # b.ne scale               an override is set
              0x1e204140,   # fmov s0,s10              none: render.resolution_scale
              0x1e210800):  # scale: fmul s0,s0,s1
        emit(w)
    emit(B(here(), site + 4))   # back
    fs_put(site, [B(site, start)], 'render scale: the mode\'s resolution scale times the host\'s render scale (state +0x10); 1.0 = the game\'s own')
    ctx.render_scale = 2   # the largest scale the stub takes (the console override's 0.5 to the window's size)


MATCH_FRAME_STEP = {'main': [
    {'name': 'render scale times the resolution scale', 'size': 0x744c,
     'fingerprints': ['150c560bd68fa941dbf939926acaf93203cc408966cda9700bda32aa586bf5ce'],   # v131072 main+0xb447300
     'stub': render_scale},
]}

# How general each patch is and could be (dm/tools/docs_patches.py "Binding", listed in docs/agents/game-patches.md): what, its binding now, the loosest it could have (goal),
# purpose, why; blocked or kept: why the gap stays open (cannot close yet, or left open on purpose); checked: what was checked on game files (else a judgement)
BINDING = [
    {'what': 'render scale: the mode\'s resolution scale times the host\'s render scale', 'now': 'game', 'goal': 'engine', 'purpose': 'rendering',
     'blocked': "an engine binding would find the site by the cvar's name (render.resolution_scale_console), which the function matcher cannot anchor on: it takes a "
                "fingerprint or an imported symbol, and the cvar getter is the game's own code",
     'why': "the game's own render function, found by its fingerprint; the cvars it scales are the engine's (Star Fox's eclipse engine, no other game in the repository)",
     'checked': 'v131072 main: the fingerprint finds the function once, the site word and the free registers as above; pass dumps on the Mac (2026-11-21)'},
]
