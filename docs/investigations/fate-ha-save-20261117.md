# Fate/hollow ataraxia REMASTERED: the Entrance slots stay empty (2026-11-17)

## Symptom

On the Entrance (savedata select) screen every slot renders as an empty `START` tile — no thumbnail, no date, no play time — both for a fresh in-game save and for saves copied in from the console's user directory. The save files themselves are intact: the game's own `udat_rel.bin` on the Mac matches the PC release's file layout byte for byte in its header (magic `4D 59 07 16`, 7-bit encoded segments).

## Cause

`SaveloadManager::setHollowSlotInfo` (`main+0xdf8750`, the slot renderer) reads the savedata version through the game's `check_savedata_version` storage query, `atoi`'s it at `main+0xdf91e0` and compares it against the hardcoded 438 (`0x1b6`) at `main+0xdf87ac`; below it the `b.lt` at `main+0xdf87b4` skips the slot fill entirely.

The field's value comes from the game's own init: `GameDataContext::resetAll` (`main+0xd2839c`) writes 2 through the `kagLocalFlag` initializer, parsed from the shipped KAG data — not from any runtime config the host supplies. 438 is what the **PC 1.0.4** migration produces (`data/.patch/dep.dat` reads `1.0.4.729` on the PC files, and that patch step bumps the stored version); the Switch NSP has no such migration and no `dep.dat`, so every Switch save — new or imported — sits at 2 and is filtered out. `system/version.conf` is identical between the ports and is not the source.

A second gate hides the buttons: the same storage query, when `smash::fw::config`'s registry never filled (the host has no config source for the key), defaults to 1 (`mov w3,#1`, `main+0xdf9238`, read into the lookup at `main+0xdf923c`) and marks the slot hollow, so START/COPY/DELETE ignore input even after the slot renders.

## Fix (registered in `dm/games/010070E022546000/patches.py`, build `6c16aa06a25504cee9cf0f4f7b5e615c`)

| Site | Cut | Effect |
|---|---|---|
| `main+0xdf87b4` | `b.lt` → `nop` | the slot renderer fills every slot from the stored data, version gate bypassed |
| `main+0xdf9238` | `mov w3,#1` → `mov w3,#0` | the config-less default marks slots not hollow; the buttons answer |

## Verification (Mac host, 2026-11-20)

With only the version-gate cut the three slots render (thumbnails, 2026/11/17 dates, play time) but the START press does nothing; with both cuts a click on slot 0's START loads the 1.0.2 save — the game enters its in-game scenes (the Tsukiaki prologue: moon, the hotel corridor and room) and autosaves, with the host alive and presenting at vsync through frame 7,200 and on. The A/B during diagnosis (bypassing the compare only) flipped the same data from empty START to fully rendered slots in the same run.
