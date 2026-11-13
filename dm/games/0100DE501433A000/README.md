# Tetris Effect: Connected (`0100DE501433A000`)

Update 2.0.2.2 (v1310720) over the base game. SDK 13.3.2, NVN. Monstars and Resonair's game on Unreal Engine 4.27 (`++UE4+Release-4.27`; IoStore, MallocBinned3), with Epic Online Services through the Redpoint EOS plugin. Modules `main`, `subsdk0` and `sdk`.

Status: **plays on the Mac and on the iPhone.** On 2.0.2.2: the boot, the title, Journey from Area 1, and the MULTIPLAYER menus (PLAY → RANKED / FRIEND / LOCAL) on the Mac. On an iPhone (iPhone19,4, iOS 27.2, 2026-11-06): a steady 60 fps (frame time p99 16.67 ms), CPU about 85% of one core. Those runs were built by a front end ([NX2Apple](https://github.com/LNPANDY/NX2Apple)) on an earlier nxnative; on this repository's `main` the NSP script builds the game from this profile (see [Checked on main](#checked-on-main)). The base game 1.0.0 (v0) boots and opens MULTIPLAYER on the Mac with its own cuts; its shader tables are not here.

## What it needs

| Setting | Where | Why |
|---|---|---|
| The game layer's `L"RedpointEOS"` subsystem lookups point at `L"NULL"`: 32 adrp/add pairs in 1.0.0's `main`, 34 in 2.0.2's | [`patches.py`](patches.py) | see [Online](#online). Without them: `CRASH sig=11 addr=0` at `main+0x9e44cc` (1.0.0) or `main+0xb64bec` (2.0.2), about 3.5 s after boot |
| `main+0xb64e18` → `mov w0, #1; ret` (2.0.2 only) | `patches.py` | the game layer's "logged in to EOS" check; without it 2.0.2's MULTIPLAYER stays black. See [Online](#online) |
| `mem_reserve 0xF0000000`, `mem_alias 0xC8000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | [`game.tsv`](game.tsv) | the layout of the other Unreal 4.27 games (Mario & Luigi: Brothership, OCTOPATH TRAVELER II). The provisional 64 GB alias region failed on the iPhone with `FATAL reserve guest AS`. The engine-wide rules cut FMallocBinned3 and size Unreal's engine region to fit ([Unreal's engine address region and its options](../../../docs/reference/memory.md#unreals-engine-address-region-and-its-options)); the SDK-wide rules cut the node area on this `sdk` build |
| `sys_resource 0x5000000` | `game.tsv` | the NPDM declares 80 MB of system resource |
| `depth_mode zero` | `game.tsv` | the game calls `nvnDeviceSetDepthMode` with near = 0; the NSP script's heap check found it |
| `shader_groups`, `shader_note` | `game.tsv` | as the provisional profile wrote them, since both are shader inputs |

## Online

- **Current: offline, by two rewriter cuts in the game's own code.** The host never loads the EOS SDK ([Online SDKs in NROs are never loaded](../../../docs/reference/services.md#online-sdks-in-nros-are-never-loaded)), so the game's NRO loader (1.0.0: `main+0x8b2090`) leaves its "loaded" flag clear and the Redpoint plugin never makes its online subsystem: `FOnlineSubsystemModule::GetOnlineSubsystem(FName("RedpointEOS"))` returns null. The game's online layer (leaderboards, play logs, events, presence; 1.0.0 `main+0x9e4418..0xac6978`, 2.0.2 `main+0xb64b40..0xc55824`) asks for that subsystem by name and does not always check the result. The cuts make each of those adrp/add pairs load `L"NULL"` (Unreal's null subsystem, which the engine already makes) instead of `L"RedpointEOS"`. The plugin's own uses of the name (101 in 1.0.0, 124 in 2.0.2) are left as they are. This is a workaround in the game's code: the cause is the refused SDK, which the host leaves out on purpose.
- **Resolved: 2.0.2's MULTIPLAYER black** (audio on, B does nothing). The game layer's "logged in to EOS" check (`main+0xb64e18`; the debug readout `EOS LogIn: %d` calls it every frame) asks the subsystem, now the null one, for local user 0's login status. Nothing logs in through it, so 2.0.2 waited for a login forever. 1.0.0 opens the menu without it. The cut answers true. Found by probing `FName`'s constructor for UTF-16 `Redp` and following the per-frame backtrace. Two things tried first did not change the black screen: the host time for `StandardNetworkSystemClock`, and the lookups pointed at `L"Switch"` (OnlineSubsystemSwitch).
- Online modes (ranked, friend matches, leaderboards) need the network and Epic's services, so they do not work.

## Files and shaders

- `TetrisEffect/Content/Paks/TetrisEffect-Switch.pak` (index AES-encrypted; `uepak.py` finds the key in `main`) holds everything but the cooked packages and the global shader cache beside them. The packages are the export bundles of the IoStore container beside it (`TetrisEffect-Switch.utoc` / `.ucas`, Zlib, not encrypted). There is no `.ushaderbytecode`.
- Shaders: 11,923 unique (2,402 vertex, 8,782 pixel, 704 compute, 35 geometry), `LSLG` records inline in the packages ([Unreal Engine 4.27 shaders inline in IoStore export bundles](../../../docs/reference/shaders.md#unreal-engine-427-shaders-inline-in-iostore-export-bundles)) plus the global shader cache. The game has no PC version here, so the facts table holds only compute group sizes.
- [`pso_manifest.tsv`](pso_manifest.tsv): 1,926 shaders, 664 render and 14 compute pipelines, the Mac and iPhone play recordings of 2026-11-06 merged and kept to this version's shader map.

## Frame rate

- 60 fps on the iPhone. The visible problem there is first-time pipeline hitches: before the prewarm list, 18 of 33 long frames in a session were pipeline creation (about 690 ms in all, 50–150 ms each). With the list, prewarm prepares about 2,000 shaders and 450 pipelines at boot (about 1 s, in the background), but the hitches stayed at about 2 a minute: each loads shader libraries of content neither recording had played.
- A Metal 4 pipeline archive (`pso_mtl4 on`, `pso_gpu g18p g19p`) did not help: the iPhone opened the `applegpu_g19p` archive and passed its probe, but found none of its 647 lookups. The archive holds bases for the static `shader_pairs.tsv`, while play uses other pairs, and `pso_pairs.tsv` is empty. The profile has neither key.

## Checked on main

On `main` at `f63ecbd4` (chain `d6b05d7481e5`), M5 Pro, macOS 27.2, 2026-11-13: the NSP script (`--platform mac`) on the base game and 2.0.2.2 rewrites the three modules (2:28), splits 11,923 shaders whose data rows match this profile's tables (2:08), builds all 11,923 Mac shaders from an empty cache (7:15) and the pipeline prebuild (1:11). A headless boot from a fresh save goes through the logos to the title (`Version 2.0.2`) and the EULA in about 2 minutes, about 60 fps (7,200 frames in 125 s), with no red line and no crash; the log says `refused an NRR that registers only online SDKs`. No IPA was built from `main`.

## Open

- First-time pipeline hitches on the iPhone, about 2 a minute (above).
- Other Journey areas, Effect modes and Zone Battle were not played on 2.0.2.2.
- `pad_layout` keeps its default (`simple`); the touch controls were not checked for this game.
- Online modes (above).

## Found on this game

- [Online SDKs in NROs are never loaded](../../../docs/reference/services.md#online-sdks-in-nros-are-never-loaded): the game reads the vtable of the null subsystem the refusal leaves; the cuts above.
- [Unreal Engine 4.27 shaders inline in IoStore export bundles](../../../docs/reference/shaders.md#unreal-engine-427-shaders-inline-in-iostore-export-bundles): with an encrypted pak beside the container; `main` finds the same 11,923 shaders as an earlier reader written for this game (PR #1619, closed).
