# Fate/hollow ataraxia REMASTERED (010070E022546000): the rewriter's patches of this game's own builds.
# dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO build ID;
# CUTS and BINDING as described there ("Registered patches").
CUTS = {
  # 1.0.2 (v131072) main: the Entrance savedata-version gate. setHollowSlotInfo (main+0xdf8750) fills a
  # slot only when the version string read through the "check_savedata_version" storage query (the value
  # lands at main+0xdf9204, atoi'd at main+0xdf91e0) reaches 438 (0x1b6, compared at main+0xdf87ac);
  # below it every slot renders as an empty START. The Switch 1.0.2 data carries "2" there: the 438
  # migration script is a PC 1.0.4 feature (its dep.dat reads 1.0.4.729; the Switch NSP ships no such
  # bump), so valid saves - imported ones and fresh ones alike - stay hidden. The cut removes the
  # branch to the skip (b.lt main+0xdf91e0, main+0xdf87b4): slots render on the stored data regardless.
  '6c16aa06a25504cee9cf0f4f7b5e615c': [
    (0xdf87b4, '6b510054', '1f2003d5', 'savedata version gate: b.lt the skip -> nop (the field holds "2", the console data has no 438 migration)'),
    # the storage query's default (main+0xdf9238, mov w3,#1 into the check_savedata_version lookup at
    # main+0xdf923c): with no config registry the lookup yields 1, marking every slot hollow so its
    # START/COPY/DELETE ignore input; the retail data sets the key, ours has no config source -> default 0
    (0xdf9238, '23008052', '03008052', 'check_savedata_version default: mov w3,#1 -> mov w3,#0 (no config registry on the host: hollow marking would freeze the slot buttons)'),
  ],
}

BINDING = [
    {'what': 'savedata version gate: its branch to the slot skip', 'now': 'build', 'goal': 'game', 'purpose': 'runtime',
     'why': "the gate is the game's own KAG script compiled into main; a function-matched rule could carry it over the game's updates (the function is small and its prologue distinctive)",
     'checked': "1.0.2 v131072 both regions (the JP and US NSPs share the build ID): the cut's old bytes match at main+0xdf87b4; with the branch removed a real 1.0.2 save renders in all slots on the Mac host (docs/investigations/fate-ha-save-20261117.md)"},
    {'what': 'check_savedata_version lookup default 1 -> 0', 'now': 'build', 'goal': 'engine', 'purpose': 'runtime',
     'why': "the lookup is smash::fw::config's; the default only bites when the engine's config registry never filled, which is a host-environment difference rather than this game's code (an engine-wide rule keyed on the empty registry could serve every Fuzz game)",
     'checked': "v131072: old bytes match at main+0xdf9238; with both cuts the loaded scene plays and autosaves on the Mac host (run 2026-11-20); with the b.lt cut alone the slot buttons stay dead"},
]
