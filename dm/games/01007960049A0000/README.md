# Bayonetta 2 (`01007960049A0000`)

Update 1.2 (`v131072`) over the base game, SDK 3.6.0 (no other game's `sdk` build), the oldest SDK until [OCTOPATH TRAVELER](../010057D006492000/README.md)'s 3.5.1. PlatinumGames' own engine in its Wii U form (not [Bayonetta 3](../01004A4010FEA000/README.md)'s) on NVN. Modules `main`, `subsdk0` (NVN's GLSLC: `main` imports `glslcCompile` but did not call it on the route below) and `sdk`. Wwise sound through two 6-channel `AudioOut`s; CRI's file system (CPK) and Sofdec movies (USM), both decoded by the game's own code. Plays on the iPhone (IPA 12.5 GB). [Bayonetta](../010076F0049A2000/README.md) runs on the same `sdk` and `subsdk0` builds.

On the Mac: the boot, the PlatinumGames logo movie (USM), the title, a new game (slot 1, the auto-save notice, Story, Normal), the opening narration (The Legend of Aesir, which cannot be skipped on a first play), the prologue's cutscene (skipped: Plus, Down, A, A), the tutorial (closed: Plus, Down ×4, A), Verse 1 on the jet, Rodin's cutscene and the next fight with an Umbran Climax and a torture attack, with sound. 60 fps in play (frame time p50 16.67 ms, p95 about 17.5 ms; GPU 2–3.5 ms a frame on an M6), 30 in its cutscenes as the game asks; about 1.6 GB resident and 1.75 GB of Metal allocations. The title is up about 20 s after launch.

## What it needs

| Setting | Where | Why |
|---|---|---|
| 4 GB layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | [`game.tsv`](game.tsv) | the 4 GB block of the other games without virtual address memory (256 MB in front + 256 MB alias region + 256 MB stack region + 3.25 GB heap region) |
| `sys_resource 0x0` | `game.tsv` | the NPDM declares no system resource (META +0x14 = 0), so there is no virtual address memory: `nninitStartup` sets a heap of the pool − used memory (3.11 GB) |
| `mem_used 0x6600000` | `game.tsv` | the rewritten modules (102 MB) are larger than the default 64 MB of used memory, and the game's `SetHeapSize` asks for pool − used |
| `pad_layout full` | `game.tsv` | the 3D pad layout: the camera is on the right stick. The game also offers touch controls in handheld mode; the profile keeps it docked |
| `shader_groups /shadereff.dat/=eff /shader=core` | `game.tsv` | the shaders of the effect DAT and of the other shader DATs |
| `zip_store .cpk:.usm:.metallib` | `game.tsv` | already-compressed files stored as they are in the IPA |

## Files and shaders

- Six CRI CPK archives in the RomFS root (`data000.cpk` … `data006.cpk`, there is no `data004.cpk`; 9.5 GB, 6,091 files, 12 GB of them CRILAYLA-compressed) of PlatinumGames DAT archives, beside the USM movies (`movie/`) and the Wwise sound (`sound/`). The CPK layout is in [01](../../../docs/agents/01-extract.md); `dm/gfx/tools/cpk.py` reads it.
- **Current:** 1,856 BNSH files of one stage each (1,547 pixel, 309 vertex), the `.vso` / `.pso` members of `shader.dat`, `shader2.dat` and `shadereff.dat` in `data000.cpk`. The split decodes every CPK entry (`shader_split.py` `cpk_jobs`, about 2.5 minutes). All build for the Mac and iOS.
- A prewarm list recorded on the Mac over the route above (974 lines).

## Rendering

- **Resolved: the opening narration drew in a corner of a black screen.** Every frame starts with a pass whose colour target is 32x32 (then 256x256 ones) beside the 1280x720 depth buffer, and the game clears the depth there; the host cleared only that corner. The earlier opening scenes had speckled and blocky patches, and a full-screen grain overlay showed one tile. Fixed generally ([Clears of attachments larger than the render pass](../../../docs/reference/rendering.md#clears-of-attachments-larger-than-the-render-pass)).
- **Resolved: the bloom chain** (`[RED] Downsample case not implemented: 320x176 1 samples format 70 → 80x48`): `nvnCommandBufferDownsample` from a 320x176 target into 80x48 and 48x32 textures, and from the 1280x720 BGRA8 frame into an RGBA8 texture ([`Downsample` of a single-sample texture](../../../docs/reference/rendering.md#downsample-of-a-single-sample-texture)).

## Mac input

The work folder's `run/route.sh <run> [secs]` boots on a fresh save and presses A five times from the title (slot 1, the notice, Story, Normal); `run/ctl.sh` / `run/step.sh` press and take shots, `run/watch.sh` takes a series of shots, and `run/fight.sh` repeats a simple combo. `run/save_ref` holds the save after Verse 1, `run/save_ref2` the one after the second fight.

## Found on this game

- **SDK 3.6.0 answered as SDK 4**, with no host change for the SDK itself: [services.md](../../../docs/reference/services.md#sdk-360-answered-as-sdk-4).
- **CRI CPK archives and the BNSH files in their DAT archives** (`cpk.py`, `cpk_jobs`): [shaders.md](../../../docs/reference/shaders.md#bnsh-files-in-platinumgames-dat-archives-inside-cri-cpk-archives).
- **A cleared attachment larger than the render pass is cleared in a pass of its own** (`rp_begin_ex_`): [rendering.md](../../../docs/reference/rendering.md#clears-of-attachments-larger-than-the-render-pass).
- **`Downsample` of a single-sample texture as a filtered scaling copy** (`copy_t2t`): [rendering.md](../../../docs/reference/rendering.md#downsample-of-a-single-sample-texture).

## Open

- Not checked: 13 Sentinels and Catherine: Full Body's CPKs with the new reader (no files on the drives), and the clear fix in other games beyond the unit tests.
- AGENTS.md still calls Mario Kart 8 Deluxe's SDK 4.4.0 "the oldest SDK so far"; this game's SDK 3.6.0 is older.
