# Shader extraction cache implementation plan

**Goal:** Keep unchanged extracted shaders across translation-chain updates for macOS and iOS, with a measured LegendsZA rebuild.

**Spec:** ../specs/2026-11-01-shader-extraction-cache.md

**Baseline:** `b4880a9d83bfc17379af8dd3c9e00c7ab87ff2c0` (includes pipeline archive reuse). Implement inline, as authorized by the user, in the existing isolated worktree on `codex/shader-extraction-cache`.

## Review focus

- Transitive reader changes must invalidate; translation-only tools and profile notes must not.
- Every extraction setting, checked/generated table, manual fact, module and capture input must remain covered.
- Decoder provenance must be bound to executable bytes; only the chain label is normalized, with all other host build inputs retained.
- Missing/corrupt package outputs, invalid comparisons and damaged inputs must never produce a successful hit.
- Both target paths must reuse the same extraction output while preserving separate translation caches.

## Tasks

1. Save the baseline and warm the real game build; keep the benchmark source snapshot fixed while implementing.
2. Add behavioral tests in `test_nsp_incremental.py` and `test_host_msl.py`; observe the expected failures.
3. Implement transitive extractor dependencies, scoped profile/data inputs, verified decoder provenance and extraction-output integrity in `scripts/build_ipa_from_nsp.py`. Run focused tests and lint.
4. Warm the new extraction cache. Temporarily advance the chain with an output-neutral comment, rebuild the Mac app, measure the legacy split and shared iOS extraction path, compare package/table identities, and restore the chain files.
5. Run the Python suites, native archive validation and LegendsZA regression; review the final change. Record mechanism, timings and coverage limits in the shader reference and game note; regenerate the symptom index.

## Progress

Investigation confirmed broad tool/profile fingerprints and the chain-bearing decoder executable as the causes. The extractor imports 12 repository Python modules and reads `shader_groups`, `shader_note`, `shader_key_skip`, `modules`, and `shader_facts_manual.tsv`; the packer additionally checks its generated tables. No extractor or shader-chain semantics change is planned.

Implementation complete: 9 expected RED assertions covered unnecessary misses, missing transitive/module/capture inputs, corrupt outputs and decoder provenance. The implementation passes 77 focused tests (incremental, host MSL and Mac/iOS shader orchestration); lint has 391 baseline findings and no new or stale findings. A package-output digest prevents reuse after package or supplemental-pair damage. Decoder module content is hashed instead of timestamps.

Baseline warm-up completed from a frozen script at b4880a9d. It refreshed the game from its previous a7db/5b16-era chain to b7cd656abd7c: extraction took 5m42s and shader preparation rebuilt all 28,603 outputs. The app completed in 1,743.51 seconds. This cold refresh overlapped regression and is excluded from the controlled comparison. The actual legacy cache hit was verified before warming the new extraction key.

Full verification: 116 Python test files, 1,070 tests, 13 skipped, no failures (`/tmp/nxnative-extraction-suite/`). Native pipeline archive validation: 52 checks, no failures. Documentation checks: 11 tests pass. Independent fresh-context review approved the implementation with no actionable findings; the pre-existing registered-RomFS size identity and provisional timestamps remain unchanged. Benchmark performance is measured separately, not inferred from review.

The game regression passed: 28,603/28,603 shaders, no changed MSL or interface failures. LegendsZA is the only discovered local Mac build; no smoke scenario is configured, so this did not test gameplay. The comparison runner waited for both regression and baseline warm-up to finish. Drivers and logs live in `<build>/performance-research/shader-extraction-cache/`. Direct split A/B uses the same already-built decoder; the full Mac build includes host compilation. The iOS check executes the real CLI path through extraction and stops before target shader translation, so it makes no IPA/device-performance claim. The runner preserves the original chain libraries, host and runtime stamps for restoration after its temporary chain update.

The controlled update changed b7cd656abd7c to 1dafa92e1dbe with a comment only. The complete Mac app built in 505.75 seconds (8m26s): extraction reused all 28,603 shaders, including the required host compilation in its 16-second stage; Mac shader preparation restored all outputs in 4m46s; the pipeline archive reused in 16 seconds; app packaging took 2m24s. The iOS CLI reached the translation boundary in 2.33 seconds with extraction reused. Package hashes/mtimes, profile tables, decoder input identity and NSP stats remained identical; the executable hash changed.

The direct post-update extraction invocation fell from 276.043 seconds to 0.644 seconds: 275.400 seconds saved (4m35s), 99.8%. Inside `split_shaders`, the times were 275.990 and 0.590 seconds respectively. The legacy extractor's fresh package exactly matched the reused package. These are single runs of the same stage with the same already-built decoder; there is no paired legacy full-app measurement. The runner exited successfully and restored the original chain source/header, warm libraries, host and runtime stamps. Findings and coverage limits are recorded in the game note's Build performance section.

Final verification after restoration and documentation: 90 focused incremental/host/platform/chain tests and 11 documentation tests pass; the symptom index is current and `git diff --check` passes. Original host bytes, decoder identity, library stamp/key and runtime tables match their saved copies. A restored-chain extraction invocation reuses in 0.70 seconds. Temporary-chain libraries and detailed stage logs are preserved in the research folder. Implementation and benchmark are complete on `codex/shader-extraction-cache`.

Publication: the user requested a commit and PR with the benchmark and macOS/iOS validation details. All 101 focused and documentation tests passed again before committing. The branch retains the tested parent b4880a9d from the still-open archive-reuse PR #895; the extraction PR names that dependency.
