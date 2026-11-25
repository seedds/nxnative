# Batman: Arkham Asylum (`0100E870163CA000`)

The Switch version, update 1.0.1 (v65536) over the base game, SDK 14.3.0, Rocksteady's Unreal Engine 3 (package version 864, Oodle Leviathan chunks) with Scaleform menus and Bink movies, NVN. Modules `main` and `sdk`. RomFS is 2,822 files: 2,674 Unreal Engine 3 packages (`.xxx`) and `BmGame/CookedSwitch/GlobalShaderCache-Switch.bin`.

The profile is the provisional one `scripts/build_ipa_from_nsp.py` wrote from its survey of the game files, kept as it came except for the two texture keys below, and with no frame-rate key. It boots on the Mac: the app packed from these tables loads them as **31,261 map entries**, and a 147 s run reached gameplay and drew 13,200 frames without one `[RED] shader … is not in the shader map` line. The tables in this folder are the split's own output, re-run in the tree this profile is committed from: 31,261 shader rows (26,948 pixel and 4,313 vertex), every key distinct.

## What it needs

| Setting | Where | Why |
|---|---|---|
| a 4 GB layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | [`game.tsv`](game.tsv) | `main` sets a heap of at most the pool − 64 MB, a 3,328 MB heap region ([memory.md](../../../docs/reference/memory.md)); the survey's values, not yet pushed to a failing allocation |
| `sys_resource 0x0` | [`game.tsv`](game.tsv) | the NPDM declares 0 MB of system resource: `IsVirtualAddressMemoryEnabled` is false |
| `modules main sdk` | [`game.tsv`](game.tsv) | every module `main` imports from |
| `tex_flush_upload on`, `tex_dirty_upload on` | [`game.tsv`](game.tsv) | the engine fills its texture pools from the CPU and never flushes them: the owner's own 405 s session (40,678 frames) counts **234,144** writes into texture storage against **485** re-uploads, and **279,046** write-protection faults. Both default off, and a provisional profile never guesses them; with them off, a HUD sprite above an attacking enemy kept the stale contents of its pool. `tex_dirty_upload` needs `tex_flush_upload` on (the same pair Arkham Knight and Tears of the Kingdom need) |
| no frame-rate key | [`game.tsv`](game.tsv) | the game's own limiter is left in charge: every `Coalesced_*.bin` cooks `MaxSmoothedFrameRate 30` under `bSmoothFrameRate TRUE`, so this is a plain 30 fps bring-up. Raising it means writing that field at run time, which is a host change and is not here |

No rendering key, no rewriter patch and no host row: beyond that table, the profile is the provisional one the survey wrote.

## Files and shaders

- This title stores **no NVN control section** anywhere. Each shader is a descriptor record — `{u8 GUID 16, u32 v1, u16 v2 (5 or 0x105), u32 v3, u32 v4, u32 0x28, u32 0, v4 bytes, 0x50-byte tail}` — and the engine's loader builds the 0x880-byte section from a fixed template and eight tail fields. A record whose tail names a second module at `+0x1c` holds a **second vertex module inline** (`ins0 || data0 || ins1`), one shared vertex-fetch program's instructions behind the first module's data section; its control section runs to that module's end and lists both modules at `+0x748`. `shader_split.py` (`aa_records`, `aa_ctl`, `aa_code`, `aa_pair_off`) reads both forms ([docs/reference/shaders.md](../../../docs/reference/shaders.md)).
- **31,261 shaders**: 26,948 pixel and 4,313 vertex, from 2,822 files — 282 of the 2,674 packages hold at least one record, and `GlobalShaderCache-Switch.bin` holds 26 of the 70 dumped programs. That is **786 more vertex programs** than a reader without the second module finds; the whole RomFS holds 470,728 records, 23,802 of them two-module in 499 packages (most of those copies of one program: `43851cf9b762d11c` alone has 74).
- **The proof:** 70 shader pairs were dumped from the game's own run on this machine and the reader's control section and code area for each of the 70 reproduce them **byte for byte**, including the three two-module programs the earlier reader skipped. The dumps are game data and are not committed. This was measured on this title only — no other Unreal Engine 3 game's split was re-run against it.
- **One key is still not read from the files:** `8a3000a86b82578c`. The 34 keys the 30,475-key map lacked are down to this one; it is in no record the reader decodes (480,116 descriptor-shaped structures examined, and it has no dump either), so its bytes are in a form this reader does not see.

## Found on this game

- A record form with no stored control section, and a second vertex module stored inline behind the first module's data: [docs/reference/shaders.md](../../../docs/reference/shaders.md).

## Open

- **No IPA has been run and iOS is untested.** Everything here is the Mac app.
- `8a3000a86b82578c` ([above](#files-and-shaders)).
- The 786 new vertex programs are in the map and in the Mac shader library, but the route that was played did not draw one: `pso_user.tsv` gained no line, so no scene has exercised them yet. A scene that does is still to be found.
- The pipeline prewarm list `pso_manifest.tsv` is empty: nothing recorded on the Mac yet.
- The memory keys are the survey's and have not been pushed to a failing allocation; the run above settled at 2,114 MB resident, well under the layout.
