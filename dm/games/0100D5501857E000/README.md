# Sam & Max: The Devil's Playhouse (`0100D5501857E000`)

**Provisional Mac profile.** On November 20, 2026, the user played the rebuilt game and reported that it "runs great", then authorized landing the profile as provisional. Episode completion, save/reload and iPhone behavior remain unvalidated. The profile and verified shader tables are included; remaining checks are listed below.

Update 1.8.0 (v524288) on the base game v0, SDK 17.5.6. Telltale Tool: 41 `SM3_nx_*.ttarch2` archives, Lua scripts, FMOD sound through AudioOut and NVN. Modules `main` and `sdk` only; `main` imports `nn::ro`, but the RomFS has no `.nro`. Languages: en-US, en-GB, fr, de and ru.

## What it needs

The profile's memory and module values come from the NSP survey. No game-specific rewriter cut was used for the Mac build or playtest.

| Setting | Why |
|---|---|
| `sys_resource 0x0` | The NPDM declares no system resource, so `IsVirtualAddressMemoryEnabled` is false, as on Horizon |
| `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | The standard 4 GB layout; the game sets a heap of at most pool minus 64 MB |
| `shader_groups /=romfs` | Every extracted shader is in the RomFS |
| `depth_mode zero` | The Mac run logs `nvnDeviceSetDepthMode near=0`; offline pipeline bases must use the matching vertex entry point |

## Shaders

- **Checked on November 20, 2026:** all 41 archives from the merged v524288 NSP extraction: 27 `ZCTT`, 14 `NCTT`. The pre-integration splitter finds zero keys. The reader finds 24,243 unique shaders: 21,914 pixel, 2,305 vertex and 24 compute. Every control/code pair passes `check()` and the key set exactly matches the original PR.
- `SM3_nx_Shaders_mem.ttarch2` supplies all 24,243 keys. `SM3_nx_ShadersPatch.ttarch2` supplies 2,213; their keys and shader bytes are already in the base pack. No other archive supplies a shader.
- **Checked on November 20, 2026:** all 24,243 Mac libraries build with chain `0724088d3779` on an Apple M6 (`Mac18,5`), macOS 27.2.1 (`26B131`), Xcode 27.2 (`27B52`). The build reports zero undecoded binding slots, zero unrecognized binding kinds, zero unfillable driver-segment reads and nothing lost without failing. All 24 regenerated compute-fact rows equal the PR's rows.
- **Checked on November 20, 2026:** the shipped prewarm list covers 277 render pipelines and two compute pipelines from the opening Mac playtest, including three demanded pipelines present only in the device-user log. Every shader reference/stage matches the shader map; the native key decoder accepts every render row and all pipelines compile with zero failures. The vertex/pixel pair tables remain empty. See [Prewarm validation](#prewarm-validation).

## Prewarm validation

The recording held 14,860 shader-only rows as well as its pipelines. The seed keeps render/compute rows in first-seen order and leaves shader-only rows out: the existing prewarmer loads each pipeline's shader dependencies itself. This avoids loading thousands of additional libraries before the first frame. No host or shader-chain change is needed; the shader-input digest remains `efe52859c6fc`.

On the M6/macOS/Xcode combination above, the native prebuild compiles all 277 render pipelines and two compute pipelines at `depth_mode zero`, with no rejected rows, stale shader keys or failures. Five short title runs use visible windows with the HUD, isolated empty saves, no pipeline archive and explicit manifests that exclude the device-user list. The system Metal cache is shared and was left intact. No new episode or save behavior was tested.

Two alternating A/B pairs tested the initial 274-render/two-compute candidate against an empty seed. The first 299 measured frames after the first frame show:

| Metric | Empty seed, run 1 | Seed, run 1 | Empty seed, run 2 | Seed, run 2 |
|---|---:|---:|---:|---:|
| Submit-thread pipeline waits | 315.2 ms | 0 ms | 5.7 ms | 0 ms |
| Longest frame | 238.65 ms | 35.33 ms | 33.67 ms | 33.45 ms |
| Prewarm before first frame | none | 794 ms | none | 21 ms |
| Footprint at frame 300 | 720 MiB | 739 MiB | 714 MiB | 744 MiB |

The repeat's empty-seed run already benefits from the system compiler cache. These results show the first-use work moving before the first frame; they do not establish a steady frame-rate gain. The final seed adds the three device-user demand rows and was checked separately: all 279 lines prepare in 78 ms with the system cache warm, zero submit-thread pipeline/shader-load waits, a 32.92 ms longest title frame, 741 MiB at frame 300 and no `[RED]` lines. Title screenshots were inspected; the animated background differs in phase between runs.

The list covers the recorded opening route only. Pipelines first encountered elsewhere still compile on demand; the existing device-user list records them for the next start. iPhone preparation time and memory are unmeasured.

## Mac validation

The author of [PR #1965](https://github.com/seedds/nxnative/pull/1965) reported 1,356 frames in 26 seconds on an M2 Pro/macOS 27.2 with `DM_WINDOW=1`, sound and a controller, with no red line, crash or audio underrun. Only the opening minute was inspected visually; no episode or save was tested.

**Checked on November 20, 2026:** the rebuilt Mac host reaches the title's "Press A to Start" screen in a visible window with audio and the Metal HUD enabled. No `[RED]` line appeared through frame 3,000. Frames 2,701–3,000 have frame time p50 16.67 ms / p95 16.72 ms and GPU p50 3.65 ms on the M6 above; a cold first-use pipeline compile caused a 315 ms frame. This is a title-screen observation, not episode or save validation. Input, pipeline and missing-shader recording are enabled for the user's playtest.

**User playtest on November 20, 2026:** the user reported that the game "runs great". The log reaches over 10,500 frames (about three minutes), with no `[RED]` line or crash and no audio underruns reported. The last complete 300-frame window has frame time p50 16.67 ms / p95 17.08 ms and GPU p50 6.03 ms; first-use pipeline compilation still causes hitches. The save directory contains no committed save, so save/reload remains unverified.

The PR author reported only six presented frames in 30 seconds without `DM_WINDOW=1`. The cause remains unknown and this mode was not retested here. Use a visible window for validation; this is a workaround for that separate failure, not a fix.

Build with `--platform mac`, then use the printed run command with `DM_WINDOW=1 DM_AUDIO=1 DM_METAL_HUD=1`. `--provisional` can still regenerate a survey-derived profile for comparison. Keep saves, logs, input recordings and pipeline recordings in the NSP work folder. The provisional label describes the validation status; the normal packer can use this registered profile.

## Found on this game

- **Telltale effect packs:** the archive decoder and bare shader records are a general engine-format mechanism: [Telltale effect packs in ttarch2 archives](../../../docs/reference/shaders.md#telltale-effect-packs-in-ttarch2-archives).

## Open

- Validate episode progression through completion and save/reload before removing the provisional label.
- Check rendering and first-use stalls beyond the recorded opening route; record a reproducible smoke route and additional pipelines where useful.
- Diagnose the reported no-window stall if it remains on current main.
- Only merged update v524288 has been extracted here; base-only play and other builds remain untested.
- Not tried on the iPhone.
