# Metroid Prime 4: Beyond (`010019A01E2F2000`)

The Switch version, update 1.1.0 (v131072) over a cartridge dump of the base game, SDK 19.3.15, Retro Studios' own engine (little-endian RFRM forms, the engine of [Metroid Prime Remastered](../010012101468C000/README.md)), NVN (API 55.15). Modules `main`, `subsdk0` (the SDK's movie library) and `sdk`. RomFS is 445 files, 26.2 GB: `MaterialArchive.arc`, `Kernel.pak`, `MiscData.pak`, `StartupUI.pak`, `Preload/DuplicateData.pak` (3.6 GB), the UI packs under `UI/MPR4/`, `Worlds/FrontEnd/FrontEnd.pak`, 432 packs under `Worlds/Suits/` and the update's `Patch/Z_198_NXPatch.pak.patch`.

Brought up on the Mac with the provisional profile's values and no host change: boot, the opening movie, the title, the menu, the save slot and difficulty screens, a new game's text and real-time opening (the Federation base under attack, Samus's gunship landing) and the first area of Tanamaar. Played for about a minute there: walking, turning and firing, with the visor HUD, the radar, the map and the hint texts. 60 fps (frame time p50 16.67 ms, GPU p50 about 5 ms at 1600×900 on an Apple M6); the audio renderer produces sound (70,478 of the first 72,000 rendered frames not silent; not listened to); memory levels off at about 4.0 GB resident and 1.9 GB of Metal allocations in the first area (frames 18,000 to 23,000). Not yet tested on the iPhone.

## What it needs

| Setting | Where | Why |
|---|---|---|
| 4 GB layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | [`game.tsv`](game.tsv) | no virtual address memory: the game raises its heap step by step and settles on 3.16 GB in the 3.25 GB heap region, as Metroid Prime Remastered does ([memory.md](../../../docs/reference/memory.md#a-heap-request-beyond-the-heap-region-returns-resultoutofmemory)) |
| `sys_resource 0x0` | `game.tsv` | the NPDM declares 0 MB of system resource |
| `modules main subsdk0 sdk` | `game.tsv` | every module `main` imports from |
| `shader_groups` (`/Worlds/`, `/Preload/`, `/UI/`, `/Patch/`) | `game.tsv` | the shader split's groups ([Shaders](#shaders)) |
| [`dual_want.tsv`](dual_want.tsv) | profile | `af84ccd3191eaf89` writes colour output 1 that a pipeline blends as the second source (met in the first area; the host's red line names it) |

No rendering key, no rewriter patch and no host row: beyond that table, the profile is the provisional one the NSP script wrote.

## Shaders

- **Resolved: `the shader split found no shaders in main or the RomFS`**, after `ADIR of 13 entries in 0x30c bytes, neither 36 nor 52 bytes each` for 442 files. The packs' resource entries are 60 bytes here, and the program records and the packs' `MTRL` forms are zstd streams where Metroid Prime Remastered's are zlib: [SNVN program records in Retro Studios' RFRM forms](../../../docs/reference/shaders.md#snvn-program-records-in-retro-studios-rfrm-forms). 259,989 shaders: 129,992 vertex, 129,972 pixel, 19 geometry and 6 compute, first seen in the world packs (124,094), the preload packs (110,994), the patch pack (21,360), the root's files (3,445; `MaterialArchive.arc` has 3,473 stages in 1,740 programs) and the UI packs (96); 129,972 vertex / pixel pairs. `main` holds none. The split takes 2 minutes 19 s, the Mac libraries 3 hours 43 minutes and the pipeline prebuild 51 minutes (Apple M6); the libraries are 68 GB and the Mac pipeline archive 16 GB.
- `MaterialArchive.arc` also holds an `NVN2` chunk with the same table and record layout as `SNVN` (1,740 programs, other code); this build never asked for a shader outside the map, so it reads `SNVN`. What `NVN2` is for was not looked into (the Switch 2 Edition's programs, by the name).

## Current

- `vertex attribute format 0x27 (8 / 16-bit integer) feeds a float input` (RGBA8UI): converted on the CPU at replay, as for [Metroid Prime Remastered](../010012101468C000/README.md); no visible effect.
- `NVN address lookup of an unknown name: nvnInstallGlobalDebugCallback`: the game asks for a debug entry point by name and goes on without it.
- **Open: runs of 32 ms frames after a long frame.** A long frame in play (a first use of a pipeline, 28 to 111 ms in submit; a screenshot, 80 to 90 ms) is followed by 5 to 20 frames of 32 ms each whose submit and GPU times are normal (1 ms, 3 to 5 ms); the game never sets a present interval other than 1. Whether the game's own frame limiter halves its rate to catch up, as it would on the Switch, or the host's pacing does, was not traced. 21 pipelines were first built in 30 s of play although the prebuild had run; the prewarm list is empty ([Open](#open)).

## Mac routes

The work folder is `<NSP folder>/010019A01E2F2000-work`: `host.sh` copies the worktree's `dm/host/dmhost` to `build/mp4host`; `run/boot.sh <log> [secs] VAR=value…` runs it on the NSP script's build (`010019A01E2F2000_build`: the RomFS image, the rewritten modules, the shader libraries in `shader/cache`) with `DM_CTL=run/ctl.txt`; `run/ctl.sh press|snap` drives it, and `run/step.sh <name> [key [wait]]…` presses keys and takes a shot. `run/pso_archive` is a link to the NSP script's (`010019A01E2F2000_build/run/pso_archive`): without it the host prebuilds 129,972 pairs before the first frame, about 50 minutes. `tools/rget.py <path> <out>` copies a file out of the RomFS image and `tools/pak.py <pack> [type]` lists a pack's resources.

- From a fresh save, in seconds after the start: the title's `PRESS A` answers from about 60 s (`A~0.4`; a press at 52 s did nothing); then `A~0.3` four seconds apart on Play, save slot 1 (New Game) and Normal. A loading screen, the text of the opening and the real-time opening follow unskipped (skipping was not tried); the game gives control on Tanamaar within 2 minutes 30 s of the last press, about 3 minutes 45 s into the run (energy 99, missiles 30). The left stick walks, the right stick turns and A fires; ZL locks on by the game's hint (not tried). The new file's `save.bin` exists from the difficulty screen on; there is no save past the first area.

## Found on this game

General tool changes first needed here; each applies to every game:

- **60-byte pack entries, zstd program records and `MTRL` forms, and program records whose control length is not a multiple of 0x40**, which the reader had skipped without a word in Metroid Prime Remastered too (28,479 of its shaders) (`rfrm_shaders`, `snvn_records`): [shaders.md](../../../docs/reference/shaders.md#snvn-program-records-in-retro-studios-rfrm-forms).

## Open

- The iPhone: no IPA built or tested. The game needs a 3.16 GB heap in a 4 GB layout, 4.0 GB resident on the Mac, and 68 GB of Mac shader libraries; the pipeline prewarm list `pso_manifest.tsv` is still empty.
- Only the opening and the first minute of Tanamaar have been played: combat, scanning, the Morph Ball, the psychic abilities, Sol Valley and Vi-O-La, bosses, saving at a station and loading a save are untested.
- The base game 1.0.0 alone was not run (the NSP script packs it from a derived profile).
- The runs of 32 ms frames ([Current](#current)).
