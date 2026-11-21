# Fate/hollow ataraxia REMASTERED: the Entrance slots stay empty (2026-11-17)

## Symptom

On the Entrance (savedata select) screen every slot renders as an empty `START` tile — no thumbnail, no date, no play time — both for a fresh in-game save and for saves copied in from the console's user directory. The save files themselves are intact: the game's own `udat_rel.bin` on the Mac matches the PC release's file layout byte for byte in its header (magic `4D 59 07 16`, 7-bit encoded segments).

## Cause

`SaveloadManager::setHollowSlotInfo` (`main+0xdf8750`, the slot renderer) reads the savedata version through the game's `check_savedata_version` storage query, `atoi`'s it at `main+0xdf91e0` and compares it against the hardcoded 438 (`0x1b6`) at `main+0xdf87ac`; below it the `b.lt` at `main+0xdf87b4` skips the slot fill entirely.

The field's value comes from the game's own init: `GameDataContext::resetAll` (`main+0xd2839c`) writes 2 through the `kagLocalFlag` initializer, parsed from the shipped KAG data — not from any runtime config the host supplies. 438 is what the **PC 1.0.4** migration produces (`data/.patch/dep.dat` reads `1.0.4.729` on the PC files, and that patch step bumps the stored version); the Switch NSP has no such migration and no `dep.dat`, so every Switch save — new or imported — sits at 2 and is filtered out. `system/version.conf` is identical between the ports and is not the source.

The query is a flag, not a value: the byte the caller reads at `main+0xdf9244` decides whether the version is checked at all. With 0 the renderer jumps straight to the slot fill (`main+0xdf8790` → `main+0xdf87b8`) and the compare never runs; with 1 it reads and compares. The query defaults to 1 when `smash::fw::config`'s registry never filled (`mov w3,#1` at `main+0xdf9238`, into the lookup at `main+0xdf923c`) — the path the host takes, having no config source for the key.

## Fix (registered in `dm/games/010070E022546000/patches.py`, build `6c16aa06a25504cee9cf0f4f7b5e615c`)

| Site | Cut | Effect |
|---|---|---|
| `main+0xdf9238` | `mov w3,#1` → `mov w3,#0` | with no config registry the query answers "no version check"; the game's own unchecked path fills the slots from the stored data |

The alternative — cutting the compare's branch (`b.lt` `main+0xdf87b4` → `nop`) — renders the slots equally well, but bypasses the check even when the game explicitly asks for it. The default cut keeps the compare alive for any configuration that sets the key, so it is the one shipped.

## Verification (Mac host, 2026-11-20: three-way A/B on copies of one save with one input script, plus the shipped single-cut build)

| Build | Runtime path (probes) | Entrance |
|---|---|---|
| unpatched | query → 1, compare hit with the version 2 | slots empty |
| `b.lt` → nop only | compare still hit (2 < 438), branch dead | slots render, game advances |
| default 1 → 0 only (shipped) | query → 0, compare and its branch never hit | slots render, game advances |

On the shipped binary (main rewritten from the registered profile: `main.cuts.tsv` one line, `0xdf87b4` back to Nintendo's `b.lt`): all three slots show their stored thumbnails, their 2026-11-17 dates and play time, slot 0's START loads the save and the Tsukiaki prologue plays with its text drawn (probe `0xdf9244` x0 = 0; `0xdf87b0` and `0xdf87b4` hit counts 0). Autosave to `udat_rel.bin` works on the patched builds. An earlier intermediate note claiming both cuts were needed came from a misread intermediate run and is retracted here.
