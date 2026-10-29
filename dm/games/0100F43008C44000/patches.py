# Pokémon Legends: Z-A 2.0.2: native timing mode = vsyncs - 1 (0 = 60 fps, 1 = 30).
# Fl4sh9174's 2.0.2 FPS mod identifies the mode load at main+0x1b296d8:
# https://gamebanana.com/mods/627350
# Use the host's whole-vsync step instead of its emulator clock and code caves.
# The game's unchanged table supplies 16.667 / 33.333 / 50 ms to its callbacks.
# shortcut: rolling, ladders and battles at 60 fps remain unverified; validate before wider use.
from nso_asm import ADRP, B, STP_PRE, LDP_POST


def frame_step(ctx):
    ctx.site(0x3393e20, '2b50fe000000000056a0fc010000000080f0fa0200000000')
    for site, original, count in ((0x1b296d8, '3481c039', False), (0x1b29738, '48300290', True)):
        ctx.site(site, original)
        start = ctx.here()
        words = [
            STP_PRE(16, 17, 31, -16),
            ADRP(16, start + 4, ctx.state),
            0xb9400211,  # ldr w17,[x16]: target fps (float bits)
            0,           # cbz w17,original: target 0 leaves the game unchanged
            0xb9402214,  # ldr w20,[x16,#0x20]: host's step in vsyncs
            0x350000b4,  # cbnz w20,chosen
            0x52a83e14,  # mov w20,#0x41f00000: 30.0f, before the first present
            0x6b14023f,  # cmp w17,w20
            0x1a9f17f4,  # cset w20,eq
            0x11000694,  # add w20,w20,#1: 2 at 30 fps, otherwise 1
        ]
        if count:
            words += [0xf9401a11, 0x8b140231, 0xf9001a11]  # count vsyncs once per update (state +0x30)
        words += [0x51000694, LDP_POST(16, 17, 31, 16)]  # sub w20,w20,#1; restore scratch registers
        if count: words.append(ADRP(8, start + 4 * len(words), 0x6131000))  # replay the displaced address load
        words.append(B(start + 4 * len(words), site + 4))
        words[3] = 0x34000011 | ((len(words) - 3) << 5)
        words.append(LDP_POST(16, 17, 31, 16))
        words.append(ADRP(8, start + 4 * len(words), 0x6131000) if count else 0x39c08134)
        words.append(B(start + 4 * len(words), site + 4))
        for w in words: ctx.emit(w)
        # Set the presentation mode first, then the callback step after the game's late-frame adjustment.
        # Both sites overwrite NZCV before consuming flags, and leave every register except w20 unchanged.
        ctx.put(site, [B(site, start)], 'native timing mode from host vsyncs (target 0 = original)' if count else 'native present mode from host vsyncs (target 0 = original)')


FRAME_STEP = {'b1f12fd919eae86ab8a978317677e64b': frame_step}
GENERALIZE = "No: Trinity's timing-mode load and callback dispatch are game-build specific; pacing uses the shared host frame step"
