# OOT on M4 Pro: contributor checkpoint

This is the development branch `perf/oot-mbpn` of MeNass89/nxnative. It contains tested fixes and opt-in research. MetalFX frame interpolation is not yet a finished player feature.

## Start here

```sh
git clone --branch perf/oot-mbpn https://github.com/MeNass89/nxnative.git
cd nxnative
make -C dm/host GAME=0400486024DEC000 -j2
```

Follow [the development guide](development.md) for the Apple SDK, shader translator and game-packing prerequisites. Gameplay requires your own game files and save. The game, extracted shaders, saves, keys, pipeline archives and captured GPU data are not included in this repository.

Read [the OOT profile](../dm/games/0400486024DEC000/README.md) and [the repository's agent notes](../AGENTS.md) before changing the host. Per-build patches must be gated by the NSO build ID.

## What is verified

- The Mac host builds. The targeted NVN suite passes 306 checks and queue-origin passes 398 checks.
- Single uniform-buffer bindings keep their byte span when re-recorded in an existing command block. A public-API test reproduces the malformed command chain before the fix and passes after it. See [the mechanism](reference/nvn-objects.md#uniform-bindings-re-recorded-in-place-keep-their-byte-span).
- Camera decoding, MetalFX preview/snapshot paths, classic paired presentation, short-uniform padding, native capture, allocator wake and bounded flow tracing have focused fixture coverage. The snapshot tests include packed color, direct/deferred encoding and asynchronous generation.
- The shader constant-store changes and queue/wake fixes are recorded in their reference entries. A successful short test is not a full-game validation.

## What remains open

- The head-turn cinematic still exposes large pauses with interpolation. Keep per-frame CPU/GPU waits and actual presentation callbacks separate; native FPS is not the physical display rate.
- Thermal caps materially changed GPU frequency between trials. Do not attribute differences to a renderer setting without checking temperature, GPU clock and concurrent workloads.
- Eyes were correct in the latest user-observed run, but earlier trials showed white/flickering eyes. This is not yet a universally verified fix.
- OOT's executed NDSR chain is spatial. Its four observed passes do not provide temporal motion/depth/history inputs. The native motion-buffer route remains an investigation; no complete object-motion field has been validated.
- Broad regression across other games, sustained quality/cadence, input latency, power and deployment into the installed app remain pending.

## Experimental switches

`DM_OOT_INTERPOLATION_PREVIEW=3` enables the paired-frame research path. `DM_OOT_FRAMEGEN_ASYNC=1` uses separate generation/display queues. `DM_PRESENT_SNAP=3` gives that path a third snapshot slot. These are comparison settings, not a recommended stable preset.

The native-motion experiment is separate from the normal profile. Its [assembly and controlled fixture](../dm/games/0400486024DEC000/research/native-material-refresh/README.md) are included for inspection; the patched game binary is not. Do not enable the native motion options or install that hook by default.

## Reproduce focused checks

```sh
make -C dm/gfx/tests GAME=0400486024DEC000 t_nvn t_queue_origin
dm/gfx/tests/t_nvn
dm/gfx/tests/t_queue_origin
make -C dm/gfx/tests GAME=0400486024DEC000 \
  check_t_agl_camera check_t_temporal_preview check_t_temporal_snapshot \
  check_t_present_classic_pair check_t_uniform_padding \
  check_t_native_upscale_capture check_t_allocator_wake \
  check_t_flow_trace check_t_flow_exit
python3 dm/tools/tests/test_docs.py
```

These are the focused checks run for this checkpoint. The complete graphics suite and the multi-game gameplay regression have not been completed for this branch.

## Capture and timing discipline

Use `scripts/bench.py` with a copied save and independent pipeline archive. Its route `secs` value is a duration. By default it captures visual proof after the timing window; in-window screenshots wait for the GPU and write PNG on the render path, so they can create the pause being investigated.

For a bounded run with no screenshot at all, pass `--shots 1000000000`; an empty shots list restores the default screenshot. Use pixel captures to inspect quality in a separate run, not as timing evidence.

Record the branch/commit, shader-chain version, NSO build ID, scene/save, settings, thermal state and actual display callbacks. Keep failed hypotheses and limitations visible. Commit coherent tested units on a feature branch and open a PR against this fork when ready for review.
