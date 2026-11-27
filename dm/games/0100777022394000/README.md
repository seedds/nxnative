# Cozy Land (`0100777022394000`)

Base game 1.0.0 (v0), SDK 17.5.8. Unity 2023.2.20f1 IL2CPP with FMOD on NVN. Modules `main`, `subsdk0` (the movie module: the RedDeer.Games splash and the title's background videos, H.264 with AAC, demuxed by the native `movie::Extractor` from Unity's own stream reader) and `sdk`.

Original bring-up reported in [PR #2167](https://github.com/seedds/nxnative/pull/2167): on an M2 Max, the splash, title menu and seven minutes in the builder (category/item lists, placing houses, shops and decorations, zooming and reshaping the plot) at 60 fps with no red line; 2.2–2.6 GB resident and 1.2–1.5 GB of Metal allocations. The author also played on an iPhone19,4 (A20 Pro, iOS 27.3 beta); a short launch check logged 60 fps, a 0.96 GB footprint and no red line. These runs used the original H.264 change, not the codec-backed replacement now on main; current verification is recorded below.

## What it needs

| Setting | Where | Why |
|---|---|---|
| Node area cut 1 GB → 62 MB | `dm/tools/sdk_patches.py`: the SDK-wide rule (`MATCH_CUTS`) of the SDK 15.3.2 to 18.3.5 constructor, which the `sdk` build `71b1a025…` has at Ace Attorney Investigations Collection's SDK 17.5.3 offsets (`sdk+0x139458`, the movs at `+0x139468` and `+0x1394fc`) | `StandardAllocator::Initialize` aborted in `nninitStartup`; see [Memory](#memory) |
| Codec-backed H.264 output and release-driven pumping | existing host, `hle_movie_avc.c` / `hle_movie.c` | Unity never sends EOS for the splash; see [Movies](#movies) |
| `sys_resource 0x1000000` | [`game.tsv`](game.tsv) | the NPDM's 16 MB of system resource: virtual address memory |
| 5 GB guest layout: `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000`, `stdalloc_cap 0x40000000` | `game.tsv` | the heap/address check measured initial alias use and selected a 1 GB pool; see [Memory](#memory) |
| `pad_layout full` | `game.tsv` | the D-pad zooms and reshapes the plot; see [Input](#input) |
| `depth_mode zero` | `game.tsv` | the game selects near = 0; pipeline prebuilding must use the same vertex entry |
| `shader_groups /Data/=data` | `game.tsv` | everything is under `Data/`: the player's serialized files and the Addressables bundles in `StreamingAssets/aa` |

## Memory

- The NPDM declares 16 MB of system resource, so `IsVirtualAddressMemoryEnabled` is true: `nninitStartup` (`main+0x907af0`) hands application pool − used memory (2 MB aligned) to StandardAllocator as virtual address memory and never calls `SetMemoryHeapSize`.
- **Resolved: an `nn::diag` abort at `sdk+0x12b1a0` from `StandardAllocator::Initialize` (`main+0x907b5c`) before `nnMain`**; the original heap check stopped before `nnMain` with exit 134. The existing [SDK address-region rules](../../../docs/reference/memory.md#the-sdks-address-region-node-area) supply both the node-area reduction and 2 MB alignment. No game-specific `CUTS` row or `patches.py` is needed. The heap check selected a 1 GB pool after observing the hardware pool's region reach alias +4.26 GB.
- The original seven-minute Mac run reported 2.0 GB mapped, alias high-water +3.75 GB against the profile's 4.375 GB alias region, and 2.2–2.6 GB resident. Current Mac hosts reserve at least the Switch's 64 GB alias region; those historical numbers are not a new test of the iPhone layout.
- The current rewrite, using only SDK-wide rules, matches the node area at `sdk+0x139468` / `+0x1394fc` (implementation 4 of 9) and alignment at `+0x139654` / `+0x13965c` / `+0x139660` (4 of 4). The resulting modules reach the menu with the host fix on main.

## Movies

- The splash is a 4.3 s RedDeer.Games logo (High profile, I B B P, 29.97 fps, AAC) stored inside `Data/sharedassets0.resource`; the title menu plays background videos as well.
- **Resolved: the splash stayed on its final red screen.** Unity's feeder (`main+0xf12020`) sends EOS only when `GetSampleTime` succeeds after the last sample, but the native extractor returns `ERROR_END_OF_STREAM` (0x2000). The host retained four pictures, so Unity's frame queue (`main+0xf0f9f4`, count at +0x2a0) never emptied. The existing [H.264 output without end of stream](../../../docs/reference/movies.md#h264-output-without-end-of-stream) fix uses codec picture order and buffer occupancy, not an inferred PTS interval. No game-specific movie patch is needed.
- On an M6 Mac, host fix `48808752` reaches New Creation / Load Creation without input. In the controlled comparison, the menu is visible by 20 s while main `d6585a2de864` stays on the splash throughout 35 s. The menu's Baseline H.264 background plays too. Real VideoToolbox replay delivers 129 of the splash's 130 pictures in order before EOS, versus 126 on the old host; the remaining reference leaves when the replay supplies EOS. The game itself never needs to submit it to reach the menu.

## Pipeline depth

- **Resolved profile mismatch:** the game calls `nvnDeviceSetDepthMode` with near = 0. The original profile omitted `depth_mode`, so packaging defaulted to GL and stamped its archive `depth near=-w`, while runtime selected `main0_z`. On M6 the original prebuild spent 36.2 s making 3,728 bases for the wrong mode. Runtime rendering already follows the game; the mismatch costs compilation, not a forced change of projection.
- `depth_mode zero` uses the existing [pipeline depth selection](../../../docs/reference/shaders.md#offline-pipeline-archive-and-profile-depth-disagree). It changes the archive's inputs, not shader translation or the host. The prewarm seed list stays empty; pair prebuilding already covers the shader pairs, and no additional seed-list benefit has been measured.
- **Verified:** the normal NSP Mac build now stamps `depth near=0` and builds 3,728 bases from 3,743 pairs in 29.8 s on M6, with 15 left to their draws and zero failures. A fresh-save launch using that archive also logs near = 0 / `main0_z` and skips the full pair prebuild. Runtime counters report six archive lookups found and none missed, plus 32 additional base compiles over the route: this does not eliminate all first-use compilation. The timings are observations of separate builds, not a controlled speed comparison.

## Input

- Controller: the left stick moves the cursor, A places, B goes back, Y opens the category's item list, L / R switch the category, the D-pad zooms (up / down) and changes the plot's shape (left / right) (the HUD also shows X for delete and ZL / ZR for turning, not tried). It stays docked at 1920x1080 with the 3D pad layout, which has a D-pad. Whether it reads touch has not been tried.

## Files and shaders

- 7,554 shaders in the player's serialized files and the 4 Addressables bundles under `Data/`, and the player's own GLSLC outputs in `main`. Unity 2023.2's `Shader` objects needed no layout written out.
- **Resolved:** the original unused image-atomic compute shader failure and 59 deleted bindless accesses in 36 shaders no longer occur. The initial verification on chain `a5415a3e93e1` builds all 7,554 Mac shader outputs with zero translation losses, undecoded bindings or unfillable driver-bank reads; all 3,743 vertex/pixel pairs pass interface checks. The landing rebuild on chain `d5cd712acdf5` also translates all 7,554 successfully with no losses or undecoded bindings. No shader inputs or chain rules are changed by this profile correction.

## Current verification

- 2026-11-17, M6 Mac, host `48808752`: a 50-second fresh-save run with the corrected archive covers the splash, animated menu, New Creation, the builder, item selection, floor placement, cursor movement, category switching and a house preview. Screenshots confirm the menu at frame 300 and builder at frame 780; the floor remains placed after changing categories. No red diagnostic, missing shader or depth mismatch occurs.
- Builder windows at frames 1,800–2,700 report frame-time p50 16.66–16.67 ms and GPU p50 4.80–4.85 ms; footprint is about 2.5 GB. This establishes short-route playability, not a speedup or long-session memory result.
- The normal `build_ipa_from_nsp.py --package v0 --platform mac` path selects this repository profile, reuses all 7,554 shader outputs with unchanged inputs, builds the host and regenerates the corrected prebuild. Profile and documentation checks pass. Runtime depth is also checked with the regression harness's existing profile/depth comparison.
- **Landing recheck:** after rebasing onto main `b3fe7770`, the same normal build recompiles all 7,554 shaders for its new chain `d5cd712acdf5`. The prebuild revalidates 3,728 archived bases from 3,743 pairs in 10.7 s (15 left to their draws, zero failures) and stamps the new chain with `near=0`. Another 50-second fresh-save run repeats the route above, including the menu, placed floor and house preview, with no red diagnostic or depth mismatch. It skips full pair prebuilding at launch and finds all 22 archive lookups with zero new base compiles. This run reuses the already-warmed archive; it is not a cold-cache comparison.
- Regression risk is low: only this game's profile, tables and notes change. No host, rewriter or shader-chain change, and no additional other-game regression run. iOS was not rebuilt or retested for this correction; the older device report is attributed above.
- Run artifacts are under `<NSP folder>/0100777022394000-work/pr2167-corrected-prebuild/` and `pr2167-rebased-prebuild/`; each run uses an isolated save and the normal build's `run/pso_archive`.

## Mac routes

- From a fresh save, the title menu comes up about 15 s after launch (after the splash); A on New Creation opens the builder. Then `R` / `Y` / `A` pick a category and an item and place it, `LLeft` / `LUp` move the cursor.

## Found on this game

General mechanisms used by this game:

- [H.264 output without end of stream](../../../docs/reference/movies.md#h264-output-without-end-of-stream): codec-backed readiness and output resumed by a released slot.
- Existing [SDK address-region rules](../../../docs/reference/memory.md#the-sdks-address-region-node-area) and [pipeline depth selection](../../../docs/reference/shaders.md#offline-pipeline-archive-and-profile-depth-disagree) apply without new code.

## Open

- The profile was migrated from `app_pool 0x40000000` to the hardware pool (`app_pool 0xCD500000`) plus `stdalloc_cap 0x40000000` (1 GB StandardAllocator cap; [The heap check's 1 GB pool picked a game's low graphics](../../../docs/reference/memory.md#the-heap-checks-1-gb-pool-picked-a-games-low-graphics)); re-testing under `DM_ALIAS_SIZE=<mem_alias>` on Mac / on iPhone with that layout remains open.
- Current iPhone gameplay, long-session memory behavior, touch input and the untried controls above remain unverified. The original iPhone report predates the landed H.264 replacement.
- The prewarm list (`pso_manifest.tsv`) is intentionally empty; there is no demonstrated need for additional seeds.
