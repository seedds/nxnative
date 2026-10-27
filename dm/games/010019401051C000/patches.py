# Mario Strikers: Battle League (010019401051C000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO build ID;
# CUTS, OBS and FRAME_STEP as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
CUTS = {
  # main 1.3.2 (v393216): nninitStartup (main+0x620420) initializes its StandardAllocator with virtual address memory over 10 GB of address space,
  # StandardAllocator::Initialize(nullptr, 0x280000000) at main+0x620690, which the Switch's 64 GB alias region holds and the host's 4.375 GB alias region cannot: nn::diag abort at
  # sdk+0x32472c (StandardAllocator::Initialize) on the first provisional boot. A workaround in the game's own code: the allocator commits pages as it goes and the pool (3.2 GB) bounds what
  # it maps, so 3 GB of address space holds it (about 2.0 GB resident in the menus, 2.5 to 2.8 GB in a match); it starts right after the node area with this sdk build's cuts
  # (dm/tools/sdk_patches.py).
  # The menus' background movies run through an event thread (main+0xab4d84) that waits on the video decoder's four events and the player's. After a video "output ready"
  # event (its holder at manager+0x3b0) that is not for the current player it reads the current player (manager+0x20) without checking it; the code for every other event
  # checks first or does not read the player.
  # Leaving a menu tears its player down on the main thread (main+0xab55a4: it parks the event thread, zeroes the holders' owners, frees the player, then nulls the pointer), and an
  # output-ready event the event thread took after that was a null read at main+0xab4e18 (CRASH sig=11 addr=0, lr main+0xab4df8): on the Mac the game closed on the way into
  # some menus. Such an event now goes straight back to the wait, as the others do
  '2e90f0815b1f92a1cbf013b3f4d358f4': [
    (0x620684, '0100b0d2', '0100b8d2', 'StandardAllocator::Initialize(nullptr, size): mov x1,#0x80000000 → mov x1,#0xc0000000'),
    (0x620688, '4100c0f2', '1f2003d5', 'StandardAllocator::Initialize(nullptr, size): movk x1,#2,lsl#32 → nop (10 GB → 3 GB)'),
    (0xab4df8, 'd7000034', 'b7fdff34', 'movie event thread, output ready not for the current player: cbz w23,#0xab4e10 → cbz w23,#0xab4dac (back to the wait)'),
  ],
}
