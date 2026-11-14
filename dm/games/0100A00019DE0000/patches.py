# Hades II (0100A00019DE0000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py;
# MATCH_FRAME_STEP as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
from nso_asm import ADRP, B
from nso_match import MatchError

# Render size. The app's init (the function that also runs the main loop) sets the size of everything the game draws once, right after nn::vi::CreateLayer: mov x8,#0x500;
# movk x8,#0x2d0,lsl #32 (1280 | 720 << 32), stored to the app settings (+8) and the window rectangle; the swapchain, every screen-sized target and the HUD's layout follow
# it (1080p: 45 targets at 1920x1080 and 16 at 960x540 where 720p has 1280x720 and 640x360). The game asks no service for a size (no vi or oe resolution import), so only
# its code can change it, and it is the same 1280x720 docked and handheld. The stub sizes it by the host's render scale (state +0x10, DM_RENDER_SCALE, written by the
# loader), each side rounded to a multiple of 4 (every 0.05 step gives whole sizes: 64 x 36 pixels); 1.0, or no scale written, keeps the game's constant. Up to 3.0 (3840x2160).
# padsiwono found the site and ran 1600x900 to 3840x2160 on update 1.0.131248 (issue #772)
#
# The function is found as the one that calls nn::vi::CreateLayer, not by its fingerprint: the fingerprint covers the whole body, and v0 and 1.0.141667 already differed in
# one global's member offset (0xb40 / 0xb10), so every update that touched the function needed a new one. find_size checks what the stub relies on, in that build's code:
#   - one call to CreateLayer in the function, and after it the one movz xN,#0x500 / movk xN,#0x2d0,lsl #32 pair of the function, the pair once in all of main's text
#   - no branch in the function lands on the movk (the stub returns past it)
#   - the function uses none of x16, x17, v16, v17, v18, which the stub writes
# Anything else: no stub, a red line in the rewrite log, the game at 1280x720. Reviewed: v0 main+0x2a4410 (the size at +0x190, x8), 1.0.141667 (v655360) main+0x2a5790
# (+0x190, x8); 1.0.131248's site, main+0x2a4eb0, per padsiwono's report (no copy here).
def is_size(words, i):   # movz xN,#0x500 then movk xN,#0x2d0,lsl #32 at words[i]: N, else None
    w = words[i]
    return w & 31 if w & 0xffffffe0 == 0xd280a000 and i + 1 < len(words) and words[i + 1] == 0xf2c05a00 | (w & 31) else None


def find_size(fn):
    """(offset of the size in the function, its register) after the checks above; MatchError otherwise"""
    sites = [(4 * i, rd) for i in range(len(fn.words)) if (rd := is_size(fn.words, i)) is not None]
    if len(sites) != 1: raise MatchError(f'{len(sites)} 1280x720 sizes in the function at {fn.start:#x} (need one)')
    (off, rd), = sites
    if len(fn.calls) != 1 or fn.calls[0] > off: raise MatchError(f'the size at {fn.start + off:#x} does not follow the one CreateLayer call ({len(fn.calls)} calls)')
    n = sum(1 for i in range(len(fn.text)) if is_size(fn.text, i) is not None)
    if n != 1: raise MatchError(f'{n} 1280x720 sizes in main\'s text (need one)')
    if fn.start + off + 4 in fn.branch_targets(): raise MatchError(f'a branch lands on the movk at {fn.start + off + 4:#x}')
    used = fn.uses({'x16', 'x17', 'v16', 'v17', 'v18'})
    if used: raise MatchError(f'the function at {fn.start:#x} uses {", ".join(sorted(used))}, which the stub writes')
    return off, rd


def render_size(ctx):
    here, emit, fs_site, fs_put = ctx.here, ctx.emit, ctx.site, ctx.put
    off, rd = ctx.found
    site = ctx.base + off
    fs_site(site, (0xd280a000 | rd).to_bytes(4, 'little').hex() + (0xf2c05a00 | rd).to_bytes(4, 'little').hex())   # mov xN,#0x500; movk xN,#0x2d0,lsl #32
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
              0xaa108220 | rd):  # orr xN,x17,x16,lsl #32   width | height << 32
        emit(w)
    emit(B(here(), site + 8))
    emit(0xd280a000 | rd); emit(0xf2c05a00 | rd); emit(B(here(), site + 8))   # orig: mov xN,#0x500; movk xN,#0x2d0,lsl #32; back
    fs_put(site, [B(site, start)], 'render size: 1280x720 times the host\'s render scale (state +0x10; 1.0 = the game\'s 1280x720)')
    ctx.render_scale = 3   # the largest scale the stub takes (3840x2160)


MATCH_FRAME_STEP = {'main': [
    {'name': 'render size from the host\'s render scale', 'calls': '_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE', 'find': find_size, 'stub': render_size,
     'reviewed': [(0x190, 8)]},   # v0 main+0x2a4410, 1.0.141667 main+0x2a5790
]}

# How general each patch is and could be (dm/tools/docs_patches.py "Binding", listed in docs/agents/game-patches.md): what, its binding now, the loosest it could have (goal),
# purpose, why; blocked or kept: why the gap stays open (cannot close yet, or left open on purpose); checked: what was checked on game files (else a judgement)
BINDING = [
    {'what': 'render size: 1280x720 times the host\'s render scale', 'now': 'game', 'goal': 'game', 'purpose': 'rendering',
     'why': "the game's own size constant, found in the one function that calls nn::vi::CreateLayer on any build whose code there passes the stub's checks (no fingerprint); the game "
            "asks no service for a size, and no other game in the repository runs Supergiant's NVN engine (Hades is Vulkan)",
     'checked': 'v0 and 1.0.141667 mains: the anchor finds the reviewed site (+0x190, x8) in each, the constant once in main, the stub\'s registers unused in the function (2026-11-10)'},
]
