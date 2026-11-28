# The rewriter's engine-wide function patches: fixes of middleware code (Unity, Unreal, ...) that hold for every game shipping that engine code, matched by function
# (dm/tools/nso_match.py) in every game's module of the named kind, whatever the profile and with no build ID. dm/tools/patch_nso.py applies them as it does
# sdk_patches.py's MATCH_CUTS: to a module whose build has no CUTS, with a plain line (no red) where nothing matches, as most games ship other engines. A game's own
# patches.py holds what only that game needs; sdk_patches.py holds Nintendo's SDK. Profiles can also import a shared rule to opt in.

import struct

from loader_census import adrp, got_values, ldr64
from nso_match import MatchError


def check_lets_go_period_table(module, start):
    # Match the relocated table behind the constructor's ADRP/LDR, whose
    # address the fingerprint normalizes across builds.
    page_word, load_word = struct.unpack_from('<II', module.image, start + 0xc0)
    page = adrp(page_word, start + 0xc0)
    load = ldr64(load_word)
    if page is None or load is None or page[0] != load[1]:
        raise MatchError('the frame-period table load does not match')
    table = got_values(module.image).get(page[1] + load[2])
    periods = struct.pack('<QQ', 16_666_667, 33_333_334)
    if table is None or table < module.text_end or module.image[table:table + len(periods)] != periods:
        raise MatchError('the first two frame periods are not 16,666,667 and 33,333,334 ns')


# Both Let's Go profiles import this rule through their MATCH_CUTS. The framework constructor initializes the
# periods that drive presentation and simulation. Their application override
# resets the simulation period every update, so the two cuts apply together.
lets_go_frame_periods = {
    'name': 'one-tick render and simulation periods',
    'functions': [
        {'size': 0x224,
         'fingerprints': ['2ceb7798508227178c3b40a4a1cab7d56793ccedf9727f84185b533edc596b94'],
         'check': check_lets_go_period_table,
         'cuts': [(0xc8, '080540f9', '080140f9', 'initialize both frame periods from the one-tick entry')]},
        {'size': 0x18,
         'fingerprints': ['81673279c08d7094e975f22e4d974b45e6b994714cb3257a3ff11ccb06bba883'],
         'cuts': [(0x10, 'e2030032', '02008052', 'select the one-tick simulation period on every update')]},
    ],
}

lets_go_frame_period_binding = [{
    'what': 'one-tick render and simulation periods',
    'now': 'game', 'goal': 'game', 'purpose': 'frame rate',
    'why': "the profiles choose a faster default and the game's application override sets the simulation period. Both functions and the relocated period table must match",
    'checked': 'Pikachu and Eevee 1.0.0 and 1.0.2 match both functions and the period table offline',
}]


MATCH_CUTS = {'main': [
    # Unity 2018's ThreadedStreamBuffer, the ring buffer every Unity worker thread is fed through (UnityGfxDeviceWorker, BatchDeleteObjects, ...), wakes a sleeping side with a Dekker
    # handshake on two words. The reader that finds nothing to read sets its "reader sleeping" flag (+0x118) with a store-release and loads the write position (+0x80) again before
    # sleeping on its semaphore; a writer stores the write position with a store-release and then checks that flag. The mirror pair: a writer short of space sets "writer sleeping"
    # (+0xd8) and loads the read position (+0x40) again, and the reader stores the read position and checks that flag. The second access of each side is a plain ldr, and AArch64
    # lets a plain load complete before an earlier store-release is visible (store → load order needs a load-acquire or a barrier), so both sides can read the old value and neither
    # wakes the other. The Switch's Cortex-A57 did not show it; on Apple cores it happened within seconds in 3 of 5 Mac runs of FINAL FANTASY CRYSTAL CHRONICLES Remastered Edition:
    # the main thread posts a command to UnityGfxDeviceWorker and waits on that command's semaphore while the worker sleeps on its own with the command in the buffer, every other
    # thread then waits on the main thread and the game never presents a frame. Each of those plain loads becomes a load-acquire (ldar), which a store-release before it orders after
    # the store: 14 of 14 runs booted and kept presenting (the game's note). The cause is in the engine's code, which the rewriter changes deliberately.
    # Checked 2026-11-09: FINAL FANTASY CRYSTAL CHRONICLES Remastered Edition 1.0.3 (Unity 2018.4.2f1, the registered cuts until then; rewritten byte-identical) and 1.0.0 (the
    # same eight sites 0x25380 earlier, which no cut covered), and Raging Loop 1.0.0 (Unity 2018.3.10f1): the same four functions, instruction for instruction; the fourth calls a log
    # helper of another body in the other two builds, hence its three fingerprints.
    # Checked 2026-11-20: Untitled Goose Game 1.1.2 (Unity 2018.4.1f1): the first three functions match at main+0x182ed20, +0x182f310 and +0x182f650. The fourth
    # (main+0x182fa50) calls a different log helper and keeps the stlr w25 to +0xd8 and the ldr of +0x40 at +0x168. Unity 2017.4 (Cuphead, AI: THE SOMNIUM FILES) has other code there, not reviewed.
    # Thronebreaker 1.0.2 (Unity 2018.4.2f1) keeps the same writer-sleep handshake and the plain load at +0x168. Its game note records the ordering test.
    {'name': 'Unity 2018 ThreadedStreamBuffer: load-acquires after its store-releases', 'functions': [
        # the reader going to sleep (FINAL FANTASY CRYSTAL CHRONICLES 1.0.3 main+0x1e67930)
        {'size': 528, 'fingerprints': ['6b48e9a19281a61d041d534127efd9514bcfd37c971463d342ed1ffe3bde0630'], 'cuts': [
            (0x120, '080140b9', '08fddf88', 'stream reader going to sleep: ldr w8,[x8] → ldar (write position after the store-release of "reader sleeping")')]},
        # the reader (main+0x1e67f20)
        {'size': 824, 'fingerprints': ['ee4b0475ad2c62eb78283eaac6b27c094b08fe8542f906fe02ad25c68944204b'], 'cuts': [
            (0x13c, '080140b9', '08fddf88', 'stream reader: ldr w8,[x8] → ldar ("writer sleeping" after the store-release of the read position)'),
            (0x234, '080140b9', '08fddf88', 'stream reader: ldr w8,[x8] → ldar ("writer sleeping" after the store-release of the read position)'),
            (0x2b8, '080140b9', '08fddf88', 'stream reader: ldr w8,[x8] → ldar ("writer sleeping" after the store-release of the read position)')]},
        # the writer (main+0x1e68260)
        {'size': 828, 'fingerprints': ['e2c1c6cb95191a9a2a7694e03f0f6304a9b54058d92795c5a25b3e56d7da0a2e'], 'cuts': [
            (0x148, '080140b9', '08fddf88', 'stream writer: ldr w8,[x8] → ldar ("reader sleeping" after the store-release of the write position)'),
            (0x238, '080140b9', '08fddf88', 'stream writer: ldr w8,[x8] → ldar ("reader sleeping" after the store-release of the write position)'),
            (0x2bc, '080140b9', '08fddf88', 'stream writer: ldr w8,[x8] → ldar ("reader sleeping" after the store-release of the write position)')]},
        # the writer going to sleep (main+0x1e68660; Raging Loop's and FINAL FANTASY CRYSTAL CHRONICLES 1.0.0's call a log helper of another body)
        {'size': 576, 'fingerprints': ['aa8a3dc16b30ea6c510161b632162019a41a95b81a71ccf909d439a08691b1b6', '57a283d159be07dc3178d4158ddecd7c4ebb34e6631a7965111c38c02724b2f2',
                                     'eb41f41ceeffc7e1428740e81625bc7b8aa12aa6823cf1fd4152e50e752e2d4f', '1ef244e58a3747e8662a00de83267f6eb3add6bdc8bb0f93639f08eada3e74bb',
                                     'e1bff2e6d23005e70825edfaa2b79abb67f475c3615a6d364cebc76befd49815'], 'cuts': [
            (0x168, '290140b9', '29fddf88', 'stream writer going to sleep: ldr w9,[x9] → ldar (read position after the store-release of "writer sleeping")')]},
    ]},
]}
