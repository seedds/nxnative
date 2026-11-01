# Mario Kart World 1.8.0 on nx2: Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (with the agentic-workflow overrides) to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Mario Kart World 1.8.0 (`0400C3F00006E000`) plays the first two races of the 150cc Mushroom Cup on the Mac host of the `nx2` branch, with its profile, note, games table row and gallery entry.

**Architecture:** The dump is packed with `scripts/pack_dump.py` into the NSP script's extraction layout, then the modules are rewritten, the shaders split and translated, and the Mac host runs the game, as for Donkey Kong Bananza. Bring-up goes milestone by milestone; each milestone task finds what stops the game, fixes it at its root cause (general first, AGENTS.md) and records the fix once.

**Tech Stack:** Python 3 tools (`scripts/`, `dm/tools/`, `dm/gfx/tools/`), the C / Objective-C Mac host (`dm/host`, `dm/gfx/nvn`), the shader translator (`dm/gfx/tools/dmsh_*`, `dmsh_xlate_bin`), Metal.

**Spec:** `docs/superpowers/2026-11-01-mkw-profile.md` (the PRD). Executors read both.

## Global Constraints

- Mode: **serial** (the user's answer, 2026-11-01). One author at a time in the main checkout `/Users/lego/github/switch/nxnative-nx2`.
- Branch: `feature/2026-11-01-mkw-profile` from `origin/nx2` (`c805059a`), rebased onto `origin/main` (`cd859c38`) after Task 6; the PR goes to `main` (the user's decision, 2026-11-01). Every commit: `git -c user.name=seedds -c user.email=seedds@gmail.com commit` (AGENTS.md). No push, no PR without the user's command.
- Commit order: each general fix (host, translator, tools) is its own commit, before the profile and docs commits that use it. Commit subjects follow the repository's style, a plain sentence (for example `c805059a Donkey Kong Bananza plays on the Mac: terrain through loaded pointers, …`), not Conventional Commits. The approved plan is the ask that AGENTS.md ("Commit only when asked") requires for the commits it names.
- Dump: `/Users/lego/github/switch/mkw/` (`base/` = 1.0.0, `updates/917504/` = 1.8.0). Build folder `/Users/lego/github/switch/mkw/0400C3F00006E000_build`, work folder `/Users/lego/github/switch/mkw/0400C3F00006E000-work`. Nothing from the game in the repository; no key values printed.
- Build IDs of 1.8.0: main `d00e4f399e411d0613aa8d26b1c02fef`, sdk `7a68b13d88183767fb9c7babffe89f3d`, subsdk0 `746302f49f095cfca3d2188423c2bd77`, subsdk1 `8dec9c0b1bed821364e77b10a07b9a28`.
- #834 (`origin/pr834`): facts only. Do not read `dm/gfx/tools/nx2_lower/` or any #834 code diff; copy no text, no table rows. Facts allowed: the PRD's "Facts from PR #834".
- Every profile key carries a comment with its reason and its source (`main+0x…`, the NPDM, a measurement), as in `dm/games/040074A01BF12000/game.tsv`.
- Root cause before a fix (superpowers: systematic-debugging; AGENTS.md "Find a bug's root cause"). A workaround only where the cause cannot be fixed, said in the code comment and the note.
- Fixed addresses are gated by the NSO build ID (AGENTS.md "Per-build knowledge").
- A chain tool change: `python3 dm/gfx/tools/dmsh_chain.py --header`, commit `dm/gfx/nvn/dmsh_chain.h` with it, full shader rebuild of this profile, rebuild the host (AGENTS.md shader chain procedure).
- Where a finding goes (AGENTS.md): a game fact in `dm/games/0400C3F00006E000/README.md`; a general mechanism as an entry in `docs/reference/<subsystem>.md` (Symptom, Cause, Fix, Found on), then `python3 dm/tools/docs_symptoms.py`, plus a line under the note's "Found on this game".
- English everywhere; red log lines start with `[RED] `.
- Authors run the build and only the tests of the changed code, never the full suite (agentic-workflow skill, Run: Author tests).
- Do not download or run third-party code. A public video capture for the picture comparison is looked at, not downloaded into the repository.
- Screenshots and logs stay in the work folder. Only the gallery screenshot goes into `docs/images/`, with no profile name or personal data on it.
- Test list (CLAUDE.md §2), the only reasons to write a test: a check against a reference (original program, golden snapshot); a parser or data format; a rule with more than one outcome that no reference check or type check covers (dates, parsing, prices, state machines); a regression of a real bug (one test per bug). No tests for: constants and data tables; wiring, scaffold, CLI arguments, logging; what the compiler, a linter or a standard check catches; UI and layout; what another test already covers. No mutation tests.
- Weakened assertions: none planned (the empty list approved by the user, 2026-11-01). Any change that removes or loosens an existing assertion, skips a test or widens a tolerance stops the run for the user.

## Review Focus

1. Donkey Kong Bananza after a shared NVN2 / Switch 2 change (the API answer, NVN2 entry points, Ampere translation): it must boot, render and pass `regress.py check` as before. Owner: every task that changes `dm/host`, `dm/gfx`; checked in Task 11.
2. A fixed address used without a build-ID gate: it would run on 1.0.0 or a later update. Owner: Tasks 4–8; the task reviewer checks each new address for its gate.
3. A profile key that turns a feature off to hide a symptom (a workaround without a known cause). Owner: Tasks 2, 4–8; the reviewer checks each new key's comment for its cause.
4. First boot with an empty save folder (no save data, first-run dialogs): the game must reach the menu. Owner: Task 4 runs with an empty `run/save`.
5. Shaders seen only at run time: they must be in the map before `pso_manifest.tsv` is recorded, or the next start compiles them during play. Owner: Task 9.

---

### Task 1: Pack the dump and check its identity

**Files:**
- Create (outside the repository): `/Users/lego/github/switch/mkw/0400C3F00006E000_build/extract/v917504/{ExeFS,RomFS.img}`, `/Users/lego/github/switch/mkw/pack_dump.log`

**Interfaces:**
- Produces: the extraction layout at `/Users/lego/github/switch/mkw/0400C3F00006E000_build/extract/v917504/`; the four build IDs confirmed.

- [ ] **Step 1:** Create the branch: `git switch -c feature/2026-11-01-mkw-profile origin/nx2` (if HEAD is not already on it).
- [ ] **Step 2:** Run `python3 scripts/pack_dump.py /Users/lego/github/switch/mkw > /Users/lego/github/switch/mkw/pack_dump.log 2>&1` (in the background; the update has 206,461 files). Expected: exit 0, the last line `done: …/extract/v917504`, a line `0400C3F00006E000 version 917504`.
- [ ] **Step 3:** Read the build ID of each NSO in `extract/v917504/ExeFS` (main, sdk, subsdk0, subsdk1) with `dm/tools/nso.py`: `nso.read(path).build_id[:16].hex()` (`Module.build_id` is 0x20 bytes, the ID is its first 0x10, `nso.py:50`). Expected: the four values of the Global Constraints. A mismatch stops the run for the user.
- [ ] **Step 4:** `df -h /Users/lego/github/switch/mkw`. Expected: at least 100 GB free; less stops the run for the user (PRD, Decisions: Disk).
- [ ] **Step 5:** Read `sdk`'s SDK version string and spec (the host reads them in `dm_sdk_major`, `dm/host/loader.c:131`, and `dm_sdk_ounce`, `loader.c:151`). Expected (assumption from #834): SDK 21.4.0, spec `Ounce`. Record the result in the ledger for Task 2.

No commit (nothing in the repository changes).

### Task 2: The profile and the rewritten modules

**Files:**
- Create: `dm/games/0400C3F00006E000/game.tsv`, `dm/games/0400C3F00006E000/README.md` (a short note: status "in bring-up", What it needs, Files), `dm/games/0400C3F00006E000/patches.py` only if the rewriter needs a cut
- Create (outside): `/Users/lego/github/switch/mkw/0400C3F00006E000_build/patched_v917504/`

**Interfaces:**
- Consumes: Task 1's extraction layout.
- Produces: the profile `DM_GAME=0400C3F00006E000`; the rewritten modules in `patched_v917504/` that the host runs.

- [ ] **Step 1:** Find how the NSP script runs the rewriter, the heap check and the profile guess on an extracted version (`scripts/build_ipa_from_nsp.py`: the rewrite function around line 1097, `dm/games/profile_guess.py`; and the precedent's build folder `/Users/lego/github/switch/dkb/040074A01BF12000_build`). Assumption to check first: `build_ipa_from_nsp.py --platform mac --work-dir /Users/lego/github/switch/mkw/0400C3F00006E000_build` runs from the packed layout without NSPs. If it does not, run the steps one by one (`dm/tools/patch_nso.py <src> <out> <module>` for main, subsdk0, subsdk1, sdk) and write the commands into the ledger.
- [ ] **Step 2:** Write `game.tsv` by the keys of `dm/games/README.md` and the layout of `dm/games/040074A01BF12000/game.tsv`: identity, app (`name Mario Kart World`, `ipa MarioKartWorld.ipa`), `build v917504 1.8.0 main=<id> subsdk0=<id> subsdk1=<id> sdk=<id>` (row format `<version> <display version> <module>=<build ID>…`, as `dm/games/040074A01BF12000/game.tsv:16`), `package v917504`, `shader_versions v917504`, `modules main subsdk0 subsdk1 sdk`, `romfs_marker` (a file of 1.8.0's RomFS; check that `Movie/StaffRoll_00.webm` exists), `zip_store`, the memory keys from this NPDM (`sys_resource` from its system resource size) and the heap check, `shader_groups` from the RomFS's shader folders, `render_cond on` only once a black scene shows its conditional clears (Task 6), not before.
- [ ] **Step 3:** Run the rewriter on the four modules with `DM_GAME=0400C3F00006E000`. Expected: exit 0 per module, `patched_v917504/` written, no `[RED]` line without an explanation in the ledger.
- [ ] **Step 4:** Read the game's NVN API check in `main` (the caller of `nvnDeviceGetInteger` item 0 / 1 near NVN setup; #834 says it needs 231.39). Record the address and the condition (major ==, minor ≥) in the note. Read Donkey Kong Bananza's at `main+0x5c61a4` the same way (its build folder). Record in the ledger whether one answer (231, the larger minor) satisfies both.
- [ ] **Step 5:** NVN API answer (PRD Decisions: NVN2 API version), its own commit when it changes shared code: if Step 4 shows one answer satisfies both games, change `nvn_api` in `dm/gfx/nvn/hle_nvn.c:182-191` (the Ounce branch at line 189) to it and update the comment at lines 186-187 with both games' checks; build the host for Donkey Kong Bananza (`make -C dm/host GAME=040074A01BF12000`) and boot it with `/Users/lego/github/switch/dkb/run.sh --no-prebuild` for 60 s: expected the log line `[nvn] device API 231.<minor>` and its frame loop as before. Commit: "NVN2: API 231.<minor> for every Switch 2 SDK; Mario Kart World requires it". If no answer satisfies both, set `device_api` in `game.tsv` with the check's address in its comment instead.
- [ ] **Step 6:** `python3 -m pytest dm/tools/tests/test_docs.py -q`. Expected: PASS, or failures only for the games table row; if the test requires the row for every note, add the row now in the format of `dm/games/README.md:77`.
- [ ] **Step 7:** Commit `dm/games/0400C3F00006E000/` (and `patches.py` if made): "Mario Kart World (Switch 2, 1.8.0): profile and rewritten modules".

### Task 3: Shader split and full shader build

**Files:**
- Create: `dm/games/0400C3F00006E000/{shader_map.tsv,shader_facts.tsv,shader_pairs.tsv,pso_pairs.tsv}` (the split writes them), `bf_want.tsv` only if the split asks for it
- Modify: `dm/gfx/tools/shader_split.py` only if a reader for this game's containers is missing
- Create (outside): `/Users/lego/github/switch/mkw/0400C3F00006E000_build/shader/`, `/Users/lego/github/switch/mkw/shader_split.log`, `/Users/lego/github/switch/mkw/dmsh_build.log`

**Interfaces:**
- Consumes: Task 2's profile (`shader_groups`).
- Produces: the shader tables in the profile; Mac shader libraries in `…_build/shader/cache` (the host's `DM_MSL_DIR`); `c1_full.tsv`, `cb_extent.tsv` (git-ignored).

- [ ] **Step 1:** Run the split as the NSP script does (its shader step; the precedent's `shader_split.log`), writing the package to `/Users/lego/github/switch/mkw/0400C3F00006E000_build/shader/pkg`. Expected: exit 0; a shader count; the readers used. Assumption to check (from Donkey Kong Bananza's note, Files): BNSH in `bfsha`, agl BAHS archives and the NDSR pack; anything else is a new container.
- [ ] **Step 2:** If the split finds no shaders in a container that holds them (a magic the readers skip), add its reader to `shader_split.py` as its own commit, with a test in `dm/gfx/tests/` (where `test_shader_split_progress.py` and `test_geometry_split.py` are) (a data format: on the test list), using a small synthetic sample, never game data.
- [ ] **Step 3:** Run the full shader build for the Mac (no `IOSOUT`), in the background (about 75 min or more): `DM_GAME=0400C3F00006E000 python3 dm/gfx/tools/dmsh_build.py /Users/lego/github/switch/mkw/0400C3F00006E000_build/shader/pkg /Users/lego/github/switch/mkw/0400C3F00006E000_build/shader/cache > /Users/lego/github/switch/mkw/dmsh_build.log 2>&1` (usage `dmsh_build.py:5`; the cache dir is `run.sh`'s `DM_MSL_DIR`). Expected: exit 0; the count of translated and failed shaders.
- [ ] **Step 4:** List the failing shaders by error (`dm/gfx/tools/shader_regressions.py` style). An error class that the races may need (decided in Tasks 6–8 when a draw misses) stays open here; record the list in the note's Shaders section.
- [ ] **Step 5:** Commit the shader tables: "Mario Kart World: shader tables".

### Task 4: Boot into the frame loop

**Files:**
- Create (outside): `/Users/lego/github/switch/mkw/run.sh` (as `/Users/lego/github/switch/dkb/run.sh`, with `G=0400C3F00006E000`, `W=/Users/lego/github/switch/mkw/${G}_build`, `DM_ROMFS=$W/extract/v917504/RomFS.img`, `patched_v917504`, no `DM_AOC_DIR` if there is no DLC)
- Modify: what the root causes need (`dm/host/*`, `dm/gfx/nvn/hle_nvn.c`, …); each fix its own commit
- Test: one regression test per bug fixed in host or tool code, in `dm/host/tests` / `dm/gfx/tests` / `dm/tools/tests`, where the harness can hold it

**Interfaces:**
- Consumes: Tasks 2–3.
- Produces: `run.sh`; a host built with `make -C dm/host GAME=0400C3F00006E000` that reaches the frame loop.

- [ ] **Step 1:** Build the host: `make -C dm/host GAME=0400C3F00006E000`. Expected: exit 0.
- [ ] **Step 2:** Font step: produce `$W/font` as the NSP script's Mac build does (check its font function; the precedent has `…_build/font`).
- [ ] **Step 3:** Run `run.sh --no-prebuild` with an empty `run/save`, `DM_WATCH=60`, for up to 120 s, log to the work folder. Read the log by `docs/agents/03-bringup.md` (Reading the log).
- [ ] **Step 4:** For each stop (abort, null read, hang, missing service, NVN2 entry point): find the root cause (`docs/agents/symptoms.md` first, then `docs/agents/05-debugging.md`), fix it there, add its regression test if on the test list, commit it alone. The NVN API answer is settled in Task 2 Step 5.
- [ ] **Step 5:** Repeat Steps 3–4 until the log shows the game's frame loop (present calls at a steady rate) for 60 s.
- [ ] **Step 6:** Add each fix to the note's Boot section (numbered, what stopped, what fixed it) and each general fix to its reference entry; commit the note.

### Task 5: The title and the main menu

**Files:**
- Modify: as the root causes need; each fix its own commit
- Test: one regression test per bug, where the harness can hold it

**Interfaces:**
- Consumes: Task 4's host and `run.sh`.
- Produces: a recorded `DM_KEYS` sequence (in the work folder) from start to the main menu.

- [ ] **Step 1:** Run with the window, `DM_CTL` screenshots at fixed times, until the title shows. Compare each screenshot with what the title and menu should show (a public capture).
- [ ] **Step 2:** Fix what stops or breaks the title and the menu (movies: nx2 serves `nn::codec` since Donkey Kong Bananza, check whether #834's `nv::InitCheck` abort still occurs; text; textures), each at its root cause, each its own commit.
- [ ] **Step 3:** Write the `DM_KEYS` steps that go from boot to the main menu (or record them with `DM_REC`). Expected: the main menu in the screenshot at the end.
- [ ] **Step 4:** Update the note (Boot, Rendering); commit.

### Task 6: The first race starts

**Files:**
- Modify: as the root causes need; `dm/games/0400C3F00006E000/game.tsv` for a key with its cause (for example `render_cond`); each fix its own commit
- Test: one regression test per bug, where the harness can hold it

**Interfaces:**
- Consumes: Task 5's `DM_KEYS` to the menu.
- Produces: `DM_KEYS` steps from boot to the start of the first race of the 150cc Mushroom Cup Grand Prix.

- [ ] **Step 1:** Extend the `DM_KEYS` steps: Grand Prix, 150cc, Mushroom Cup, character and kart defaults, start.
- [ ] **Step 2:** Run to the race start; screenshots at the countdown and 10 s after it.
- [ ] **Step 3:** Fix what stops the race from loading or starting, at its root cause. A black 3D scene: check the conditional clears first (PRD Facts; Donkey Kong Bananza's `render_cond`), then set `render_cond on` with the address of the conditional clear in its comment.
- [ ] **Step 4:** Update the note; commit.

### Task 7: The first race completes with a correct picture

**Files:**
- Modify: as the root causes need (`dm/gfx/nvn/*`, `dm/gfx/tools/dmsh_*`, `dm/gfx/tex/*`, …); each fix its own commit; a chain tool change follows the chain procedure (Global Constraints)
- Test: one regression test per bug, where the harness can hold it

**Interfaces:**
- Consumes: Task 6's `DM_KEYS` to the race start.
- Produces: `DM_KEYS` steps that drive three laps of the first race to its results screen; screenshots of the course in the work folder.

- [ ] **Step 1:** Write `DM_KEYS` steps that drive the first race: accelerate and steer by fixed frames (or record a lap with `DM_REC` from a controller only if the user plays; otherwise scripted). The race must reach the results screen; a race lost is fine.
- [ ] **Step 2:** Screenshots every 10 s; compare the course, the ground, the karts and the HUD with a public capture of the same course. Assumption from #834: the ground may show flat patches (terrain array layers past 768 MB); check it before you fix it.
- [ ] **Step 3:** For each wrong picture or stop: find the draw or pass (`DM_NVN_HILITE`, `DM_NVN_SNAPRAW`, `DM_NVN_GPUPROF`, `docs/agents/04-graphics.md`), its root cause, fix it there. A graphics feature the race does not need (mesh shaders, shading rate images, if the race draws right without them) goes to Open in the note.
- [ ] **Step 4:** Repeat until the race reaches its results screen and the screenshots match the capture. Keep one screenshot per course in the work folder for the user.
- [ ] **Step 5:** Update the note (Rendering, Shaders, Open); reference entries for general fixes; `python3 dm/tools/docs_symptoms.py`; commit.

### Task 8: The second race

**Files:**
- Modify: as the root causes need; each fix its own commit
- Test: one regression test per bug, where the harness can hold it

**Interfaces:**
- Consumes: Task 7's `DM_KEYS`.
- Produces: `DM_KEYS` steps through the second race of the cup to its results screen.

- [ ] **Step 1:** Extend the steps to the second race (three laps) and its results.
- [ ] **Step 2:** Fix what stops it or draws wrong, as in Task 7 Step 3.
- [ ] **Step 3:** Record the play time, the frame rate (`DM_NVN_TSV`) and the peak memory footprint; write them in the note.
- [ ] **Step 4:** Update the note; commit.

### Task 9: Runtime-only shaders and the seed list

**Files:**
- Modify: `dm/games/0400C3F00006E000/shader_map.tsv` (and the other tables the split rewrites)
- Create: `dm/games/0400C3F00006E000/pso_manifest.tsv`

**Interfaces:**
- Consumes: Task 8's `DM_KEYS` through both races.
- Produces: a shader map with the runtime-only shaders; the seed list.

- [ ] **Step 1:** Run both races with `DM_NVN_SHDUMP`; count the programs missing from the map.
- [ ] **Step 2:** Add them through the split's existing inputs, as nx2 does for other games (`docs/agents/04-graphics.md`, `docs/reference/shaders.md`: find the mechanism other profiles use for runtime shaders). Run the shader build for the changed shaders; rebuild the host.
- [ ] **Step 3:** Run both races again with `DM_NVN_SHDUMP`. Expected: no missing program.
- [ ] **Step 4:** Record the seed list with prewarm off (`DM_PSO_MANIFEST=off DM_PSO_RECORD`, `docs/development.md` Prewarm lists) through boot, the menu and both races; write it to `pso_manifest.tsv`.
- [ ] **Step 5:** Run the pipeline prebuild (`run.sh` without `--no-prebuild`), then both races. Expected: exit 0 of the prebuild; the race runs without compile stalls in the log.
- [ ] **Step 6:** Commit: "Mario Kart World: runtime shaders and the prewarm seed list".

### Task 10: The note, the games table and the gallery

**Files:**
- Modify: `dm/games/0400C3F00006E000/README.md`, `dm/games/README.md` (games table row), `docs/images/gallery.tsv`, README's gallery (via `dm/tools/docs_gallery.py`), `docs/agents/symptoms.md` (via `docs_symptoms.py`)
- Create: `docs/images/mario-kart-world.jpg`

- [ ] **Step 1:** Complete the note with the sections of Donkey Kong Bananza's note: intro with the status ("Plays on the Mac", what was tested), What it needs, Files, Shaders, Rendering, Boot (Mac), Open, Found on this game. Facts from #834 in new words, each with this run's measurement.
- [ ] **Step 2:** Complete or add the games table row in `dm/games/README.md`, in alphabetical order after Mario Kart 8 Deluxe, in the format of Donkey Kong Bananza's row (`dm/games/README.md:77`): name with the note link, `0400C3F00006E000`, `v917504 (Switch 2)`, `<SDK from Task 1> (Ounce)`, the engine, `NVN2`.
- [ ] **Step 3:** A gallery screenshot from a race (no profile name), JPEG as the others; the row in `docs/images/gallery.tsv`; `python3 dm/tools/docs_gallery.py`.
- [ ] **Step 4:** `python3 dm/tools/docs_symptoms.py`; `python3 -m pytest dm/tools/tests/test_docs.py -q`. Expected: PASS.
- [ ] **Step 5:** Commit: "Mario Kart World plays on the Mac: note, games table, gallery".

### Task 11: Regression run

**Files:**
- Modify: `dm/games/<id>/validated.tsv` of the checked games (with `--record`)
- Create (outside the repository): the report under the harness's `reports/`

- [ ] **Step 1:** `python3 scripts/regress.py impact origin/main..HEAD`; take Donkey Kong Bananza plus the four games it ranks highest among the local `--platform mac` builds. Record the five and why in the ledger. Assumption to check first: which local games `--games-dir` can see (it expects NSP folders named `<name> [<title ID>]`, `scripts/regress.py:24`) and which have a `smoke.tsv` route (Donkey Kong Bananza has none, so the harness runs only its chain checks: boot it by hand as in Task 2 Step 5, 60 s, and compare a screenshot with one from `origin/main`'s host).
- [ ] **Step 2:** `python3 scripts/regress.py check --root <REGRESS_ROOT> --games-dir <REGRESS_GAMES> --games <ids> --record --out /Users/lego/github/switch/mkw/0400C3F00006E000-work/regress-report`, in the background (`--root` and `--games-dir` are required, `scripts/regress.py:23-25`; take their values from the environment or the precedent's last run, else stop and ask the user). Expected: each game `PASS` or `CHANGED`; exit 0. A `NEW-FAIL` or `REGRESSION`: a debugger subagent finds the cause, a fix commit, one new run.
- [ ] **Step 3:** Skipped: no IPA build in this run (the user's ruling, 2026-11-01: macOS only for now).
- [ ] **Step 4:** `git status`: no game file. Write the report's summary (games, why, result, explained `CHANGED` rows) into `docs/superpowers/2026-11-01-mkw-profile-pr.md` for the PR text.
- [ ] **Step 5:** Commit the `validated.tsv` files: "Mario Kart World: regression record".
