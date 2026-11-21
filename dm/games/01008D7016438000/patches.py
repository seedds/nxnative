# 13 Sentinels: Aegis Rim, Asian release (01008D7016438000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a
# module's NSO build ID; CUTS as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
CUTS = {
  # 1.00 (v0) main: the main thread's wait for the render thread at the end of a frame (main+0x30c194). While the render thread is busy (main+0x30909c) it sleeps 1 ms at a time
  # (main+0x2c7c2c), but only for what is left of the frame's time ([x19+0x300] + interval − now, in µs): with nothing left (b.ls), under 1 ms left (b.lo) or the count of sleeps used up
  # (b.ne falls through) it goes on to the next frame without the render thread (w25 = 1, a skipped frame). On the host the render thread's first submission waits for the pipeline
  # archive to open and for the prepare step (nvn_mtl.m warm_start): 0.1 s from an internal disk, 7 s and more from a USB 2.0 disk (a 131 MB archive in a library folder). The main
  # thread then skips every frame meanwhile and runs on (it reads the next screens' files from the same disk, which makes the wait longer still), and the render thread's first draws
  # bind a texture entry of index −1: SIGSEGV at main+0x309128 (x0 = entries + 0xffffffff × 0xe8), in 13 of 13 starts whose wait was 7 s or longer and in none of 8 below 6.5 s. What
  # the main thread did to that texture meanwhile was not traced. With the three branches cut the main thread waits until the render thread is done, however long: 9 of 9 starts ran
  # (1200 to 1800 frames); their waits were 2 to 3.5 s, since the main thread no longer reads from the disk meanwhile, so no start with the cut had a wait of 7 s
  '311947020f64f5742ce3c16162c4d87f': [
    (0x30c27c, '29020054', '1f2003d5', 'frame end, no frame time left: b.ls main+0x30c2c0 (skip the frame) → nop (wait for the render thread)'),
    (0x30c284, 'e3010054', '1f2003d5', 'frame end, under 1 ms left: b.lo main+0x30c2c0 (skip the frame) → nop (wait for the render thread)'),
    (0x30c2bc, '41ffff54', 'faffff17', 'frame end, the wait loop: b.ne main+0x30c2a4 → b main+0x30c2a4 (no end by count: only the render thread being done ends it)'),
  ],
}

# How general each patch is and could be (dm/tools/docs_patches.py "Binding", listed in docs/agents/game-patches.md)
BINDING = [
    {'what': 'frame end: the main thread waits for the render thread instead of skipping the frame', 'now': 'build', 'goal': 'game', 'purpose': 'race',
     'why': "Vanillaware's frame loop; the western release (01003FC01670C000) runs the same engine, and a function-matched rule could carry the cut to it and over updates",
     'blocked': 'the western build was not at hand to check the function there'},
]
