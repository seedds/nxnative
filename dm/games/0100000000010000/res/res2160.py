# Super Mario Odyssey 1.4.1 (v393216) main, docked render size 3840x2160 (README "Higher render resolutions"). Not loaded by default: copy it over a
# patches.py in this folder (or in a DM_GAMES_DIR profile) to use it, with game.tsv mem_reserve 0x240000000 and app_pool 0x200000000.
CUTS = {
  'b424be150a8e7d78701cbe7a439d9ebf': [
    (0x4bf2a4, '09c880d2', '09e081d2', 'framework window size 1600 -> 3840 (mov x9)'),
    (0x4bf2a8, '8970c0f2', '090ec1f2', 'framework window size 900 -> 2160 (movk x9, lsl 32)'),
    (0x4bf34c, '04c88052', '04e08152', 'docked width 1600 -> 3840 (graphics system setup)'),
    (0x4bf350, '85708052', '050e8152', 'docked height 900 -> 2160 (graphics system setup)'),
    # main+0x5efc20 (only caller main+0x6f144c): a view's render size = min(requested, a 1600x900 limit); take the request
    (0x5efc2c, '08b1891a', '1f2003d5', 'render size width: no clamp to the limit'),
    (0x5efc40, '08b1891a', '1f2003d5', 'render size height: no clamp to the limit'),
    # main+0x624ad0: graphics heap 170 MB -> 370 MB and its render-target pool 100 MB -> 300 MB (3840x2160 targets ran out at 100 MB)
    (0x624b4c, '0054a152', '0004a652', 'graphics heap 0xaa00000 -> 0x17200000'),
    (0x624b7c, '08c8a052', '0878a552', 'render-target pool 0x6400000 -> 0x12c00000'),
    # nninitStartup (main+0x4e321c): SetMemoryHeapSize 0xC3000000 -> 6 GB (orr x0, xzr, #0x180000000); game.tsv mem_reserve 0x1C0000000, app_pool 0x190000000
    (0x4e321c, '0060b852', 'e00f63b2', 'memory heap size 3.05 GB -> 6 GB'),
    # main+0x4bf714: sead RootHeap size 0xC0C00000 -> 4 GB (movz x8,#1,lsl #32), inside the 6 GB nn heap
    (0x4bf714, '0818b852', 'e80761b2', 'RootHeap 3.01 GB -> 4 GB'),
    # main+0x6a2f30: StationedHeap = parent's largest block - 25 MB computed in 32 bits; do it in 64 bits
    (0x6a2f30, '0000080b', '00c0288b', 'StationedHeap size: add x0, x0, w8, sxtw'),
    # main+0x6a3210: SceneHeap (frame heap) = min(0x1ED00000, the parent's largest block); its 3840x2160 targets ran it out
    (0x6a324c, '15daa352', '15daa952', 'SceneHeap 493 MB -> 749 MB'),
    (0x4e27bc, '03c88052', '03e08152', 'docked viewport width 1600 -> 3840'),
    (0x4e27c0, '84708052', '040e8152', 'docked viewport height 900 -> 2160'),
  ],
}
