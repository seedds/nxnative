# Super Smash Bros. Ultimate (01006A800016E000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO build ID;
# CUTS, OBS and FRAME_STEP as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
import struct
from nso_asm import ADRP, B
CUTS = {
  # Super Smash Bros. Ultimate 13.0.5 (v2031616) main: the engine's memory (the nu:: allocator, set up by a static constructor at main+0x37c9a20) reserves 62 GB of address space with
  # nn::os::AllocateAddressRegion(0xf80000000) and hands it out in 64 KB units: a number line of free ranges, first [0, 0xf7f20) units from the region's start (main+0x37c9b98, given to
  # main+0x37c9c00), and the top 14 MB (region + 0xf7f200000) as the pool of its range nodes, committed 64 KB at a time up to the region's end (the bound checks at main+0x37c9a9c, +0x37c9c50,
  # +0x37ca3cc and +0x37caf68). The Switch's 64 GB alias region holds it; an iPhone's single 6 GB reservation cannot, and the abort at main+0x37c9bd0 followed. Booted on the Mac with a
  # 64 GB alias region, the game commits from the region's start upward (1.73 GB contiguous from it at the title, 75 s in) and the node pool's first 64 KB at the top, so the region is
  # cut to 4 GB (the application pool, 3.2 GB, bounds what it can commit), every constant derived from its size with it: the node pool at 4 GB − 14 MB, the first free range 0xff20 units
  '21450c647b8c5940ed5b68218ea128fd': [
    (0x37c9a5c, 'e11361b2', '2100c0d2', 'AllocateAddressRegion size mov x1,#0xf80000000 → mov x1,#0x100000000 (62 GB → 4 GB)'),
    (0x37c9a6c, 'e97f7cb2', '09fe9fd2', 'node pool top check, low part mov x9,#0xffffffff0 → mov x9,#0xfff0'),
    (0x37c9a70, '0be4afd2', '0be4bfd2', 'node pool start mov x11,#0x7f200000 → mov x11,#0xff200000'),
    (0x37c9a74, '09e4aff2', '09e4bff2', 'node pool top check movk x9,#0x7f20,lsl #16 → movk x9,#0xff20,lsl #16 (0xff20fff0)'),
    (0x37c9a78, 'eb01c0f2', '0b00c0f2', 'node pool start movk x11,#0xf,lsl #32 → movk x11,#0,lsl #32 (0xff200000 = 4 GB − 14 MB)'),
    (0x37c9a9c, 'e91361b2', '2900c0d2', 'node pool bound: region end mov x9,#0xf80000000 → mov x9,#0x100000000'),
    (0x37c9b98, '01e48f52', '01e49f52', 'first free range in 64 KB units mov w1,#0x7f20 → mov w1,#0xff20'),
    (0x37c9b9c, 'e101a072', '0100a072', 'first free range movk w1,#0xf,lsl #16 → movk w1,#0,lsl #16 (0xff20 units = 4 GB − 14 MB)'),
    (0x37c9c50, 'e91361b2', '2900c0d2', 'node pool bound: region end mov x9,#0xf80000000 → mov x9,#0x100000000'),
    (0x37ca3cc, 'e81361b2', '2800c0d2', 'node pool bound: region end mov x8,#0xf80000000 → mov x8,#0x100000000'),
    (0x37caf68, 'e91361b2', '2900c0d2', 'node pool bound: region end mov x9,#0xf80000000 → mov x9,#0x100000000'),
  ],
}
# The same 11 cuts for builds without registered CUTS (dm/tools/nso_match.py), as one patch of the four functions they lie in, which applies whole or not at all: a region cut in
# some of them and not the others would put the node pool outside the region or let the bound checks pass it. Each function is found by its fingerprint and its direct callees':
# the constructor (main+0x37c9a20, 472 bytes: the size, the node pool's place and top check, the first bound and the first free range) and the three functions with a region-end
# bound (main+0x37c9c00, 536 bytes; main+0x37ca180, 1,328 bytes; main+0x37caf10, 788 bytes). The base game 1.0.0 (v0, main fd471eb4…) has the same allocator, its 62 GB constant
# at five of these sites (main+0x27f869c, +0x27f86d4, +0x27f8868, +0x27f9108, +0x27f9c80), but none of the four functions is the same code (GitHub issue #375: 13.0.1 not checked)
region = 'engine region 62 GB → 4 GB (11 instructions in four functions)'
def _function(start, size, fingerprint):
    return {'size': size, 'fingerprints': [fingerprint], 'cuts': [(off - start, old, new, f'{why} (as 13.0.5 main+{off:#x})')
                                                                  for off, old, new, why in CUTS['21450c647b8c5940ed5b68218ea128fd'] if start <= off < start + size]}
MATCH_CUTS = {'main': [{'name': region, 'functions': [
    _function(0x37c9a20, 472, '96d9585e1be0b5ad1a975ae94df0b9e5540467bbd4f646fe7b4eb7abb6ba89fc'),
    _function(0x37c9c00, 536, '6a1ad9e4dcca89cdb736dddc23b8cb7640ce06dce95ce497362749cb75b43e61'),
    _function(0x37ca180, 1328, '6248aa2f59006b37f9af1690691000914210a3482e3a0defef56460c042cc43f'),
    _function(0x37caf10, 788, '846ba936f6b9870853808ed82d482ce0e104aa7ef300c692d79ca7d4837dd07b'),
]}]}

# Docked render size of 13.0.1 (v1769472) main b9b166df…: DM_RENDER_SIZE=2560x1440 (dm/host/hle_render_size.c, which also scales the render pipeline's records through the gates below). Smash
# makes its render targets once, at boot, from sizes spread over its code; each site below is a constant of 1920, 1080, 1920.0 or 1080.0, or a choice made by comparing with 1920. Found by
# MacNativeGames (games/ssbu/runtime/game_patches.c, at 13.0.2's addresses; MIT). Here every word was checked on 13.0.1 by disassembly: from main+0x35b33fc on they sit 0xc80 before 13.0.2's with
# the same words, the two shader constant sequences 0x210 before (the only two places with their words in the text). State page: +0xa0 u32 width, +0xa4 u32 height (read together as
# width | height << 32), +0xa8 / +0xac the two as floats. All 0 (1080p, handheld) leaves the game's own value at every stub. x16 is dead after each site that borrows it, on every path to a write,
# a call or a return (walked through the direct branches), x10 at the viewport divisors, s3 and w11 at the sequences likewise; no direct branch enters a replaced sequence
render_build = 'b9b166df1db90baffbd8027eb0df14d6'
def _render_size(ctx):
    here, emit, state = ctx.here, ctx.emit, ctx.state
    hex_of = lambda word: struct.pack('<I', word).hex()
    ldr_w = lambda rt, rn, off: 0xb9400000 | off // 4 << 10 | rn << 5 | rt
    ldr_x = lambda rt, rn, off: 0xf9400000 | off // 8 << 10 | rn << 5 | rt
    cbz_w = lambda rt, by: 0x34000000 | by // 4 << 5 | rt
    cbz_x = lambda rt, by: 0xb4000000 | by // 4 << 5 | rt
    cbnz_w = lambda rt, by: 0x35000000 | by // 4 << 5 | rt
    mov_w = lambda rd: 0x2a1003e0 | rd   # mov wd, w16
    mov_x = lambda rd: 0xaa1003e0 | rd   # mov xd, x16
    bfi_high = lambda rd: 0xb3607e00 | rd   # bfi xd, x16, #32, #32
    str_sp = lambda off: 0xb90003f0 | off // 4 << 10   # str w16, [sp, #off]
    WIDTH, HEIGHT, FWIDTH, FHEIGHT = 0xa0, 0xa4, 0xa8, 0xac
    def after(off, orig, words, why):   # the original instruction, then words with x16 at the state page
        ctx.site(off, hex_of(orig))
        start = here(); emit(orig); emit(ADRP(16, here(), state))
        for w in words: emit(w)
        emit(B(here(), off + 4))
        ctx.put(off, [B(off, start)], why)
    # Group A: the configuration the render pipeline setup takes its targets from (all twelve 1920x1080 textures made at boot, the three swapchain textures among them)
    after(0x37432dc, 0xf80ff128, [ldr_w(16, 16, WIDTH), cbz_w(16, 12), str_sp(0x354), str_sp(0x358)], 'render target configuration: width and width (the height follows from it)')
    # Groups B and E: width | height << 32, built by a mov and this movk
    for off, orig, rd, why in ((0x3613e58, 0xf2c08708, 8, 'screen size: pipeline setup'), (0x37cab48, 0xf2c08708, 8, 'screen size: the OperationModeChanged handler'),
                               (0x3742be0, 0xf2c08708, 8, "screen size: nuGraphics' first"), (0x35b3400, 0xf2c0870b, 11, 'an object default')):
        after(off, orig, [ldr_x(16, 16, WIDTH), cbz_x(16, 8), mov_x(rd)], why)
    # Groups B and C: the size event's width and height, the window crop, and an object default
    for off, orig, rd, field, why in ((0x3613e88, 0x52808717, 23, HEIGHT, 'size event height: pipeline setup'), (0x37cab78, 0x52808716, 22, HEIGHT, 'size event height: the OperationModeChanged handler'),
                                      (0x37407c4, 0x52808715, 21, HEIGHT, 'screen height'), (0x37407c8, 0x32190ff6, 22, WIDTH, 'screen width'),
                                      (0x3742c54, 0x52a89e09, 9, FWIDTH, 'screen width 1920.0 in four globals'), (0x3742c60, 0x52a890ea, 10, FHEIGHT, 'screen height 1080.0 in four globals'),
                                      (0x3746cdc, 0x32190fe9, 9, WIDTH, 'window crop width'), (0x3746ce8, 0x52808709, 9, HEIGHT, 'window crop height'),
                                      (0x36e4058, 0x32190fe8, 8, WIDTH, 'an object default: width'), (0x36e405c, 0x52808709, 9, HEIGHT, 'an object default: height')):
        after(off, orig, [ldr_w(16, 16, field), cbz_w(16, 8), mov_w(rd)], why)
    for off, orig, rd, why in ((0x3613e8c, 0xf2c0f016, 22, 'size event width: pipeline setup'), (0x37cab7c, 0xf2c0f015, 21, 'size event width: the OperationModeChanged handler')):
        after(off, orig, [ldr_w(16, 16, WIDTH), cbz_w(16, 8), bfi_high(rd)], why)
    # Group E: the UI's size when its byte is set; no register to borrow here (a jump table follows), so each loads into its own destination
    for off, orig, field, why in ((0x378dc28, 0x32190fe8, WIDTH, "the UI's width"), (0x378dc3c, 0x52808708, HEIGHT, "the UI's height")):
        ctx.site(off, hex_of(orig))
        start = here(); emit(ADRP(8, here(), state)); emit(ldr_w(8, 8, field)); emit(cbnz_w(8, 8)); emit(orig); emit(B(here(), off + 4))
        ctx.put(off, [B(off, start)], why)
    # Group D: the screen width turned into an index (1280 -> 1, 1920 -> 2): compared with the host's width
    ctx.site(0x363b944, hex_of(0x711e011f))
    start = here(); emit(ADRP(16, here(), state)); emit(ldr_w(16, 16, WIDTH)); emit(cbz_w(16, 12)); emit(0x6b10011f); emit(B(here(), 0x363b948))   # cmp w8, w16
    emit(0x711e011f); emit(B(here(), 0x363b948))
    ctx.put(0x363b944, [B(0x363b944, start)], 'screen width to index: compared with the host width')
    # Group I: the game's own viewport scale is the screen size over these two divisors (rodata 1920.0, 1080.0), so it stays 1.0. x10 is loaded anew after each
    for off, orig, field, why in ((0x36112e8, 0xbd4d5941, FWIDTH, 'viewport scale divisor: width'), (0x36112f4, 0xbd48ad41, FHEIGHT, 'viewport scale divisor: height')):
        ctx.site(off, hex_of(orig))
        start = here(); emit(orig); emit(ADRP(10, here(), state)); emit(ldr_w(10, 10, field)); emit(cbz_w(10, 8)); emit(0x1e270141); emit(B(here(), off + 4))   # fmov s1, w10
        ctx.put(off, [B(off, start)], why)
    # Group J: the UI's factor for the screen. Each chooses 1.0 when the UI width is 1920 or its byte is set, else 0.6667 (the handheld 1280/1920), which at 2560 drew the title logo's mask over
    # a quarter of the picture. The host's size computes it instead, equal to the shipped value at 1920 and 1280
    def sequence(off, old, gate, new, why):   # old and new are words, or (rd, page) for an adrp; gate: the words that branch to the old ones when no size is set, the last a cbz
        def word(w, at): return ADRP(w[0], at, w[1]) if isinstance(w, tuple) else w
        ctx.site(off, b''.join(struct.pack('<I', word(w, off + 4 * i)) for i, w in enumerate(old)).hex())
        back = off + 4 * len(old); start = here(); emit(ADRP(16, here(), state))
        for w in gate[:-1]: emit(w)
        emit(gate[-1](4 * len(new) + 8))
        for w in new: emit(word(w, here()))
        emit(B(here(), back))
        for w in old: emit(word(w, here()))
        emit(B(here(), back))
        ctx.put(off, [B(off, start)], why)
    sequence(0x378e0a8, [0x1e2e1001, 0x711e017f, 0x1a9f17eb, 0x2a0b014a, 0x7200015f, (10, 0x446f000), 0xbd4ed140],   # fmov s1, #1.0; cmp w11, #1920; cset w11, eq; orr w10, w10, w11; tst w10, #1; adrp x10; ldr s0, [x10, #0xed0]
             [ldr_w(16, 16, WIDTH), lambda by: cbz_w(16, by)],
             [0x1e220160, 0x52a89e0b, 0x1e270161, 0x1e211800, 0x1e2e1001, 0x7200015f],   # scvtf s0, w11; mov w11, #1920.0f; fmov s1, w11; fdiv s0, s0, s1; fmov s1, #1.0; tst w10, #1
             "the UI's factor for its pixel rectangles (scissor and mask): width / 1920")
    for off, why in ((0x1534c08, "the UI's factor in shader constants (first place): width / the host width"), (0x153c138, "the UI's factor in shader constants (second place)")):
        sequence(off, [(11, 0x446f000), 0xbd4ed160, 0x711e015f, (10, 0x4470000), 0xbd4d5944, (10, 0x4470000), 0x1e2e1001, 0xbd48ad45, 0x1a9f17ea],   # 0.6667; cmp w10, #1920; 1920.0 in s4; 1.0 in s1; 1080.0 in s5; cset w10, eq
                 [ldr_w(11, 16, FWIDTH), lambda by: cbz_w(11, by)],
                 [0x1e220140, 0x1e270163, (10, 0x4470000), 0xbd4d5944, 0x1e231800, 0x1e2e1001, 0xbd48ad45, 0x2a1f03ea],   # scvtf s0, w10; fmov s3, w11; adrp x10; ldr s4; fdiv s0, s0, s3; fmov s1, #1.0; ldr s5; mov w10, wzr
                 why)
FRAME_STEP = {render_build: _render_size}
CAPABILITIES = {render_build: ('RENDER_SIZE',)}
# Group G: the other 80 render targets take their size from framebuffer records of the loaded pipeline data, copied by these three. Group H: the passes draw at viewports from records in the same
# data, read where a nu::ViewportPlug is made. The host scales a record before the original load reads it (gate numbers 27 and 28, OBS_RENDER_RECORD and OBS_RENDER_VIEWPORT, the record's register
# in the upper bits; dm/host/hle_render_size.c). No callback attached at 1080p
OBS = {render_build: [
    (0x3821260, 27 | 22 << 8, 'b9400ac9', 'framebuffer record copy: ldr w9, [x22, #8]'),
    (0x3821390, 27 | 19 << 8, 'b9400a68', 'framebuffer record copy: ldr w8, [x19, #8]'),
    (0x3821450, 27 | 19 << 8, 'b9400a69', 'framebuffer record copy: ldr w9, [x19, #8]'),
    (0x3834340, 28 | 22 << 8, 'a940a6c8', 'viewport record read: ldp x8, x9, [x22, #8] (x, y, width, height)'),
]}

# How general each patch is and could be (dm/tools/docs_patches.py "Binding", listed in docs/agents/game-patches.md): what, its binding now, the loosest it could have (goal),
# purpose, why; blocked or kept: why the gap stays open (cannot close yet, or left open on purpose); checked: what was checked on game files (else a judgement)
BINDING = [
    {'what': 'engine region 62 GB → 4 GB (11 cuts in four functions)', 'now': 'game', 'goal': 'game', 'purpose': 'address space',
     'checked': 'MATCH_CUTS enrolled on its builds',
     'why': "function-matched for builds of the same code (MATCH_CUTS); the region and its node pool are the nu:: engine's own"},
    {'what': 'docked render size 2K (DM_RENDER_SIZE; 25 stubs and 4 gates)', 'now': 'build', 'goal': 'game', 'purpose': 'rendering',
     'checked': '13.0.1 by disassembly, word by word, and played in scripted sessions',
     'blocked': 'only 13.0.1 was available to check; 13.0.2 and 13.0.5 hold the same words at other addresses and were not run here',
     'why': "the sizes are the game's own constants, spread over its render setup; function fingerprints could carry them to the builds that keep the code"},
]
