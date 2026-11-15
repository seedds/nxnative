# OOT integration validation — November 14, 2026

The initial publication checks below validated upstream `72047f1aa94f42f6f8d359e4934787304d85d5cc` and its exact `d254f45e8002` shader chain. The subsequent conflict-resolution validation is recorded at the end of this document. These actual M4 Pro and Python results are separate from the older private performance experiments.

## Executed checks

On the M4 Pro, a private source mirror was built and tested. The installed player application and player saves were not replaced.

- `make -j4 -C dm/host GAME=0400486024DEC000`: passed; full host build, 20.69 seconds in the post-review validation.
- `make -k -j3 -C dm/gfx/tests GAME=0400486024DEC000 check_t_args_gen check_t_vc_ghost check_t_wait_event check_t_queue_lifetime check_t_uniform_padding check_t_temporal_preview check_t_temporal_snapshot check_t_present_classic_pair check_t_upscale_recipe check_t_allocator_wake check_t_flow_trace check_t_tsv_stream check_t_feedback check_t_ghost_lru check_t_dispatch_refused check_t_dispatch_replacement check_t_nvn check_t_native_upscale_capture`: passed, 50.98 seconds. The legacy ghost target explicitly executes current upstream `t_vc_lifetime`; it does not pretend to run the removed private-owner fixture. Encoding-mode variants are run by the Makefile targets.
- `make -j4 -C dm/host/tests GAME=0400486024DEC000 run`: passed, 68.42 seconds. Real output was disabled with `DM_AUDIO=0`. The mocked Core Audio device tests require unmuted sample data internally and emit no hardware audio; forcing `DM_AUDIO_MUTE=1` was a harness error, corrected without changing product audio.
- `python3 -m unittest dm.tools.tests.test_docs dm.tools.tests.test_game_profile dm.tools.tests.test_nsp_chain`: 40 tests passed.
- `git diff --check` and `git diff --cached --check`: passed before publication.

Scoped LLVM analysis was also executed with the repository's real `lint_c.py compdb --game 0400486024DEC000` and `lint_c.py tidy -j 3` for `nvn_mtl.m`, `hle_vi.c`, `front_mac.m`, `hle_codec_video.c` and, after review, `hle_thread.c`. Two nullable capture-output paths were fixed with explicit guards; the ratchet was not raised. The corrected host was rebuilt, `make -C dm/gfx/tests GAME=0400486024DEC000 check_t_native_upscale_capture` passed on Metal, and scoped tidy then passed. This is not a claim that every file received a fresh full-project static-analysis pass.

Earlier integration-test failures were resolved: retired owner internals, the current 256-byte uniform-buffer prefix, the scaler's seven-argument/lazy-texture interface, a presentation callback argument, and standalone VI frontend stubs. No failing test was silently counted as passing.

## Validation boundary

The newest combined-chain source has not yet received a fresh full-game run, a clean portable-package run or the five-game impact-selected gameplay regression. Existing shader data from the old `bc8830d4ba5a` chain must be rebuilt, not relabeled. Other games are not gameplay-qualified for this combined fork.

The 18 selected graphics targets and the additional upload-budget target do not constitute the complete graphics suite. The documented historical player sessions and GPU comparisons remain useful evidence, but do not certify constant native 60 throughout Hyrule Field, sustained 2K/4K120, complete-game stability or perfect interpolation quality. See [the current findings](oot-20261114-findings.md) and [contributor guide](oot-m4-pro-fork.md).

## Claude review and follow-up

The complete fork diff was reviewed by Claude before push. Confirmed integration defects were corrected and revalidated:

- The old private-owner LRU fixture was removed; its explicit compatibility target runs upstream `t_serial_owner`. The view-cache alias similarly runs `t_vc_lifetime`.
- A known refused compute dispatch with no eligible replacement returns before render-pass/first-store/image side effects. A real Metal regression checks the open encoder and green pixel readback; NDVE replacement checks remain.
- New HDR diagnostics use distinct IDs within the enlarged one-time diagnostic table.
- Non-native output removes stale screen pins, unavailable native displays fall back coherently to 2K, and 120 FPS remains selectable as an explicitly experimental target. The selector was compiled on the Mac and its expanded self-test passed.
- The native-video cadence query now reads fixed atomic publication slots instead of waiting for a decoder's conversion lock. Consumption-based expiry, repeated reads, reset and close remain covered. The actual codec API test calls the query while both decoder/registry mutexes are held.
- `DM_YIELD_NODEPRESS=0` restores `sched_yield`; the tested non-depressing Mach default remains. Both routes pass deterministic deadline tests in fresh processes.
- Runtime diagnostic switches are catalogued, while test-only flags and the unapplied research control are not advertised as product settings.

The full host build, complete host CPU suite, 18 selected graphics targets, both yield modes and selector tests passed after these changes. Python documentation/profile/chain checks still pass all 40 tests. These selected graphics targets add the owner, refused/replacement-dispatch, generic presentation-cap and native-capture checks to the earlier 13-target command.

The cap arithmetic remains general: a 30/60 source cap counts its own clock units rather than physical 120-Hz display ticks. Additional actual C fixture checks cover engine-native 30/60 caps, native 120 and uncapped intervals. This is not a second game's ProMotion gameplay qualification.

Two suspected issues were not reproduced in source: snapshot acquisition calls `mtl_present_wait` directly, so its timed stalled-link/drop/rescue path runs on the waiting caller; it does not depend on a guest thread waking it. Every production flow-options caller supplies fresh options or nil, so feedback accumulation from reused options was not found. Reader-completion holds were not weakened to silence the review.

Dirty texture re-uploads switch to a new texture or an idle, unowned spare (`spare_fence <= done` and `own_object_free`); prior readers retain their old texture. This is source evidence against an in-place overwrite hypothesis, not a new multi-game rendering run. There is one `g_cur_frame` definition in the renderer, not a second accidental global.

The corrective follow-up in the same Claude review found no remaining blocking code defect in that scope. Two remaining documentation descriptions (focus retention and preview mode 2) were checked against source and corrected. The additional `check_t_upload_budget` target passed with the current eight-argument upload API, and the final five-file scoped LLVM run passed without increasing the ratchet. None of this closes the gameplay and full-suite limits above.

## Subsequent main conflict resolution — 18:49 UTC

The branch now incorporates upstream `d33e5152710e0a31df6cb0bc0a0e1de06b6bdf8d`, including archive-library readahead, per-source incremental host builds and the updated geometry mesh path. The only textual conflict was in `dm/host/Makefile`; the upstream object-build rules were retained. Clang's generated dependency file includes our `probe_context.inc`, so editing that include still rebuilds its object without restoring the old monolithic build.

The new shader-chain header and every chain input exactly match upstream: `66eab9c9a6b4`. A cache from either `d254f45e8002` or the older private chain must not be relabeled to this identity.

Fresh validation on the M4 Pro private mirror passed:

- Complete host build: 11.59 seconds.
- All 18 selected Metal targets above, plus `check_t_geometry_mesh` and `check_t_pso_bundle`: 68.00 seconds, including their encoding-mode variants.
- Complete host CPU suite: 66.23 seconds.
- Both yield modes, selector compilation and selector self-test: passed.
- The portable documentation/profile/chain checks: 40 tests passed.
- On macOS, the host-MSL build tests and shader-pair/geometry Python suite: 40 cases, 35 passed and five explicitly skipped for missing optional offline-translator inputs. `DM_GAME=0400486024DEC000` selects the required profile.
- A dry incremental build with `-W probe_context.inc` schedules `probe.c` recompilation, confirming the generated include dependency.

These are integration checks, not a new game-performance result. The installed application and player saves were not replaced. The full-game, portable-package and other-game validation boundaries remain open.

The first expanded Python invocation exposed harness prerequisites: the private rsync mirror has no Git metadata for the generated-doc index test, and the shader-pair import needs `DM_GAME`. Documentation checks passed in the real Git checkout; macOS-specific build and shader tests were rerun successfully with the explicit profile. No product or test assertion was weakened.

## NDVE first-network startup — November 15

A current-chain cold start exposed a race between asynchronous host-network preparation and the first movie frame. The first layer could see initialization still pending and fall back to the translated network. Even when the host kernels became ready later in that frame, sequence tracking correctly refused to start in its middle. The replay then synchronously compiled `d15370a382630d4e_b`, a 1.83 MB translated movie kernel, instead of reaching the next frame. A sampled replay stack was inside Metal's compute-pipeline compiler, not waiting for an application GPU fence.

The fix publishes the initialization group through `dispatch_once` and gives only the first network layer one bounded readiness wait. Comparison mode and inactive AB phases keep their existing paths. Initialization failure or timeout preserves the original fallback; a timeout cannot add the same wait to every later frame. No network arithmetic, shaders, NSO patches or image-quality settings change.

- The actual production readiness gate passes 13 CPU checks on the M4 Pro: deferred success/failure, comparison-mode bypass, timeout, one-shot behavior and late availability. The new fixture is included in the CPU-only GitHub test job.
- The complete host builds successfully. The existing counter-clock and timestamp fixtures also pass.
- The existing NDVE source-contract checks and documentation/profile/chain tests pass: 43 Python tests.
- A real current-chain startup now logs the first network frame and resampler running through the host engine, and proceeds into level loading without requesting the blocking original compute pipeline. This is startup evidence, not a new Field FPS certification.

The private reduced shader profile used to qualify the current renderer proved insufficient once Field loaded additional variants. Those partial-render runs are invalid as performance measurements and were not promoted to the player installation. The installed player's full-cache native-60 controls are tracked separately. Completing current-chain full-game shader and gameplay qualification remains open.

## Installed player: bounded native-60 Field qualification — November 15

This is a separate qualification of the installed player's unchanged `158a3306e175e6dd5c50cb495263238034e4e2cb6503c8f786724e98054027d9` host and complete `bc8830d4ba5a` shader library. It does not qualify the current merged renderer or establish a new code speedup.

On the M4 Pro, with a 2560×1440 output through MetalFX, a 60-native target, interpolation off, HDR off and the existing GPU-clock guard, a short test measured 2,340 actual native presentations: 15 seconds static and two 12-second camera rotations. Every phase sustained 59.9998 frames/s; the largest interval was 16.676625 ms, with zero intervals above 20 ms and zero generated frames. Timed windows showed no pipeline or shader-library compilation and no GPU errors. The game clock remained at real-time speed.

The test established actual fullscreen using the window's accessibility state, its Exit Full Screen menu item and display-sized window bounds. Game Mode and Direct presentation were not independently verified. Living gameplay was positively checked before and after the timed windows; the final image still showed Link alive while enemies attacked. No further A inputs or walking occurred after the world-ready marker. The player application and protected save were unchanged.

Earlier longer trials ended in Game Over, including one with camera input only. Their later frame rates are not accepted as Field performance. This corrected short run establishes native-60 capability at the tested Field location and camera rotations; it is not a guarantee for every region, encounter or long session, nor evidence of 2K/4K120.

## Main submission-scheduling integration — November 15

Upstream `b2ae958f296c0015df47800d0fcaf0d93202af24` is merged. Its held-submission batching and per-submission feedback are retained. Both classic and display-link presentation retain the new guest-texture commit wait as well as this fork's generated/native phases, immutable crops and HDR state. Flow diagnostics wrap actual queue commits, including batched work and uploads. The NDVE first-network readiness fix and its CPU-only CI entry remain intact. An independent source review found no blocking ordering or ownership defect in this integration.

The expanded Mac tests first exposed an inherited counter-clock failure. A probe reproduced 7,995 rejected valid samples in 100,000 calls; the root cause is documented in [Valid GPU feedback rejected on the current clock tick](reference/frame-rate.md#valid-gpu-feedback-rejected-on-the-current-clock-tick). The correction is separate from the merge: quantize feedback seconds to integer nanoseconds before subtracting, with explicit invalid/overflow/future guards. A fixed-tick regression fails against the old production function and passes against the corrected function, without changing its two-nanosecond tolerance. All 39 clock checks pass.

Actual final-source checks on the M4 Pro:

- Full host build: passed, 6.37 seconds.
- All 27 selected renderer targets: passed, 68.94 seconds. The previous 20-target set is extended with `t_rec_draws`, `t_sub_overlap`, `t_counter_clock`, `t_ndve_ready`, `t_gmem_words`, `t_linear_readback` and `t_alpha`; this is not the full graphics suite. The new submission wait passes all 8 checks, NDVE readiness all 13, linear readback all 65 and alpha replay all 1,216.
- Complete host CPU suite: passed, 64.64 seconds. Both yield modes, selector build and selector self-test passed.
- Portable Python tests: 61 cases, 60 passed and one Metal-compiler-only case skipped on Linux. The Mac alpha target separately compiles and runs its Metal fixture.
- Scoped LLVM 23.1.5 analysis of the renderer and included code: passed the existing ratchet, with zero file/check pairs above baseline. Source-input hashes were checked before validation and after analysis.

This is build and targeted correctness evidence. No new gameplay performance run, full graphics run, five-game gameplay regression or portable-package qualification is claimed for this merged renderer. Those existing draft-PR limits remain open. The installed player, its cache, settings and saves were not changed during this maintenance pass.

## Main frame-step and command-memory refresh — November 15

Merged upstream `b1ba6570fc334a1142d8d89ac628161e58e5b99e`. The only textual conflict was the OOT note's findings list; both sets of findings are preserved. The command-recording ownership, frame-step GPU-probe and save-mutation fixes merge without changing this fork's MetalFX snapshot/presentation, integer timestamp conversion or NDVE readiness implementation.

Fresh M4 Pro checks passed: full host build (6.99 seconds); ten selected targets (28.60 seconds): `t_nvn`, `t_fstep`, `t_counter_clock`, `t_ndve_ready`, `t_queue_lifetime`, `t_uniform_padding`, `t_temporal_snapshot`, `t_present_classic_pair`, `t_dispatch_replacement`, `t_wait_event`; complete host CPU suite including `t_fs_mutation` (64.74 seconds); both yield modes; selector build and self-test. Scoped LLVM analysis of `hle_nvn.c` and `hle_fs.c` passed the existing ratchet. Critical source hashes matched before and after validation. Documentation/profile/chain/NDVE Python checks passed all 43 cases.

These are build and targeted regression checks, not a new gameplay qualification of upstream's frame-step improvement. Full graphics, multi-game gameplay and newest-chain full-game qualification remain open. No installed player, shader cache, settings or saves were modified.

## Rendering and shader-chain refresh — November 15

Merged upstream `ae3f1699`, including chain `8e41177b6eb3`, far-plane depth handling, multi-level feedback snapshots, tiled downsample commands, capture-aware Metal 4 pipeline selection, and host audio/video/font/controller updates. The only textual conflict was the OOT findings list. Both sets are retained; the fork's findings now follow upstream's list rather than occupying its prepend location, reducing recurring documentation conflicts. The MetalFX snapshot shared-event allocation check is retained alongside the fork's existing snapshot/interpolation logic.

Actual M4 Pro checks passed: full host build (7.69 seconds); 27 targeted renderer checks (88.79 seconds), including interpolation, snapshots, feedback, tiled resolves, pipeline archives/pending work, geometry and clock/startup regressions; complete host CPU suite (73.50 seconds); both yield modes; selector build/self-test; scoped LLVM analysis of the renderer, NVN entry points, Mac frontend and video codec (56.87 seconds), with no ratchet increase. Critical input hashes were verified before and after. These are selected targets, not the complete graphics suite.

Portable Python coverage ran 123 cases, with 119 passes and four optional-platform skips. The initial broader Linux command included Mac packaging tests and failed because `sysctl` was unavailable. On macOS, 74 of 75 packaging cases passed immediately; `test_info_plist` required real Git metadata absent from the rsynced mirror. It passed unchanged when rerun against the actual published branch's shallow Git metadata and the candidate mirror as its dirty worktree. Neither a fabricated revision nor a weakened assertion was used. Final documentation checks pass all six cases.

The installed player, shader library, settings and saves are unchanged. Its earlier gameplay measurements do not qualify the new chain. Full-game cache rebuild, newest-chain gameplay, full graphics, multi-game gameplay and portable-package qualification remain open; no new FPS improvement is claimed by this maintenance pass.
