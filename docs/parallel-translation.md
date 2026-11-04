# Translating one submission on several cores (design, work in progress)

Status: milestone 1 built and measured (`DM_NVN_PAR=measure`, log only; results below). Each milestone below lands as its own commit on this branch.

## Why

The Metal replay translates a submission on one thread: the game's own submitting thread, inside `nvnQueueSubmitCommands` (`hle_nvn.c`), holding `g_sub_mu`. The GPU can only run what has been translated and committed so far (`sub_cut` segments), so in a busy scene it waits for the CPU.

Measured on Donkey Kong Bananza's busiest handheld view on an M1 (about 10,200 draws a frame, MetalFX on):

| | Encode thread off | Encode thread on |
|---|---|---|
| Translation thread CPU per frame | 14.9 ms | 13.5 ms |
| GPU, first start to last end | 22.3 ms | 22.2 ms |
| Frame time | 33.6 ms (30 fps) | 33.4 ms (30 fps) |

The GPU is busy for about 13.7 ms of that 22 ms span. The rest is waiting for segments. About 4–6 ms of the waiting is the GPU waiting outright, at segment starts; the remainder is vertex work the profiler doesn't time.

Bananza's frame-rate controller times each frame from its start to the later of its submission and the GPU's end, and chooses interval 2 (30 fps) above about 20 ms (`dm/games/040074A01BF12000/README.md`). Translation sits directly on that path. Cutting the CPU cost per draw has gained about 5–10% per change; the encode thread (`DM_NVN_ENC_THREAD`) moves Metal's own calls to a second thread, but the GPU then waits for that thread instead, and the span stays at 22 ms.

The M1 has four performance cores. If one submission were translated by three or four workers, the 13–15 ms would shrink to roughly a third, and the GPU would receive the frame's work early enough to stop waiting.

## Shape

1. **Scan** (sequential, cheap). Walk the submission's operations once, applying only state operations (`state_op`, render target, clears, binds, uploads that matter to later state), and pick split points: render-pass boundaries where a later pass can start from a known state. Record the state at each split point (a snapshot) and the chunk ranges.
2. **Translate** (parallel). Each worker translates one chunk, starting from its snapshot, into its own `MTL4CommandBuffer` from its own `MTL4CommandAllocator`, with its own frame arena, post-operation list and visibility buffer. Metal 4 allows command buffers to be encoded concurrently on different threads.
3. **Validate.** When worker *k−1* reaches the start of chunk *k*, it compares its real state with chunk *k*'s snapshot through a boundary contract (below). On a mismatch, chunk *k*'s output is thrown away and worker *k−1* carries on through it serially. A wrong guess therefore costs time, never correctness.
4. **Merge and commit.** The command buffers are committed in chunk order, in one `commit:count:` where possible, so the queue sees exactly the serial order. Post operations (fences, counter reports, readbacks, occlusion results) are concatenated in chunk order before the submission's fence.

The submission's start (`mtl_sub_begin`) and end (`mtl_sub_commit`) stay on the submitting thread. Nothing outside the submission can tell it was split, which is the same rule the encode thread follows today.

## What blocks a split point

Most of the replay's cleverness looks across pass boundaries. A split point is only valid where none of these is in flight:

- **Pass merging and continuation:** a same-target continuation (`g_rt_merged`), a merged pass pair (`g_fz`), a slice-merged pass (`g_sm_pass`), a ranged pass (`g_rt_range_*`).
- **Lookahead decisions:** hoisted pure-clear passes (`g_hoist_n`), deferred draw groups (`g_defer_*`), lazy clears (`lc_push`), a pending mesh run (`g_mrun`).
- **Deferred inline uniform updates** not yet landed (`g_nubd`). The pass end lands them, so a split right after a pass end is normally clear.
- **A GPU wait inside the submission:** an indirect-count read, a CPU readback, a drain (`mdi_wait`, `mtl_wait_at`). These are serial points. Chunks may run in parallel between them, but not across.

The scan only proposes split points; the boundary contract decides. In the first version, a split is taken only where the serial replay would itself have called `sub_cut`.

## The boundary contract

This is the state a chunk inherits. It must be equal between the snapshot and the real serial run at the split point:

- `S` (draw state) and `R` (render target state), compared whole. The scan computes them with the same `state_op`.
- The submission's write tracking that later decisions read: pending writes (`g_pend_*`), storage-buffer writes (`g_sbw_*`), live GPU writes (`g_gw_*`), the uniform update ring position. These feed `ubp_get` and the uniform conflict check, so a chunk that missed an earlier chunk's write would decide differently. Uploads and inline updates are recorded by the scan. Storage-buffer writes made by draws and dispatches come from their shaders' bindings, which the scan reads without encoding anything.
- Occlusion query state: whether a `SAMPLES_PASSED` count is open, and its slots so far. Option: split only where no count is open. Alternative: link the slot lists at merge time, since results are read only after the GPU finishes anyway.
- Program binding (`S.bst`, combined programs), sampler and texture registration generations (`g_reg_gen`, `g_ser_gen`).

The contract is a struct with a compare function. A debug mode (`DM_NVN_PAR=check`) runs serially and checks every proposed split point against the real state. That proves the scan before any thread is started.

## Global state to move

`nvn_mtl.m` has about 460 file-scope and 160 function-scope static variables. Each has to be sorted into one of these:

1. **Per-submission replay state:** the current encoders and command buffer (`g_cb`, `g_enc`, `g_blit`), `g_pass`, segment counters, post list, visibility buffer, arenas, the draw loop's cached pipeline (`cur_ps`, `cur_pr`, …), deferred uniform updates. This moves into a worker context (`rx`), reached through one pointer passed down or one thread-local.
2. **Caches shared across submissions:** pipelines, shaders, texture views, sampler states, the address lookup cache (already per thread). These need a lock or a thread-safe structure. Most are already filled from background threads, so the audit is mostly about the replay's own fast paths.
3. **Per-shader reuse fields:** `ab_buf`, `ab_pass` and friends on `NvShader`. Two workers can draw with the same shader, so these become per worker (indexed by worker) or keyed by the worker's arena.
4. **Diagnostics:** pass dumps, the GPU profiler's labels, counters. Per worker, merged at commit, or allowed only in serial mode.

`g_rmu` (the texture and sampler registry lock) is taken once per argument encode. Four workers would contend for it, so it should become a reader-writer lock, or the hot reads should go lock-free through generation checks.

## Milestone 1 result

Donkey Kong Bananza, busiest handheld view, M1, MetalFX on, `DM_NVN_PAR=measure`. Per frame, averaged over 300 frames:

| | Light area | Busy view |
|---|---|---|
| Draws (in 4 submissions) | 1,060 | 10,667 |
| Clean pass boundaries | 25.1 | 47.5 |
| Boundaries blocked | 62.9 | 45.1 (37 by deferred uniform updates, 8 by lazy clears) |
| GPU waits inside a submission | 0 | 0 |
| Estimated speedup with 4 workers (by draws) | 1.91x | 2.95x |
| State-only scan | 0.26 ms | 1.28 ms (70,000 operations) |

What this means for the design:
- **The busy view splits well.** About three of the four workers' worth of work can run at once. With no GPU waits inside a submission, there are no forced serial points.
- **Every clean boundary has an occlusion count open.** Splitting only where no count is open is therefore not an option for this game. The slot lists have to be linked at merge time, which is the contract's second option.
- **Deferred uniform updates block most of the rest.** Carrying the pending update list in the contract, as a copy a chunk starts from, would roughly double the clean boundaries.
- **The scan costs about 1.3 ms against 15 ms of translation.** Estimated translation with 4 workers: 15 / 2.95 + 1.3 ≈ 6.4 ms, before merge costs and lock contention.

The measurement itself raised the translation cost per draw (13,190 → 14,170 instructions), because restoring state after the scan makes the next draw re-encode its arguments. That only happens with the switch on.

## Milestones

Each one is a commit on this branch and keeps the default behaviour unchanged until the last.

0. **This design.**
1. **Measure the opportunity** (built: `DM_NVN_PAR=measure`, `par_*` in `nvn_mtl.m`). Count, per frame, how many `sub_cut` points the serial replay passes with a clean contract, and the draws between them. Time a state-only scan of the submission. Log lines only, no behaviour change.
2. **Replay context.** Move class 1 state into a context struct, with one context and serial replay, so behaviour is identical. This is the large mechanical change. It can land in slices by subsystem (encoders and segments, arenas and post list, the draw loop's statics, uniform deferral).
3. **Scan and contract in check mode.** `DM_NVN_PAR=check` scans, snapshots, and compares at every proposed split point while still replaying serially. Done when Bananza, Splatoon Raiders and Breath of the Wild show no mismatches over several minutes.
4. **Workers.** `DM_NVN_PAR=<n>` translates chunks on *n* threads with speculation and fallback, off by default. Classes 2–4 are made safe here.
5. **Measure and tune.** Run the same spots as above and compare frame time, GPU span and fallback rate, then decide on a default (per profile at first).

## Expected result

If three workers each take a third of the 13.5–15 ms, plus the scan (target: under 1.5 ms) and the merge, translation is about 6 ms. In the Bananza view above, the controller's measure would drop from about 22 ms to about 13–15 ms, under its 20 ms threshold, so the game would hold 60 fps whenever the GPU's own work fits. That's an estimate from these measurements, not a result.

## Risks

- **Silent state the contract misses:** a decision that reads something not in the contract would make a chunk translate differently from the serial run. Check mode is the guard, so it has to run on many games.
- **Lock contention** in the caches could eat the gain. Milestone 1 measures, before workers exist, how often the replay touches shared caches.
- **Memory:** each worker has its own arenas and command buffer allocator.
- **Games with many GPU waits per submission** get little parallelism. They still run correctly, just serially.
