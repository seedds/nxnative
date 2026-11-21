# Fate/hollow ataraxia REMASTERED (010070E022546000): the rewriter's patches of this game's own builds.
# dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO build ID;
# CUTS and BINDING as described there ("Registered patches").
CUTS = {
  # 1.0.2 (v131072) main: the Entrance savedata-version gate. setHollowSlotInfo (main+0xdf8750) asks the
  # storage query "check_savedata_version" whether a save's version must be checked; the byte it returns
  # (read at main+0xdf9244) decides: 0 jumps straight to the slot fill (main+0xdf8790→0xdf87b8) and the
  # 438 compare (main+0xdf87b0, branch to the skip main+0xdf87b4) never runs; 1 reads the version string
  # (atoi main+0xdf91e0) and compares it - and the Switch 1.0.2 data carries "2" (the 438 migration is a
  # PC 1.0.4 dep.dat feature, the Switch NSP ships no bump), so every slot renders as an empty START.
  # The query's default when smash::fw::config's registry never filled is 1 (mov w3,#1 at main+0xdf9238,
  # into the lookup at main+0xdf923c): the retail data sets the key, the host environment has no config
  # source. The cut moves that default to 0: the game's own unchecked path fills the slots from the
  # stored data, and an explicit check stays effective should a config ever set the key. (Cutting the
  # compare's branch instead, b.lt main+0xdf87b4 → nop, also renders the slots - three-way A/B 2026-11-20,
  # docs/investigations/fate-ha-save-20261117.md - but it bypasses the check even when the game asks for
  # it, so the default cut is the one kept.)
  '6c16aa06a25504cee9cf0f4f7b5e615c': [
    (0xdf9238, '23008052', '03008052', 'check_savedata_version default: mov w3,#1 -> mov w3,#0 (no config registry on the host: the 438 gate would filter every valid Switch save)'),
  ],
}

BINDING = [
    {'what': 'check_savedata_version lookup default 1 -> 0', 'now': 'build', 'goal': 'engine', 'purpose': 'runtime',
     'why': "the lookup is smash::fw::config's; the default only bites when the engine's config registry never filled, which is a host-environment difference rather than this game's code (an engine-wide rule keyed on the empty registry could serve every Fuzz game)",
     'checked': "v131072: old bytes match at main+0xdf9238; alone (no b.lt cut) a 1.0.2 save renders its slots on Entrance and the game advances to its scenes on the Mac host - probe main+0xdf9244 reads 0 and the 438 compare main+0xdf87b0 is never hit (run 2026-11-20); without the cut the same save renders empty START"},
]
