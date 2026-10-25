# 하늘의 궤적 the 1st / Trails in the Sky 1st Chapter (01001E9023920000): the rewriter's patches of this game's own builds.
# dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO build ID ("Registered patches"); CUTS, OBS and FRAME_STEP as described there.
CUTS = {
  # 1.0.6.1 (v393216) main: Falcom's SORA engine gives its "native allocator" a virtual address region of ten times the allocator's own capacity
  # (main+0x5746f4 fmov d0,#10.0: capacity 0x48200000 from main+0x5732a0, rounded up to 2 MB, 0x2d1400000 = 12.1 GB). On the Switch the alias region is 64 GB, but the
  # engine only maps what it uses (402 MB through the opening movie); the 5 GB-class guest layout has a 4 GB alias region, so the factor is 3 (3.375 GB).
  # The reservation is virtual address space only: the pages it commits come from the alias region and the memory the game actually uses is reported separately
  # (1.4 GB resident through the opening).
  '6656ba1b5b8ac30af10728f8b257a24c': [
    (0x5746f4, '0090641e', '0010611e', 'virtual address reservation: fmov d0,#10.0 → fmov d0,#3.0 (12.1 GB → 3.375 GB)'),
  ],
}
