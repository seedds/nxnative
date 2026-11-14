# Test logs

The logs of host unit test runs, kept in the repository on purpose: when a run fails on one machine (the Windows PC that runs the cross-built tests, a Linux machine running the Docker build), its logs are committed and pushed, so that whoever fixes the failure, a developer or an agent on another machine, reads exactly what happened there. This folder is not git-ignored; the build trees in `build/` are. [docker/README.md](../docker/README.md) has the build and run commands.

| Folder | Written by |
|---|---|
| `logs/linux/` | `sh dm/host/tests/run_host_tests.sh build/linux`, in the `nxnative-build` Docker image |
| `logs/win64/` | `dm\host\tests\run_win64_tests.ps1` (PowerShell) or `sh dm/host/tests/run_host_tests.sh build/win64` (Git Bash), on a Windows PC, for the tree cross-built in Docker; the AOT tests with the Windows packs of `build\packs\win64` when it exists ([docker/README.md](../docker/README.md#the-recompiled-corpus-in-the-host)): built there by `build_packs.sh`, or the artifact `win64-packs` of CI's `aot` workflow for the same commit unzipped there |
| `logs/aot/` | `sh dm/tools/aot/tests/run_aot_tests.sh build/aot`, in the `nxnative-aot` Docker image: the recompiler's vector harness, census test, vectors-current check, discovery over the corpus, manifest test, corpus reference run, the corpus recompiled and run natively, the per-function differential test (and again at tier C), the JIT fallback's modes, the threaded programs, the safepoint pause test, the pack caches, the indirect calls' reloads, functions split for size, tiers by profile, Windows packs, the analysis on threads, the host's JIT misses, the workers bounded by memory, gap code, the tables' object, the line table, the lifter's opcode list and the benchmark: every CTest test of [dm/tools/aot/CMakeLists.txt](../dm/tools/aot/CMakeLists.txt) ([dm/tools/aot/README.md](../dm/tools/aot/README.md)) |

## Files

- `<test>.log`, one per run of a test program, named after the run as `build/<platform>/host_tests.tsv` names it (a run with arguments has them in its name: `t_preempt--all-peers.log`), and one per script test of `build/<platform>/host_scripts.tsv` (the tests that are a Python driver or a shell script, run after the programs: `t_second_pack`, on Linux and Windows; `t_vk_spirv`, Linux only). It starts with three lines: `date:` (UTC), `commit:` (`git rev-parse HEAD` of the checkout, followed by `(with uncommitted changes)` when tracked files outside `logs/` differed from it) and `command:` (the exact command that ran the test, from its build folder). After a blank line comes everything the test wrote, standard output and standard error together, and last its `exit:` line (the exit code, and the time it took).
- `summary.txt`, one per run of the runner. It starts with the same three lines (`command:` is the runner's own command line), then the folder it ran in, the host system, the build folder, and the build's own facts from `host_tests.tsv`: platform, system, compiler, game profile, when it was configured, and the tests not built for that platform or not run on it (`not run on Windows: t_vk_spirv (<why>)`); the PowerShell runner adds the packs folder its AOT tests ran (`packs:`), and both add the environment variables the script tests read (`script tests' environment: NXAOT=<path>` or `NXAOT not set`, and the same for `NXAOT_PACKS`). Then one line per run (`PASS`, `FAIL` with its exit code, or `TIMEOUT`; a script test that exits with its skip code, 77, is `SKIP` with the script's own skip line quoted and the variables it lacked, as `SKIP t_second_pack (exit 77, 0 s; NXAOT not set; NXAOT_PACKS not set): "t_second_pack: skipped: no nxaot ..."`, and does not fail the run), and two totals: the programs' runs, and the script tests' (passed, failed, skipped). To run `t_second_pack`, set `NXAOT=<path of nxaot>` in the runner's environment, or give it the packs `build_packs.sh` prebuilt for it: on Windows, which has no `nxaot`, the PowerShell runner passes its packs folder (`-Packs`, `NXAOT_PACKS` or `build\packs\win64`: the `win64-packs` artifact), whose `second_pack\` it takes.

## Rules

- A run replaces the `*.log` files and `summary.txt` of its folder first: a folder always holds exactly one run, so the commit that adds it says which run it is.
- When a run fails, commit its folder and push, with the number of failures in the message:

  ```sh
  git add logs/win64
  git commit -m "Host test logs: Windows, 2 failed"
  git push
  ```

  A passing run needs no commit; commit one only when someone asks for the record.
- The logs hold test output only. Game files, keys and anything extracted from games never go here ([AGENTS.md](../AGENTS.md)).
