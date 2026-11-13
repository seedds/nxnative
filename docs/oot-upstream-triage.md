# OOT checkpoint: upstream extraction triage

This maps `release/oot-metalfx-20261113` for review, not wholesale replacement of main. See [the playable profile](oot-m4-pro-fork.md). Engine checkpoint: `09a2572`; common ancestor with upstream: `f02d0474`. The engine contribution range has 112 files, +6,613/−374 lines. An endpoint-tree diff incorrectly includes later upstream additions as removals.

Preserve tested engine bytes. A rebase onto newer main needs new validation. Extract coherent fixes separately, crediting upstream PRs or commits where a mechanism already exists. A backport is not a second new contribution.

## General mechanisms without an OOT-specific contract

This describes semantic applicability, not completed multi-game testing.

- **Uniform re-recording and short buffers.** Preserve the existing record span when changing one binding and pad only the missing uniform bytes. Start with `7ca292f9`, `nvn_mtl.m`, `t_nvn.c` and `t_uniform_padding.m`. Compare main before extracting.
- **Bounded CPU texture-upload batching.** `2d1003fc` groups open-submission uploads within the staging budget and retains targets. `t_cpu_tex.m`, `t_upload_budget.m`, upload-slot and direct-upload fixtures cover correctness. The game observation does not isolate an FPS gain.
- **Texture/view lifetimes and dirty epochs.** `e9678f60` and `09a2572` retain finalized resources for existing readers and assign recycled dirty slots a new registration epoch. Main already contains `born` epochs and finalized/ghost-view mechanisms in a newer owner layout. Treat them as carried fixes, not duplicate additions.
- **In-flight visibility and allocator reuse.** `5b77e8da`, verified past the old 64/128 boundaries by `t_wait_event.m`. Main already has `SUB_FLIGHT_MAX` and the corresponding visibility/post rings.
- **Residency outside the hot mutex.** `a5863042` and `t_res_commit.m` separate the slow Metal commit from queued changes. Main already has the separate mutex and deferred changes.
- **Prediction only at a new pipeline miss.** `be568428` and `t_pso_pending.m`. Main already has `pso_predict_due` and `g_pp_admit`.
- **Bounded, deferred traces.** `189f313a` and `92b0d024`, `nvn_tsv_stream.h` and `nvn_flow_trace.h`: exact output, bounded overflow and concurrent drains, with high-frequency file I/O removed from play. This is diagnostic infrastructure, not a rendering speedup.

## Reusable after engine or platform adaptation

- **Snapshot ownership and async MetalFX.** `nvn_temporal_snapshot.inc` and `nvn_temporal_async.inc` retain immutable inputs and interpolators through both generation/display readers. Another game must identify its own color, depth, motion, reset and presentation boundaries.
- **Legal spatial outputs.** `cd966bef` and `a3c568e3` distinguish a private MetalFX destination from a drawable requiring copy/render, checking dimensions, format, orientation, usage and storage. Do not force direct output for incompatible drawables.
- **Guarded spatial replacement.** `149fa1ea`, `7dda971c`, `nvn_upscale_recipe.inc`: retain fallback and normal dispatch notifications. Each engine needs its input encoding, bindings, grid and alias contract.
- **Camera/depth reprojection.** `nvn_agl_camera.h` and `camera_motion.metal`: reusable mathematics, observed agl layout and geometric tests. Offsets are not universal, and full object motion remains unproven.
- **Mac-native pixel sizing.** `61411451`, `front_screen_mode.h`, `front_mac.m` and `t_screen_mode.c`: physical pixels versus logical points. External screens, aspect, scaling and variable refresh still need checks.
- **HDR transport research.** `ec0e8ca1` retains EDR code/fixtures. Disabled in the shared preset and outside the current release-quality claim.

## Specific to the observed OOT build

- The `fa3033d05c0a41d6_b` recipe in `game.tsv`: square-root-linear input, 1536×864 scene, 2048×1152 intermediate, exact bindings and grid.
- OOT scene/pre-UI/final surfaces, matching depth and safe history boundaries. Do not assume another engine/update has the same contract.
- The frame-ratio hook, observed agl offsets and build-gated fixed-address investigation. Native-material-refresh research is not enabled in the normal preset.
- OOT prewarm rows and routes. Compatible lists can be deduplicated, but recorded areas do not prove a complete-game cache.

## Contribution priorities

1. **Cleanly reproduce 2K60 native.** Build a separate app, preserve saves/current installation, record chain and shader coverage, then verify actual play. Build success alone is insufficient.
2. **Reduce first-visit stalls.** Compare the effective manifest plus user list with [#2324](https://github.com/seedds/nxnative/pull/2324), [#2466](https://github.com/seedds/nxnative/pull/2466) and [#2550](https://github.com/seedds/nxnative/pull/2550). Deduplicate and prepare compatible missing entries. No complete ready-to-import Mac cache was verified in the search.
3. **Extend 30→60 qualification.** The 60-FPS AV1 introduction now temporarily retains a 60-native cap; its matched timing test and the bounded 30+30 CastleTown gameplay test are documented in the contributor guide. Continue with long sessions and independent-motion quality; do not treat the movie fix as whole-game proof.
4. **Implement actual 45-native / 90-output timing.** Audit fractional ratio, inverse and integer readers; an alternating whole-step average is not even motion. Verify the display can show 90 evenly.
5. **Improve heavy-scene GPU headroom and tails.** Separate native/post work from display waits. Castle post-ablation removed features: it does not prove the same gain with features retained. Neutral palette/low-route experiments are not shipped gains.
6. **Resolve FI quality cases.** Link/Navi, translucent tutorial/cave overlap, dialogue sharpness, fog/light shafts and cuts. A view-angle FPS change does not isolate a shader's cost.
7. **Complete regression before merge.** Extract one mechanism at a time, run real fixtures and impact-selected games. This draft map does not replace those checks.

## Impact and validation boundary

The actual Mac command `scripts/regress.py impact f02d0474..09a2572` completed without a game run: three games affected, one through broad API rules, 152 profiles unknown. Breath of the Wild, Pokémon Scarlet and Tears of the Kingdom are affected by usage-manifest reachability; those results are not gameplay passes. OOT has actual player and targeted fixture evidence. The other games remain unchecked for this combined branch.

The [contributor guide](oot-m4-pro-fork.md) records engine/fixture scope and the installed host. Full current-upstream graphics, lint ratchets, multi-game regression and clean package validation are not claimed. Keep the integration contribution in draft until selected extractions and checks are complete.
