# Super Mario Odyssey 1.4.1 (v393216) main, docked render size 1920x1080 (README "Higher render resolutions"). Not loaded by default: copy it over a
# patches.py in this folder (or in a DM_GAMES_DIR profile) to use it. The default 1600x900 memory layout of game.tsv is enough.
CUTS = {
  'b424be150a8e7d78701cbe7a439d9ebf': [
    (0x4bf2a4, '09c880d2', '09f080d2', 'framework window size 1600 -> 1920 (mov x9)'),
    (0x4bf2a8, '8970c0f2', '0987c0f2', 'framework window size 900 -> 1080 (movk x9, lsl 32)'),
    (0x4bf34c, '04c88052', '04f08052', 'docked width 1600 -> 1920 (graphics system setup)'),
    (0x4bf350, '85708052', '05878052', 'docked height 900 -> 1080 (graphics system setup)'),
    # main+0x5efc20 (only caller main+0x6f144c): a view's render size = min(requested, a 1600x900 limit); take the request
    (0x5efc2c, '08b1891a', '1f2003d5', 'render size width: no clamp to the limit'),
    (0x5efc40, '08b1891a', '1f2003d5', 'render size height: no clamp to the limit'),
    (0x4e27bc, '03c88052', '03f08052', 'docked viewport width 1600 -> 1920'),
    (0x4e27c0, '84708052', '04878052', 'docked viewport height 900 -> 1080'),
  ],
}
