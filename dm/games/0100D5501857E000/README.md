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
- **Open:** the shipped pipeline manifest and vertex/pixel pair tables are empty. The Mac playtest recorded 274 render-pipeline rows and two compute rows locally, but that recording has not been checked as a shipped prewarm list and stays outside the repository. The author observed first-use frames of 0.3–1 s.

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
- Check rendering and first-use stalls during that playtest; record a reproducible smoke route and pipelines where useful.
- Diagnose the reported no-window stall if it remains on current main.
- Only merged update v524288 has been extracted here; base-only play and other builds remain untested.
- Not tried on the iPhone.
