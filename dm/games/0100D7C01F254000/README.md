# DRAGON QUEST I & II HD-2D Remake (`0100D7C01F254000`)

Update 1.0.2 (v131072), SDK 18.3.2. Square Enix / Artdink, Unreal Engine 4.27.2, NVN, CRIWARE sound and Sofdec movies. Modules `main`, `subsdk0` (`NintendoSDK_movie`) and `sdk`.

Mac gameplay tested on an M4 Pro on 2026-10-31. Both games boot, create adventure logs and resume their autosaves after restarting.

- DQ I reaches free movement in Tantegel Castle after the opening and king's dialogue. Its early test used an incomplete shader subset.
- DQ II reaches Midenhall after companion naming, the Moonbrooke attack and the king's dialogue, then continues down the castle stairs into town. Movement, camera, running, minimap, dialogue and tutorial cards work, including inline controller icons. A clean restart renders the castle exterior with no missing-shader errors; the route continues through the market to the south gate.

The complete Mac shader build, GPU pipeline archive and self-contained Mac app passed build and signature checks on 2026-11-01. No iPhone build or test yet.

## What it needs

| Setting | Why |
|---|---|
| `depth_mode zero` | The registered build selects near-zero NVN depth; offline pipeline bases must use the same vertex entry. |
| `fps_target 60` | The shared Unreal console-variable rule sets `rhi.SyncInterval=1`; see [Frame rate](#frame-rate). |
| General FMallocBinned3 and engine-region rules | Unreal reserves 62 GiB and normally builds large allocator pools. The shared rewriter and host use the host allocator and fit the engine region into the profile's alias space. |
| SDK function-matched node-area and alignment cuts | SDK build `f118ae1c…` matches the shared AddressRegionManager cuts: 62 MiB node area and 2 MiB region alignment. |
| `mem_reserve 0xF0000000`, `mem_alias 0xC8000000`, `mem_stack 0x10000000` | A 3.75 GiB block, 3.125 GiB alias region, 256 MiB stacks and 128 MiB heap region, after the block's 256 MiB prefix. |
| `app_pool 0xCD500000`, `sys_resource 0x5000000` | Application memory budget and the NPDM's 80 MiB system resource. |
| `pad_layout full` | D-pad and both sticks for the 3D interface. |

No game-specific code patch or fixed-address observation gate is needed.

## Frame rate

The base pak's `Engine/Platforms/Switch/Config/BaseSwitchEngine.ini` sets `r.VSync=1`; `Nicola/Config/Switch/SwitchDeviceProfiles.ini` sets `rhi.syncinterval=2` and `r.Streaming.PoolSize=850`. The patch does not override these settings. Across all 44 config files there is no `FrameRateLimit` or `t.MaxFPS` entry. The shared Unreal present-interval rule applies; no fixed frame-rate address or streaming-pool override is needed. `DM_FPS_TARGET=30` keeps the game's original rate. Gameplay speed at 60 fps remains to be measured.

## Memory

The normal-layout startup log reports `62.00 GB asked, 2.81 GB of address space`. The script's earlier region-check log deliberately used a 64 GiB alias region to measure the uncapped request; its 62 GiB report is not a failed cut. The initial heap request is about 6 MiB. A ten-minute diagnostic-subset run through DQ II's opening and Midenhall peaked at 4,237 MiB physical footprint; the last five minutes ranged from 4,035 to 4,137 MiB. That preview did not use uniform extent tables; memory with the completed build remains unmeasured. The shared [Unreal allocator and engine-region rules](../01003E601E324000/README.md#memory) apply without game-specific cuts.

## Files and shaders

Two encrypted-index version 11 paks under `Nicola/Content/Paks/`: the base `Nicola-Switch.pak` and update `Nicola-Switch_0_P.pak`, about 8.22 GiB combined. The base has 268,956 entries and the patch 10,430. The shared pak reader obtains the AES key from the game's code; no key belongs in this profile. Shader code archives are LZ4 compressed and split into global and game packages, with 69,924 unique shaders. The complete Mac build on 2026-11-01 has 69,924 successful records, no failed side libraries, no unrecognized bindings and no driver reads outside the host's filled ranges. Its chain is `e65a4ebd3322`, with profile input digest `9bcfe89d0d4a`.

The six DLC entries are licence-only. Their ownership metadata stays beside the build; no extra content files are required for startup.

The Mac archive contains 5,324 pipeline bases for `applegpu_g16s`. Five layer-writing vertex shaders are excluded as they are in the host. The completed libraries and uniform tables have not had another gameplay replay; testing stopped at the user's request to save time.

## Mac routes

Open `DRAGON QUEST I & II HD-2D Remake.app` beside the NSP build folder to choose settings and play. The bundle contains the rewritten modules, game files, fonts, DLC ownership metadata, full Mac libraries and their matching uniform tables.

Game-derived artifacts stay beside the NSPs in `0100D7C01F254000-work/`. Its `run/boot.sh LOG SECONDS [VAR=value ...]` runs the host and rewritten modules with isolated test saves; `run/ctl.sh` drives input and captures frames. Diagnostic subset libraries are in the build's `shader/bringup_cache`; full libraries are in `shader/cache`. Missing-stamp warnings apply only to the diagnostic subset, which is not a completed shader build.

In event dialogue, hold X to fast-forward; A advances and confirms the default answer. B closes the tutorial cards before the left stick can move the player. The right stick rotates the camera. The combined title leads to a game selector; wait for its transition before pressing Left or Right.

## Found on this game

- [ADR-relative jump tables with 64-bit bounds](../../../docs/reference/rewriter.md#adr-relative-jump-tables-with-64-bit-bounds): the main module's `cmp x9,#4` exposed the shared recognizer's width assumption. The fix follows register-copy provenance and accepts either comparison width.

- [Partial vertex builds missing paired pixel sources](../../../docs/reference/shaders.md#partial-vertex-builds-missing-paired-pixel-sources): fifteen startup-cache vertices lacked the paired pixel inputs' default outputs. The general dependency and cache-publication fix rebuilds them before the Mac archive is packed.

## Open

- Visual replay and memory measurement with the full libraries.
- Both games' combat and later areas. Ten shader records carry the translator's unresolved `imageBuffer` handle-source diagnostic; those paths were not played.
- Gameplay speed comparison at 30 and 60 fps, and memory stability after loading both games.
- Unsigned iPhone build and user-run iPhone validation.
