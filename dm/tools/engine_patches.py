# The rewriter's engine-wide function patches: fixes of middleware code (Unity, Unreal, ...) that hold for every game shipping that engine code, matched by function
# (dm/tools/nso_match.py) in every game's module of the named kind, whatever the profile and with no build ID. dm/tools/patch_nso.py applies them as it does
# sdk_patches.py's MATCH_CUTS: to a module whose build has no CUTS, with a plain line (no red) where nothing matches, as most games ship other engines. A game's own
# patches.py holds what only that game needs; sdk_patches.py holds Nintendo's SDK. Each rule names the engine versions it was checked on.

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
    # helper of another body in the other two builds, hence its three fingerprints. Unity 2017.4 (Cuphead,
    # AI: THE SOMNIUM FILES) has other code there, not reviewed.
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
                                     'eb41f41ceeffc7e1428740e81625bc7b8aa12aa6823cf1fd4152e50e752e2d4f'], 'cuts': [
            (0x168, '290140b9', '29fddf88', 'stream writer going to sleep: ldr w9,[x9] → ldar (read position after the store-release of "writer sleeping")')]},
    ]},
]}
