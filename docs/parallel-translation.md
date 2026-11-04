# Translating one submission on several cores (design, work in progress)

Status: milestones 1–2, check mode (3) and fresh/fresh-scan contexts (4a/4b) tested in Bananza. Serialized real workers (4c, `DM_NVN_PAR=thread1`) ran in Bananza; the run exposed a diagnostic capacity limit and incomplete CPU accounting, corrected below. Gameplay with those corrections is pending. Concurrent workers are not enabled. Results and test steps below. Each milestone lands as its own commit on this branch.

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

1. **Per-submission replay state:** (moving in milestone 2: into `rx_t`, reached as `RX.…`, or, for state whose types are defined further down the file, kept per context in arrays `<name>_rx[RX_MAX]` behind a macro `<name>` → `<name>_rx[RXI]`) the current encoders and command buffer (`g_cb`, `g_enc`, `g_blit`), `g_pass`, segment counters, post list, visibility buffer, arenas, the draw loop's cached pipeline (`cur_ps`, `cur_pr`, …), deferred uniform updates. This moves into a worker context (`rx`), reached through one pointer passed down or one thread-local.
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
3. **Scan and contract in check mode** (built: `DM_NVN_PAR=check`, `parc_*` in `nvn_mtl.m`: S, R and the write tracking; the scan runs in a context of its own, `RX_SCAN`, and models inline updates, copies, fills, counter reports and the storage buffers draws and dispatches write; texture-to-buffer copies and transform feedback are counted as not modelled). `DM_NVN_PAR=check` scans, snapshots, and compares at every proposed split point while still replaying serially. Done when Bananza, Splatoon Raiders and Breath of the Wild show no mismatches over several minutes.
4. **Workers.** First, built: `DM_NVN_PAR=fresh` (4a) translates serially but moves to a fresh context at clean boundaries, keeping only the contract, so anything a chunk needs beyond the contract shows as a wrong picture with the switch on. Built too: `DM_NVN_PAR=fresh-scan` (4b) takes the new context's S, R and write tracking from the scan's snapshot of the boundary, as a worker that cannot wait for the chunk before it must; its inline updates carry no ring data, so a shadow over one is refused (the pass ends and the updates land) rather than read wrong. Then `DM_NVN_PAR=<n>` translates chunks on *n* threads with speculation and fallback, off by default. Classes 2–4 are made safe here.
5. **Measure and tune.** Run the same spots as above and compare frame time, GPU span and fallback rate, then decide on a default (per profile at first).

## Expected result

If three workers each take a third of the 13.5–15 ms, plus the scan (target: under 1.5 ms) and the merge, translation is about 6 ms. In the Bananza view above, the controller's measure would drop from about 22 ms to about 13–15 ms, under its 20 ms threshold, so the game would hold 60 fps whenever the GPU's own work fits. That's an estimate from these measurements, not a result.

## Risks

- **Silent state the contract misses:** a decision that reads something not in the contract would make a chunk translate differently from the serial run. Check mode is the guard, so it has to run on many games.
- **Lock contention** in the caches could eat the gain. Milestone 1 measures, before workers exist, how often the replay touches shared caches.
- **Memory:** each worker has its own arenas and command buffer allocator.
- **Games with many GPU waits per submission** get little parallelism. They still run correctly, just serially.

## How to test (milestones 1–4b)

Every mode is off unless `DM_NVN_PAR` is set, and every mode so far translates on one thread. Run them in this order, in the same busy spot, and stop at the first one that changes the picture. That mode names the layer that's wrong:

| Step | `DM_NVN_PAR=` | What it does | If the picture breaks | If the log shows red lines |
|---|---|---|---|---|
| 0 | (unset) | the normal replay, through the context struct (milestone 2) | milestone 2's rename: bisect its six slice commits | — |
| 1 | `measure` | the scan runs (in its own context) and boundaries are counted | the scan touched the replay's state (it should not now) | — |
| 2 | `check` | the scan's S, R and write tracking compared at every clean boundary | as step 1 | each names a field the scan does not model: the contract's gap |
| 3 | `fresh` | at a clean boundary every `DM_NVN_PAR_DRAWS` (2500) draws, translation moves to a fresh context with the contract copied from the previous one | something a chunk needs is outside the contract: the log's first switches give the pass and draw, so the first wrong pass after a switch points at it | — |
| 4 | `fresh-scan` | as 3, but the contract comes from the scan's snapshot, as a worker's will | the scan's contract is not enough (step 2's red lines say which part) | as 2 |

Lowering `DM_NVN_PAR_DRAWS` (say to 500) makes step 3 and 4 switch more often, so a problem shows sooner. The log lines to read are the ones starting `parallel translation` and any `[RED] parallel translation check`. `work/apps/partest.sh <mode>` (outside the repository) runs one step and collects them.

## Before real worker threads: the state still shared

After milestone 2, about 890 file-scope variables in `nvn_mtl.m`, `nvn_geometry.inc` and `nvn_feedback.inc` are still shared (not every one is touched by translation). Sorted by name:

| Kind | About | What a worker needs |
|---|---|---|
| Statistics and counters (`*_n`, `*_draws`, `*_why`, …) | 160 | per context, summed when the submission commits, or atomic |
| Switches and configuration (`*_on`, read once from the environment) | 50 | read-only once set: initialise every one before the workers start |
| Caches and registries (pipelines, shaders, texture and sampler tables, pools) | 50 | most already have a lock (about 40 locks exist); the hot reads (`g_treg`, `g_sview`) want a reader lock or generation checks |
| Everything else | 600 | case by case; the encoder-adjacent ones first (`g_blit_*`, `g_bw_*`, `g_br_*`, `g_bar_*`, `g_gp_*`, `g_last_end_relaxed`), which belong in the context, then the submission-level pools (`g_alloc4`, `g_arenas`), which take a lock |

The modes above already exercise the context split on one thread. Threads add data races. A practical next step is to run `DM_NVN_PAR=fresh-scan` under the thread sanitizer with chunks on threads one at a time (each still waiting for the one before it), so a race is reported by name before any two chunks really overlap.

## Milestone 4c: serialized worker (`DM_NVN_PAR=thread1`)

`thread1` continues the fresh-scan ladder on a real worker. The submitting thread translates the first chunk, stops before applying the split's SetRenderTargets, and waits while one worker translates the remaining chunks in order. The continuation preserves nested call returns and later submission handles. Resuming a boundary neither counts it twice nor applies its render-target operation twice. Default replay is unchanged; chunks do not overlap.

The serialized worker is recognized through a thread-local ownership flag; it never rewrites the submitting thread's shared identity. Its autorelease pool stays on the worker and survives until its next submission, while the original submission pool is still popped on the submitting thread. Worker creation failure continues the chunks serially instead of waiting forever.

`make -C dm/gfx/tests GAME=040074A01BF12000 check_t_parallel_thread` checks nested calls, multiple handoffs, remaining handles, exact event order, context reuse across submissions and actual GPU buffer writes, with Metal validation in direct and deferred encoding. `check_t_parallel_thread_tsan` runs the same fixture under ThreadSanitizer in both encoding modes. The original four runs passed 97 checks; the default partial depth/stencil clear test passed 59 checks and the docs suite passed 11 tests. These are targeted continuation tests, not gameplay or throughput measurements. The later gameplay run and additional ownership checks are described below.

Sequential handoffs establish ordering between this worker and the submitting thread. ThreadSanitizer can expose races with other background threads in exercised paths, but a clean run does not establish safety for multiple overlapping translation workers. The shared-state audit above remains necessary before enabling overlap.

Validation scope: the broad game regression run remains pending. The local harness inventory finds Splatoon Raiders without a smoke route; Bananza is built in the separate combined test-all work folder, and Breath of the Wild has no discovered local build. These targeted tests do not replace the three-game milestone checks.

### November 4 worker follow-up

The Bananza `par-thread1-codex` run reached 17,820 worker chunks and 5,591 busy-view samples. It logged 16 write-tracking warnings (the logging cap), with continuing mismatches in the summaries. Early comparisons had about 6,800 written pages, beyond the checker's 1,024-page snapshot. At that size it skipped the inclusion check and reported any differing hash as a missing write. The snapshot now holds `SBW_N` entries, the entire write table capacity; synthetic checks at 6,800 and 16,384 pages accept a safe superset and still reject a missing replay page. This fixes the diagnostic blind spot, but a new gameplay run must establish whether the observed differences were all safe supersets.

The run's 9.03 ms median `@cpu_ms` is the presenting thread's CPU time, not total translation CPU. Chunk work is now charged to `@sub_cpu_ms`, `@sub_instr` and the cumulative translation summaries, including cycles and kernel CPU time. Submission wall time is counted once. Compare `@sub_wall_ms` and frame time for latency, `@sub_cpu_ms` for translation CPU work, and `@proc_cpu_ms` for whole-process CPU; retain `@cpu_ms`'s existing presenting-thread meaning. Deferred Metal encoding and uniform-copy workers are not included in `@sub_cpu_ms`. No speedup is established by the old run.

Context selection (`RX`, `S`, `R`) is now thread-local. The serialized worker explicitly receives and returns its selected context across the semaphore pair. Queue activation, finalization and submission start rebind the last committed context under the submission lock, so changing guest submitting threads does not lose queue state. Shader argument reuse, copy-encoder hazard ranges and compute dispatch parameters are stored per context. Tests exercise simultaneous independent context selection and real GPU writes from alternating submitting threads.

These ownership changes do not enable overlapping translation. The remaining production dependencies include:

- `thread1` now reserves a distinct command allocator pool per replay context, but `par_fresh_switch` still transfers the new command buffer, arenas, update ring, post list and visibility buffer between contexts. Concurrent jobs still need independent resources and ordered completion/retirement.
- `sub_cut` commits immediately. Speculative chunks need bounded command-list ranges and a way to discard failed speculation before any GPU commit or CPU-visible side effect.
- Every measured busy-scene boundary has an open occlusion query. The scan must carry query/reset state, and reports must combine preceding chunks' visibility slots in command order.
- Deferred recording now has independent held streams and the uniform-copy queue has atomic producer reservation, tested concurrently below. Replay jobs must still publish complete streams in command order and finish their copies before releasing discarded scratch storage.
- Upload batches, texture state, lazy shader/pipeline caches and diagnostics still require an ownership/locking audit. A successful serialized ThreadSanitizer run does not cover simultaneous writers.

Do not advertise `thread1` as a parallel throughput build or infer that adding threads to its existing driver is safe.

Follow-up validation: 111 worker checks pass in each of direct encoding, deferred encoding, and both ThreadSanitizer variants. Queue-origin rendering passes 398 checks in both encoding modes, copy-alias coverage passes 101 in both, the default partial depth/stencil clear test passes 59, and the NVN fixture including worker timing passes 240. The combined tree passes 111 worker checks in both encoding modes. The docs suite passes 11 tests; the C analyzer reports no baseline regressions. No updated gameplay run or full multi-game regression is claimed. The installed Bananza app has not been rebuilt with this follow-up.

### Context allocator and query ownership

`thread1` now gives each replay context an independent command allocator pool. A submission reserves at most one allocator from each pool; returning to a context reuses that reservation without resetting it. Every reserved allocator is retired at the final submission fence, including those used only by early segments. Pools grow lazily, with eight allocators per context; normal serial replay retains its original single pool and limit. Ordinary geometry and copy cuts keep their existing entry point and allocator.

Occlusion-query accumulation now belongs to each replay context, including the live slot list, counting flags, current pass and visibility-ring retirement index. Serialized handoffs transfer ownership and clear the previous context's pointers. Recorded reports retain independent snapshots, so subsequent resets cannot erase their results. This is ownership groundwork; independently translated chunks still need a prefix merge for queries spanning chunks.

The worker fixture now passes 146 checks in direct/deferred encoding and both ThreadSanitizer variants. It cycles through nine handoffs in one submission, checks distinct allocator objects and final-fence retirement, verifies every GPU write after context reuse, and checks query-report prefixes across a handoff and reset. The query fixture uses known slot values to isolate accumulation from rasterization. The combined tree passes the same 146 checks in both encoding modes. Queue-origin rendering passes 398 checks in both modes; deferred-pass rendering passes 59 checks. Static analysis reports no new findings. Overlapping translation and an updated gameplay build remain pending.

### Concurrent recording, copy producers and scratch reuse

Each replay context now owns its current record block. A held stream keeps full blocks private instead of automatically publishing them. Once its producer has finished, the coordinator can enqueue its entire chain atomically in command order or discard it, releasing retained objects without executing the records. Waiting for GPU work from an unpublished held stream is rejected. Queue callbacks, upload commits and signals respect holding even before the encode thread starts; the startup flag is atomic. Serialized context switches publish the new command-buffer prefix before the next context records its commands.

This is command-record cancellation, not rollback of arbitrary replay-side mutations. The full translator does not yet run speculative jobs through held streams. Texture updates, residency, query prefixes and other shared state still need the earlier audit and an ordered replay-job coordinator. Metal encoding itself remains on one consumer thread.

Uniform-copy producers now serialize slot reservation and publication with a separate producer mutex. The copy consumer never takes that mutex, so ring backpressure can wait for progress without blocking the consumer. Dependencies within each producer retain their enqueue order. Scratch-arena claim, trim and retirement use the same pool mutex; every context initializes its own active arena list, and the arena creation statistic is atomic.

`check_t_parallel_record` and `check_t_parallel_record_tsan` pass 32 checks. Two record producers finish in reverse order; publication still yields ordered callbacks and dependent GPU writes, including full-block boundaries. Cancellation releases captured objects and executes no callbacks, and a discarded stream is reusable. Two uniform-copy producers enqueue 16,418 dependent copies, verifying unique completion sequences and all copied bytes through ring wraparound. Two contexts also claim and retire scratch buffers 1,000 times each, proving simultaneous ownership is distinct and completed buffers are reused. Tests explicitly exercise cold encoder startup. These are concurrent component tests, not a Bananza throughput measurement or proof that the whole replay is safe to overlap.

Producer follow-up validation: the combined tree passes the 32 concurrent-component checks and 146 worker checks in each encoding mode. The PR tree also passes both 146-check worker ThreadSanitizer runs, the default NVN fixture (240 checks), and the docs suite (11 tests); static analysis has no baseline regression. The combined tree needed the existing PR queue-callback record helper carried over. No app rebuild, gameplay speedup, commit or push is claimed for this stage.

### Invocation-aware plans and bounded continuation

A boundary is now identified by its opcode address, return stack and remaining submission handles. The same command list can be called several times; matching only its address could select a later invocation's snapshot for an earlier call. The scan owns its cursor and no longer depends on the replay context's remaining handles. Incomplete walks, excessive nesting and unsupported write operations invalidate the entire plan, including snapshots already collected. This keeps a partially modeled submission on serial replay.

The continuation driver accepts an exclusive invocation endpoint and propagates that stop through recursive calls without consuming the saved return stack. It validates endpoint reachability before replay so an invalid range cannot partially execute. The existing serialized worker uses the same driver with no endpoint. Bounded execution is currently exercised only by the fixture: production overlap still requires lookahead confinement, independent resource ownership, query-prefix merging and ordered publication. A bounded opcode loop alone does not constrain optimizations which execute later work early.

The tests distinguish repeated calls and repeated submission handles, check exact snapshot selection after skipped boundaries, reject malformed/cyclic control flow, and verify individual state operations across bounded replay and nested returns. They also verify that an empty range executes nothing and an unreachable endpoint changes no state. These are correctness tests, not a throughput result; no new gameplay build has been installed for this stage.

## The coordinator, slice A: planned chunks (`DM_NVN_PAR=plan`)

The scan plans the chunks before the replay starts. Of its full snapshots (one every `DM_NVN_PAR_DRAWS` draws, 400 by default in this mode), it picks the ones nearest a quarter, a half and three quarters of the submission's draws. The submitting thread's replay stops before the first split. At the commit, `par_plan_run` ends that chunk: its pass, the uniform updates still deferred, and its last segment, committed in order. If the boundary is clean and the replay's S and R equal the scan's, it then translates each next chunk with `par_continue` from the snapshot, in a fresh context, into its own command buffer begun from that context's allocator, with its own arenas. A boundary that isn't clean, a state that differs, or a range the driver refuses finishes the submission serially in the current context. Every chunk's arenas retire with the submission's fence.

The chunks still run one after another on the submitting thread. Still passed along from chunk to chunk, where overlap will need an independent copy merged in command order: the post list, the visibility buffer, the segment count and clock, the update ring, the commit blocks and the submission's autorelease pool.

`make -C dm/gfx/tests GAME=040074A01BF12000 check_t_parallel_plan` (and `_tsan`): six lists, two of them called from a parent list, each changing the render target, drawing, filling four bytes and signalling. The real scan plans three splits. Every event comes out once and in order, every GPU write lands, chunks take allocators from their own pools, arenas retire, the autorelease pool is popped once, and a snapshot made to differ finishes the submission serially at that split, still exactly. 73 checks, in direct and deferred encoding and under ThreadSanitizer.
