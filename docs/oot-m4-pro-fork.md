# OOT on M4 Pro: playable checkpoint and contributor guide

This fork collects the OOT work from November 11–13, 2026. Its engine checkpoint is `09a2572523bea861d0c7c7006826e57801d16de5`; the sharing branch adds a portable preset and documentation without changing that engine code. This is a contribution checkpoint, not a full-game certification or a promise of a fixed frame rate on every Mac.

The latest player-confirmed configuration is **2560×1440 output, a 60-native-FPS cap, MetalFX spatial upscaling, interpolation off and HDR off**. On November 13, Nassim confirmed normal speed and very smooth play on his M4 Pro. He initially thought interpolation was running; the live configuration confirmed it was disabled. This is player evidence, not a controlled frame-time or battery benchmark.

## Build your own app

```sh
git clone --branch release/oot-metalfx-20261113 https://github.com/MeNass89/nxnative.git
cd nxnative
python3 scripts/build_ipa_from_nsp.py /path/to/OcarinaOfTime.nup \
  --platform mac-app \
  --env-file dm/games/0400486024DEC000/presets/2k60-native.env
```

Follow [the development guide](development.md) for the supported Apple SDK, compiler, Python and packing prerequisites. The tested update is `v131072`. This checkpoint uses shader chain `bc8830d4ba5a`; a different chain's cache is not a substitute. Build the full shader map for coverage beyond recorded playtest areas; `--seen-shaders` deliberately omits shaders those areas have not used.

The repository supplies source, synthetic fixtures, profile metadata and settings. Game files, extracted shader libraries, keys, saves, pipeline archives and GPU captures are not supplied. Build from your own game files. The private development launcher is not a portable installer; use the supported packer above.

For an already packed app, merge the preset into `~/Library/Application Support/nxnative/0400486024DEC000/env.txt` while the game is closed. Back up that file first. Preserve existing game/shader/save paths and unrelated preferences; do not replace the entire file. User `env.txt` overrides bundle settings, and inherited launch variables take precedence over both. Check the startup log when a setting appears ineffective.

Output size is distinct from internal rendering resolution. This preset makes no battery-life promise and starts no GPU clock helper. The development Mac has a separate clock-guard service; performance and power observations must state its policy. The packer already disables Game Mode for the [documented clock issue](reference/frame-rate.md#macos-game-mode-holds-the-gpu-at-its-lowest-clock).

The clean public packing command with this new env file has not yet been replayed end to end. The installed host, relevant build-mirror sources, shader-map identity and live settings have been checked. That is not a claim that every clean packing environment has been validated.

## What this checkpoint contains

- Correctness work on uniform re-recording and padding, CPU texture uploads, queue/event ordering, finalized textures/views and in-flight visibility buffers. Some mechanisms are now in main; the [extraction triage](oot-upstream-triage.md) distinguishes backports from remaining candidates.
- A guarded MetalFX spatial replacement recipe, with color/format/dimension/usage/alias checks and native fallback. OOT's observed input is square-root encoded linear color. Replaced dispatches preserve scene-discovery notifications so they do not silently disable interpolation.
- Opt-in asynchronous MetalFX generation, immutable snapshot slots, camera/depth reprojection, reader timelines, paired presentation and retained resources. These are source and fixture foundations; generation remains experimental.
- Native-panel output sizing and bounded timing/flow capture. Instrumentation helps diagnosis; it is not an FPS gain by itself.

Read [the OOT profile](../dm/games/0400486024DEC000/README.md), [triage and remaining work](oot-upstream-triage.md), and [agent notes](../AGENTS.md) before changing the host. Fixed guest addresses must remain gated to their verified build.

## Interpolation: current limits

`DM_OOT_INTERPOLATION_PREVIEW=3` with `DM_OOT_FRAMEGEN_ASYNC=1` enables the research path, not the preset above. Its implemented discovery path also requires a screen reported as at least 120 Hz.

The attempted 30-native / 60-displayed profile caused user-observed slow motion. Returning to 60 native with FI disabled restored normal speed. Both settings changed, so the cause is not isolated. The host's game-time counter at 1.000 of real time did not establish correct motion for every engine consumer.

Requested final targets are 30→60, 45→90 and 60→120, with Switch 2 output, 2560×1440 and aspect-correct Mac-native output choices. Those nine combinations are **not implemented and qualified as a settings product**. Current integer 1/60-second frame steps resolve caps to 30 or 60; changing a label cannot implement even 45-native FPS. Even 90-FPS presentation also needs a suitable display mode or verified VRR behavior.

Camera/depth reprojection does not fully represent independent object motion. Link/Navi, translucent tutorial overlap, dialogue transitions and cuts remain quality cases. OOT's executed four-stage NDSR upscaler is spatial and supplies no observed temporal motion/depth/history inputs. The separate [native-material-refresh investigation](../dm/games/0400486024DEC000/research/native-material-refresh/README.md) is not enabled by the normal preset.

HDR research remains in source history, but HDR is off and outside this delivery. No sustained 120-FPS, whole-game stability, latency or battery certification is claimed.

## Validation and reproduction

- The `09a2572` dirty-epoch/view candidate passed 16 focused runtime fixtures on M4 Pro across the final receipts. `t_vc_ghost` passed 19 checks enabled and 17 disabled. The old dirty implementation failed the three new re-registration checks; the candidate passed them. `t_tbuf_align` passed 80 checks under its Makefile's normal environment. An earlier validation-harness abort was corrected, not hidden as a passing run.
- The visibility fixture passed 101 checks per encoding mode, including 160 retained submissions and the former 64/128 boundaries. This verifies lifetimes, not an isolated speed gain.
- The publication pass reran docs, game-profile and chain tests: 32 Python tests passed before the doc update; final checks are recorded in the PR. Actual camera, pacing, frame-step, TSV and flow fixtures also passed under Linux/GCC. These are not claims that the Mac Makefile or Metal suite ran on Linux.
- Installed signed host SHA256: `d6273954e9798d2e4c842f52e90950daf897cd8e0ee07eadd1a0310d05b3ba4e`; pre-signature build receipt: `44d7ac2ec80898c17ffec98013c37b3a12b9761f3648ff4a4ca43bf74ca2fc1d`. Three formerly dirty renderer/front-end inputs in its mirror match committed `5b77e8d`, rather than silently including the live checkout's diagnostics.

Focused Mac commands from a prepared checkout:

```sh
make -C dm/host GAME=0400486024DEC000
make -C dm/gfx/tests GAME=0400486024DEC000 \
  check_t_dirty_slots check_t_dirty_race check_t_dirty_retire_race \
  check_t_vc_ghost check_t_wait_event check_t_queue_lifetime
make -C dm/gfx/tests GAME=0400486024DEC000 \
  check_t_temporal_preview check_t_temporal_snapshot \
  check_t_present_classic_pair check_t_uniform_padding \
  check_t_native_upscale_capture check_t_allocator_wake
python3 -m unittest dm.tools.tests.test_docs \
  dm.tools.tests.test_game_profile dm.tools.tests.test_nsp_chain
```

The complete graphics suite, final multi-game regression, clean public package run and merge onto current upstream remain open. Targeted historical checks are not substitutes. Other games are unqualified for this combined branch.

Use [the development loop](agents/06-dev-loop.md) when extracting fixes. Copy saves and pipeline archives for experiments, silence automated game runs and preserve the player's profile. Capture visual evidence separately: synchronous screenshots can create the hitch being investigated. Record actual display callbacks, source cadence, GPU work, display mode, thermal state and helper policy separately.
