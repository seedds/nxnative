# Pokemon Pokopia (0400A3A01EE4C000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's
# CUTS, OBS and FRAME_STEP as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
from nso_asm import ADRP, B

# Render scale (DM_RENDER_SCALE, state +0x10; the launcher's Render scale). KTGL's dynamic resolution sizes the 3D views from a ladder of 31 sizes, 960x540 to 1920x1080 in steps
# of 32x18 (widths at main+0x2849658, heights at main+0x28496e0, read by main+0x1129990 / main+0x11299d0; indices 31-33 are 2560, 3840, 7680 wide). Its controller
# (main+0x22cd20, run every frame on the game's task threads) picks an index into [obj+0x88]: from the operation mode and the graphics scale tier (main+0x22d338: handheld
# tier 0 starts at 20, 1600x900; docked 30, 1920x1080), stepped down and up by its load counters (+0x9c, +0xa0 against 1.25), which on this host never move (index 20 held
# through a 2.5-minute iPhone run and Mac runs). At main+0x22cf68 it stores the index as the current one (+0x2a), and the views' sizes follow from that index (w0, the argument
# of the size lookups after it). The stub there replaces w0 by the ladder step of the host's scale, a fraction of the 1920x1080 output: round((2 x scale - 1) x 30), so 0.5 is
# 960x540, 0.75 1440x810, 0.9 1728x972, held to the game's own index, so the scale only lowers the size the operation mode picks (handheld: 0.85 and up stay 1600x900;
# docked: the whole range). 1.0 (the default), unset or over runs the game's own index. Registers: x16, x17, q16, q17 saved; the flags are free (the next consumer, fcsel at
# main+0x22cf90, follows the cmp at main+0x22cf80). The game steps by the time that passed (game.tsv, Frame rate), so the page holds settings only: no frame step (ctx.no_step)
_index_store = 0x22cf68   # strb w0,[x19,#0x2a]

def render_scale(ctx):
    here, emit = ctx.here, ctx.emit
    ctx.site(_index_store, '60aa0039')
    s = here()
    words = [
        0xa9bd47f0,                       # stp x16,x17,[sp,#-0x30]!
        0xad00c7f0,                       # stp q16,q17,[sp,#0x10]
        None,                             # adrp x16, state page
        0xb9401211,                       # ldr w17,[x16,#0x10]     the host scale's bits
        0x34000231,                       # cbz w17,keep            unset
        0x52a7f010,                       # mov w16,#0x3f800000     1.0
        0x6b10023f,                       # cmp w17,w16
        0x540001c2,                       # b.hs keep               1.0 or over: the game's own index
        0x1e270230,                       # fmov s16,w17
        0x1e302a10,                       # fadd s16,s16,s16        2 x scale
        0x1e2e1011,                       # fmov s17,#1.0
        0x1e313a10,                       # fsub s16,s16,s17        - 1
        0x1e27d011,                       # fmov s17,#30.0
        0x1e310a10,                       # fmul s16,s16,s17        x 30
        0x1e200211,                       # fcvtns w17,s16          to the nearest step
        0x528003d0,                       # mov w16,#30
        0x6b10023f,                       # cmp w17,w16
        0x1a903231,                       # csel w17,w17,w16,lo     at most 30 (1920x1080)
        0x6b00023f,                       # cmp w17,w0
        0x1a803231,                       # csel w17,w17,w0,lo      at most the game's own index: the scale only lowers the mode's size
        0x2a1103e0,                       # mov w0,w17              the index the game stores and sizes from
        0xad40c7f0,                       # keep: ldp q16,q17,[sp,#0x10]
        0xa8c347f0,                       # ldp x16,x17,[sp],#0x30
        0x3900aa60,                       # strb w0,[x19,#0x2a]     the original
    ]
    for w in words: emit(ADRP(16, here(), ctx.state) if w is None else w)
    emit(B(here(), _index_store + 4))
    ctx.put(_index_store, [B(_index_store, s)], "dynamic resolution index: the step of the host render scale below 1.0 (else the game's own)")
    ctx.render_scale = 1   # it only lowers the game's size: the loader holds DM_RENDER_SCALE to 1.0 and the launcher offers the row (patch_nso.py RENDER_SCALE)
    ctx.no_step = True     # settings only: the host keeps its own pacing (display mode, the player's Frame rate)

FRAME_STEP = {'76a489f3bd3642fa5e18bd83d32c98ba': render_scale}   # v655360 (2.0.1) main

BINDING = [
    {'what': 'render scale at the dynamic-resolution index store', 'now': 'build', 'goal': 'game', 'purpose': 'rendering',
     'why': "KTGL's dynamic-resolution controller in the game's main; its function fingerprint (MATCH_FRAME_STEP) would carry it to later updates of this game, and possibly other KTGL "
            'games on the same engine version, which nobody has compared yet',
     'checked': 'v655360 main on the Mac: the controller runs every frame and holds index 20 (1600x900) handheld; DM_RENDER_SCALE runs (2026-11-15)'},
]
