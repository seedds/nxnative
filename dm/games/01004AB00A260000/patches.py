# DARK SOULS: REMASTERED (01004AB00A260000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO build ID;
# CUTS, OBS and FRAME_STEP as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
from nso_asm import B

CUTS = {
  # Dark Souls Remastered 1.0.3 (v196608) main: 60 fps (30 on the Switch). The game presents at interval 1; its render thread (HGCommandDispatcher, main+0x8a4b28) ends each frame in a CPU limiter,
  # main+0x707090(this, fps): 1000 / fps ms less the time since its last call (nn::os::GetSystemTick), slept out when more than 2 ms are left; the main thread waits for the render thread.
  # The 30 is a constant at the only call (main+0x706fa4). The game does not step by the time that passed: the main thread's frame update (main+0x6a337c, on the manager at GOT 0x2287be0)
  # takes a fixed step, the float 1/30 at main+0x167a6ec loaded at main+0x667904, so at 60 fps everything ran twice as fast (on the iPhone). The step loads the 1/60 that rodata holds at
  # main+0x167a834 instead (as the community's 60 fps cheat for this build does). Climbing ladders still misplays at 60 (known with that cheat too). The limiter's frame-skip mode only starts
  # above 36.4 ms a frame, so 60 does not reach it
  'df3766a2bb651a3e822828abf10b6f7b': [
    (0x706fa4, 'e10f1f32', '81078052', 'frame limiter target: mov w1,#30 → mov w1,#60 (main+0x707090 sleeps out 1000 / fps ms since the last frame)'),
    (0x667904, '08ed46bd', '083548bd', 'frame update step: ldr s8,[x8,#0x6ec] (1/30) → ldr s8,[x8,#0x834] (1/60), passed to main+0x6a337c'),
  ],
}

def bound_cluster_model_count(ctx):
    site = 0x622e58
    ctx.site(site, 'd72240b9')  # ldr w23, [x22, #0x20]

    # The inline arrays hold 384 instances. The next load to x9 and compare
    # overwrite the scratch register and flags before either is used.
    stub = ctx.here()
    ctx.emit(0xb94022d7)  # ldr w23, [x22, #0x20]
    ctx.emit(0x52803009)  # mov w9, #384
    ctx.emit(0x6b0902ff)  # cmp w23, w9
    ctx.emit(0x1a8992f7)  # csel w23, w23, w9, ls
    ctx.emit(B(ctx.here(), site + 4))
    ctx.put(site, [B(site, stub)], 'bound model effect instances to 384')


STUBS = {
    'df3766a2bb651a3e822828abf10b6f7b': bound_cluster_model_count,
}

# How general each patch is and could be (dm/tools/docs_patches.py "Binding", listed in docs/agents/game-patches.md): what, its binding now, the loosest it could have (goal),
# purpose, why; blocked or kept: why the gap stays open (cannot close yet, or left open on purpose); checked: what was checked on game files (else a judgement)
BINDING = [
    {'what': 'bound model effect instances to 384', 'now': 'build', 'goal': 'build', 'purpose': 'memory',
     'why': "the game's inline effect arrays and their exact count-load site"},
    {'what': 'frame limiter 30 → 60 fps and the 1/60 update step', 'now': 'build', 'goal': 'game', 'purpose': 'frame rate',
     'why': "the game's own limiter constant and fixed step"},
]
