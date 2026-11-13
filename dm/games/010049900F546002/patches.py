# Super Mario 3D All-Stars, program 2 (Super Mario Sunshine, 010049900F546002): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py
# and applies those of a module's NSO build ID; CUTS, OBS and FRAME_STEP as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
CUTS = {
  # 1.1.4 (v393216) main: Hagi reserves its coroutine stacks as one 1 GB address region (main+0x151218, the size at +0x151214; its other start path at +0x151398) after the 4 GB + 2 MB region that
  # maps the GameCube, and hands them out with a bump allocator. They use under 2 MB in play, so 64 MB is plenty, and both regions then fit a 4.25 GB alias region: the whole guest block fits
  # the 6 GB iOS allows one reservation. A platform limit: the Switch's alias region is 64 GB
  'b21e3733e11d6fcf1a8d57ce51bef4a5': [
    (0x151214, 'e2030232', '0280a052', 'coroutine stack region size: orr w2,wzr,#0x40000000 → mov w2,#0x4000000 (1 GB → 64 MB)'),
    (0x151398, 'e2030232', '0280a052', 'coroutine stack region size, the other start path: 1 GB → 64 MB'),
  ],
}

# How general each patch is and could be (dm/tools/docs_patches.py "Binding"): what, its binding now, the loosest it could have (goal), purpose, why; kept: why the gap stays open
BINDING = [
  {'what': 'Hagi coroutine stack region 1 GB → 64 MB', 'now': 'build', 'goal': 'game', 'purpose': 'memory',
   'why': "a size inside Hagi's own code: nothing the host sees tells a 1 GB reservation it can shrink from one it cannot", 'checked': 'the final build 1.1.4 (the only one the profile packs), 2026-11-04',
   'kept': '1.1.4 is the last update; a match over Hagi would serve no other build of this game'},
]
