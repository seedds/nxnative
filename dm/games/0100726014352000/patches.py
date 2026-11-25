# Diablo II: Resurrected (0100726014352000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO build ID;
# CUTS, OBS and FRAME_STEP as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
CUTS = {
  # 1.0.34.0 (v2228224) main: offline play is allowed only within 30 days of the last Battle.net login. main+0x1978ac reads the saved setting "User Last Online" (key at main+0x1d53e80,
  # through main+0xdf5fe0) and sets the offline-allowed flag [x20+0x2c] when it exists and now − it < 0x9356907420000 ns (30 days); with no such setting (a save that was never online)
  # the flag is 0. Without it the title's login (main+0x2d0710 → the account request state machine main+0xe114b0) fails, as the host is offline, and the game shows "Failed to
  # authenticate" and stays on the title; no answer of the host's nifm or account services passes it. Both stores now set 1: the game behaves as a console that was online within 30 days,
  # and the title goes to the main menu. (The check was first described in d2r-mobile's notes on a community offline fix; this cut is its own)
  'db0507f36b84200ac05f75d76ed5026f': [
    (0x1979bc, 'e8a79f1a', '28008052', 'offline allowed (last online within 30 days): cset w8,lt → mov w8,#1'),
    (0x1979c4, 'e8031f2a', '28008052', 'offline allowed (no "User Last Online" setting): mov w8,wzr → mov w8,#1'),
  ],
  # 1.0.35 (v2293760) main: offline allowed (as 1.0.34.0) + redirect statically linked scalar C zlib inflateInit2_ / inflateInit_ / inflate / inflateEnd to host libz via OBS_ZLIB.
  '721f42ea94fe5f6f1af00ba3836032a8': [
    (0x1979bc, 'e8a79f1a', '28008052', 'offline allowed (last online within 30 days): cset w8,lt → mov w8,#1'),
    (0x1979c4, 'e8031f2a', '28008052', 'offline allowed (no "User Last Online" setting): mov w8,wzr → mov w8,#1'),
    (0xf40ef0, 'f70f1cf8f65701a9', '1f2003d5c0035fd6', 'zlib inflateInit2_ → nop; ret (host OBS_ZLIB takes over)'),
    (0xf410a0, 'f50f1df8f44f01a9', '1f2003d5c0035fd6', 'zlib inflateInit_ → nop; ret (host OBS_ZLIB takes over)'),
    (0xf41200, 'ff0303d1fc6f06a9', '1f2003d5c0035fd6', 'zlib inflate → nop; ret (host OBS_ZLIB takes over)'),
    (0xf42d50, 'f30f1ef8fd7b01a9', '1f2003d5c0035fd6', 'zlib inflateEnd → nop; ret (host OBS_ZLIB takes over)'),
  ],
}
OBS = {
  '721f42ea94fe5f6f1af00ba3836032a8': [
    (0xf40ef0, 27 | (1 << 8), 'd503201f', 'OBS_ZLIB kind 1: inflateInit2_ → host libz'),
    (0xf410a0, 27 | (0 << 8), 'd503201f', 'OBS_ZLIB kind 0: inflateInit_ → host libz'),
    (0xf41200, 27 | (2 << 8), 'd503201f', 'OBS_ZLIB kind 2: inflate → host libz'),
    (0xf42d50, 27 | (3 << 8), 'd503201f', 'OBS_ZLIB kind 3: inflateEnd → host libz'),
  ],
}
# The same cut for builds without registered CUTS (dm/tools/nso_match.py): the reader of "User Last Online" (main+0x1978ac in 1.0.34.0, 316 bytes, the cuts at +272 and +280), found by
# its fingerprint and its direct callees' (main+0x1422c4, the setting read main+0xdf5fe0, the clock main+0x1b6bb60). Both cuts set the flag the function itself stores, so they mean the same wherever it matches.
# The base game (v0, main 4de22400…) has no such check: it only writes "User Last Online" after a Battle.net login (main+0x206bf4 → main+0xbd1a40), and no 30-day constant is in its code,
# so it needs no cut, and the rule finds no function there (a red "skipped" line on v0 is expected)
offline = 'offline allowed without a recent Battle.net login'
MATCH_CUTS = {'main': [
    {'name': offline, 'size': 316, 'fingerprints': ['0904297729da61276db29eeb9880aad2da62c0003c822113b7e6142be7cd8db1'],
     'cuts': [(272, 'e8a79f1a', '28008052', 'offline allowed (last online within 30 days): cset w8,lt → mov w8,#1 (as 1.0.34.0 main+0x1979bc)'),
              (280, 'e8031f2a', '28008052', 'offline allowed (no "User Last Online" setting): mov w8,wzr → mov w8,#1 (as 1.0.34.0 main+0x1979c4)')]},
]}

# How general each patch is and could be (dm/tools/docs_patches.py "Binding", listed in docs/agents/game-patches.md): what, its binding now, the loosest it could have (goal),
# purpose, why; blocked or kept: why the gap stays open (cannot close yet, or left open on purpose); checked: what was checked on game files (else a judgement)
BINDING = [
    {'what': 'offline play without a Battle.net login in the last 30 days', 'now': 'game', 'goal': 'game', 'purpose': 'runtime',
     'checked': 'MATCH_CUTS enrolled on 1.0.34.0; the base game has no such check (2026-11-12)',
     'why': "the game's own offline policy (a saved last-online time), which an offline host cannot satisfy; function-matched (MATCH_CUTS) over the game's updates"},
    {'what': 'zlib inflate redirected to Apple libz (OBS_ZLIB)', 'now': 'build', 'goal': 'engine', 'purpose': 'frame rate',
     'blocked': 'OBS gates have no function-matched form yet',
     'checked': 'tested on 1.0.35 (v2293760) overworld traversal on Apple M4 (2026-11-25)',
     'why': '1.0.35 statically links a slow scalar zlib; OBS_ZLIB routes inflateInit2_, inflateInit_, inflate and inflateEnd to Apple libz (hle_zlib.c)'},
]
