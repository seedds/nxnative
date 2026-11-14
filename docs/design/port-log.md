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
| (this commit's parent) | Tools: the switch inventory covers the port's non-`DM_` switches (62 rows added); a retired name the docs quote is checked, not listed as a warning |

Done on the machine, not in the repository: Docker emptied on the owner's request (every image and container, the owner's other projects' included; the data volumes `aitrade_pgdata`, `smilepetstore_pgdata`, `smilepetstore_redisdata` kept).

### Checked

- Python suites at `649d422` without Docker: `test_docs` 10 of 10, the symptom index check and `env_switches.py --check` pass. The other suites end in errors only where this PC lacks `lz4`, `capstone`, the Mac tools, APFS or `DM_GAME`.
- GitHub Actions: no run since 2026-10-29 for any later commit. `port` and `aot` are active and their triggers match a push to this branch; `tests` was disabled by hand on 2026-10-31. The reason is not visible through the API: the repository's Actions settings and the account's billing are to be checked by the owner.

### In progress: written by agents, NOT built and NOT verified

Verification under way (started 2026-11-14): the branch `verify/wip-1` (worktree `.claude/worktrees/verify`) is the tip plus `wip/second-pack-profile`, `wip/launcher-rule-by-function` and `wip/runner-scripts`; `scripts/port_docker.sh verify --low-disk` runs on it (its first real run). Green: those three commits go onto `multiplataforma` and their `wip/*` branches are deleted. Not in it: `wip/provoking-strips-polygons` (its SPIR-V headers must be regenerated in the build image first).

No Docker image exists (the disk filled while ten worktrees were checked out, 280 MB each), so nothing below has been compiled or run. Each item is uncommitted in its worktree under `.claude/worktrees/`; a new session reads the diff there (`git -C <worktree> diff`), verifies it in Docker, and only then cherry-picks it. One build at a time: the two images (`nxnative-build` 1.9 GB, `nxnative-aot` 1.9 GB) do not fit beside the worktrees.

| Worktree | Task | Files it owns |
|---|---|---|
| branch `wip/second-pack-profile` (`cfad601`, no worktree) | The profile half of `t_second_pack`: `aot_synth`'s entry runs `synth_hot` for 20 ms. Its agent ran 27 checks and the module's other tests green before the images went; the full suites were not run | `dm/tools/tests/aot_synth.py`, `dm/host/tests/t_second_pack.py` |
| branch `wip/launcher-rule-by-function` (parked: one commit, no worktree) (on top of `cfad601`) | The launcher's "a better pack is possible" rule counts sampled functions, not instruction rows: the host's profile gets `# function` lines, the packer's `made` rows and the launcher read them | `dm/host/aot_profile.c`, `dm/launcher/launcher_model.*`, `scripts/build_ipa_from_nsp.py` (`made_rows`), their tests |
| `agent-ab4b98db3bb336302` | Hot indirect targets (design 12 of the recompiler): the host writes `<module>.targets.tsv`, nxaot guards the dominant target with a direct call at tier H | new `dm/host/aot_targets.c`, new `t_aot_targets.c`, `dm/tools/aot/src/build/pack.cpp`, `src/lift/*`, new `test_profile_targets.py` |
| branch `wip/provoking-strips-polygons` (parked: one commit, no worktree) | The provoking vertex of converted quad strips and polygons; a flat-varying check through the guest GL layer. The SPIR-V headers under `shaders/gen` must be regenerated in the image | `dm/gfx/hostvk/vk_index.c`, `shaders/index_expand.comp`, `index_restart.comp`, `t_vk_provoking.c`, `t_vk_guest_gl.c` |
| branch `wip/runner-scripts` (parked: one commit, no worktree) | The host test runners also run the Python-driver tests (`t_second_pack`, `t_vk_spirv`) | `dm/host/CMakeLists.txt` (two registrations), `run_host_tests.sh`, `run_win64_tests.ps1` |
| branch `wip/apple-log-one-call` (parked: one commit, no worktree) | Apple's log macros write each line in one stdio call, as the port's `LOG` does since `9872bae`. Prepared for a Mac: not to be merged before a Mac or macOS CI run | `dm/host/dm.h` (Apple branches), `docs/reference/threads.md` |

### Left, not started

- Verify and integrate every row above (Docker; the Apple one on a Mac).
- The launcher's rule and the packer's `made` rows for the targets file.
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
