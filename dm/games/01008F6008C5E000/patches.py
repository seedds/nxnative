# Pokémon Violet (01008F6008C5E000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO build ID;
# CUTS, OBS and FRAME_STEP as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
CUTS = {
  # Pokémon Violet 4.0.0 (v786432) main: nninitStartup (main+0x2b7da90, through its helper at main+0x167d510) initializes its StandardAllocator with virtual address memory over 12 GB of
  # address space (main+0x167d54c), which the Switch's 64 GB alias region holds and a phone's single 6 GB reservation cannot: the same call Pokémon Legends: Z-A and
  # Pokémon Scarlet make, whose cuts the host's StandardAllocator cap replaced (BINDING below). The pool bounds what the allocator maps (profile mem_map_limit)
  '709bfd66115298640155fcc4979dba15': [
    (0x167d54c, '6200c0d2', '2200c0d2', 'StandardAllocator::Initialize(nullptr, size, false): address space mov x2,#0x300000000 → mov x2,#0x100000000 (12 GB → 4 GB)'),
  ],
}

# How general each patch is and could be (dm/tools/docs_patches.py "Binding", listed in docs/agents/game-patches.md): what, its binding now, the loosest it could have (goal),
# purpose, why; blocked or kept: why the gap stays open (cannot close yet, or left open on purpose); checked: what was checked on game files (else a judgement)
BINDING = [
    {'what': 'StandardAllocator address space cut to 4 GB', 'now': 'build', 'goal': 'host', 'purpose': 'address space',
     'why': "the host's StandardAllocator cap (hle_svc.c) lowers the same request to what the alias region has room for, as it does for Pokémon Scarlet, whose same cut was removed",
     'blocked': 'not yet checked on Violet with the cut removed'},
]
