# OOT findings — November 14, 2026

The goal remains constant 60-native-FPS Hyrule Field play, with image quality preserved, before further interpolation work. **No new native-FPS win is established here.** The earlier bounded 30-native + 30-generated results and the title-movie correction remain documented in the [contributor guide](oot-m4-pro-fork.md); they do not qualify native 60 throughout Field.

This page records completed investigations against the older private renderer and shader chain. The branch now integrates newer upstream work through `72047f1aa94f42f6f8d359e4934787304d85d5cc`. Historical GPU receipts are not a runtime qualification of that merged renderer. Use the current `DMSH_CHAIN` in [dmsh_chain.h](../dm/gfx/nvn/dmsh_chain.h), rebuild compatible shader data, and record fresh validation for the combined build. Do not substitute a cache from the old `bc8830d4ba5a` chain.

## Correctness: argument validity and bound-program ownership

Upstream [f31276ad](https://github.com/seedds/nxnative/commit/f31276ada3f7ae2c5462374d2aaa4dbf12b727ab) fixes two related contracts: validity belongs to each shader's remembered argument generations, and composite-program ring replacement must skip the currently bound slot. A stage-wide dirty flag could be cleared by another shader; a ring replacement could also preserve pointer identity while changing the program, bypassing invalidation.

A minimal backport to the older private renderer was tested on actual M4 Pro GPU execution. The old control failed six of 26 checks; the candidate passed all 26 with encoding-thread recording both off and on. Storage-buffer-write and queue-lifetime checks each passed 38 checks in both modes. Geometry checks passed 61 checks with the cut control off and on. The older renderer lacked upstream's uniform-only argument-block patch path, so the backport retained full encoding on generation mismatch. It was an adaptation, not a verbatim import of newer main.

A subsequent private Field smoke exited normally after 123.68 seconds including preparation, with no reported GPU error or protected-save change. Static/camera physical output was about 54.20/54.26 FPS. One startup input was recorded outside the measured windows. This establishes a bounded smoke pass, not a speedup or a diagnosis of an earlier player-visible failure.

The merged upstream already contains f312 and its newer argument machinery. Keep that upstream mechanism; do not layer the private compatibility backport over it or count it as a second contribution. The historical owner/lifetime changes alone did not already repair these argument-generation and bound-ring cases.

## Shader candidates: real correctness evidence, no promoted optimization

Direct-emission foliage mesh candidates, earlier terrain discard work and TASK integer-reduction rewrites were investigated with actual Metal execution. Qualification and timing are separate gates:

- A complete mesh-body comparison used a real TASK payload and real mesh argument resources. All 40 cases and 104,992 checks passed, including bitwise vertex data, indices, writer coverage, winding and both Z conventions. This covered one observed active payload plus controlled prefixes; it was not universal shader equivalence or candidate hardware-mesh output proof.
- The complete TASK reduction comparison matched all 1,604 scratch words, including the 800-word outgoing payload, with zero differences. Its independent width-32 integer-reduction fixture covered 738 cases. The mesh-composition checks also passed. This does not mean ten rewritten TASK bodies each received a full in-game qualification.
- Two terrain fragment candidates passed their synthetic GPU pixel/helper cases. Those cases do not establish complete-game visual equivalence.

Production candidates subsequently compiled and received bounded Field comparisons. The five-shader mesh/terrain batch was neutral: static physical output was 59.74 FPS for original versus 59.70 for candidate; native GPU-CB span-sum medians were 15.038 versus 15.182 ms. The ten-TASK reduction batch was worse in the observed pair: static 58.56 versus 56.73 FPS, with span-sum medians 15.834 versus 16.422 ms. These trials do not establish the cause of the difference, but give no basis to enable either candidate. Fewer source operations, less scratch storage or a passing correctness fixture do not promise lower GPU time.

No extracted game MSL, shader libraries, payload dumps or overlays are distributed with this report. A reusable translator optimization needs its own general pattern, preservation proof, synthetic regression and current-chain measurement; title-specific replacements remain research.

## Long-pass splitting: preserved output, no Field benefit

Upstream [18313129](https://github.com/seedds/nxnative/commit/18313129dc9d3b34163bcb97df8cd3749b758df5) can submit part of an eligible long render pass earlier, preserving attachments across the resumed pass. It is already part of the newer upstream integration. The older private experiment was explicitly disabled by default and compared off against threshold 256; upstream policy must not be inferred from that private default.

An actual NVN/Metal fixture passed all four forced-branch cases: four or 320 draws, splitting off or on, with pixel preservation checks. The Field pair did not show a gain: static physical output was 59.41 FPS off versus 58.67 at 256; camera was 59.00 versus 58.87. This is useful correctness and negative performance evidence, not a reason to duplicate the implementation or claim the merged default has been game-qualified.

## Measurement corrections that matter

A particular 107.786-ms snapshot/presentation stall was traced to synchronous PNG screenshot work on the presentation thread. That observer caused that specific pause. It does not exonerate every other long interval. Capture visual evidence outside timing windows, and distinguish requested screenshots, pipeline compilation and actual rendering delays.

A clean private run removed TSV/FLOW/GPU-clock collection and in-window captures. It exited normally after 149.70 seconds, with no physical controller-input records, no generated images, no reported errors and no protected-save change. Physical output was 55.03 FPS static, 52.32 during the scripted camera phase and 53.35 settled; each phase had a 25-ms p95 display interval. Periodic summaries showed no new pipeline/library work in the measured region.

These were **windowed, floating, all-Spaces visibility tests with Game Mode disabled**, not the installed fullscreen player path. They neither establish a fullscreen hardware ceiling nor prove a renderer regression from an older view. Workloads also changed: later settled windows reached roughly 2,370–2,423 draws per frame versus roughly 1,800–1,840 static, despite no physical controller input. Compare actual draws, view and phase, not just save identity.

The logged native GPU metric sums command-buffer start/end spans. It excludes the separate final display/MetalFX buffer and is not an exclusive shader-busy counter. Stage timestamps likewise delimit overlapping execution envelopes; a pre-raster interval is not exclusive geometry cost. The attempted per-draw attribution produced invalid zero/missing timestamps and supplies no foliage-versus-terrain cost verdict.

Clock-guard policy remained explicit in comparisons. An old heavy-versus-light helper gain does not apply to the already-light current policy. A bounded guard-off hint and older pulse tests do not establish an always-on replacement or a general improvement; no guard change is proposed here. Likewise, a 120-tick display-link experiment for native 60 increased 25/8.33-ms alternation and was not adopted.

## One remaining source hypothesis: voluntary-cut coalescing

The accepted early FLOW subset contained 39 native command buffers per frame. **That is not 39 voluntary cuts.** The periodic split-submit delta was about 900 over 300 frames: approximately three extra cuts per frame. Most buffers came from actual NVN submission boundaries.

In the audited older renderer, segment timestamps plus encoding-thread recording disabled the counter-heap path, which in turn prevented the existing busy-GPU coalescer from suppressing eligible voluntary cuts. This identifies a source condition, not the cause of all gaps or a proven fix. The early FLOW trace overflowed; only 94 complete filtered frames were accepted, outside the known PNG stall. Its between-buffer gaps cannot all be assigned to those three cuts.

The next useful discriminant is read-only: count busy-GPU voluntary cuts whose entire immutable NVN submission is proven free of guest counter reports, including nested command calls. Unknown cases keep their existing boundaries; all counter-report kinds matter. If none qualify, close this lead. If some qualify, a guarded private change still needs guest timestamp/fence checks and image equivalence before performance testing. Changing timestamp mode or disabling the encoding thread is not an equivalent control. No coalescing patch or gain is published here.

## Contribution boundary

Argument validity, program ownership, counter semantics, attachment preservation and resource retirement are general native-API correctness concerns. Mesh/reduction lowering needs translator and GPU-family adaptation. OOT bindings, payload layouts, output encoding and route/cache observations are title/build-specific. The [extraction triage](oot-upstream-triage.md) maps those boundaries.

Maintainer-retained receipts distinguish synthetic GPU qualification, actual-game smoke and measured comparisons; they are historical evidence, not downloadable game fixtures. This publication contains source-level explanations and measured summaries only. The merged build still needs its own graphics checks, impact-selected game regression and clean-package validation. Constant native 60 across Field, sustained 2K120, full-game interpolation quality and representative fullscreen qualification remain open.

The subsequent merged-source build, selected real Metal tests, complete host CPU suite and scoped LLVM checks are recorded in [the integration validation](oot-validation-20261114.md). Those fresh checks do not change the historical performance conclusions above.
