# Pokémon Legends: Arceus (01001F5010DFA000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py; MATCH_CUTS as described there
# ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py

# A workaround in the game's own code; the cause is open (the note's "Crash in the capture demo", GitHub issue #712). main+0x16e33ec reads a weak reference at this+0x88, locks it and
# then reads [result + 0x118] without checking the result. The capture demo task calls it once per ball (main+0x19d2368 from main+0x1ccf1a4). Null there was a data abort at
# main+0x16e34fc after 37 to 65 minutes of play. The reference is null from the object's constructor until the field setup's state 0x18 (main+0x1877a4c through main+0x16dde90), and the
# lock returns null for a dead object; which of the two the crashes are is unknown. The cuts return the function's empty result ({0, 0} through x20) on both, before it takes any reference.
# All 12 direct reads of the result (the ball's +0x810) null-check it.
#   main+0x16e33ec, 2672 bytes, in 1.1.1. The room is two of the function's five identical blocks (mov w23, wzr; b main+0x16e3a14) at main+0x16e3da4, whose two branches move to the
#   block at main+0x16e3d88. Taking the function's abort stub instead, as the issue's first form did, moves its callers to a stub in the next function, which a function match cannot hold.
capture = 'capture demo: the empty result when the weak reference at +0x88 is unset or dead'
MATCH_CUTS = {'main': [
    {'name': capture, 'size': 2672, 'fingerprints': ['123f93c4fa74d68f6100dab6ec8310a5047b2b3535b72361b6e658e3e48d792e'],
     'cuts': [
         # stur xzr, [x29, #-0x60]  →  b the empty result
         (48, 'bf031af8', '64020014', capture + ': no reference at +0x88 (as 1.1.1 main+0x16e341c)'),
         # b main+0x16e3420  →  b the null check
         (1084, 'fefeff17', '5f010014', capture + ': after the lock (as 1.1.1 main+0x16e3828)'),
         # b.ne to the freed blocks  →  b.ne to the identical block at main+0x16e3d88
         (1480, '811f0054', 'a11e0054', capture + ': frees a duplicate block (as 1.1.1 main+0x16e39b4)'),
         (1552, '811d0054', '611c0054', capture + ': frees a duplicate block (as 1.1.1 main+0x16e39fc)'),
         # the null check: ldur x8, [x29, #-0x60]; cbnz x8, main+0x16e3420. The empty result: stp xzr, xzr, [x20]; b the epilogue at main+0x16e3cd0
         (2488, 'f7031f2a1bffff17f7031f2a19ffff17', 'a8035af8c8b3ffb59f7e00a9c8ffff17', capture + ': the null check and the empty result (as 1.1.1 main+0x16e3da4)'),
     ]},
]}

# How general each patch is and could be (dm/tools/docs_patches.py "Binding", listed in docs/agents/game-patches.md): what, its binding now, the loosest it could have (goal),
# purpose, why; blocked or kept: why the gap stays open (cannot close yet, or left open on purpose); checked: what was checked on game files (else a judgement)
BINDING = [
    {'what': 'capture demo: the empty result when the weak reference at +0x88 is unset or dead', 'now': 'game', 'goal': 'game', 'purpose': 'runtime',
     'why': "the game's own function dereferences its own weak reference; no host service is on the path, so no host or engine rule can hold it. Whether another Game Freak game shares the function is unchecked",
     'checked': "function-matched on 1.1.1, the only build on hand: exactly one match, a boot to the field setup, and the patched function run on the module's own bytes in a small interpreter for both null paths. Issue #712's reporters ran the earlier cuts for about 7 hours with no crash; the path itself has not been reached with these cuts"},
]
