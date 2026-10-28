# Running the host unit tests on a Windows PC

The portable host (Phase 0 of the Windows and Linux port, [docs/design/migration-status.md](design/migration-status.md)) is cross-built for Windows on Linux or on GitHub; nothing is compiled on the PC. This page is the short path: get the test programs, run them, and send the logs back when something fails. The full build commands are in [docker/README.md](../docker/README.md).

## 1. What you need on the PC

- Windows 10 or 11, 64-bit.
- Git (Git for Windows is enough).
- PowerShell: the one that comes with Windows (5.1) or PowerShell 7. Nothing else: no Docker, no compiler, no .NET.

## 2. Get the repository and the test programs

Open PowerShell and clone the branch (or `git pull` if the clone exists):

```powershell
git clone -b multiplataforma https://github.com/charcsllc/nxnative.git
cd nxnative
```

The test programs come from GitHub Actions, where every push builds them:

1. Open the repository on GitHub, the **Actions** tab, the workflow **port**, and the newest run of the branch `multiplataforma`.
2. At the bottom of the run page, under **Artifacts**, download **win64-tests** (a zip).
3. Unzip it into the folder `build\win64` inside the clone, so that `build\win64\host_tests.tsv` and the `.exe` files (`t_fs.exe`, `t_time.exe`, …) are directly in that folder:

```powershell
New-Item -ItemType Directory -Force build\win64 | Out-Null
Expand-Archive -Path "$HOME\Downloads\win64-tests.zip" -DestinationPath build\win64 -Force
Get-ChildItem build\win64
```

(If you have Docker Desktop and want to build them yourself, follow "From Git Bash on Windows" in [docker/README.md](../docker/README.md) instead.)

## 3. Run the tests

From the repository root, in PowerShell:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File dm\host\tests\run_win64_tests.ps1 -BuildDir build\win64
```

The script runs every program listed in `build\win64\host_tests.tsv` (120 s each at most), prints one `PASS`, `FAIL` or `TIMEOUT` line per test and a final count, and writes:

- `logs\win64\<test>.log`: the output of each test, headed by the date, the commit and the exact command;
- `logs\win64\summary.txt`: the whole run.

Exit code 0 means every test passed; 1 that one or more failed; 2 that the script could not run (no build folder, no `host_tests.tsv`).

## 4. When something fails: send the logs

The `logs\` folder is tracked by git on purpose ([logs/README.md](../logs/README.md)). Commit it and push:

```powershell
git add logs\win64
git commit -m "Windows host tests: logs of a failed run"
git push
```

The next session reads `logs/win64/` from the repository and fixes the cause. When everything passes, nothing needs to be committed: the logs can stay on the PC.

## 5. The same run on GitHub

The workflow **port** also runs these tests itself on a Windows runner (job `win64-test`); its logs are the artifact **logs-win64** of the run. A failure there can be read without the PC.
