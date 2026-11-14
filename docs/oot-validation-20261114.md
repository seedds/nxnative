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
