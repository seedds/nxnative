# Port work log

A dated record of what is done, what is in progress and what is left on the Windows and Linux port (branch `multiplataforma`). The phases, decisions and open items of the port are in [migration-status.md](migration-status.md); this file is the running log a session reads first to pick up work that another session left in progress. Newest entry first.

## 2026-11-14

### Done and pushed

| Commit | What |
|---|---|
| `8b44de4` | Handoff: the state at the pause |

Done on the machine, not in the repository: Docker emptied on the owner's request (every image and container, the owner's other projects' included; the data volumes `aitrade_pgdata`, `smilepetstore_pgdata`, `smilepetstore_redisdata` kept).

### Checked

- Python suites at `649d422` without Docker: `test_docs` 10 of 10, the symptom index check and `env_switches.py --check` pass. The other suites end in errors only where this PC lacks `lz4`, `capstone`, the Mac tools, APFS or `DM_GAME`.
- GitHub Actions: no run since 2026-10-29 for any later commit. `port` and `aot` are active and their triggers match a push to this branch; `tests` was disabled by hand on 2026-10-31. The reason is not visible through the API: the repository's Actions settings and the account's billing are to be checked by the owner.

### In progress: written by agents, NOT built and NOT verified

No Docker image exists (the disk filled while ten worktrees were checked out, 280 MB each), so nothing below has been compiled or run. Each item is uncommitted in its worktree under `.claude/worktrees/`; a new session reads the diff there (`git -C <worktree> diff`), verifies it in Docker, and only then cherry-picks it. One build at a time: the two images (`nxnative-build` 1.9 GB, `nxnative-aot` 1.9 GB) do not fit beside the worktrees.

| Worktree | Task | Files it owns |
|---|---|---|
| branch `wip/second-pack-profile` (`cfad601`, no worktree) | The profile half of `t_second_pack`: `aot_synth`'s entry runs `synth_hot` for 20 ms. Its agent ran 27 checks and the module's other tests green before the images went; the full suites were not run | `dm/tools/tests/aot_synth.py`, `dm/host/tests/t_second_pack.py` |
| `agent-a5e784acaf61ec07e` (on top of `cfad601`) | The launcher's "a better pack is possible" rule counts sampled functions, not instruction rows: the host's profile gets `# function` lines, the packer's `made` rows and the launcher read them | `dm/host/aot_profile.c`, `dm/launcher/launcher_model.*`, `scripts/build_ipa_from_nsp.py` (`made_rows`), their tests |
| `agent-ab4b98db3bb336302` | Hot indirect targets (design 12 of the recompiler): the host writes `<module>.targets.tsv`, nxaot guards the dominant target with a direct call at tier H | new `dm/host/aot_targets.c`, new `t_aot_targets.c`, `dm/tools/aot/src/build/pack.cpp`, `src/lift/*`, new `test_profile_targets.py` |
| `agent-aaa1c2d479f167006` | The provoking vertex of converted quad strips and polygons; a flat-varying check through the guest GL layer. The SPIR-V headers under `shaders/gen` must be regenerated in the image | `dm/gfx/hostvk/vk_index.c`, `shaders/index_expand.comp`, `index_restart.comp`, `t_vk_provoking.c`, `t_vk_guest_gl.c` |
| `agent-a255f3c0fc84abb1d` | The host test runners also run the Python-driver tests (`t_second_pack`, `t_vk_spirv`) | `dm/host/CMakeLists.txt` (two registrations), `run_host_tests.sh`, `run_win64_tests.ps1` |
| `agent-a9b88962d77203ce4` | Python tests skip with a reason where this PC lacks a module or a Mac tool, instead of ending in errors | `dm/tools/tests/test_*.py`, `dm/ios/tests`, `dm/gfx/tests` |
| `agent-a0e9229602796991b` | Why the last CI runs failed (`tests` #13, `aot` #11, `chain` #2), and fixes to the workflow files | `.github/workflows/*.yml` |
| `agent-ada28324b2ad7f998` | The switch inventory covers the port's non-`DM_` switches (`NXHOST`, `NXPACK`, `NXAOT`, ...); five stale mentions of removed switches | `dm/tools/env_switches.py`, `docs/env_switches.tsv` |
| `agent-aea2f0238dea05726` | `scripts/port_docker.sh`: builds the images when missing with disk checks, runs the whole verification, a low-disk mode that never holds both images | new `scripts/port_docker.sh`, new `test_port_docker.py` |
| `agent-a3536462689c8c57c` | `measure_packs.py`: measures two builds of a pack against each other (pinned, alternating rounds, layout spread) | new files under `dm/tools/aot/tests/` |
| `agent-aeff9ad0fb6c3dfd8` | Apple's log macros write each line in one stdio call, as the port's `LOG` does since `9872bae`. Prepared for a Mac: not to be merged before a Mac or macOS CI run | `dm/host/dm.h` (Apple branches), `docs/reference/threads.md` |

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
