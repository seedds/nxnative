# Hollow Knight: Silksong (010013C00E930000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO
# build ID (FRAME_STEP as described there, "Registered patches"). The game has no frame step stubs: FRAME_STEP only gives its main the state page that the screen shape stub reads
# (DM_SCREEN_ASPECT, as for Breath of the Wild and Animal Crossing: New Horizons); with DM_SCREEN_ASPECT unset the page stays zero and the stub runs the original instructions.
import struct
from nso_asm import ADRP, B

# Screen shape (DM_SCREEN_ASPECT, loader.c: state +0x48 the output height for a width of 1920, 0 = 16:9; +0x58 the screen's width / height as a float). Unity's Switch player
# renders into its 1920x1080 window textures and crops them to the screen size it applies (main+0x129074c, the screen manager's apply: this, w1 width, w2 height; it clamps both to
# the window textures, +0xac / +0xb0, stores them at +0xb4 / +0xb8, which the renderer's crop and Screen.width / height read). The game asks for 1920x1080 docked and 1280x720
# handheld (and while a cinematic plays). Its camera (ForceCameraAspect) fills any shape from 16:9 to 16:10 with more of the scene above and below, as on a Steam Deck, and
# letterboxes a taller one to 16:10 with a 16:9 view inside it, its bars in debug colours (Mac, 1544x1080 for an iPhone Duo's 1.43:1). So a 16:9 request keeps its height and
# takes the screen's width / height, at most 16:10: 1728x1080 docked, 1152x720 handheld; the host presents a crop of that shape in its own shape (nvn_mtl.m present_shape).
# x16 / x17 and s16 / s17 are free at the function's entry. The stub's body (clang):
#   ldr w17, [x16, #0x48]; cbz w17, orig; add w17, w1, w1, lsl #3; cmp w17, w2, lsl #4; b.ne orig     a 16:9 request with a screen shape set
#   ldr s16, [x16, #0x58]; mov w17, #0xcccd; movk w17, #0x3fcc, lsl #16; fmov s17, w17; fmax s16, s16, s17     the screen's width / height, at least 1.6
#   ucvtf s17, w2; fmul s16, s16, s17; fcvtns w1, s16; and w1, w1, #0xfffffffe                                  width = height x that, even
_apply_words = [0xb9404a11, 0x340001b1, 0x0b010c31, 0x6b02123f, 0x54000141, 0xbd405a10, 0x529999b1, 0x72a7f991,
                0x1e270231, 0x1e314a10, 0x1e230051, 0x1e310a10, 0x1e200201, 0x121f7821]


# Unity's screen size getter (main+0x124e224: this, x1 width, x2 height; 1280x720 handheld, 1920x1080 docked by nn::oe::GetOperationMode) sets the first size at start-up
# (main+0x1265370), and the operation mode change handler (main+0x1290908) asks it again before it applies. Handheld the game never asks for another size, so the apply above
# never runs: the getter is wrapped, and its answer takes the same shape. The wrapper calls the getter through a copy of its first instruction and converts what it stored;
# the getter's calls go through the PLT, which takes x16, so the state page is addressed again after it. x9..x11, x16 / x17 and s16 / s17 are free after the call. Body (clang):
#   ldp x1, x2, [sp, #0x10]; ldp x29, x30, [sp], #0x20                                                                the getter's pointers back
#   ldr w9, [x1]; ldr w10, [x2]; add w11, w9, w9, lsl #3; cmp w11, w10, lsl #4; b.ne ret                                 a 16:9 size
#   ldr s16, [x16, #0x58]; mov w17, #0xcccd; movk w17, #0x3fcc, lsl #16; fmov s17, w17; fmax s16, s16, s17               the shape, at least 16:10
#   ucvtf s17, w10; fmul s16, s16, s17; fcvtns w9, s16; and w9, w9, #0xfffffffe; str w9, [x1]; ret                       width = height x that, even
_get_after = [0xa9410be1, 0xa8c27bfd]
_get_words = [0xb9400029, 0xb940004a, 0x0b090d2b, 0x6b0a117f, 0x54000161, 0xbd405a10, 0x529999b1, 0x72a7f991,
              0x1e270231, 0x1e314a10, 0x1e230151, 0x1e310a10, 0x1e200209, 0x121f7929, 0xb9000029, 0xd65f03c0]


def _cbz_w17(frm, to): return 0x34000000 | ((((to - frm) >> 2) & 0x7FFFF) << 5) | 17
def _bl(frm, to): return 0x94000000 | (((to - frm) >> 2) & 0x3FFFFFF)


def screen_shape(ctx):
    here, emit, site, put, STATE = ctx.here, ctx.emit, ctx.site, ctx.put, ctx.state
    at, orig = 0x129074c, 0xa9bb7bfd   # stp x29, x30, [sp, #-0x50]!
    site(at, struct.pack('<I', orig).hex())
    start = here(); emit(ADRP(16, here(), STATE))
    for w in _apply_words: emit(w)
    emit(orig); emit(B(here(), at + 4))
    put(at, [B(at, start)], "Unity's screen apply: a 16:9 request takes the screen's shape, at most 16:10 (DM_SCREEN_ASPECT)")
    at, orig = 0x124e224, 0xa9bc7bfd   # stp x29, x30, [sp, #-0x40]!
    site(at, struct.pack('<I', orig).hex())
    # 3 + 3 + 2 + 1 + len(_get_words) words before the copy of the getter's first instruction (the trampoline)
    start = here(); tramp = start + 4 * (9 + len(_get_words))
    emit(ADRP(16, here(), STATE)); emit(0xb9404a11)   # ldr w17, [x16, #0x48]   the output's height (0 = 16:9)
    emit(_cbz_w17(here(), tramp))                       # cbz w17, the getter unchanged
    emit(0xa9be7bfd); emit(0xa9010be1)                  # stp x29, x30, [sp, #-0x20]!; stp x1, x2, [sp, #0x10]
    emit(_bl(here(), tramp))                            # bl: the getter
    for w in _get_after: emit(w)
    emit(ADRP(16, here(), STATE))
    for w in _get_words: emit(w)
    assert here() == tramp, 'screen size getter wrapper layout'
    emit(orig); emit(B(here(), at + 4))
    put(at, [B(at, start)], "Unity's screen size getter: a 16:9 size takes the screen's shape, at most 16:10 (DM_SCREEN_ASPECT)")


FRAME_STEP = {
    'fc9ea4ccc955d5799f37752b2d730b31': screen_shape,   # 1.0.30000 (v655360) main
}

# How general each patch is and could be (dm/tools/docs_patches.py "Binding", listed in docs/agents/game-patches.md): what, its binding now, the loosest it could have (goal),
# purpose, why; blocked or kept: why the gap stays open
BINDING = [
    {'what': "screen shape: Unity's screen apply takes the screen's shape, at most 16:10 (DM_SCREEN_ASPECT)", 'now': 'build', 'goal': 'game', 'purpose': 'rendering',
     'why': "the apply is Unity's engine code, but the shape the game's own camera takes (16:9 to 16:10) is this game's",
     'blocked': 'the stub has no function-matched form yet'},
]
