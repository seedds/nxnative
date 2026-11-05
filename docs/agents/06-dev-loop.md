# 06 — The developer loop: which tool when, and before a pull request

One page for people and agents: the tools this repository has for building, running, debugging, measuring and checking a change, when to use which, and what a pull request needs. It links to where each tool is described instead of repeating it. Game files, keys, builds and runs stay outside the repository ([AGENTS.md](../../AGENTS.md)).

## 0. Before you start: existing work, then a branch

Most work here is done by agents, started by different people who do not see each other's sessions. A look at what already exists is what lets each session build on the others: it finds the fix that is half done, the design being replaced, the person to ask. Look first, every time, also when picking a task back up after a few days: `main` and the open work move daily. Fetch first (`git fetch origin`), so that the searches and your branch see today's `main`; before filing an issue, rebuild from it and try again, since the problem may be fixed already.

**What to search**, with the names a change would touch (the game's name and title ID, the SDK function or service, the source file, the shader opcode, the log text):

| Where | How |
|---|---|
| Work on the same game or area, by label | The workflows label every pull request by the files it changes (`dm/tools/pr_areas.py`) and every issue by its text (`issue_areas.py`): a game as `game: <title ID>`, an area as `nvn`, `host`, `shader chain`, `rewriter`, `packing`, `mac`, `ios`… `gh pr list --repo seedds/nxnative --label "game: 040074A01BF12000"` lists the open work on one game, `--label nvn --label "shader chain"` on an area, `gh issue list` the same for issues. Labels miss a pull request whose game shows only in its title, and an issue whose labels were never added |
| The same game or area, by title | `gh pr list --repo seedds/nxnative --state all --search "0400486024DEC000 in:title"` and `--search "Ocarina in:title"`: the title ID and the game's name (titles drop "The Legend of Zelda:", and use the base game's ID), the same for `gh issue list`; for an area, the function, service or file name |
| Open pull requests, drafts included | `gh pr list --repo seedds/nxnative --state open --search "<term>"`; also `--state merged` for work that landed under another name |
| Issues | `gh issue list --repo seedds/nxnative --search "<term>"`, and the labels for the area and game |
| Branches | `git fetch origin`, then `git branch -r --sort=-committerdate \| head -30` for what is active, and `git log --all --oneline -i --grep "<term>"` for commits not on `main` |
| Recent changes to the files | `git log origin/main --since=2.weeks --oneline -- <paths>`: a file changed this week may be in the middle of a redesign |
| The repository's own record | the game's note (its Open list), the [symptom index](symptoms.md), [decisions.md](../decisions.md), and `grep` over `docs/reference` and the code for the function or service |

Contributors' forks are seen only through their pull requests, and these searches find only what is labelled and titled well. When the search finds related work: build on it, comment on it, or ask its author before starting something parallel; one service built once, together, is worth more than two built apart.

Without `gh`, the same searches run on the web: the [pull requests](https://github.com/seedds/nxnative/pulls) and [issues](https://github.com/seedds/nxnative/issues) pages take the same query in their search box, for example `is:pr is:open label:"game: 040074A01BF12000"`, `is:pr 0400486024DEC000 in:title` or `is:issue label:nvn`; the [branches page](https://github.com/seedds/nxnative/branches/active) lists the active branches.

**Make your own work findable**, the other half of the same rule:

- **Open a draft pull request early**, as soon as there is a branch worth showing. Its labels come from the files it changes, so it is labelled as it grows.
- **Name the game and its title ID in the title** of every pull request and issue about one game (`Star Fox (0400E88025B1A000): …`), and the area in words the labels match (NVN, host, shader chain, rewriter, iOS, Mac). An issue is labelled from its title alone.
- **Link the issue** the work belongs to (`Fixes #…` or `Part of #…`), and say in the description what it does and does not cover yet.

**Then one branch per effort**, from current `main`:

- **A game's bring-up** (`<game>-bringup`) holds what only that game needs: its profile, note, patches and shader tables. It merges when the game plays.
- **Each general fix the bring-up turns up** (an SDK call taken over, a shader opcode, an NVN feature) goes into a short branch and pull request of its own as soon as it works, judged at its own reach ([charter](../charter.md#pull-requests)). The game's branch then rebases onto `main` and gets the fix from there. Waiting to merge it with the game is how the same service ends up written twice.
- **An optimization or a change to a shared path** is its own branch from the start, with the evidence its tier asks for.
- **Docs** go with the change they describe: a game's note with its bring-up, a reference entry with its fix. Only a docs-only change, such as a process or guide edit, is a pull request of its own.

**The cycle.** `main` is the one trunk everyone starts from and merges back into, in small pieces, often. Nobody works on it directly: your local `main` only mirrors `origin/main`. Contributors without write access do the same in their fork, and open the pull request against `seedds/nxnative`'s `main`.

1. **Start** from the newest `main`: `git fetch origin && git switch -c <short-name> origin/main`, or `git worktree add ../nxnative-<short-name> -b <short-name> origin/main` to keep it apart from other work in progress.
2. **Show it early:** push the branch and open a draft pull request as soon as there is something worth showing (`git push -u origin <short-name>`, then `gh pr create --draft` or the web page), titled with the game and title ID.
3. **Keep up:** `git fetch origin && git rebase origin/main`, at least daily; resolve conflicts while they are small.
4. **Finish:** checks for its tier ([which tier](#which-tier)), mark it ready, and once merged delete the branch and fast-forward your `main` (`git switch main && git merge --ff-only origin/main`).

Keep branches short-lived: rebase onto `main` at least daily, and merge or close within days. `main` takes dozens of commits a day, and a branch that waits for weeks meets a different codebase: the merge then costs more than the work, and its tests no longer say much about what it will do on `main`. Leave nothing uncommitted on `main`: work that sits in a working tree is invisible to everyone else and gets mixed up with the next task. The stash stack is shared by every worktree of a checkout, and another session can push or pop it at any time; `stash@{0}` may not be yours by the time you use it. So name each stash (`git stash push -u -m <tag>`), apply it by its SHA (`git stash list --format='%H %gs'`), and drop it once applied.

## 1. Build and run on the Mac

| Step | Tool | Details |
|---|---|---|
| Pack a game for the Mac | `scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> --platform mac` | Extraction, rewriter, shader split and chain, the Mac host; prints the command that runs the game. [A Mac-only build](../development.md#a-mac-only-build-for-development) |
| A double-clickable app | the same with `--platform mac-app` (adds the offline pipeline archive) | [A Mac app](../development.md#a-mac-app) |
| An iPhone build | the same without `--platform` (an unsigned IPA the user sideloads) | [The packing script in detail](../development.md#the-packing-script-in-detail) |
| Rebuild only the host | `make -C dm/host GAME=<id>` | [Build by hand (Mac)](../development.md#build-by-hand-mac) |
| Rebuild only the shaders | `DM_GAME=<id> dm/gfx/tools/dmsh_build.py` (after a chain change: `dmsh_chain.py --header` first) | the chain procedure in [AGENTS.md](../../AGENTS.md); [04](04-graphics.md) |
| Run with scripted input | `DM_KEYS` (button presses at fixed seconds or frames), `DM_CTL` (presses, screenshots and dumps while it runs), `DM_REC` (records what the player does as `DM_KEYS` steps, to replay a bug report's path or make a route) | [05, Scripted input](05-debugging.md#scripted-input-and-thread-scheduling) |

Start Mac runs with the Metal performance HUD (`DM_METAL_HUD=1`). A new title starts from the [checklist](README.md#checklist-for-a-new-title) and [03](03-bringup.md).

## 2. When something is wrong

1. Search the [symptom index](symptoms.md) for the `[RED]` line, abort text or what you see; it leads to the reference entry and the games it was found on.
2. Crashes: the host's `CRASH sig=…` report, the macOS crash report, lldb scripts, Ghidra / objdump on the rewritten modules: [05](05-debugging.md#reading-a-crash-report).
3. Hangs and threads: `DM_WATCH=<s>` / `DM_WATCH_STALL=<s>` dump every thread ([03, Reading the log](03-bringup.md#reading-the-log)); `DM_THR=1`, `DM_SVC=1`, `DM_FS=1` log thread SVCs, memory SVCs and file opens; `sample` / `xctrace` name the guest threads during a hitch ([05](05-debugging.md#scripted-input-and-thread-scheduling)); `DM_HLECOUNT=<file>` counts every HLE function and SVC the game called.
4. Guest state without a debugger: `DM_PROBE` and its watch and heap variants ([05](05-debugging.md#probes-without-a-debugger-dm_probe)).
5. Wrong picture: `DM_NVN_SHOT` screenshots, `DM_NVN_PASSDUMP` (every pass, its attachments, bindings and draw state), `DM_NVN_TEXDUMP` / `DM_NVN_TEXSNAP` (render targets as PNG), `DM_NVN_CBDUMP` (uniform buffers, vertex streams), `DM_NVN_HILITE` / `DM_NVN_DEPTHOVR` / `DM_NVN_SKIP_FS` (isolate a draw), `DM_NVN_KEYDUMP` and `DM_NVN_MAGSCAN` on the phone: [04, Diagnostic switches](04-graphics.md#7-diagnostic-switches-host) and [05, Intermittent rendering glitches](05-debugging.md#intermittent-rendering-glitches).
6. Slow frames: the `[longframe]` lines and the hitch dump (`DM_HITCH_DUMP`), pipeline compile waits ([05](05-debugging.md#pipeline-compile-waits)), GPU time per pass (`DM_NVN_GPUPROF`, `DM_AB` A/B phases, `DM_GPU_CAPTURE` for Xcode; read with `dm/tools/gpuprof.py`): [05, GPU time on the device](05-debugging.md#gpu-time-on-the-device). Attachment traffic: `dm/gfx/tools/tbdr_*.py` over a pass dump.
7. Memory: the host's memory line, `footprint`, `vmmap`, `DM_NVN_ALLOCPROF=1`: [05, Memory growth](05-debugging.md#memory-growth).
8. Shaders: `dm/gfx/tools/shader_regressions.py` (shaders that built in the previous chain and fail now), the build log's [`[RED] lost without failing`](../reference/shaders.md#losses-a-shader-build-reports) line, `DM_NVN_SHDUMP` (programs missing from the map), the offline link check ([05](05-debugging.md#offline-pipeline-link-check)).
9. On the iPhone: switches in `Documents/env.txt`, results in `Documents/log.txt` ([05, On-device diagnostics](05-debugging.md#on-device-diagnostics-ios)).
10. Game updates and fixed addresses: `dm/tools/nso_fnmap.py`, `nso_syms.py`, `insn_census.py`, `sdis.py`, `nso2elf.py` ([Tools](../development.md#tools-dmtools)).

## 3. Measuring

`scripts/bench.py <id>_build --route <name>` runs a route of the profile's `bench.tsv` from a fixed save and gives frame-time percentiles, GPU time and screenshots; variants (`--variant`) compare two hosts, shader libraries or sets of switches in one invocation, which is the only comparison it supports ([A benchmark on the Mac](../development.md#a-benchmark-on-the-mac)). Power and energy per frame: [Power](../development.md#power). Pass `--lock <file>` when other agents or people measure on the same Mac: the lock is held only while a game runs, and heavy builds should wait for it.

## 4. Checking a change on every game: `scripts/regress.py`

One chain and one host serve every profile, and each contributor owns only some games (GitHub issue #272). `scripts/regress.py` compares a baseline revision (by default the merge base with `main`) with a candidate (by default this checkout, uncommitted files included) on every game the Mac has a `--platform mac` build of. Its shader count uses the rule of `shader_regressions.py`, which the NSP script applies to a game you rebuild; the harness adds the other games, the MSL, the pipeline interface and a run. Full description: [The regression harness](../development.md#the-regression-harness).

| Command | What runs | Cost |
|---|---|---|
| `regress.py chain` | Static: each revision's shader build, compared shader by shader (built or failed, MSL / reflection / variant hashes), and the vertex / pixel interface of the pipeline archive's bases (issue #283) | nothing when the chain did not change; a changed chain translates every shader again (14 min for Breath of the Wild's 19,592 on an M2 Max) |
| `regress.py check --quick` | The above plus a short boot per game: the route's `quick` seconds, no pipeline prebuild | about a minute per game and run |
| `regress.py check` | The whole route with the pipeline prebuild first (`DM_PSO_PREBUILD=tool`) under a watchdog | Breath of the Wild about 5 min, Tears of the Kingdom about 7 |
| `regress.py replay` | Each game's current frame capture replayed by both revisions' `nvn_replay` with their own shader libraries, compared with the live frame within twice the capture's noise; `regress.py capture --rev main` records the capture (the route's `capture` frame) and measures its noise ([Frame capture and replay](../development.md#frame-capture-and-replay)) | seconds per game once the replayers and chains are built; checks the replay layer only (`nvn_mtl.m` and below) |
| `regress.py baseline --rev main --runs 2` | Records a baseline (a missing one is recorded by `check` itself); two runs give the noise | as `check`, per run |

The routes are `dm/games/<id>/smoke.tsv`; a game without one takes part in the chain checks only. Add one when you own a game: a fixed save, the presses that load it (written by hand, or played once with `DM_REC` and joined to one line), screenshots only while the player stands (where a walk ends moves from run to run), and the frame it must reach.

Add `--record` to the check on your games (`check --record`, or `baseline --record`) and commit the `dm/games/<id>/validated.tsv` it writes with the pull request: it shows which chain each game was last green on (`regress.py list` prints it). A record needs `PASS` or `CHANGED` and successful boot runs, even when the baseline has the same boot failure. Derived build profiles do not validate repository profiles. The report is saved before the metadata is written. Without `--record` the harness changes nothing in `dm/games`.

NVN smoke runs also compare the observed depth mode with the profile's `depth_mode`, even without an archive in quick mode, and detect a bundled archive rejected for a stamp mismatch. See [archive/profile depth mismatches](../reference/shaders.md#offline-pipeline-archive-and-profile-depth-disagree). Missing runtime observations remain unknown.

Reading the report (`report.md` beside `report.json` in `<root>/reports/`): one row per game, then the details. `PASS` and `CHANGED` (MSL or numbers moved, nothing worse) exit 0; `NEW-FAIL` (a shader, the interface check, a crash, a hang or the frame that worked in the baseline fails, or a shader [lost more without failing](../reference/shaders.md#losses-a-shader-build-reports)) and `REGRESSION` (new `[RED]` lines, a screenshot, frame time, memory or prepare time beyond the tolerances) exit 1; `ERROR` (the harness could not check a game) exits 2. Failures the baseline has too are listed as known and do not fail the check. Changed MSL is expected after a chain change: say in the pull request why it changed. A screenshot difference may be a fix: look at the run's screenshots in `<NSP folder>/<id>-work/regress/runs/` before calling it either. The report holds only numbers, hashes, shader keys and normalized `[RED]` texts, so it can be pasted as it is.

## 5. Which games a change can reach: usage manifests and `regress.py impact`

`regress.py` checks only the games on your Mac. For the others, each profile can carry a usage manifest, `dm/games/<id>/usage.tsv`, written by someone who owns the game with `python3 scripts/regress.py usage --games <id> --write`: the features of its translated shaders (stages, binding kinds, image and buffer atomics, transform feedback, layer and integer outputs, translator notes, texture types), and from one run of its smoke route the NVN functions, HLE functions and SVCs it called, its present path and its profile keys. It holds names and counts only.

`python3 scripts/regress.py impact <base>..<head>` (or one commit) maps a change to those names: the functions around its changed lines, followed through the host's callers to the NVN entry points and HLE functions they serve, and the rules of `dm/tools/impact_map.tsv` (a file, or functions in it, to manifest rows: `dmsh_atomic.py` to the image atomic features, the prepare-step functions of `nvn_mtl.m` to the games that run it). It lists the games that use what the change touches, those reached only through a broad rule (every game of the API), and the profiles without a manifest as unknown. It reads only git and the committed manifests, so it needs neither game files nor a harness root: the report is printed, and written as `impact.md` and `impact.json` to `--out` or, when there is one, `<root>/reports/`. Run it before a change to the chain or host, and run `regress.py check` on the games it names that you have; ask their owners for the others. A changed file no rule maps is listed: add a rule for it.

## 6. Continuous integration

`.github/workflows/tests.yml` runs on every pull request: the host's unit tests and ThreadSanitizer targets, the host and CPU texture tests again under ASan, UBSan and TSan (a ratchet over the findings listed in `dm/sanitizers/`: `make -C dm/host/tests GAME=<id> asan tsan-threads` and `make -C dm/gfx/tex/tests asan` locally), line coverage of those tests and the tool tests with the diff coverage of the pull request in the job summary (report only; `make coverage` in both folders and `dm/tools/coverage_report.py` locally), the CPU-only graphics tests, the Python tool tests (`dm/tools/tests`, `dm/ios/tests`, `dm/gfx/tests`) including the translator's synthetic-shader tests, Mac host builds for an NVN, a Vulkan and an OpenGL profile, and the iOS source compile and smoke project ([Continuous integration](../development.md#continuous-integration)). `.github/workflows/fuzz.yml` fuzzes the file parsers nightly from synthetic seeds; a change to a parser in `romfs.py`, `nacp.py`, `shader_split.py` or `uepak.py` can run its harness locally (`dm/tools/fuzz/fuzz_<parser>.py --time 300`). GitHub's Macs have no Metal 4 and no game files, so these stay local: `make -C dm/gfx/tests GAME=<id> run` (the Metal graphics tests), the translator tests with `DMSH_TEST_METAL=1` / `DMSH_TEST_GPU=1`, `profile_guess.py --check`, and everything `regress.py` does.

## 7. Before you open a pull request

### Which tier

The [charter](../charter.md#pull-requests) asks more of a pull request the more it can break. Judge the tier before starting, and again before opening the pull request; when in doubt, take the higher one.

1. **Read its labels** (they come from the files it changes; `python3 dm/tools/pr_areas.py < <(git diff --name-only main...HEAD)` prints them before you push): only `game: …`, `docs`, or `tools` for a tool few games use is **narrow**; `host`, `nvn`, `vulkan`, `opengl`, `textures`, `rewriter` or `packing` is **shared**; `shader chain` is **wide**.
2. **Then ask what it changes, not only where.** A change in a shared file is **wide** when it touches:
   - the order of GPU work, barriers or synchronization (the replay's submission, encoders, fences, `DM_NVN_BARRIER`);
   - threads, priorities or waits (`hle_thread.c`, the submit and encode threads, spin waits);
   - the guest memory layout, pools or allocators, or anything that grows memory for every game;
   - a default every game gets (a setting, a switch's default, a platform key such as Game Mode);
   - several of the shared areas at once, or about a thousand lines or more.
3. **And who it reaches:** `python3 scripts/regress.py impact main..HEAD`. A change it maps to many games, or to "every game of the API", is at least shared; one that reaches the benchmarks' paths is wide.
4. **Game-specific code can still be wide:** a profile key that turns off a host feature, or a rewriter cut shared through `sdk_patches.py`, reaches every game that ships that SDK.

State the tier and the reason in the pull request ("Shared: `nvn`, a new texture format; `impact` names 6 games, checked 3").

By what the change touches (the largest that applies):

- **Docs only:** `python3 -m unittest dm/tools/tests/test_docs.py`; after a change to `docs/reference`, `python3 dm/tools/docs_symptoms.py`; after editing `docs/images/gallery.tsv`, `python3 dm/tools/docs_gallery.py` (also resolves conflicts inside README's generated gallery).
- **A Python tool or script** (`scripts/`, the tools under `dm/`): the ratchet `lint-python.yml` runs, ruff and pyright with no new finding: `<venv>/bin/python3 dm/tools/lint_python.py`, from a venv with `.github/requirements/lint.txt` installed, kept on the drive of the NSP folders, not in the home folder ([Continuous integration](../development.md#continuous-integration)). The system Python has neither tool, and the check stops at a `[RED]` line there: that is not a pass. A generated page (`docs_*.py`) is compared in CI on a clean checkout, so it must not read untracked or git-ignored files such as `dm/host/gen`.
- **One game's profile or note** (`dm/games/<id>/`): the Python tool tests; a full shader build of that game after a change to its shader inputs; the game on the Mac (or the iPhone) along its route, with `scripts/bench.py` numbers when the change is about speed; the finding in the game's note ([AGENTS.md](../../AGENTS.md)).
- **The host, the chain or the tools** (`dm/host`, `dm/gfx`, `dm/tools`, `dm/gfx/tools`, `dm/ios`, `scripts`): everything above, `make -C dm/gfx/tests GAME=<id> run`, `dmsh_chain.py --header` after a chain change, `python3 scripts/regress.py impact main..HEAD` to see which games it reaches, then `python3 scripts/regress.py check --games <ids>` (`--quick` while iterating, the full run before the pull request), and its `report.md` pasted into the pull request with a word on every `CHANGED`, `NEW-FAIL` or `REGRESSION`. Choose the games, not every local one: the five the change is most likely to reach or break (those `impact` names, then games that cover the APIs, engines and paths it touches). Take more than five only when the change truly needs them (a broad chain or host change no five games cover), and fewer when its reach is narrow and clear (a fix on one game's path, a tool only some games use). Name the games the report covers and why you chose them: the others are unchecked. After a change to C or Objective-C in `dm/host` or `dm/gfx`, `dm/tools/lint_c.py compdb` and `tidy` with LLVM's release run what the `lint-c` workflow runs: no new finding (fix it, or `// NOLINT(<check>)` with a reason on a false positive), and `tidy --update` after fixing one ([Continuous integration](../development.md#continuous-integration)).

## 8. The shader chain version

The shader translation chain has one version for every game, `DMSH_CHAIN` in `dm/gfx/nvn/dmsh_chain.h`, which the host is compiled with and loads only libraries of. It covers the chain's tools: `dm/gfx/tools/dmsh_*` (`dmsh_build.py`, `dmsh_chain.py`, `dmsh_fetch.py`, `dmsh_vin.py`, `dmsh_pull.py` with its decoder `dmsh_pull.metal`, `dmsh_geometry.py`, `dmsh_facts.py`), `Program.cs`, `ios_metal.py`, `dm/gfx/nvn/dmsh_abi.h` and `dmsh_chain.lock`. A profile's own inputs (`shader_map.tsv`, `shader_facts.tsv`, `shader_pairs.tsv`, `bf_want.tsv`, its `shader_*` / `nvn_*` keys) are not part of it: they are in each shader's cache key, and their digest stamps the libraries (`b-<V>/inputs.tsv`). The procedure:

1. After changing a chain tool, run `dmsh_chain.py --header` once and commit `dm/gfx/nvn/dmsh_chain.h` with the change (`dm/tools/tests/test_nsp_chain.py` fails on a stale one).
2. After that, or after changing a profile's inputs, rebuild that profile's shaders: a full `dmsh_build.py` run (Mac and, with `IOSOUT`, iOS) stamps its libraries and writes the profile's `c1_full.tsv` and `cb_extent.tsv` from its Mac outputs (git ignores them; the host takes them only beside the stamp they name). Give each game its own output directory.
3. Rebuild the host.

The translator is `dm/gfx/tools/dmsh_xlate_bin`, which matches `dmsh_chain.lock`; a fresh `dotnet publish` does not.

## 9. Per-build knowledge

Per-build knowledge is keyed by the NSO build ID: rewriter cuts, observation gates and frame-step stubs in the profile's `patches.py` (a game's own modules) or `dm/tools/sdk_patches.py` (Nintendo's `sdk` modules, which several games ship), which `dm/tools/patch_nso.py` (the general rewriter) loads, the profile's `build` rows (the builds the game was brought up on, and their display versions; the NSP script packs any other version from a derived profile, without those builds' fixes), and host behaviour that uses fixed module addresses checks `module_t.build_id` (from `fixups.tsv`; `dm_main_build()`). When a new game build appears, carry addresses over with `dm/tools/nso_fnmap.py` and check them by disassembly; never let fixed addresses run on a build they were not found in. For automatic cuts across updates, opt individual patches into `MATCH_CUTS` only after verifying their function fingerprints and assumptions; follow [Function-matched cuts](../reference/rewriter.md#function-matched-cuts-for-unregistered-builds). The shared matcher derives addresses from code; fixed addresses remain build-gated.
