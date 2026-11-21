# Nintendo Switch Sports (0100D2F00D5C0000), provisional profile: the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py
# (DM_GAMES_DIR included) and applies those of a module's NSO build ID; CUTS as described there ("Registered patches").
CUTS = {
  # 1.5.0 (v589824) main: nn::font::TextureCache::Initialize (main+0x238b74) takes the glyph cache size from its InitializeArg, width at +0 (0x400) and height at +4 (0x8000).
  # A 1024x32768 R8 linear texture exceeds Metal's 16384 limit in 2D, so the host refused it and every text of the game (buttons, dialogs) drew nothing. The cache's own height
  # is the one value its glyph placement and UVs use ([this+0x2c]), so it is read as 0x4000 here: half the rows for glyphs, all on a texture Metal can create
  '2f6883e890a578ce341bf36530f0883b': [
    (0x238e5c, 'c80640b9', '08008852', 'font glyph cache height: ldr w8, [x22, #4] -> mov w8, #0x4000 (Metal 2D limit 16384)'),
  ],
}

BINDING = [
    {'what': 'font glyph cache height clamped to 16384', 'now': 'build', 'goal': 'game', 'purpose': 'rendering',
     'why': "Metal's 2D texture limit; a host-side rule could clamp any TextureCache InitializeArg height instead"},
]
