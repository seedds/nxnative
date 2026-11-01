# Bayonetta (`010076F0049A2000`)

Update 1.1 (`v65536`) over the base game, SDK 3.6.0. PlatinumGames' own engine in its Wii U form on NVN, as [Bayonetta 2](../01007960049A0000/README.md): the same `sdk` and `subsdk0` builds (`subsdk0` is NVN's GLSLC, which `main` imports), and no host change. CRI middleware throughout: the file system (CPK), ADX2 sound (`se/` and `bgm/`, `.acf` / `.awb`) through one 6-channel `AudioOut`, and Sofdec movies (USM), all decoded by the game's own code. Plays on the iPhone (IPA 8.8 GB). Its profile is Bayonetta 2's with its own build row and tables.

On the Mac from a fresh save: the boot, the opening movie (USM; Plus skips it), the title and its attract demo, a new game (PLAY, NEW GAME, the auto-save notice, Normal), the prologue on the falling clock tower, the graveyard cutscene (skipped: Plus, Down, A, then Left, A for Yes), the touch-controls tips, the practice tutorial (punch, kick, combos, evade, Witch Time), the graveyard fight of Verse 1 with Witch Time, its autosave and the next loading screen's practice, with sound and no red line. 60 fps in play (frame time p50 16.65 ms, p95 17.1–17.5 ms; GPU 2–4 ms a frame on an M6), 30 in its cutscenes, where the game sets present interval 2; about 2.45 GB resident and 1.54 GB of Metal allocations, flat over 15 minutes.

## What it needs

| Setting | Where | Why |
|---|---|---|
| 4 GB layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | [`game.tsv`](game.tsv) | the 4 GB block of the other games without virtual address memory (256 MB in front + 256 MB alias region + 256 MB stack region + 3.25 GB heap region) |
| `sys_resource 0x0` | `game.tsv` | the NPDM declares no system resource (META +0x14 = 0): `nninitStartup` sets a heap of the pool − used memory (3.11 GB) |
| `mem_used 0x6600000` | `game.tsv` | the rewritten modules (102 MB) are larger than the default 64 MB of used memory, and the game's `SetHeapSize` asks for pool − used |
| `pad_layout full` | `game.tsv` | the 3D pad layout: the camera is on the right stick. The touch controls are handheld only; the profile keeps it docked |
| `shader_groups /shader.dat/=core` | `game.tsv` | the one shader DAT |
| `zip_store .awb:.cpk:.usm:.metallib` | `game.tsv` | already-compressed files stored as they are in the IPA |

## Files and shaders

- Nine CRI CPK archives in the RomFS root (`data00.cpk` … `data03.cpk`, `data10.cpk` … `data14.cpk`, 4.25 GB) of PlatinumGames DAT archives, beside the movies (`movie/`) and the sound; `cpk.py` reads them as they are.
- **Current:** 458 BNSH files of one stage each (236 pixel, 222 vertex), the `.vso` / `.pso` members of `shader.dat` in `data00.cpk` (CRILAYLA); the split (`cpk_jobs`) takes about 1.7 minutes. All build, and the picture was right on the route to the first fight (the clock tower, the graveyard, Witch Time's tint, the practice screens, the HUD) with no change: the two fixes made for Bayonetta 2 cover this engine.
- A prewarm list recorded on the Mac over the route above (513 lines).

## Game state

- The prologue's practice tutorial waits on a Witch Time page until a dodge lands right before the strike (the work folder's `run/witch.py` times it; it took 20–30 tries).

## Mac input

The work folder's `run/see.sh <run> [secs]` boots on a fresh save (or `SV=<save>`), presses `PRESS` once frame `AT` is logged and takes a shot every `EVERY` s; `run/ctl.sh`, `run/step.sh`, `run/watch.sh` and `run/fight.sh` as for Bayonetta 2; `run/tut.sh` works through the practice tutorial's pages (Minus is Next), and `run/witch.py` times the Witch Time page's countdown from shots and dodges at a sweep of offsets. `run/save_ref` is the save at the prologue's chapter screen, `run/save_ref2` the checkpoint after the tutorial (the tutorial comes back after a load), `run/save_ref3` the one after Verse 1.

## Found on this game

Nothing new: the CPK reader and the two rendering fixes made for [Bayonetta 2](../01007960049A0000/README.md#found-on-this-game) cover it.

## Open

- Nothing known.
