# Pikmin 4: patches to the game's main module, keyed by NSO build ID.
from nso_asm import B, LDRX


def sequence_fade_rate(ctx):
    # The fade duration is in seconds. The end and current positions are in
    # sequence frames, so use the playback position's input FFrameRate.
    # Enter before the two instructions that prepare the normal sequence end.
    # The alternate path joins at the second instruction and keeps its clock.
    site = 0x1ca4590
    ctx.site(site, '294c221e150140b9c81804f0084144f901a947bd0008211e1600381e')
    start = ctx.here()
    ctx.emit(0x1e224c29)  # fcsel s9,s1,s2,mi: original end subframe
    ctx.emit(0xb9400115)  # ldr w21,[x8]: original end frame
    ctx.emit(LDRX(8, 19, 0x38 // 8))  # the sequence player tested above
    for word in (
        0xb9458109,  # ldr w9,[x8,#0x580]: numerator
        0xb9458508,  # ldr w8,[x8,#0x584]: denominator
        0x1e220121,  # scvtf s1,w9
        0x1e220102,  # scvtf s2,w8
        0x1e221821,  # fdiv s1,s1,s2
        0x1e210800,  # fmul s0,s0,s1: fade duration in sequence frames
    ):
        ctx.emit(word)
    ctx.emit(B(ctx.here(), site + 0x18))  # original truncation and comparison
    ctx.put(site, [B(site, start)], 'cutscene fade scheduling uses the sequence frame rate')


STUBS = {
    '060f827bd2b38703e9ea11e3210d89bc': sequence_fade_rate,
}

BINDING = [
    {'what': 'cutscene fade scheduling uses the sequence frame rate', 'now': 'build', 'goal': 'game', 'purpose': 'frame rate',
     'why': "the title's fade scheduler compares sequence-frame positions after converting seconds with the engine frame rate",
     'blocked': 'the scheduler has no unwind entry in 1.1.1, which the function matcher needs',
     'checked': '1.1.1 disassembly and post-Porquillon runtime trace of the fade trigger, sequence rate and opacity'},
]
