from nso_asm import ADDI, ADRP, B, BL, LDP_POST, LDRX, STP_PRE


def enable_60_fps(ctx, logo_clock, transition_offset):
    startup = ctx.others['startup']
    logo = ctx.others['boot logo']
    stage_update = ctx.others['stage update']
    setter = ctx.others['rate setter']
    if None in (startup, logo, stage_update, setter):
        return

    # The splash scene rounds each elapsed half tick to zero at 60 fps. Keep half ticks
    # in its counter and scale both the logo duration and the skip delay to match.
    clock = logo + logo_clock
    cuts = [
        (startup + 0x108, '810e40b9', 0x52800781, 'Pass 60 fps to the startup rate setter'),
        (clock, '0a00381e', 0x1e18fc0a, 'Accumulate logo time in half ticks'),
        (clock + 0x10, '5f01096b', 0x6b09055f, 'Compare the logo duration in half ticks'),
        (clock + 0x44, '1f01096b', 0x6b09051f, 'Compare the logo skip delay in half ticks'),
    ]
    enter = ctx.base
    leave = stage_update + transition_offset
    ctx.site(enter, 'fe0f1ef8')  # str x30, [sp, #-0x20]!
    ctx.site(leave, 'e0030091')  # mov x0, sp, after submitting confirm or cancel
    for off, old, _, _ in cuts:
        ctx.site(off, old)

    # The shipped Scaleform stage selector can ignore every button with a 60 Hz update.
    # Use the original 30 fps mode for that screen, then restore 60 on confirm or cancel.
    app = ctx.address(startup + 0x20)

    def set_rate(fps):
        ctx.emit(STP_PRE(0, 30, 31, -16))
        ctx.emit(ADRP(0, ctx.here(), app))
        ctx.emit(LDRX(0, 0, (app & 0xfff) // 8))
        ctx.emit(LDRX(0, 0))
        ctx.emit(0x52800001 | (fps << 5))  # mov w1, #fps
        ctx.emit(BL(ctx.here(), setter))
        ctx.emit(LDP_POST(0, 30, 31, 16))

    entry = ctx.here()
    set_rate(30)
    ctx.emit(0xf81e0ffe)  # original entry stack adjustment and LR spill
    ctx.emit(B(ctx.here(), enter + 4))

    resume = ctx.here()
    set_rate(60)
    ctx.emit(ADDI(0, 31, 0))  # original mov x0, sp
    ctx.emit(0xd65f03c0)  # ret to the stage update

    for off, _, word, why in cuts:
        ctx.put(off, [word], why)
    ctx.put(enter, [B(enter, entry)], 'Use 30 fps while selecting a battle stage')
    ctx.put(leave, [BL(leave, resume)], 'Restore 60 fps after confirming or cancelling stage selection')


rate = 'Storm Connections native 60 fps, logo clock and stage selection'
rate_setter = {'size': 0x48, 'fingerprints': ['5293869774ef36c9264d85b8261a63a485f126c54852ffebbdea9a57e9eb2297']}
MATCH_STUBS = {'main': [
    # Base game (1.0.0)
    {'name': rate, 'size': 0x84, 'fingerprints': ['9360b4273612d020f51a17c8d83586f56e3a7019f5458502319fc286de67ab82'],
     'stub': lambda ctx: enable_60_fps(ctx, 0x144, 0xd8),
     'others': {
         'startup': {'size': 0x6b0, 'fingerprints': ['61cf6b2894ec2b9534e760da03618c5791d6af5dce9eb1b7c6d2ff3705cc49b5']},
         'boot logo': {'size': 0x26c, 'fingerprints': ['5da3e438c2f6d903435b26b7dca209cc36db10db9c1732afdce5a23443affc75']},
         'stage update': {'size': 0xf0, 'fingerprints': ['d87669931f99aa387792cdb941231fb12aa5c77d7aca9267bd66e0edc09f01e4']},
         'rate setter': rate_setter,
     }},
    # Update 1.7.1 (v720896)
    {'name': rate, 'size': 0x88, 'fingerprints': ['7841b114eb4f8a1402ece46880babedcc5482e19d2ede52f7df1686baca0b43a'],
     'stub': lambda ctx: enable_60_fps(ctx, 0x164, 0x120),
     'others': {
         'startup': {'size': 0x6e8, 'fingerprints': ['66578e3a3d7668cfed8b6b70be56c42c84de80b6e9f5b89729f48d1b14589795']},
         'boot logo': {'size': 0x2a0, 'fingerprints': ['30ce5bb479803251ce6ac0c5b70830915fcbbc6fe5f49e8506becdef2a37ea60']},
         'stage update': {'size': 0x138, 'fingerprints': ['41d7f9f82eac05025a8f670f9afaa604ebc4c61c2d74b98bb264a84d5b8e7755']},
         'rate setter': rate_setter,
     }},
]}

BINDING = [
    {'what': rate, 'now': 'game', 'goal': 'game', 'purpose': 'frame rate',
     'checked': 'startup setter, logo clock and stage selection lifecycle disassembled on 1.0.0 and 1.7.1',
     'why': 'the title selects its own rate and owns its logo clock and stage selector. The host already honors the one-refresh present interval.'},
]
