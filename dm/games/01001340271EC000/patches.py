# Hello Kitty Party Land (01001340271EC000): patches of the game's own builds (dm/tools/patch_nso.py "Registered patches"; README.md, Frame rate)
CUTS = {
  # 1.0.2 (v131072) main: run the game's own 60 fps mode everywhere.
  # The game's frame-rate class (statics at [[main+0xcdba088]+0xb8], an int at +0 = 1 in 60 fps mode, 0 in 30) has SetMode(int) at main+0x863b2a0:
  # 1 → QualitySettings.set_vSyncCount(1) and the field 1; 0 → set_vSyncCount(2) and the field 0; 2 → as 1 and one more call (main+0x863b580).
  # Each gameplay section calls SetMode(0) when it starts and SetMode(1) when it returns to the menus (nine pairs, main+0x35e5a3c / +0x35e5ca4 and so on), so on the Switch the menus run at
  # 60 fps and gameplay at 30; main+0x863b4a0 is a 30 fps setter of the same shape. The cuts make the 30 fps paths the 60 fps ones: vSyncCount 1 and the field 1 (str w0, the 1 just
  # loaded), so the field stays consistent with the rate. The minigames step by elapsed time: played at 60 they keep their speed (README.md, Frame rate)
  'ca82f3aba950e98060aa8b6aa8f24217': [
    (0x863b318, '40008052', '20008052', 'frame rate SetMode(0): vSyncCount mov w0,#2 → mov w0,#1 (the game\'s own 60 fps mode)'),
    (0x863b31c, '1f0100b9', '000100b9', 'frame rate SetMode(0): 60 fps field str wzr → str w0 (1, as SetMode(1))'),
    (0x863b4ec, '40008052', '20008052', '30 fps setter: vSyncCount mov w0,#2 → mov w0,#1'),
    (0x863b4f8, '1f0100b9', '000100b9', '30 fps setter: 60 fps field str wzr → str w0 (1)'),
  ],
}

# How general each patch is and could be (dm/tools/docs_patches.py "Binding", listed in docs/agents/game-patches.md): what, its binding now, the loosest it could have (goal),
# purpose, why; blocked or kept: why the gap stays open (cannot close yet, or left open on purpose); checked: what was checked on game files (else a judgement)
BINDING = [
    {'what': "the game's 30 fps mode → its 60 fps mode (vSyncCount 1 and the mode field)", 'now': 'build', 'goal': 'game', 'purpose': 'frame rate',
     'why': "the game's own frame-rate class; a function-matched rule could carry it over the game's updates",
     'checked': 'about an hour on the Mac and 30 minutes on an iPhone 15 Pro Max at 60 fps, the minigames at their intended speed, 2026-11-01'},
]
