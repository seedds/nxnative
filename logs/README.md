# Test logs

The logs of host unit test runs, kept in the repository on purpose: when a run fails on one machine (the Windows PC that runs the cross-built tests, a Linux machine running the Docker build), its logs are committed and pushed, so that whoever fixes the failure, a developer or an agent on another machine, reads exactly what happened there. This folder is not git-ignored; the build trees in `build/` are. [docker/README.md](../docker/README.md) has the build and run commands.

| Folder | Written by |
|---|---|
| `logs/linux/` | `sh dm/host/tests/run_host_tests.sh build/linux`, in the `nxnative-build` Docker image |
| `logs/win64/` | `dm\host\tests\run_win64_tests.ps1` (PowerShell) or `sh dm/host/tests/run_host_tests.sh build/win64` (Git Bash), on a Windows PC, for the tree cross-built in Docker |

## Files

- `<test>.log`, one per run of a test program, named after the run as `build/<platform>/host_tests.tsv` names it (a run with arguments has them in its name: `t_preempt--all-peers.log`). It starts with three lines: `date:` (UTC), `commit:` (`git rev-parse HEAD` of the checkout, followed by `(with uncommitted changes)` when tracked files outside `logs/` differed from it) and `command:` (the exact command that ran the test, from its build folder). After a blank line comes everything the test wrote, standard output and standard error together, and last its `exit:` line (the exit code, and the time it took).
- `summary.txt`, one per run of the runner. It starts with the same three lines (`command:` is the runner's own command line), then the folder it ran in, the host system, the build folder, and the build's own facts from `host_tests.tsv`: platform, system, compiler, game profile, when it was configured, and the tests not built for that platform. Then one line per run (`PASS`, `FAIL` with its exit code, or `TIMEOUT`), and the total.

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
