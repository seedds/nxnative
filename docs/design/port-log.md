# Port work log

A dated record of what is done, what is in progress and what is left on the Windows and Linux port (branch `multiplataforma`). The phases, decisions and open items of the port are in [migration-status.md](migration-status.md); this file is the running log a session reads first to pick up work that another session left in progress. Newest entry first.

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

Done on the machine, not in the repository: Docker emptied on the owner's request (every image and container, the owner's other projects' included; the data volumes `aitrade_pgdata`, `smilepetstore_pgdata`, `smilepetstore_redisdata` kept).

### Checked

- Windows cross-build at `93b94dd`, done locally as CI's `win64-cross` job does it (`mstorsjo/llvm-mingw:20261020`, `-DNXHOST_PROGRAM=ON -DNXHOST_LAUNCHER=ON`, targets `all nxhost-run nxlauncher`): builds and links, 84 `.exe`, 24 warnings and no error. This covers every commit since CI's last run (2026-10-29) for compiling on Windows; nothing was run on Windows.
- Python suites at `649d422` without Docker: `test_docs` 10 of 10, the symptom index check and `env_switches.py --check` pass. The other suites end in errors only where this PC lacks `lz4`, `capstone`, the Mac tools, APFS or `DM_GAME`.
- GitHub Actions: no run since 2026-10-29 for any later commit. `port` and `aot` are active and their triggers match a push to this branch; `tests` was disabled by hand on 2026-10-31. The reason is not visible through the API: the repository's Actions settings and the account's billing are to be checked by the owner.

### In progress: written by agents, NOT built and NOT verified

Verified and integrated on 2026-11-14 (nxaot CTest 28 of 28; host CTest 99 of 99 with and without `DM_AOT_ARENA_CHECK`, `NXAOT` given): the three rows that were `wip/second-pack-profile`, `wip/launcher-rule-by-function` and `wip/runner-scripts`. Agent at work: why the 4,000-function benchmark stays in the JIT fallback on non-local transfers.

No Docker image exists (the disk filled while ten worktrees were checked out, 280 MB each), so nothing below has been compiled or run. Each item is uncommitted in its worktree under `.claude/worktrees/`; a new session reads the diff there (`git -C <worktree> diff`), verifies it in Docker, and only then cherry-picks it. One build at a time: the two images (`nxnative-build` 1.9 GB, `nxnative-aot` 1.9 GB) do not fit beside the worktrees.

| Worktree | Task | Files it owns |
|---|---|---|
| branch `wip/apple-log-one-call` (parked: one commit, no worktree) | Apple's log macros write each line in one stdio call, as the port's `LOG` does since `9872bae`. Prepared for a Mac: not to be merged before a Mac or macOS CI run | `dm/host/dm.h` (Apple branches), `docs/reference/threads.md` |

### Waiting on the owner

What only the owner can do, in the order that unblocks most:

1. **GitHub Actions.** No workflow has run since 2026-10-29, so nothing later is checked on Windows or macOS by CI. `port` and `aot` are active and trigger on every push to this branch; `tests` was disabled by hand on 2026-10-31. Look at the repository's Settings, Actions (are actions allowed, is there a spending limit) and at the account's billing; re-enable `tests`. The first run will also show whether the fixes of `fc0a8f1` and `931af1a` hold.
2. **The Windows PC.** Nothing of the port has run on Windows since 2026-10-29. With CI back: unzip the artifacts `win64-tests` into `build\win64` and `win64-packs` into `build\packs\win64` of a checkout of the same commit, then `powershell -NoProfile -ExecutionPolicy Bypass -File dm\host\tests\run_win64_tests.ps1`, and push `logs\win64\`. Without CI the two folders can be made on Linux: `mstorsjo/llvm-mingw:20261020` for `build/win64` and `build_packs.sh` for the packs (docker/README.md). Expected with the JIT now in the Windows host: `t_aot_host` runs all 18 corpus programs, `t_second_pack` ends `29 checks, 0 failed`.
3. **A game package.** Phases 3 and 5 need one NSP with its keys in a folder outside the repository and the home folder, and `scripts/build_ipa_from_nsp.py --platform linux` (or `windows`) on it. Everything so far ran on homebrew and a synthetic module.
4. **The RTX 4060.** The Vulkan backend has only run on lavapipe (software). `nxhost-run` with a packed game on that GPU, and the `t_vk_*` tests there, are the next step of phase 4.
5. **A Mac or macOS CI** for the parked branch `wip/apple-log-one-call` (its commit message lists the checks) and for the Metal backend's provoking vertex.

### Left, not started

- Verify and integrate every row above (Docker; the Apple one on a Mac).
- `t_astc_mesa` is registered outside `host_test` and is in no runner (declared in the manifest as CTest only).
- Hot indirect targets: once a site is guarded its dominant target never binds again while other targets may, so over sessions a guarded site can lose dominance and its guard (the counts are binds, not calls).
- The 4,000-function benchmark spends 92 % of its samples in the JIT fallback on non-local transfers, also after a repack with its misses: the cause is to be found (a game that switches stacks would run mostly in the JIT).
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
