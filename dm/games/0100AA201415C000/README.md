# FINAL FANTASY V (`0100AA201415C000`)

The Pixel Remaster, update 1.2.0_5 (v393216) over the base game. Unity 2020.3.37f1 IL2CPP, SDK 14.3.0 and NVN 55.15. Modules `main`, `subsdk0` (NintendoSDK_movie) and `sdk`. Plays on the Mac: opening scenes, world-map movement on Boko, menus, saving, and entry into Tycoon Meteorite. iPhone and iPad remain untested.

## What it needs

| Setting | Why |
|---|---|
| `sys_resource 0x1000000` | The NPDM declares 16 MB of system resource, enabling virtual address memory |
| `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000` | The 5 GB guest layout, with 4.38 GB available for alias regions on iOS |
| `app_pool 0x80000000` | A 2 GB application pool leaves room beside StandardAllocator's reservation for Unity's other address regions; the survey's 3.21 GB pool exceeded the iOS alias region in gameplay |
| `depth_mode zero` | The game calls `nvnDeviceSetDepthMode` with near = 0; offline pipeline bases must use that convention |

There are no game-specific patches, frame-rate overrides or graphics switches. The existing function-matched SDK rules reduce the address-region node area and align large regions to 2 MB ([SDK address-region rules](../../../docs/reference/memory.md#the-sdks-address-region-node-area)). Its `sdk` build is shared with [Chained Echoes](../0100C510166F0000/README.md).

## Memory

- **Resolved profile guess:** with the survey's 3.21 GB pool, StandardAllocator reserves 3.14 GB at alias +0.06 GB. The initial 15-second check reaches +4.20 GB, but the opening and world-map run reaches at least +4.76 GB. The Mac's default 64 GB alias region accommodates that; the profile's iOS region does not. The final pool is 2 GB, rather than increasing the iOS reservation or adding a game-code cut.
- The first run's resident memory settled at 1,830 MB, with 660 MB allocated by Metal, through frames 23,700–26,700 on the world map. Map loads add allocations; no claim is made about a long session.
- **Current:** with the final 2 GB pool and `DM_ALIAS_SIZE=0x118000000` on the Mac, loading File 1 restores the saved world-map position. StandardAllocator reserves 1.94 GB, and the address-region high-water log reaches +3.26 GB of 4.38 GB. Resident memory is 1,722 MB, Metal allocations 559 MB; the loaded map runs at 60 fps (frame time p50 16.66–16.67 ms, p95 16.75–16.77 ms). This validates the smaller address layout on the Mac, not iOS device memory pressure.
- Device memory pressure and later areas are untested.

## Files and shaders

- RomFS: 2,561 files, 681,152,632-byte `RomFS.img`. Unity's serialized player files and 2,516 UnityFS bundles hold its resources. `Data/globalgamemanagers` identifies Unity 2020.3.37f1.
- 4,203 unique shaders: 2,071 vertex, 2,075 pixel, 56 compute and one geometry shader; 4,192 from Unity resources and 11 GLSLC outputs from `main`. The split supplies 2,071 vertex/pixel pairs.
- All 4,203 libraries compiled for both Mac and iOS on chain `66eab9c9a6b4`, with no translation failure, unrecognized binding or unfillable driver-bank read. The initial Mac pipeline prebuild made 2,058 bases from the pairs, with zero failures and 13 deferred to their draws.
- `pso_manifest.tsv` records 141 shaders and 79 render pipelines from the opening, world map, menus, saving and Tycoon Meteorite entry. `shader_seen.tsv` contains those 141 shader keys. Later scenes compile additional pipelines on first use.

## Frame rate and audio

- **Measured on Apple M6:** the game requests present interval 1 and runs at 60 fps. Near the meteor-area transition, 300-frame windows reported frame time p50 16.66–16.67 ms / p95 16.74–16.76 ms, GPU p50 1.41–1.72 ms, and CPU 34–39% of one core. This is a bring-up measurement, with no performance-change baseline.
- The AudioOut callback log reports 240 frames per callback and zero underruns through 94,000 callbacks in the first run. Sound content was not independently assessed.

## Mac routes

Build with `scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> --platform mac-app` for the app, or omit `--platform` for an unsigned IPA.

- From a fresh save, A advances the title and selects New Game. The opening visits Tycoon Castle, the pirate ship and Bartz's camp before player control on the world map. A advances dialogue; Plus closes the first controls tutorial.
- D-pad or left stick moves Boko. X opens the menu; from Items, Up twice selects Save, then A enters the slot list and A saves into an empty first slot. A closes the completion message, B returns and X closes the menu.
- Follow the valley east and then north into Tycoon Meteorite. The game autosaves on entering the area.
- Agent run artifacts are in `<NSP folder>/0100AA201415C000-work/`; `save_ref` preserves the world-map save before the meteor. The builder's live save is in `0100AA201415C000_build/run/save`.
- Loading File 1: select Load Game with A, select the file with A, then Left and A on the confirmation (which defaults to No). The automated layout check used `DM_KEYS="20:Left,20.5:A,24:Left,24.5:A,28:Left,28.5:A,32:Left,32.5:A,36:Left,36.5:A,40:Left,40.5:A"`; the repeated Left/A cycles handle menu transitions without accepting the default No.

## Testing and open work

- 2026-11-15, Mac (Apple M6): under eight minutes from boot through the opening, world-map movement, menus, a completed manual save, and Tycoon Meteorite entry/autosave. No crash, red line or missing shader in that run.
- A separate bounded layout check loaded the manual save with the final 2 GB pool and the iOS alias-region size. No red line, address-space failure or missing shader; the restored world map was captured before the 75-second watchdog ended the run (`sig=14`, expected).
- Built `FINAL FANTASY V.app` (about 1.0 GB, ad-hoc signature verified) and `FinalFantasyV.ipa` (707 MB, unsigned). The Mac gameplay checks used the development host; the packaged app was not separately launched. The IPA has not been installed or run on a device.
- No other-game regression run: this addition changes only this game's profile, tables and notes, with no host, rewriter or shader-chain change (low regression risk).
- Open: battles, later maps, a long session, physical controllers, and iOS device testing. Only scripted controls were exercised.

## Found on this game

- The existing [SDK address-region rules](../../../docs/reference/memory.md#the-sdks-address-region-node-area) apply without a new implementation or a build-address table.
