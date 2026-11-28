# Xenoblade Chronicles 3: changes to its own modules, checked by NSO build ID.
# Nintendo SDK module changes belong to dm/tools/sdk_patches.py.
from nso_asm import ADRP, B


CUTS = {
    # 2.2.0 / 2.2.1 share this main build ID (issue #847; see README.md).
    # nninitStartup reserves 60 GB for Monolith's allocator. Its first
    # arena reserves 4 GB + 128 KB, maps the first 128 KB for bookkeeping, and
    # takes additional pages on demand. iOS cannot reserve that address space.
    # The original arena commits 203 MB at the title and 225 MB in the opening
    # battle. A 1 GB first arena inside a 4 GB container leaves too little room
    # for the battle's 390 MB core buffer: its 512 MB virtual window ends
    # 70.75 MB past the container (12429a4, before any physical mapping).
    # Reserve 512 MB for the first arena and 4 GB for the container, changing
    # the bump increment, subtraction and stored capacity together. This is
    # an address-space adaptation; physical limits still follow the Switch
    # pool. Later-game allocation peaks remain to test.
    '82d187fe9ef9be921b8e1b1cddf7bd28': [
        (0x128e58c, 'e001c0d2', '2000c0d2', 'malloc address region: 60 GB to 4 GB'),
        (0x1241870, 'e98340b2', 'e9ff9fd2', 'first arena bump size, low part: mov x9,#0xffff'),
        (0x1241880, '4900a0f2', '4900a4f2', 'first arena bump size: 512 MB + 128 KB + 64 KB - 1'),
        (0x12418d8, 'e97b5fb2', 'e98361b2', 'first arena subtraction, sign extension'),
        (0x12418dc, 'c9ffbff2', 'c9ffbbf2', 'first arena subtraction: -(512 MB + 128 KB)'),
        (0x12418e8, '2800c0f2', '4800a4f2', 'first arena stored capacity: 512 MB + 128 KB'),
    ],
}

# All six cuts exist only to fit an iPhone's 4.375 GB alias region (mem_alias). On the Mac, whose
# alias region is the Switch's 64 GB, the loader restores the original instructions at load so the
# game keeps Monolith's 60 GB layout (docs/reference/memory.md, Cuts that only fit an iPhone's alias region).
ALIAS_CUTS = {
    '82d187fe9ef9be921b8e1b1cddf7bd28': [0x128e58c, 0x1241870, 0x1241880, 0x12418d8, 0x12418dc, 0x12418e8],
}


def render_hook(ctx, site, original, state_reg, body, reason, *, replay_first=False):
    ctx.site(site, original.to_bytes(4, 'little').hex())
    start = ctx.here()
    if replay_first:
        ctx.emit(original)
    ctx.emit(ADRP(state_reg, ctx.here(), ctx.state))
    for word in body:
        ctx.emit(word)
    if not replay_first:
        ctx.emit(original)
    ctx.emit(B(ctx.here(), site + 4))
    ctx.put(site, [B(site, start)], reason)


def clip_axis(units, base_reg, result):
    # s3: selected scale, s16/s17: lower/upper ratios relative to the target,
    # s4: the game's 0.1-pixel bias. Round the lower end up and the upper end
    # down on the 8-pixel grid. If the interval has no grid point, use the first
    # aligned size at or above its lower end (also fixes a single preset size).
    return (
        0x52800009 | units << 5,   # mov w9,#units   unscaled pixels / 8
        0x1e220126,   # scvtf s6,w9
        0x1e2308c6,   # fmul s6,s6,s3
        0x1e3008c5,   # fmul s5,s6,s16        lower end
        0x1e3108c6,   # fmul s6,s6,s17        upper end
        0x1e2900a9,   # fcvtpu w9,s5
        0x1e3100ca,   # fcvtmu w10,s6
        0x531d7129,   # lsl w9,w9,#3
        0x531d714a,   # lsl w10,w10,#3
        0x1e220125,   # scvtf s5,w9
        0x1e220146,   # scvtf s6,w10
        0x1e220007 | base_reg << 5,   # scvtf s7,wBase
        0x1e2428a5,   # fadd s5,s5,s4
        0x1e2428c6,   # fadd s6,s6,s4
        0x1e2718a5,   # fdiv s5,s5,s7
        0x1e2718c6,   # fdiv s6,s6,s7
        0x1e2568c6,   # fmaxnm s6,s6,s5     if the rounded interval is empty
        0x1e256800 | result << 5 | result,   # fmaxnm sResult,sResult,s5
        0x1e267800 | result << 5 | result,   # fminnm sResult,sResult,s6
    )


def render_scale(ctx):
    # Applying the graphics configuration normally copies the checkerboard flag.
    # Its reconstructed path has a 960x544 history layout and a different crop.
    # Use the game's ordinary 1280x720 path when a native scale is selected; keep
    # both the stored preference and the stock behavior when no scale is selected.
    render_hook(ctx, 0xe98eb4, 0x39000128, 10, (
        0xb940114a,   # ldr w10,[x10,#0x10]   render scale; zero = game's own
        0x3400004a,   # cbz w10,original
        0x2a1f03e8,   # mov w8,wzr
    ), 'graphics configuration: use the non-checkerboard path for a selected native render scale')

    # The enable byte belongs to the engine. Substitute its value in the controller
    # and its world-view consumer rather than writing into the controller or INIB
    # settings. The controller's disabled path clears its step and hysteresis.
    for site, original, reason in (
        (0xeb6e40, 0x39400268, 'DRS controller: take the selected dynamic resolution mode'),
        (0xf54b6c, 0x394002e8, 'world view: take the selected dynamic resolution mode'),
    ):
        render_hook(ctx, site, original, 17, (
            0xb9401230,   # ldr w16,[x17,#0x10]   selected render scale
            0x34000050,   # cbz w16,back
            0xb9404228,   # ldr w8,[x17,#0x40]    selected DRS enable
        ), reason, replay_first=True)

    # The engine's two mode branches have their own factors and use different
    # registers for the bounds. Supply target-relative caps after these factors,
    # leaving timing, step selection and hysteresis in the game.
    bounds = (
        0xb9404230,   # ldr w16,[x17,#0x40]   DRS enable
        0x340000f0,   # cbz w16,done
        0xbd401230,   # ldr s16,[x17,#0x10]   scale relative to the 720p base
    )
    render_hook(ctx, 0xeb6ed4, 0x2d448662, 17, bounds + (
        0x2d540e22,   # ldp s2,s3,[x17,#0xa0]  min,max
        0x1e300842,   # fmul s2,s2,s16
        0x1e300863,   # fmul s3,s3,s16
        0x1e204041,   # fmov s1,s2            min Y
        0x1e204060,   # fmov s0,s3            max Y
    ), 'DRS controller: use the selected target-relative limits', replay_first=True)
    render_hook(ctx, 0xeb6f98, 0x1e233844, 17, bounds + (
        0x2d540a23,   # ldp s3,s2,[x17,#0xa0]  min,max
        0x1e300863,   # fmul s3,s3,s16
        0x1e300842,   # fmul s2,s2,s16
        0x1e204060,   # fmov s0,s3            min Y
        0x1e204041,   # fmov s1,s2            max Y
    ), 'DRS controller: use the selected limits after mode scaling')

    # Clip the two calculated world ratios before the game's history/resize
    # check and cap. w22/w21 are backing W/H: require the ordinary 720p layout.
    # The following block uses neither w9/w10 nor s5-s7/s16/s17 before a call.
    render_hook(ctx, 0xf54cc8, 0xbd60a663, 8, (
        0xb9401109,   # ldr w9,[x8,#0x10]    render scale
        0x340005a9,   # cbz w9,original
        0x711402df,   # cmp w22,#1280
        0x54000561,   # b.ne original
        0x710b42bf,   # cmp w21,#720
        0x54000521,   # b.ne original
        0x1e270123,   # fmov s3,w9
        0x2d544510,   # ldp s16,s17,[x8,#0xa0]  min,max; fixed = both 1
        *clip_axis(160, 22, 0),
        *clip_axis(90, 21, 1),
    ), 'world view: apply the aligned render limits before the resize check')
    ctx.render_only = True
    ctx.render_scale = 1
    ctx.capabilities.update(('DYNAMIC_RESOLUTION', 'DRS_BOUNDS'))


FRAME_STEP = {'82d187fe9ef9be921b8e1b1cddf7bd28': render_scale}


# How general each patch is and could be (dm/tools/docs_patches.py "Binding", listed in docs/agents/game-patches.md): what, its binding now, the loosest it could have (goal),
# purpose, why; blocked or kept: why the gap stays open (cannot close yet, or left open on purpose); checked: what was checked on game files (else a judgement)
BINDING = [
    {'what': 'malloc region 60 GB → 4 GB and its first arena', 'now': 'build', 'goal': 'game', 'purpose': 'address space',
     'why': "Monolith's own allocator; its constants span two functions (a multi-function rule). The cause, an iPhone's address space, is a platform limit"},
    {'what': 'native world render scale and DRS limits', 'now': 'build', 'goal': 'game', 'purpose': 'rendering',
     'why': "Monolith's world-view layout and dynamic resolution controller are specific to this build",
     'checked': '2.2.1 original main: exact words and the surrounding register use checked'},
]
