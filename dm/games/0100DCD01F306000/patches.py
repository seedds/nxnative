# Mina the Hollower (0100DCD01F306000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO build ID;
# CUTS, OBS and FRAME_STEP as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
CUTS = {
    # 1.0.5 (v262144) main: the engine's startup (main+0x6d850, called from nnMain) sets up two TLSF arenas, "Default Mem" (main+0x6d8ac) and "Render Mem" (main+0x6dc0c), each
    # with init(arena, size, 16 MB, 32 MB) through main+0xdec90 → main+0xdda90. The init rounds the size up to its page, derives the first-level bin count from it (0x36 − clz) and
    # reserves the whole size with nn::os::AllocateAddressRegion (main+0x1089b0 → main+0x1089c8), committing pages as the arena grows; nothing else reads the size. Each asks for 4 GB,
    # which the Switch's 64 GB alias region holds; an iPhone's 4.375 GB alias region cannot hold both beside the StandardAllocator. Measured with a 64 GB alias region through the
    # opening on the boat (7 minutes): Default Mem maps 320 MB, Render Mem 16 MB. Platform workaround: 1 GB and 256 MB, with the StandardAllocator capped beside them (game.tsv)
    '2bc484524f0c86bc7c2228d6fbfb0817': [
        (0x6d8ac, '2100c0d2', '0100a8d2', 'TLSF arena "Default Mem" size mov x1,#0x100000000 → mov x1,#0x40000000 (4 GB → 1 GB)'),
        (0x6dc0c, '2100c0d2', '0100a2d2', 'TLSF arena "Render Mem" size mov x1,#0x100000000 → mov x1,#0x10000000 (4 GB → 256 MB)'),
    ],
}

# How general each patch is and could be (dm/tools/docs_patches.py "Binding", listed in docs/agents/game-patches.md): what, its binding now, the loosest it could have (goal),
# purpose, why; blocked or kept: why the gap stays open (cannot close yet, or left open on purpose); checked: what was checked on game files (else a judgement)
BINDING = [
    {'what': 'TLSF arenas "Default Mem" and "Render Mem": 4 GB → 1 GB and 256 MB', 'now': 'build', 'goal': 'game', 'purpose': 'address space',
     'checked': 'the arena init and both call sites disassembled on 1.0.5, 2026-11-03',
     'why': "the sizes are immediates in the engine's startup; a function-matched rule could carry them over the game's updates",
     'blocked': 'only one build of the game (1.0.5) on hand to check a fingerprint against'},
]
