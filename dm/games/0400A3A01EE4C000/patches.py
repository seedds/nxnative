# Pokopia's patches for the rewriter (dm/tools/patch_nso.py reads every profile's patches.py; the formats are its own).
# OBS: observation gates, which change no instruction: the rewriter moves the instruction into a stub that first calls the host.

# Link Play's "Interact everywhere" (hle_coop_interact.c, the launcher's switch DM_COOP_INTERACT) and Story co-op (DM_COOP_STORY). Gate number 27 is OBS_COOP_INTERACT (dm.h); the site's id is in its upper bits
# (COOP_SITE_MASK = 1, COOP_SITE_TALK = 2). The callback does nothing while the switch is off
OBS_COOP_INTERACT, COOP_SITE_MASK, COOP_SITE_TALK = 27, 1, 2
OBS = {
    '76a489f3bd3642fa5e18bd83d32c98ba': [   # 2.0.1 main
        (0xb1030, OBS_COOP_INTERACT | COOP_SITE_MASK << 8, 'f2c0002d', 'Interact everywhere: the permissions a story area gives online players'),
        (0x4c06d8, OBS_COOP_INTERACT | COOP_SITE_TALK << 8, '2a000328', 'Story co-op: a visitor talks to NPCs as the host does'),
    ],
    '816c5358e8178d802fcec4a7ce5a741a': [   # 1.0.5 main: the same two sites (the mask builder at main+0xa61e0, the talk handler at main+0x469424)
        (0xa62b0, OBS_COOP_INTERACT | COOP_SITE_MASK << 8, 'f2c0002d', 'Interact everywhere: the permissions a story area gives online players'),
        (0x4698b0, OBS_COOP_INTERACT | COOP_SITE_TALK << 8, '2a000328', 'Story co-op: a visitor talks to NPCs as the host does'),
    ],
}

# docs/agents/game-patches.md (dm/tools/docs_patches.py)
BINDING = [
    {'what': "Link Play's Interact everywhere and Story co-op: observation gates at the story-area permission-mask builder and the talk handler (hle_coop_interact.c)", 'now': 'build', 'goal': 'game',
     'purpose': 'runtime', 'why': "the game's own story logic: a function fingerprint of the mask builder and the talk handler would carry the two sites to a new build",
     'checked': "2.0.1 (76a489f3) and 1.0.5 (816c5358) mains: the mask builder is the same instruction for instruction, the talk handler's site sits at the same offset between the same calls (pokopia-native, 2026-11)",
     'kept': 'the gates change no instruction and do nothing while the launcher switches are off; 2.0.1 is the newest update, so a fingerprint would serve no other build yet'},
]
