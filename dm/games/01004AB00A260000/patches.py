# DARK SOULS: REMASTERED (01004AB00A260000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO build ID;
# CUTS, OBS, FRAME_STEP and MATCH_STUBS as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
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

# FXR emitter 60 fps rate fix (#2355):
#
# Original Dark Souls ran at 30 fps (dt = 1/30 s), so "emit every frame" repeating FXR emitters author their interval as 0.0166667 s (0x3c888889, half a 30 fps frame = 1/60 s,
# used by 2,765 of 5,416 type-0x7e7 emitters across DSR's 3,134 FXR files, including 551 of its 1,278 model-cluster emitters and 9 more with interval 0.0 s) so 30 fps dt jitter
# never misses a frame. Under our 60 fps patch (dt = 1/60 s = 0x3c888889), the repeating-emitter state machine (1.0.3 main+0xaa0244, 1.0.0 main+0xaa9ae4; called only by
# main+0xaa0164) compares state_timer ([x20, #0x78]) directly against the authored interval with no 1/30 s floor, so 0.0166667 s and 0.0 s intervals fire on every 60 fps frame
# (60 Hz = 2x intended rate) while particle expiration (main+0x116d4a4) stays in wall seconds.
#
# During Ornstein & Smough, a 30-particle/emit model-cluster effect (T = 0.25 s, max_particles = 481 from main+0x1162688) peaks at 8 x 30 = 240 live particles at native 30 Hz,
# but at 60 Hz reaches 13 x 30 = 390 live particles at t = 12/60 s = 0.20 s, overflowing the model-cluster renderer's 384-slot inline arrays (main+0x622cfc) into its embedded lock
# vtable (+0x8618) and crashing with SIGSEGV at main+0x624868. Across all 3,134 FXR files in the game, 100% of all 1,278 model-cluster emitters (aid 108) use repeating node types
# 0x7e7 (1,045) or 0x7f0 (233), both handled directly by main+0xaa0244.
#
# Flooring all four repeating-emitter interval comparisons in main+0xaa0244 to 0.03125 s (1/32 s) / 32 ms restores exact 30 Hz emission at 60 fps (16.67 ms < 31.25 ms < 33.33 ms,
# 17 ms < 32 ms < 33 ms), leaves intervals >= 32 ms untouched, and is a no-op at native 30 fps. Scratch registers w9 and s2 are dead at all four sites (each branches straight to
# the epilogue).


def fxr_emitter_rate(ctx):
    em = ctx.base
    # Float interval sites (+0xfc: node types 0x7e4/0x7e8; +0x324: node type 0x7e7): max(s1, 0.03125f); fcmp s0, s1
    for off, label in ((0xfc, '0x7e4/0x7e8'), (0x324, '0x7e7')):
        site = em + off
        ctx.site(site, '0020211e')  # fcmp s0, s1
        start = ctx.here()
        ctx.emit(0x52A7A009)  # mov   w9, #0x3d000000 (0.03125f = 1/32 s)
        ctx.emit(0x1E270122)  # fmov  s2, w9
        ctx.emit(0x1E222020)  # fcmp  s1, s2
        ctx.emit(0x1E21BC41)  # fcsel s1, s2, s1, lt (preserves NaN)
        ctx.emit(0x1E212000)  # fcmp  s0, s1
        ctx.emit(B(ctx.here(), site + 4))
        ctx.put(site, [B(site, start)], f'FXR emitter ({label}): floor repeat interval to 1/32 s (30 Hz max at 60 fps)')

    # Integer ms interval sites (+0x284: 0x7f0/0x7f2 cmp w8, w21; +0x480: 0x7ef/0x7f3 cmp w8, w22): cmp w8, max(wReg, 32)
    for off, orig_hex, cmp_imm, csel_w, label in (
        (0x284, '1f01156b', 0x710082BF, 0x1A95B129, '0x7f0/0x7f2'),
        (0x480, '1f01166b', 0x710082DF, 0x1A96B129, '0x7ef/0x7f3'),
    ):
        site = em + off
        ctx.site(site, orig_hex)  # cmp w8, w21 / w22
        start = ctx.here()
        ctx.emit(0x52800409)  # mov  w9, #32
        ctx.emit(cmp_imm)     # cmp  w21/w22, #32
        ctx.emit(csel_w)      # csel w9, w9, w21/w22, lt
        ctx.emit(0x6B09011F)  # cmp  w8, w9
        ctx.emit(B(ctx.here(), site + 4))
        ctx.put(site, [B(site, start)], f'FXR emitter ({label}): floor repeat interval to 32 ms (30 Hz max at 60 fps)')


MATCH_STUBS = {'main': [
    {   # 1.0.3 (v196608) main+0xaa0244, 1.0.0 (v0) main+0xaa9ae4 (identical function across builds)
        'name': 'FXR 60 fps emitter rate',
        'size': 0x4E0,
        'fingerprints': ['e45256b0998f1d2f3a105bd1139884750584e4defcd7b28cd7a131c939f37fbf'],
        'stub': fxr_emitter_rate,
    },
]}

# How general each patch is and could be (dm/tools/docs_patches.py "Binding", listed in docs/agents/game-patches.md): what, its binding now, the loosest it could have (goal),
# purpose, why; blocked or kept: why the gap stays open (cannot close yet, or left open on purpose); checked: what was checked on game files (else a judgement)
BINDING = [
    {'what': 'frame limiter 30 → 60 fps and the 1/60 update step', 'now': 'build', 'goal': 'game', 'purpose': 'frame rate',
     'why': "the game's own limiter constant and fixed step"},
    {'what': 'FXR 60 fps repeating-emitter interval floor (1/32 s / 32 ms)', 'now': 'game', 'goal': 'game', 'purpose': 'frame rate',
     'checked': '1.0.0 (v0) and 1.0.3 (v196608) mains plus all 3,134 FXR files in dvdbnd0.bdt: emitter state machine, capacity calculator, cluster updater and all 1,278 model-cluster emitters verified by disassembly, archive scan, full rewrite and instruction replay (2026-11-25)',
     'why': "the game's own FXR repeating-emitter state machine assumes 30 fps (2,765 emitters author interval = 1/60 s as half a 30 fps frame, so our 60 fps patch doubles their emission rate); function-matched (MATCH_STUBS) across 1.0.0 and 1.0.3"},
]
