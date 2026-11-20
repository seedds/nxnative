# Kirby and the Forgotten Land (01004D300C5AE000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a
# module's NSO build ID; CUTS as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
CUTS = {
  # 1.1.0 (v65536) main: 60 and 120 fps. The frame pacer (the function at main+0x52fa50, its object at [[main+0xa65f58]+8]) picks the units a frame waits from the mode at
  # [x19+0x60]: 0 → [x19+0x64]*[x19+0x10], 1 → [x19+0x64]*[x19+0x24], 2 → [x19+0x64]*[x19+0x3c] (2 in play: 30 fps on the Switch, [+0x3c] = 2); it waits until that many
  # units (16,666,666 ns, a vsync) have passed since the last frame, measures the frame's length in units (at least 1), and the game steps by their average, the float at
  # [x19+0x6c] that its 17 readers take as the frame step in 1/60 s. So the game keeps its speed at any rate.
  # The cuts take mode 2's path whatever the mode (theboy181's 60 fps patch for 1.0.0, main D9BA7DB7 0x5330D8, carried to this build by the block's shape and checked by
  # disassembly), make the unit 1/120 s (8,333,333 ns, in each of the pacer's four waits and measures), and halve the measured units where they become the float step, so the
  # step stays in 1/60 s: 0.5 a frame at 120 fps. Then [+0x3c] counts 1/120 s: the host writes 1 for 120 or 2 for 60 (hle_fpslock.c, fps_target; the game had 1 there at boot)
  'a6ce40dc3aedb1becacd2f1ff4515307': [
    (0x533cd8, 'a0010054', '0d000014', 'pacer: b.eq main+0x533d0c → b (mode 2 whatever the mode)'),
    (0x532e68, '49058a52', 'a9028552', 'pacer unit 1/60 → 1/120 s: mov w9, #0x502a → #0x2815'),
    (0x532e6c, 'c91fa072', 'e90fa072', 'pacer unit 1/60 → 1/120 s: movk w9, #0xfe → #0x7f, lsl #16 (8,333,333 ns)'),
    (0x532e74, '28058a92', '88028592', 'pacer unit 1/60 → 1/120 s, negated: mov x8, #-0x502a → #-0x2815'),
    (0x532e78, '28e0bff2', '08f0bff2', 'pacer unit 1/60 → 1/120 s, negated: movk x8, #0xff01 → #0xff80, lsl #16'),
    (0x532ec0, '49058a52', 'a9028552', 'pacer unit 1/60 → 1/120 s: mov w9, #0x502a → #0x2815'),
    (0x532ec4, 'c91fa072', 'e90fa072', 'pacer unit 1/60 → 1/120 s: movk w9, #0xfe → #0x7f, lsl #16 (8,333,333 ns)'),
    (0x532ecc, '28058a92', '88028592', 'pacer unit 1/60 → 1/120 s, negated: mov x8, #-0x502a → #-0x2815'),
    (0x532ed0, '28e0bff2', '08f0bff2', 'pacer unit 1/60 → 1/120 s, negated: movk x8, #0xff01 → #0xff80, lsl #16'),
    (0x532fd8, '49058a52', 'a9028552', 'pacer unit 1/60 → 1/120 s: mov w9, #0x502a → #0x2815'),
    (0x532fdc, 'c91fa072', 'e90fa072', 'pacer unit 1/60 → 1/120 s: movk w9, #0xfe → #0x7f, lsl #16 (8,333,333 ns)'),
    (0x532fe4, '28058a92', '88028592', 'pacer unit 1/60 → 1/120 s, negated: mov x8, #-0x502a → #-0x2815'),
    (0x532fe8, '28e0bff2', '08f0bff2', 'pacer unit 1/60 → 1/120 s, negated: movk x8, #0xff01 → #0xff80, lsl #16'),
    (0x533d50, '49058a52', 'a9028552', 'pacer unit 1/60 → 1/120 s: mov w9, #0x502a → #0x2815'),
    (0x533d54, 'c91fa072', 'e90fa072', 'pacer unit 1/60 → 1/120 s: movk w9, #0xfe → #0x7f, lsl #16 (8,333,333 ns)'),
    (0x533d5c, '28058a92', '88028592', 'pacer unit 1/60 → 1/120 s, negated: mov x8, #-0x502a → #-0x2815'),
    (0x533d60, '28e0bff2', '08f0bff2', 'pacer unit 1/60 → 1/120 s, negated: movk x8, #0xff01 → #0xff80, lsl #16'),
    (0x532f34, '0001221e', '00fd021e', 'pacer: scvtf s0, w8 → scvtf s0, w8, #1 (the measured 1/120 s units halved into the step in 1/60 s)'),
    # The pacer's own wait goes: the host's display link paces the frames (the frame step's hold), and the game only measures them. The wait (a loop of 10 µs sleeps until
    # [+0x64]*[+0x3c] units have passed on the game's grid, anchored at boot) and the display link are two clocks that drift apart: while the grid's ticks fell just after
    # the link's, each frame waited for both, two ticks of 1/120 s, so a 4 ms frame came late at 120 fps and the host held 60 for minutes (the hub, standing still), then 120
    # again once the phase moved on. The measure counts the grid's ticks crossed since the last frame, exact over time; a frame that crosses none (its start near a grid tick)
    # steps 0 and the next 2, where the clamp to 1 had made it 1 and 2: the step the game takes is the mean of the last 4 ([+0x44] = 4), 1 again
    (0x533dc0, '8a000054', '04000014', "pacer's own wait: b.ge main+0x533dd0 → b (the host's display link paces the frames)"),
    (0x532f30, '08c59f1a', '1f2003d5', 'pacer: csinc w8, w8, wzr, gt → nop (a frame that crosses no grid tick steps 0, not 1: the mean of 4 stays exact)'),
    # Distant animations: the branch past a distance test ([x26+0x7c] squared against s3) is taken always, so distant animations run every frame, as theboy181's "Full" variant
    # describes it (1.0.0 0x7F0D1C; the function is an exact match across the builds, main+0x7f0a30 → main+0x7f5bc0)
    (0x7f5eac, '25020054', '11000014', 'distant animations: b.pl main+0x7f5ef0 → b (they animate every frame)'),
  ],
}

# How general each patch is and could be (dm/tools/docs_patches.py "Binding", listed in docs/agents/game-patches.md): what, its binding now, the loosest it could have (goal),
# purpose, why; blocked or kept: why the gap stays open (cannot close yet, or left open on purpose); checked: what was checked on game files (else a judgement)
BINDING = [
    {'what': "frame pacer in 1/120 s units (60 and 120 fps), distant animations every frame", 'now': 'build', 'goal': 'game', 'purpose': 'frame rate',
     'why': "the game's own pacer and its distant-animation test; a function-matched rule could carry them over the game's updates",
     'checked': '1.0.0 and 1.1.0 main disassembled: the same pacer block (three of that shape in each, the third patched), its four unit constants, and the same animation function'},
]
