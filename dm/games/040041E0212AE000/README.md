# Fast Fusion (`040041E0212AE000`)

A Switch 2 game: v262144, SDK 20.5.13 (`Ounce`), NVN2, Shin'en's engine. Modules `main` and `sdk`. **Plays on the Mac** (M6, 16 GB, macOS 27.2, 2026-11-14): title, Championship menus, the first available race, acceleration and scenery streaming, with sound enabled. The final race run renders the sky and volumetric fog; the earlier black sky was the missing fog dispatch described below. Testing totalled 9 minutes 50 seconds, including starts, menus and a final 30-second boot/menu check after rebasing onto current main; no race was completed. No iPhone or iPad test or IPA build.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `modules main sdk` | [`game.tsv`](game.tsv) | the host supplies the imports that otherwise reach `subsdk0` and `subsdk1`; neither needs to load |
| `mem_reserve 0x270000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0x240000000` | `game.tsv` | the Switch 2's 9 GB pool; the game requests an 8.94 GB heap in the 9 GB heap region |
| `sys_resource 0x0` | `game.tsv` | the NPDM declares no system resource, so virtual address memory is off |
| `depth_mode zero` | `game.tsv` | the game sets near = 0; the offline pipeline archive must use the same depth entry |
| `shader_groups`, `shader_note` | `game.tsv` | name the packages of loose shaders, scene resources and the NDSR pack |

There is no game-specific rewriter patch, host branch or shader override. The shared resource reader has `engine` binding through the `SHN!` / `SHDROUNC` signatures; the instruction and texture-table fixes have `host` binding through NVN2 semantics. Both are at their loosest correct binding. The build row registers this main build only (`e76e60ef4fe21decb40223b5de5e6396`); other versions have not been tested.

## Files and shaders

- The supplied NUP is named v0 but contains a standalone update v262144, with no base package, control data or icon. The actual display version is unknown; the app bundle uses the packer's fallback version 1.0. The update holds all files needed by the tested route.
- RomFS: 3,355 files, 3.62 GB. The `SHN!` wrapper also carries textures, particles and other resources; finding that magic alone does not identify a shader.
- 152 loose `.shdr` files and 402 `.w3d` scene files contain 2,948 unique shader stages in `SHDROUNC` objects. All 26 shader control keys and GPU-code areas captured at startup match the offline extraction byte for byte. See [SHN resources and embedded shader objects](../../../docs/reference/shaders.md#shn-resources-and-embedded-shader-objects).
- The loose NDSR pack contributes 28 more stages. [`shader_map.tsv`](shader_map.tsv) has 2,976: 1,398 vertex, 1,431 pixel, 85 compute, 31 task and 31 mesh. Every shader builds on the Mac.
- The task and mesh programs need [subword stage-memory accesses](../../../docs/reference/shaders.md#subword-task-and-mesh-payload-accesses). Before those were implemented, all 62 failed to translate.
- `cded8d0899ba4a6d` (`shaders/volumetricfog.shdr`) needs [indexed texture handles in NVN2 compute c0](../../../docs/reference/shaders.md#indexed-texture-handles-in-nvn2-compute-c0). Before the fix its reflection could not load and the race's sky was black. All 2,975 other base Metal shader sources are byte-identical across that fix.
- [`pso_manifest.tsv`](pso_manifest.tsv) records the title, menus and first race: 932 shader, 292 render-pipeline and 2 compute-pipeline rows. It does not cover the other courses.

## Running on the Mac

Build with `python3 scripts/build_ipa_from_nsp.py <folder> --platform mac-app`. The Mac app is self-contained. The NUP has no icon, so `--icon <image>` is needed for a game-specific app icon.

For development, use `--platform mac` and the command it prints, with `DM_WINDOW=1 DM_WINDOW_TOP=1 DM_METAL_HUD=1`. The work folder on this machine is `/Volumes/S790/nsp/Fast Fusion/040041E0212AE000-work`; `run/boot.sh <log> <seconds> [VAR=value ...]` starts its own renamed host, with `DM_CTL=run/ctl.txt`. `DM_FOCUS_KEEP=1` keeps scripted runs progressing when another app takes focus. The test's memory cap is 10 GB.

From the title: `L+R`, then four separate `A` presses select Championship, the first league, the first cup and the first vehicle/start. Wait between menu transitions. `A` accelerates; `LLeft` / `LRight` steer. A frame-based route from a fresh start is `f600:L+R~f30,f780:A,f900:A,f1020:A,f1140:A,f2200:A~f2400`. Capture after frame 2400 for an early race frame. The first shader and pipeline preparation can delay the first start.

## Rendering and memory

The race evaluates DLSS at 960x540 to 1920x1080; the shared MetalFX temporal scaler answers it. The window's captured surface is 3840x2160. Resident memory settled around 6.7–7.0 GB during the first race; GPU allocation about 6.3–6.6 GB, with no sustained growth over the observed interval. On the M6 above, the final run's frame-3000 sample reports frame time p50 34.19 ms (about 29 fps), p95 59.81 ms; GPU time p50 20.30 ms, p95 32.77 ms. This is a diagnostic run with the Metal HUD, shader/pipeline recording and frame captures, not a controlled performance benchmark; the capture at frame 2400 caused a roughly one-second hitch. No performance optimization or comparison against a correctly rendered earlier build is claimed.

## Validation

All 2,976 shaders build on the Mac. The focused graphics/parser suite runs 181 tests with no failures (one optional test skipped), including GPU checks of subword shared stores and synthetic indexed NVN2 texture tables. After rebasing, all 38 documentation, game-profile, chain and environment-switch checks and all 70 Mac packaging tests pass. Python lint adds no findings. The initial broader tool run's environment-switch documentation failure is fixed by current main; its three .NET runtime-selection failures pass when allowed to use the installed .NET 11 runtime.

The broader native graphics suite completes its other targets but fails the clock test's two-nanosecond feedback-conversion tolerance intermittently (`feedback conversion uses the resumed guest epoch`); the same binary passes when rerun alone. The clock implementation and test are unchanged by this port. This broad run is not a clean pass.

Regression level: moderate, because shared shader extraction and translation changed, with the new paths gated by resource signatures or NVN2 instruction/layout semantics. The Binding of Isaac Rebirth is the light original-NVN smoke test: its 19 translated shaders are unchanged, it reaches the same route with no new red log messages, and `regress.py check --quick` passes. The Mac was busy during that run, so its timing differences are not treated as performance evidence. Fast Fusion itself covers the new paths through the title, menus and first race described above.

## Found on this game

- [SHN resources and embedded shader objects](../../../docs/reference/shaders.md#shn-resources-and-embedded-shader-objects).
- [Subword task and mesh payload accesses](../../../docs/reference/shaders.md#subword-task-and-mesh-payload-accesses).
- [Indexed texture handles in NVN2 compute c0](../../../docs/reference/shaders.md#indexed-texture-handles-in-nvn2-compute-c0).
- [DLSS scalers released under their encoders](../../../docs/reference/rendering.md#dlss-scalers-released-under-their-encoders): already fixed before this bring-up; the earlier report reached its eighth network after about nine minutes.

## Open

- Later courses, a complete race, multiplayer, online modes and frame generation have not been tested.
- The log still reports an unbound vertex uniform slot in the startup effect (`eb5ec3e356bf7ad1`, slot 1), two unimplemented DLSS overlay setters called with zero, and a cube view of a one-face texture. Their effects outside the tested route are not established; none was hidden with a profile switch or stub.
- No on-device iOS validation. The app's icon is absent because the supplied NUP carries none.
