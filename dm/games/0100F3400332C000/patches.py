"""Native world render controls for the verified XC2 3.0.0 main NSO."""

from nso_asm import ADRP, B, BEQ, CBNZW


def cbz(reg, pc, target):
    return CBNZW(reg, pc, target) ^ 0x01000000


def hook(ctx, site, original, reason, body, *, replay_first=False):
    """Check the rewriter site and replay its position-independent instruction."""
    ctx.site(site, original)
    start = ctx.here()
    word = int.from_bytes(bytes.fromhex(original), 'little')
    if replay_first:
        ctx.emit(word)
    for insn in body(ctx.here()):
        ctx.emit(insn)
    if not replay_first:
        ctx.emit(word)
    ctx.emit(B(ctx.here(), site + 4))
    ctx.put(site, [B(site, start)], reason)


def world_guard(ctx, pc, back):
    # This path sizes the world; act only on its ordinary 1280x720 backing.
    # The zero scale check comes first, so the stock branch changes no flags.
    return (
        ADRP(17, pc, ctx.state),
        0xb9401229,  # ldr w9, [x17,#0x10]   selected scale bits, zero = game's own
        cbz(9, pc + 8, back),
        0x711402bf,  # cmp w21,#1280         backing width
        BEQ(pc + 16, back) | 1,  # b.ne
        0x710b429f,  # cmp w20,#720          backing height
        BEQ(pc + 24, back) | 1,  # b.ne
    )


def world_route(ctx, pc):
    # x19 is the view and x22 the controller. The original load into w8
    # precedes the guard so the full-size path can use that flag pair.
    # w9, w10 and x17 are scratch; the original route forms its own comparisons.
    choose = pc + 7 * 4
    return (
        *world_guard(ctx, pc, choose + 10 * 4),
        0xb940422a,  # ldr w10,[x17,#0x40]   dynamic resolution switch
        cbz(10, choose + 4, choose + 12),
        B(choose + 8, 0x7a14b0),  # read the controller's live output (+0x20/+0x24)
        0x52a7f00a,  # mov w10,#0x3f800000
        0x6b0a013f,  # cmp w9,w10
        BEQ(choose + 20, choose + 28) | 1,
        B(choose + 24, 0x7a14b8),  # reset native reduced-size bookkeeping
        0x1e270122,  # fmov s2,w9           fixed X ratio
        0x1e204041,  # fmov s1,s2           fixed Y ratio
        B(choose + 36, 0x7a1508),  # common aligned world-size path
    )


def controller_bounds(ctx, pc):
    # Both console-mode paths now have their max X/Y in s2/s0 and min
    # X/Y in s3/s1. Leave the governor, stage, timers and hysteresis alone.
    # x8 and s4-s6 are live; w9, w10, x17 and s16 are scratch here.
    return (
        ADRP(17, pc, ctx.state),
        0xb9401229,  # ldr w9,[x17,#0x10]
        cbz(9, pc + 8, pc + 44),
        0xb940422a,  # ldr w10,[x17,#0x40]
        cbz(10, pc + 16, pc + 44),
        0x1e270130,  # fmov s16,w9
        0x2d540a23,  # ldp s3,s2,[x17,#0xa0]
        0x1e300863,  # fmul s3,s3,s16
        0x1e300842,  # fmul s2,s2,s16
        0x1e204061,  # fmov s1,s3
        0x1e204040,  # fmov s0,s2
    )


def limit_axis(base, base_reg, ratio):
    # s18: scale; s16/s17: bounds relative to that scale; s5: stock 0.1px
    # bias. Align the lower end up and the upper down in units of eight.
    # An empty interval uses the first grid point at or above the lower end.
    return (
        0x52800009 | (base // 8) << 5,  # mov w9,#(base / 8)
        0x1e220126,  # scvtf s6,w9
        0x1e3208c6,  # fmul s6,s6,s18
        0x1e3108c7,  # fmul s7,s6,s17
        0x1e3008c6,  # fmul s6,s6,s16
        0x1e2900c9,  # fcvtpu w9,s6
        0x1e3100ea,  # fcvtmu w10,s7
        0x531d7129,  # lsl w9,w9,#3
        0x531d714a,  # lsl w10,w10,#3
        0x1e220126,  # scvtf s6,w9
        0x1e220147,  # scvtf s7,w10
        0x1e220004 | base_reg << 5,  # scvtf s4,wBase
        0x1e2528c6,  # fadd s6,s6,s5
        0x1e2528e7,  # fadd s7,s7,s5
        0x1e2418c6,  # fdiv s6,s6,s4
        0x1e2418e7,  # fdiv s7,s7,s4
        0x1e2668e7,  # fmaxnm s7,s7,s6
        0x1e266800 | ratio << 5 | ratio,  # fmaxnm sRatio,sRatio,s6
        0x1e277800 | ratio << 5 | ratio,  # fminnm sRatio,sRatio,s7
    )


def world_limits(ctx, pc):
    # At the native resize check, s2/s3 are the aligned ratios, s0/s1 are
    # 100/0.01, and s5 is the pixel bias. s4 is about to be reloaded, and
    # s6/s7/s16-s18 are dead. The host sets both bounds to 1 for fixed size.
    selected = (
        0x1e270132,  # fmov s18,w9
        0x2d544630,  # ldp s16,s17,[x17,#0xa0]
        *limit_axis(1280, 21, 2),
        *limit_axis(720, 20, 3),
        # With the stock 0.01 threshold an 8px width-only change can be
        # cached but never applied. 0.005 detects any step on this grid.
        0x1e2c1004,  # fmov s4,#0.5
        0x1e240821,  # fmul s1,s1,s4
    )
    return (*world_guard(ctx, pc, pc + 4 * (7 + len(selected))), *selected)


def render_scale(ctx):
    hook(ctx, 0x7a1484, '68665f79', 'World view: select fixed or native dynamic render size',
         lambda pc: world_route(ctx, pc), replay_first=True)
    for site, original in ((0x7e3510, '8408261e'), (0x7e35ac, '05ad43bd')):
        hook(ctx, site, original, 'DRS controller: apply target-relative bounds after mode scaling',
             lambda pc: controller_bounds(ctx, pc))
    hook(ctx, 0x7a1588, '64c24fbd', 'World view: enforce aligned pixel limits at the native resize check',
         lambda pc: world_limits(ctx, pc))
    ctx.render_only = True
    ctx.render_scale = 1
    ctx.capabilities.update(('DYNAMIC_RESOLUTION', 'DRS_BOUNDS'))


FRAME_STEP = {'feee7afa8a1541bd67aed1a0061fbb57': render_scale}


BINDING = [
    {'what': 'XC2 world render scale and DRS limits', 'now': 'build', 'goal': 'game', 'purpose': 'rendering',
     'why': "the route, controller and resize checks use Monolith's own world-view layout at this main build's addresses",
     'checked': '3.0.0 original words checked at all four sites; opening battle measured in all three modes on Mac and iPhone 15 Pro'},
]
