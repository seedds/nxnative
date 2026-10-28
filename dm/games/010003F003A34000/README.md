# Pokemon: Let's Go, Pikachu! (`010003F003A34000`)

Update 1.0.2, SDK 5.4.151, Game Freak's engine with NintendoWare, NVN and Wwise. Modules: `main`, `subsdk0` (GLSLC), `subsdk1` (movies), `sdk`. Main build ID: `c208db6a4ef4361f28b8402be0f7cd9a`.

## Status

Mac bring-up on an Apple M1 Pro with macOS 27.2 and Xcode 27.1. The user reports gameplay, sound and paired Joy-Con input working. Logs reached over 18,900 frames without a red line or SDK abort on the normal gameplay path. Latest 300-frame samples: median 33.3 ms, GPU roughly 6 ms, no audio underruns. Some earlier sections had frame-time spikes. This is not full-game validation; iOS has not been tested locally.

## What it needs

- `op_mode handheld`: avoids the docked single-Joy-Con / Poke Ball Plus control path. This is scoped to the profile, not a global change to `dm_opmode`.
- `mem_used 0x4600000`: rewritten modules exceed the default 64 MB. The NPDM has no system resource, and the startup heap request fits the profile's 4 GB layout.
- BLE initialization and idle discovery hooks in `dm/host/hle_offline.c`. Without them, startup aborts through `sdk+0x250230`, called from `main+0x1139d4`, after attempting to open service `bt`. No Bluetooth peripheral service is actually implemented.
- Existing upstream Palma handling, NVN device mode getters, GFPAK extraction and shader readers. No game-specific rewriter patch or origin/culling override was needed on the tested Mac route.

## Shaders

11,160 shaders extracted and compiled from the user's own game files with the current translation chain. No missing shader errors on the logged route. The `shader_note` retains the generated text to preserve the tested input digest. The local extractor called the update `v1` because the input filename lacked a bracketed version; this profile uses its canonical version `v131072`. The display version and module build IDs identify the tested code.

Do not copy game shader binaries into this profile. `c1_full.tsv` and `cb_extent.tsv` are generated locally by a full shader build and remain ignored.

## Mac routes and validation

- `smoke.tsv`: empty save, no input, stationary controller-selection screen for 90 seconds. Reach at least frame 900 by 45 seconds; compare screenshots at 50, 55 and 85 seconds. No save data is distributed.
- Full `regress.py check` against upstream `beea17cfdbd8`, two runs per revision: upstream aborts before the first frame on missing service `bt`; the candidate reaches frames 2683/2685 at 30 fps with no red lines. Status `CHANGED`: one red-line kind removed, no shader or interface regressions. The same external profile was supplied to both revisions because upstream has no profile for this title.
- Separate `regress.py baseline --rev worktree --record` on the repository profile: two full runs passed, both frame 2685, 30 fps, peak footprint 1334 MB. `validated.tsv` is the harness-generated record, not a claim of full-game validation.
- `saved-game-idle`: a local copy of the player's save, A every three seconds from 3 to 60 seconds, then stand still. Two runs measured 65-90 seconds: 30 fps, frame p50 33.33 ms, p99 35.83-35.99 ms, GPU p50 5.58-5.73 ms, zero red lines. The tested save stands on the outdoor path shown in the gallery screenshot. Other saves need their own route; this save is not included.
- Runs were serialized, windowless and silent. Since both harnesses force `DM_WINDOW=1` and expose no unset option, local shell launchers outside the repository ran the unmodified generated hosts with `/usr/bin/env -u DM_WINDOW`. The benchmark uses the supported `host=` variant. These are offscreen frame measurements, not display/input-latency or rumble tests. The gallery screenshot was supplied by the user from the Mac app, showing the trainer and Pikachu. It is resized without cropping; the original stays local.
- Host unit tests including `t_ble`, all Python tool tests, and `make -C dm/gfx/tests GAME=010003F003A34000 run` passed. The shader chain is unchanged.

## Found on this game

- [Offline BLE initialization and discovery](../../../docs/reference/services.md#offline-ble-initialization-and-discovery): startup abort, cause, host hooks, tests and remaining GO-menu limitation.

## Known limitations

- Rumble output is unverified: the host detects the paired Joy-Cons' haptics, but the user reports no vibration. No diagnostic rumble trace was captured.

- **Open: Pokemon GO settings abort.** Entering the peripheral/GO configuration path reaches `sdk+0x25588c`, `sdk+0x252aa0`, `main+0x114124` and triggers `svcBreak` at `sdk+0xd989c`. BLE support here only allows startup and idle discovery; GO pairing and transfers are unsupported. Do not treat the settings page as working.
- **Open: intermittent right Joy-Con latency.** The left controller remained responsive; reconnecting the pair temporarily restored responsiveness. Logs show reconnections but cannot measure Bluetooth transport latency. Cause not established. No local button-remapping change is included; the user chose system-wide remapping instead.
- iOS packaging, orientation, later-game shaders, and full-game progression are unverified.

## Prior work

- [Issue #224](https://github.com/seedds/nxnative/issues/224): reports iPhone gameplay, handheld mode, origin/culling diagnostics, Palma/service changes and extra runtime shader rows. Its global handheld override and broad IPC success stubs were not applied. The attached shader rows are references, not shader binaries, and were not blindly added to the tested map.
- [PR #36](https://github.com/seedds/nxnative/pull/36): same 1.0.2 module builds and memory layout, GFPAK findings and BLE hooks. The BLE initialization/idle-event approach was adapted for the observed startup abort. Its audio/movie/time fixes and GFPAK reader already have upstream equivalents; its binary dumps, alternate-key machinery and replacement keyboard map were not imported.
