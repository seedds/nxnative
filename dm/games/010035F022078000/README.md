# DRAGON BALL: Sparking! ZERO (`010035F022078000`)

Update 2.0.76 (v327680) over the base game, SDK 14.3.0, with the six add-on contents of the folder (two with content, four licences only). Spike Chunsoft / Bandai Namco on Unreal Engine 5.1.1 (`SDK MW+EpicGames+UnrealEngine-5_1_1`, project `SparkingZERO`; shaders `SP_GLSL_SWITCH`), IoStore, NVN, CRIWARE sound and movies (`CRI ADX`, `CriMana`). Modules `main`, `subsdk0` (the movie module) and `sdk`. 60 fps (30 on the Switch), 120 on the Mac when chosen.

Status: plays on the Mac. Headless runs on 2026-11-07 went from an empty save through the strap notice, the add-on content notices ("Downloadable content has been applied": the Pre-order Pack and the Ultimate Upgrade Pack), the privacy notice, the language choice and the autosave notice, to the opening (its real-time scenes and its movie), the title, and the tutorial battle against Vegeta: moving, flying and attacking, with the tutorial's steps ticked. One red line throughout, a known one (`vertex attribute format 0x2d … converted on the CPU`). On 2026-11-08 the user played the tutorial battle in a window with sound at 60 and at 120 fps and found the speed right at both. Anything past the tutorial's first steps is untested, as is an iPhone; no IPA has been built.

## What it needs

No rewriter cuts of its own and no `patches.py`.

| Setting | Where | Why |
|---|---|---|
| FMallocBinned3 constructor calls cut (both), engine region given what the alias region has room for | the rewriter's engine-wide rules (`dm/tools/ue_malloc.py`, `dm/tools/ue_region.py`), no build ID | its constructor is called from two places, one lazy construction inlined twice; see [Memory](#memory) |
| 3.75 GB guest block: `mem_reserve 0xF0000000`, `mem_alias 0xC8000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | [`game.tsv`](game.tsv) | Octopath Traveler 0's layout, in place of the provisional 64 GB alias region; see [Memory](#memory) |
| `sys_resource 0x1e00000` | `game.tsv` | the NPDM declares 30 MB of system resource |
| `depth_mode zero` | `game.tsv` | the game sets near = 0 at startup (the NSP script's address region check) |
| `pad_layout full` | `game.tsv` | a 3D arena, the D-pad and both sticks in use |
| `shader_groups GlobalShaderCache=global #Engine/=engine #SparkingZERO/=game` | `game.tsv` | the global shader cache, the engine's packages and the game's |
| `fps_target 60` | `game.tsv`, `hle_fpslock.c` | the host writes `rhi.SyncInterval` 1 and the engine's fixed frame rate 60 (`dm/tools/ue_fixed_rate.py`, no build ID); see [Frame rate](#frame-rate) |
| `fps_max 120` | `game.tsv` | **Frame rate** offers 120 on the Mac: speed judged right by the user; see [Frame rate](#frame-rate) |

## Files and shaders

- `SparkingZERO/Content/Paks/`: 18 containers `pakchunk0-17-Switch.utoc` / `.ucas` (TOC version 5, Oodle in 64 KB blocks, encrypted, directory index encrypted too; `pakchunk0` 280,679 chunks, `pakchunk1` 72,883, the others about 195 each, localized packages mounted at `../../../SparkingZERO/Content/L10N/<language>/…`), one pak beside each, and `global.utoc` (script objects). Every pak's index is encrypted with the key `main` holds (GUID zero). `pakchunk0-Switch.pak` (3.2 GB, 5,003 files: 3,438 `.res`, 648 `.awb`, 341 `.usm` movies, localization, configs, fonts and the global shader cache) holds everything that is no cooked package; the other 17 paks are empty (365 bytes, no file, mount point `/`).
- Shaders: 14,842 unique (9,927 pixel, 3,698 vertex, 1,215 compute, 2 geometry), inline in the export bundles and `Engine/GlobalShaderCache-SP_GLSL_SWITCH.bin` (2,125 entries), each an Oodle stream of an `LSLG` record. The split takes about 30 s, a cold Mac build about 12 minutes on an M6: [Unreal Engine 5.1 shaders inline in encrypted IoStore containers](../../../docs/reference/shaders.md#unreal-engine-51-shaders-inline-in-encrypted-iostore-containers).

## Memory

- **Current: the allocator is the host's.** `nninitStartup` (`main+0x1e4d620`) reserves one 62 GB region for the engine (`main+0x1e4da70`, its size stored from x1 at `main+0x1e4da6c`; the rewriter's gate gives it what the alias region has room for). FMallocBinned3's constructor (`main+0x1eaacdc`, vtable `main+0x8703ab8`, name getter at slot 19, the 4.26 / 4.27 layout) is called at `main+0x1e4db30` in `nninitStartup` and at `main+0x1e4e048` in a getter, each `if the global at main+0x92ec6a0 is null, malloc 0x1c38 bytes, construct, store`. Until 2026-11-07 the rule took one call only, printed `[RED] … 2 calls and 0 jumps, not one call`, and Unreal's own allocator ran, mapping up to +38 GB of the region in the first 15 s ([Unreal's FMallocBinned3 replaced by the host allocator](../../../docs/reference/memory.md#unreals-fmallocbinned3-replaced-by-the-host-allocator)).
- On the Mac with the iPhone's alias region (`DM_ALIAS_SIZE=0xC8000000`: 3.12 GB) through the tutorial battle: `Unreal's engine region: 62.00 GB asked, 2.81 GB of address space`, the alias region mapped up to +0.51 GB, the Unreal heap up to 1.25 GB, about 2.8–3.2 GB resident and 0.75–0.93 GB of Metal allocations.

## Frame rate

- **Current:** 60 fps (30 on the Switch), by the host's writes of `rhi.SyncInterval` (2 → 1) and of the engine's fixed frame rate (`main: Unreal fixed frame rate (GEngine->FixedFrameRate) 30 → 60 fps`). `DM_FPS_TARGET=30` keeps the game's own 30.
- **The battles step by the engine's fixed frame rate.** `SparkingZERO/Config/DefaultEngine.ini` sets `bUseFixedFrameRate=True` and `FixedFrameRate=30.000000`: Unreal steps every frame by exactly 1 / `FixedFrameRate` s and holds its tick to that rate. Unreal's Switch frame pace (`main+0x1e1e904`, mode 0 / 1 / 2) caches `rhi.SyncInterval`, sets it to 1 for mode 2 and 2 for the others, and stores {30, 24, 60}[mode] into `GEngine->FixedFrameRate` (`GEngine` at `main+0x9aefa30`, through the GOT slot `main+0x8dbfb40`; the property at `+0x9c8`, its reflection entry at `main+0x895f6a0`). The title runs near 60 at the game's own setting; the battles and their cinematics at 30. The game sets no `t.MaxFPS`. The rewriter finds the property and `GEngine` without a build ID: [Unreal's fixed frame rate, found by its reflection entry](../../../docs/reference/frame-rate.md#unreals-fixed-frame-rate-found-by-its-reflection-entry).
- With `rhi.SyncInterval` alone (2026-11-07, before that rule) the battles stayed at 30.0 fps.
- **Speed at 60** (2026-11-08, headless runs from `run/save`, `run/ab2.sh`, a shot a second through the tutorial battle's start, `DM_FOCUS_KEEP=1 DM_WINDOW_TOP=1`): the battle at 56–60 fps. The counter at the HUD's top left stepped every 13 s at 30 and every 14 s at 60 (shots a second apart). The runs' timed presses landed at different points (the title came sooner in the 60 run), so their scripted events do not line up. The user then played the tutorial battle in a window and found the timing right at 60.
- **120 on the Mac when chosen** (`fps_max 120`; [120 fps follows the frame step](../../../docs/reference/frame-rate.md#120-fps-follows-the-frame-step)): the host's write follows the hold (`30 → 120`, `120 → 60` while a movie plays, `60 → 120` after). The user played about 3 minutes in a window with the Metal HUD and sound on 2026-11-08 and found the speed right: 119.3 fps in most windows of 300 frames, 86–117 in a few, 59.8 in one with a movie. Not measured with inputs repeated at both rates.

## Rendering

- **Flat green landscape (resolved 2026-11-10 on the Mac; the iPhone open).** On the Mac (M1, 2026-11-10, base v0 packed from a derived profile) the stage's floor was one flat lit green with no texture; rocks and characters right; no red line. The same picture is [#1889](https://github.com/seedds/nxnative/issues/1889) on an iPhone 17 (a grid of repeating tiles). The floor is Unreal's runtime virtual texture (`SW_Map007_Landscape.umap`, pixel shader `6c9d24ca7bf78489`, `reg130` the page table, `reg129` / `reg128` the BC3 sRGB and BC3 tile caches of 4488x4488, 33x33 tiles of 136 texels, `reg6` the VT uniforms: 512x512 pages, 8 levels, page size 128 / 4488). A texture dump showed both caches full of varied grass and dirt blocks (the compressed-alias copy, [Copies into an uncompressed alias of a compressed texture](../../../docs/reference/rendering.md#copies-into-an-uncompressed-alias-of-a-compressed-texture), works here), and the page table's level 0 holding 255 in all 262,144 entries. The page table update draws (passes of 48 indices x 3 instances into each mip of the R16Uint table, vertex shader `478292f0f6d79ae4`, pixel shader `17eda07e4bac39f8`) wrote a flat input and three zeros through a `float4` output, as the shader had no integer entry point: [Integer entry points for flat inputs passed through beside constants](../../../docs/reference/shaders.md#integer-entry-points-for-flat-inputs-passed-through-beside-constants). Rebuilt with the change, the floor of the Piccolo and Gohan battle shows its dirt, grass patches and tracks at the same single known red line; the user played it at 60 fps.
- Dumps on the Mac: `DM_CTL=<file>` with `passdump 1` and `texraw <serial> <path>` for the page table and the caches; the update passes attach the table's mips one by one (`P … c0 ser=<table> fmt=23 512x512 … lv<n>`).

## Mac routes

- Work folder scripts: `run/boot.sh <log> [seconds] [VAR=value …]` (headless unless `DM_WINDOW=1`, the Metal HUD with any window, `DM_CTL=run/ctl.txt`, the host copy `build/dbzhost`), `run/see.sh <shot> <seconds> [keys …]` (presses, then a screenshot to `run/snap_<shot>.png`), `run/ab2.sh <tag> [VAR=value …]` (a copy of `run/save`, the route into the tutorial battle, a shot a second for 40 s), `run/sheet.py <tag> 40` (the shots on one sheet).
- Empty save: A at the strap notice (about 30 s in), A through the two add-on notices, Left A to accept the privacy notice, Y to confirm the language. The opening follows.
- With a save: A at the strap notice (about 40 s in), A at the autosave notice, then the title (A). The intro movie plays and goes on into the tutorial battle; Plus pauses it (Continue Battle, Skip Tutorial, Explanation of Controls). The left stick moves, L jumps and flies, Y and A attack.

## Found on this game

General changes first needed here; each applies to every game:

- **Encrypted Unreal paks and IoStore containers with the shaders inline**, the empty paks beside the containers, labels under a container's mount point, and `LSLG` records in Oodle shader tables (`uepak.py`, `shader_split.py`): [Unreal Engine 5.1 shaders inline in encrypted IoStore containers](../../../docs/reference/shaders.md#unreal-engine-51-shaders-inline-in-encrypted-iostore-containers).
- **Every call of FMallocBinned3's constructor cut** (`ue_malloc.py`): [Unreal's FMallocBinned3 replaced by the host allocator](../../../docs/reference/memory.md#unreals-fmallocbinned3-replaced-by-the-host-allocator).
- **Unreal's fixed frame rate written at `fps_target`** (`ue_fixed_rate.py`, `hle_fpslock.c`): [Unreal's fixed frame rate, found by its reflection entry](../../../docs/reference/frame-rate.md#unreals-fixed-frame-rate-found-by-its-reflection-entry).
- **A `WaitSync` on a sync not yet fenced passes** (`nvn_queue.inc`): since the NVN queue scheduler landed (ea0c98df), the game stopped presenting at its first load after the title. A wait before the queue's first fence of that sync parked the queue for good. Resolved: [GPU event waits order work across queues](../../../docs/reference/nvn-objects.md#gpu-event-waits-order-work-across-queues) ([#1808](https://github.com/seedds/nxnative/issues/1808)).
- **A forked shader worker died opening the compile cache** (`shader_cache.py`, `dmsh_build.py`): its 14,040-shader Mac build stopped with `BrokenProcessPool` after 3 to 4 s on a reporter's Mac, and built with `DMSH_PROCS=0`. Resolved: [A forked shader worker dies opening the compile cache](../../../docs/reference/shaders.md#a-forked-shader-worker-dies-opening-the-compile-cache) ([#2354](https://github.com/seedds/nxnative/issues/2354)).

- **The virtual texture page table written as float** (`dmsh_entry.py` `fs_uint`, `nvn_mtl.m` `pso_desc`): [Integer entry points for flat inputs passed through beside constants](../../../docs/reference/shaders.md#integer-entry-points-for-flat-inputs-passed-through-beside-constants) ([#1889](https://github.com/seedds/nxnative/issues/1889)).

## Open

- The landscape after the page table fix on an iPhone ([#1889](https://github.com/seedds/nxnative/issues/1889)): checked on the Mac only; see [Rendering](#rendering).
- Past the tutorial's first steps: the rest of the tutorial, the episodes, other stages and characters, the add-on characters.
- Speed at 60 and 120 judged by eye in the tutorial battle only; not compared over the same inputs at both rates.
- No IPA built, nothing run on an iPhone.
