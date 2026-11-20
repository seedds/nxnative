# Kirby and the Forgotten Land (01004D300C5AE000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a
# module's NSO build ID; CUTS as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
CUTS = {
  # 1.1.0 (v65536) main: 60 fps. The frame pacer (the function at main+0x52fa50) picks the vsyncs a frame waits from the mode at [x19+0x60]: 0 → [x19+0x64]*[x19+0x10],
  # 1 → [x19+0x64]*[x19+0x24], 2 → [x19+0x64]*[x19+0x3c] (2 in play, 30 fps); it then waits until that many vsyncs (16,666,666 ns each) have passed since the last frame.
  # The game measures each frame's length in vsyncs (at least 1) and steps its time by it, so a frame of one vsync runs at 60 fps at the same speed.
  # The cuts take mode 2's path whatever the mode and wait one vsync there, as theboy181's 60 fps patch for 1.0.0 does (main D9BA7DB7: 0x5330D8 and 0x533110; carried to this
  # build by the block's shape and checked by disassembly: same compares, loads and multiply, 1.1.0 adds a 10 µs sleep register w28 to the wait loop)
  'a6ce40dc3aedb1becacd2f1ff4515307': [
    (0x533cd8, 'a0010054', '0d000014', '60 fps: b.eq main+0x533d0c → b (the pacer takes mode 2 whatever its mode)'),
    (0x533d18, '367d081b', '36008052', '60 fps: mul w22, w9, w8 → mov w22, #1 (mode 2 waits one vsync a frame)'),
    # Distant animations: the branch past a distance test ([x26+0x7c] squared against s3) is taken always, so distant animations run at 60 fps too, as the patch describes it (theboy181's "Full" variant, 1.0.0 0x7F0D1C; the function is an exact match across the builds, main+0x7f0a30 → main+0x7f5bc0)
    (0x7f5eac, '25020054', '11000014', '60 fps: b.pl main+0x7f5ef0 → b (distant actors animate every frame)'),
  ],
}

# How general each patch is and could be (dm/tools/docs_patches.py "Binding", listed in docs/agents/game-patches.md): what, its binding now, the loosest it could have (goal),
# purpose, why; blocked or kept: why the gap stays open (cannot close yet, or left open on purpose); checked: what was checked on game files (else a judgement)
BINDING = [
    {'what': "frame pacer: one vsync a frame (60 fps), distant actors animate every frame", 'now': 'build', 'goal': 'game', 'purpose': 'frame rate',
     'why': "the game's own pacer and its actors' update distance; a function-matched rule could carry them over the game's updates",
     'checked': '1.0.0 and 1.1.0 main disassembled: the same pacer block (three of that shape in each, the third patched) and the same actor function'},
]
