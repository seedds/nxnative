# Port work log

A dated record of what is done, what is in progress and what is left on the Windows and Linux port (branch `multiplataforma`). The phases, decisions and open items of the port are in [migration-status.md](migration-status.md); this file is the running log a session reads first to pick up work that another session left in progress. Newest entry first.

## 2026-11-15

### Done and pushed

| Commit | What |
|---|---|
| `35d6a49` | Tools: `regress.py impact` no longer hangs on a range that touches the stb or zycore headers (the pattern that finds C definitions took 2^lines steps on them: the report of `origin/main..HEAD` ran 20 hours before it was stopped; now 18 s). `test_usage` has the shape; `test_usage` and `test_regress` pass (31 tests) |
| `daefef3` | Vulkan: `t_vk_present`'s failure under load (about 2 % of runs) traced: lavapipe runs the blit inside `vkQueuePresentKHR`, so the first present passed the presenter's 100 ms limit and the fourth frame was dropped, as its rule says. The drop's red line now names the step that held the present thread and for how long, per run of drops; the test asserts the guarantee and has a held-thread check. Verified in Docker (host 100 of 100 twice, `t_vk_present` 55 / 55 / 11, 40 repetitions, the tree without the JIT, the Windows cross-build) |

### In progress

- **Safepoint polls left out of loops bounded by a constant, at tiers H and H+** (agent's measurements: int_ops-clang -8.9 %, simd-gcc -7.9 %, switch-gcc and simd-clang -2.9 %, the rest within 1 %; the second pack 6.6 % slower to make and 44 MiB more at its peak on the benchmark; unit key 6; two `polls` rows in `t_aot_host`). Reviewed by the lead by reading the diff; NOT built or run on this base: parked as one commit on the local branch `wip/safepoint-polls-bounded` (on `d163e57`, its conflict with `t_aot_host.c`'s `guestbt` rows resolved; its message has the next steps). To verify: the pattern of the earlier ones (nxaot built and its CTest, expected 29 of 29; the corpus packs; host CTest with `NXAOT`, plain and with `DM_AOT_ARENA_CHECK=1`; a tree with `-DNXHOST_JIT=OFF`; the Windows cross-build).
- **`nxaot.exe`: the recompiler built for Windows** (an agent, worktree under `.claude/worktrees/`): LLVM 18.1.3 and lld cross-built with llvm-mingw from the release's source tarballs by a committed script, `nxaot.exe` linked statically, taken into the Windows bundle by `scripts/package_port.py --aot`, a runner for the Windows PC. Its build tree is on tmpfs and is lost when the machine is switched off; the script and the changes are in its worktree.
- The owner switches the machine off to enlarge the disk once the agent ends: `/tmp` (tmpfs) is emptied by that, so the corpus packs and `nxaot` of the verifications are built again by the next verification (its first stage does it).

### Left, not started (added today)

- The presenter holds the queue lock across `vkQueuePresentKHR` ([frame-rate.md](../reference/frame-rate.md#a-held-present-thread-drops-the-waiting-frame-and-names-what-held-it), Open): a queue of its own where the device has a second one; lavapipe has one queue, so it waits for the RTX 4060.
- The Windows bundle still needs Python 3.12 with `lz4` and `capstone` and the .NET SDK on the player's PC (`scripts/package_port.py`, README.txt): an embedded Python with the wheels and a prebuilt `nx_extract` would remove both; the Vulkan shader translator is not in the bundle either.
- The calling convention (ghccc, 0 to 4.8 % faster): not started; it changes the unwind tables of the Windows packs (`dm/tools/aot/src/build/coff.cpp`), which cannot be tested here. The owner's decision, asked on 2026-11-15.

## 2026-11-14

### Done and pushed

| Commit | What |
|---|---|
| `8b44de4` | Handoff: the state at the pause |
| `5d3f25e` | AOT: `dm/tools/aot/tests/measure_packs.py`, two builds of a pack measured against each other; its unit test passes here (9 tests); not yet run against the real `nxaot` |
| `df5c568` | Scripts: `scripts/port_docker.sh` builds the two images when missing with disk checks and runs the whole verification, `--low-disk` one image at a time; its test with a stand-in Docker passes here (13 tests); no real build or verify run yet |
| `931af1a` | CI: the `chain` job's translator build ran outside its image (a comment line after a backslash): fixed; the `aot` job prints the failed tests' logs and the runner's CPU; the last runs' failures read: the two `tests` jobs were fixed by `fc0a8f1`, the `aot` vector run's mismatch is undetermined (its log expired) |
| `d47d9b5` | Tests: the Python suites skip with a named reason where this PC lacks a module or a Mac tool (0 failures and 0 errors here now); `scripts/regress.py` loads Apple's libproc at its first use, so `regress.py impact` and `test_regress` run on Linux |
| `c9b2d4e` | Tools: the switch inventory covers the port's non-`DM_` switches (62 rows added); a retired name the docs quote is checked, not listed as a warning |
| `361d769` | Tools: `own_compilers.py` reports a missing Metal compiler when `xcrun` itself is missing |
| `8ef8546` | Host tests: the profile half of the second-pack loop (`aot_synth`'s entry runs `synth_hot` for 20 ms; `t_second_pack` 27 checks). Verified in Docker |
| `8caf012` | Launcher: the better-pack rule counts sampled functions (`# function` lines in the host's profile); `t_launcher_model` 154 checks. Verified in Docker |
| `da9f34b` | Host tests: the runners run the script tests (`t_vk_spirv`, `t_second_pack`) from `host_scripts.tsv`. Verified in Docker on Linux; the PowerShell runner not run |
| `93b94dd` | Port: `nvn_state.c`'s `SLOG` writes a line in one stdio call outside Apple; `port.yml` uploads `host_scripts.tsv` for the Windows job. Verified in Docker (99 of 99 twice) |
| `b1eff1c` | Vulkan: converted quad strips and polygons take NVN's provoking vertex (the same triangles rotated); `t_vk_provoking` 37 checks, `t_vk_guest_gl` 53. Verified in Docker on lavapipe |
| `eaa9aa1` | AOT: hot indirect targets (design 12): the host writes `<module>.targets.tsv`, nxaot guards a site's dominant target with a direct call at tier H. Verified in Docker (nxaot 29 of 29, host 100 of 100 twice, packs identical without the file, the Windows cross-build); measured within noise, no gain shown |
| `561082f` | Host tests: `t_second_pack` can run on Windows (a platform table, prebuilt packs where there is no nxaot, the engine read from the log). Verified in Docker on Linux, in a tree without the JIT, and by the Windows cross-build; nothing run on Windows |
| `61e11b3` | Port: the Windows host gets the JIT fallback (a vendored subset of Boost 1.83.0, 869 headers; two missing includes in dynarmic; the Windows fault handler takes a fault inside a JIT callback as a guest fault). The cross-build with `-DNXHOST_JIT=ON` builds and links; Linux CTest 100 of 100 twice; nothing run on Windows |
| `0ccf7a2` | Launcher and packer: the targets file in the `made` rows, the packs' reuse key (it was missing: a repack reused the old pack) and the better-pack rule ("N known call targets the packs were made without"). Verified in Docker (`t_launcher_model` 174 checks, `t_second_pack` 29 / 29 / 27 by route) |
| `4cbabbf` | AOT: a `br` after a call was lowered as a tail call with the wrong return address, which stranded the thread in the JIT fallback (the benchmark's 5,124 "nonlocal" misses): the 4,000-function benchmark goes from 1.529 s to 189.8 ms. Verified in Docker (nxaot 29 of 29, host 100 of 100 twice, `t_aot_host` 50 checks, the Windows cross-build) |
| `fb0a872` | AOT discovery: a jump table's index may depend on several values (the form of a switch on a computed value was left to the dispatcher): the benchmark's 736 tables all bounded, 0 JIT runs, 181.9 ms to 1.097 ms; except-gcc 26.1 ms to 0.64 ms. Verified in Docker (nxaot 29 of 29, host 100 of 100 twice, a tree without the JIT, the Windows cross-build) |
| `06928a9` | Host: the JIT's first use no longer stops the thread that needs it: the decoder table and a spare instance are made ahead of time on a helper thread, and the JIT glue is compiled with `-O2` (it had no optimisation): 200-233 ms to 0.03-0.08 ms. Verified in Docker (nxaot 29 of 29, host 100 of 100 twice, `t_aot_host` 52 checks, the Windows cross-build) |
| `9daae9a` | Port: the host's own C is compiled at `-O2` instead of `-O1` (CPU ASTC decode -22 %, CPU deswizzle -18 %, audio volume -20 %, the rest within noise; `-O3` adds nothing). Bench modes in five tests. Verified in Docker (host 100 of 100 twice, every check count the same at both levels, the Windows cross-build) |
| `38293f9` | AOT: the recompiled corpus measured against the same C compiled natively (`native_ratio.py`), and three lowerings closer to it (sbfm as a shift pair, a loop's registers stored once on entry, NZCV stored at a call only where live): int_ops 1.60-1.65 to 1.28-1.31 times native, switch 2.57 / 1.87 to 2.30 / 1.77. Verified in Docker (vectors 0 mismatches, nxaot 29 of 29, host 100 of 100 twice, the Windows cross-build) |
| `4b6f87d` | Port: a guest abort, `svcBreak` or fault prints the guest's call site and frames (the host's own return address and frames are not the guest's on the x86-64 engine); Apple's preprocessed text unchanged. Verified in Docker (`t_aot_host` 58 checks, host 100 of 100 twice, the Windows cross-build) |
| (this commit's parent) | Port: the audio hooks log the game's call site through the same helper. Verified in Docker (host 100 of 100 twice) |

Done on the machine, not in the repository: Docker emptied on the owner's request (every image and container, the owner's other projects' included; the data volumes `aitrade_pgdata`, `smilepetstore_pgdata`, `smilepetstore_redisdata` kept).

### Checked

- Windows cross-build at `93b94dd`, done locally as CI's `win64-cross` job does it (`mstorsjo/llvm-mingw:20261020`, `-DNXHOST_PROGRAM=ON -DNXHOST_LAUNCHER=ON`, targets `all nxhost-run nxlauncher`): builds and links, 84 `.exe`, 24 warnings and no error. This covers every commit since CI's last run (2026-10-29) for compiling on Windows; nothing was run on Windows.
- Python suites at `649d422` without Docker: `test_docs` 10 of 10, the symptom index check and `env_switches.py --check` pass. The other suites end in errors only where this PC lacks `lz4`, `capstone`, the Mac tools, APFS or `DM_GAME`.
- GitHub Actions: no run since 2026-10-29 for any later commit. `port` and `aot` are active and their triggers match a push to this branch; `tests` was disabled by hand on 2026-10-31. The reason is not visible through the API: the repository's Actions settings and the account's billing are to be checked by the owner.

### In progress: written by agents, NOT built and NOT verified

Verified and integrated on 2026-11-14 (nxaot CTest 28 of 28; host CTest 99 of 99 with and without `DM_AOT_ARENA_CHECK`, `NXAOT` given): the three rows that were `wip/second-pack-profile`, `wip/launcher-rule-by-function` and `wip/runner-scripts`. Agent at work: safepoint polls that a bounded loop does not need (3 to 9 % on switch and the benchmark).

No Docker image exists (the disk filled while ten worktrees were checked out, 280 MB each), so nothing below has been compiled or run. Each item is uncommitted in its worktree under `.claude/worktrees/`; a new session reads the diff there (`git -C <worktree> diff`), verifies it in Docker, and only then cherry-picks it. One build at a time: the two images (`nxnative-build` 1.9 GB, `nxnative-aot` 1.9 GB) do not fit beside the worktrees.

| Worktree | Task | Files it owns |
|---|---|---|
| branch `wip/apple-log-one-call` (parked: one commit, no worktree) | Apple's log macros write each line in one stdio call, as the port's `LOG` does since `9872bae`. Prepared for a Mac: not to be merged before a Mac or macOS CI run | `dm/host/dm.h` (Apple branches), `docs/reference/threads.md` |
| branch `wip/ub-fixes-shared` (parked: one commit, no worktree) | Three one-line fixes of undefined behaviour found by UBSan in trap mode, in files Apple also compiles. For a Mac: not to be merged before a Mac or macOS CI run | `dm/host/hle_ro.c`, `dm/gfx/nvn/nvn_pools.c`, `dm/gfx/tests/t_replay.c` |

### Waiting on the owner

What only the owner can do, in the order that unblocks most:

1. **GitHub Actions.** No workflow has run since 2026-10-29, so nothing later is checked on Windows or macOS by CI. `port` and `aot` are active and trigger on every push to this branch; `tests` was disabled by hand on 2026-10-31. Look at the repository's Settings, Actions (are actions allowed, is there a spending limit) and at the account's billing; re-enable `tests`. A lead (checked 2026-11-14, evening): the last runs are for the owner's own last push (`6675377`, 2026-10-29 13:53 UTC); every commit after it was pushed by the session with the stored credential, the first on 2026-10-31 18:54, and none started a run, on any workflow. The quickest test: Actions, `port`, "Run workflow" on `multiplataforma` (the session's API token cannot: 403), or one push from the owner's own account. If that run starts, Actions works and the stored credential's pushes are what does not trigger it. The first run will also show whether the fixes of `fc0a8f1` and `931af1a` hold.
2. **The Windows PC.** Nothing of the port has run on Windows since 2026-10-29. With CI back: unzip the artifacts `win64-tests` into `build\win64` and `win64-packs` into `build\packs\win64` of a checkout of the same commit, then `powershell -NoProfile -ExecutionPolicy Bypass -File dm\host\tests\run_win64_tests.ps1`, and push `logs\win64\`. Without CI the two folders can be made on Linux: `mstorsjo/llvm-mingw:20261020` for `build/win64` and `build_packs.sh` for the packs (docker/README.md). Expected with the JIT now in the Windows host: `t_aot_host` runs all 18 corpus programs, `t_second_pack` ends `29 checks, 0 failed`.
3. **A game package.** Phases 3 and 5 need one NSP with its keys in a folder outside the repository and the home folder, and `scripts/build_ipa_from_nsp.py --platform linux` (or `windows`) on it. Everything so far ran on homebrew and a synthetic module.
4. **The RTX 4060.** The Vulkan backend has only run on lavapipe (software). `nxhost-run` with a packed game on that GPU, and the `t_vk_*` tests there, are the next step of phase 4.
5. **A Mac or macOS CI** for the parked branches `wip/apple-log-one-call` and `wip/ub-fixes-shared` (each commit message lists the checks) and for the Metal backend's provoking vertex.

### Left, not started

- Verify and integrate every row above (Docker; the Apple one on a Mac).
- `t_astc_mesa` is registered outside `host_test` and is in no runner (declared in the manifest as CTest only).
- Hot indirect targets: once a site is guarded its dominant target never binds again while other targets may, so over sessions a guarded site can lose dominance and its guard (the counts are binds, not calls).
- After the tail-call fix: the JIT calls only functions with a standard entry natively (a JIT-only entry per function would cure it); a misses file written by an older host keeps its bogus `nonlocal` rows until it is removed; the calling convention: ghccc measured up to 4.8 % faster than swifttailcc (it changes the packs' ABI: the owner's decision); `t_vk_present` failed once under a loaded `-j4` run ("took no frame for 100 ms").
- Converted quads still take another provoking vertex than the Switch's (declared: the fix changes the quad's diagonal).
- The Metal backend takes the first vertex on every shape where the Switch takes the last (a Mac).
- On a real GPU: the cost of one dispatch per small index conversion, the 3 MiB arena for indirect draws.
- The first real game (no game package on this machine); anything on Windows; Vulkan on the RTX 4060.

## 2026-11-02

Done, verified in Docker and pushed, oldest first:

| Commit | What |
|---|---|
| `9872bae` | Host: one stdio call per `LOG` line on the port: the cause of `t_aot_host`'s flake |
| `3616b3d` | AOT: a self-contained `nxaot` (LLVM linked statically) for the bundle |
| `ac03f4b` | Docs: the port's documents audited against the code |
| `83ed840` | Host tests: `t_audio_sdl`'s leak check by thread IDs; the AOT runner runs `lines` |
| `da553e6` | Vulkan: every index conversion of direct draws on the GPU, lists with restart native |
| `471bf37` | AOT: the sample profile weighs tier H's code |
| `9cd61da` | Vulkan: the provoking vertex is the last, as NVN's |
| `e364c82` | Host tests: the player's second-pack loop end to end on the synthetic module |
| `1e99353` | Host: the sampler records guest instruction offsets; the exit SIGSEGV after a recorded JIT session |
| `54ed544` | Vulkan: the index conversions of indirect and transform-feedback draws |

Totals at `649d422`: host CTest 99 of 99 with and without `DM_AOT_ARENA_CHECK` (with `NXAOT` given), nxaot CTest 28 of 28.
