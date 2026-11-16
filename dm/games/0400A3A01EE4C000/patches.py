# Pokopia's patches for the rewriter (dm/tools/patch_nso.py reads every profile's patches.py; the formats are its own).
# OBS: observation gates, which change no instruction: the rewriter moves the instruction into a stub that first calls the host.

# Link Play's "Interact everywhere" (hle_coop_interact.c, the launcher's switch DM_COOP_INTERACT) and Story co-op (DM_COOP_STORY). The gate number is
# OBS_COOP_INTERACT (27, dm.h) with the site's id in bits 8 and up: 1 = the permission-mask builder (27 | 1 << 8 = 283), 2 = the talk handler (27 | 2 << 8 = 539).
# The callback does nothing while the switch is off. 1.0.5 (main 816c5358…) has the same two sites at main+0xa62b0 and main+0x4698b0 (hle_coop_interact.c lists
# them for the host side); only the registered build, 2.0.1, is rewritten here
OBS = {
    '76a489f3bd3642fa5e18bd83d32c98ba': [   # 2.0.1 main
        (0xb1030, 283, 'f2c0002d', 'Interact everywhere: the permissions a story area gives online players'),
        (0x4c06d8, 539, '2a000328', 'Story co-op: a visitor talks to NPCs as the host does'),
    ],
}

# docs/agents/game-patches.md (dm/tools/docs_patches.py)
BINDING = [
    {'what': "Link Play's Interact everywhere and Story co-op: observation gates at the story-area permission-mask builder and the talk handler (hle_coop_interact.c)", 'now': 'build', 'goal': 'game',
     'purpose': 'runtime', 'why': "the game's own story logic: a function fingerprint of the mask builder and the talk handler would carry the two sites to a new build",
     'checked': "2.0.1 (76a489f3) and 1.0.5 (816c5358) mains: the mask builder is the same instruction for instruction, the talk handler's site sits at the same offset between the same calls (pokopia-native, 2026-11)",
     'kept': 'the gates change no instruction and do nothing while the launcher switches are off; 2.0.1 is the newest update, so a fingerprint would serve no other build yet'},
]
