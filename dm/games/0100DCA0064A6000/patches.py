# Luigi's Mansion 3 (0100DCA0064A6000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO build ID;
# CUTS as described there ("Registered patches").
#
# Ghost Gallery crash (seedds/nxnative issue #672): SIGSEGV / SIGBUS at main+0x4723a0 / +0x4723c8 (once +0x47241c) when the Ghost Gallery opens or switches sections.
# The function around main+0x472334 frames the gallery camera on up to 5 bones of the shown ghost: for each bone name hash in a list (x22, ended by -1) it takes the model's
# skeleton handle (the first word of the object from vtable +0x98 of [x24 + 0xb8]), looks the handle's slot up in the engine's handle table (main+0x18868e0: 0x20000 pointers,
# indexed by handle & 0x1ffff; a u16 generation per slot at table + 0xffff8 + slot * 2, the handle's bits 17-31), binary-searches the skeleton's sorted {hash, index} pairs
# (count at +0x14, offset at +0xc) and reads that bone's matrix (+0x30) from the model ([x0 + 0x10] + index * 64). Unlike the engine's other users of the table (main+0x7bf3c8,
# main+0x7cba7c, main+0x7d58f4, ...), it does not compare the generation, and it uses the index even when the hash is not found (-1, the read at main+0x47241c).
# The table's free routine (main+0x7c23e0) clears the owner's handle, stores 1 in the slot, links the slot into the free list (each free slot holds the next one's address | 1)
# and bumps the generation. A gallery model still holding a copy of a released skeleton handle therefore reads an odd non-pointer from its slot: the garbage addresses of the
# reports (x9 = 0x15a19f949 at the fault). What releases the skeleton while the model is shown is not established (no save that far here).
# The cut: a slot whose value is odd (a free slot) and a hash that is not found both skip that bone (main+0x472670, the loop's next bone, as for the list's -1 end); with fewer
# than 2 bones found the function already leaves the framing out (cmp w19, #2 at main+0x472694). The two words at main+0x472368 swap the entry count load and the free-slot test
# (the count of 0 then reaches the search's one-entry path, which reads the word before the pairs and finds no match).
CUTS = {
  '79e5950ffa85acf63e28c9aec051eac2': [   # main of 1.4.0 (v327680)
    (0x472368, '2a1540b98a040034', '491800372a1540b9',
     'Ghost Gallery (#672): ldr w10, [x9, #0x14]; cbz w10 -> tbnz w9, #0, main+0x472670 (a free handle slot holds an odd value: skip the bone); ldr w10, [x9, #0x14]'),
    (0x4723ec, 'a1000054', '21140054',
     'Ghost Gallery (#672): b.ne main+0x472400 -> b.ne main+0x472670 (bone hash not found: skip the bone instead of reading index -1)'),
    (0x4723fc, '1c008012', '9d000014',
     'Ghost Gallery (#672): mov w28, #-1 -> b main+0x472670 (bone hash not found or empty slot: skip the bone instead of reading index -1)'),
  ],
}

BINDING = [
    {'what': 'Ghost Gallery camera: a bone whose skeleton handle slot is free, or whose hash is not found, is skipped (#672)', 'now': 'build', 'goal': 'game', 'purpose': 'runtime',
     'why': "the game's own gallery camera code, which a function fingerprint (MATCH_CUTS) could find in another update",
     'blocked': "the profile packs only 1.4.0's main, the one build on hand",
     'checked': 'main 1.4.0 (v327680): the cut function and the handle table writers and free routine read by disassembly; a completed-game save on an M2 MacBook Air, the Ghost Gallery opened and its sections switched quickly with D-pad Up, no crash (2026-11-24)'},
]
