# Mario Kart World 1.8.0 on nx2: PRD

## Goal

Mario Kart World (`0400C3F00006E000`), base 1.0.0 with update 1.8.0 (917504), plays on the Mac on the `nx2` branch. The game boots, shows the title and the menus, and plays complete races with a correct picture. The game gets its profile `dm/games/0400C3F00006E000/`, its note, its row in the games table and its gallery entry. The work goes to `main` as one pull request (upstream merged `nx2` into `main` on 2026-11-01).

Why: Mario Kart World is the second Switch 2 game after Donkey Kong Bananza (`040074A01BF12000`). PR #834 brought it to free roam with code that the repository cannot take (see Anti-goals). This work does it on the Switch 2 support that `nx2` has.

## Inputs

- The handover from the session that downloaded the update (in the conversation of 2026-11-01): the dump path, the title ID, the build IDs, the worktree, the procedure, the PR rules.
- Dump: `/Users/lego/github/switch/mkw/`, 45 GB.
  - `base/`: version 1.0.0. `exefs/` has main, main.npdm, rtld, sdk, subsdk0. `romfs/`.
  - `updates/917504/`: version 1.8.0. `exefs/` has main, main.npdm, rtld, sdk, subsdk0, subsdk1. `romfs/`.
- Build IDs of 1.8.0, from the handover and #834's `game.tsv`: main `d00e4f399e411d0613aa8d26b1c02fef`, sdk `7a68b13d88183767fb9c7babffe89f3d`, subsdk0 `746302f49f095cfca3d2188423c2bd77`, subsdk1 `8dec9c0b1bed821364e77b10a07b9a28`. The first task reads them from the dump's NSOs and compares.
- Repository: started in the worktree `/Users/lego/github/switch/nxnative-nx2` on `origin/nx2` (`c805059a`); now the main checkout `/Users/lego/github/switch/nxnative` on `origin/main`.
- Precedent: Donkey Kong Bananza, the first Switch 2 game here. Its note `dm/games/040074A01BF12000/README.md`, its profile, its commits `aca482f8..c805059a`, its build folder `/Users/lego/github/switch/dkb/040074A01BF12000_build` and its `/Users/lego/github/switch/dkb/run.sh`.
- PR #834 (`origin/pr834`): facts about the game only. See Anti-goals.

## Facts from PR #834

Source: `git show origin/pr834:dm/games/0400C3F00006E000/README.md` and `game.tsv`. Each fact is an assumption until a task checks it on this dump.

- SDK 21.4.0, NVN2. Modules `main subsdk0 subsdk1 sdk`.
- The game checks the NVN API version and needs 231.39. With 55.15 it stops NVN setup and crashes on a null global. nx2 answers 231.30 for every Switch 2 SDK (`dm/gfx/nvn/hle_nvn.c:189`). 30 is the floor that Donkey Kong Bananza needs: major 231, minor ≥ 30 (`main+0x5c61a4`, `hle_nvn.c:186-187`).
- The 3D scene was black without `render_cond on`, because the clears are conditional.
- The NPDM declares 64 MB of system resource. Thus virtual address memory is on. Donkey Kong Bananza declares none.
- #834's memory keys: `mem_reserve 0x500000000`, `mem_alias 0x280000000`, `mem_stack 0x10000000`, `app_pool 0x263000000`, `sys_resource 0x4000000`. The game gets 9.55 GiB and plans all of it.
- `romfs_marker Movie/StaffRoll_00.webm`.
- About 240 shaders exist only at run time. A run with `DM_NVN_SHDUMP` saves them.
- Open in #834:
  - The ground showed flat colour in rectangular patches. Suspected cause: the terrain array layers past 768 MB stayed zero.
  - Task and mesh shaders: `DrawMeshTasks` drew nothing.
  - `nv::InitCheck` aborted when the attract movie started. nx2 fixed this abort for Donkey Kong Bananza (its note, Boot 6).
  - Shading rate images, `SetGeometryProcessingMode` and `PushGraphicsConstants` did nothing.

## Goals

1. Pack the dump and build the inputs for the Mac host with the procedure of Donkey Kong Bananza:
   1. `scripts/pack_dump.py` packs the dump.
   2. `dm/tools/patch_nso.py` rewrites the modules.
   3. `dm/gfx/tools/shader_split.py` splits the shaders.
   4. `dm/gfx/tools/dmsh_build.py` translates all shaders (a full build).
   5. The font step writes the font folder (`DM_FONT_DIR`, as in the precedent's `run.sh`).
   6. The pipeline prebuild (`DM_PSO_PREBUILD=tool`) writes the pipeline archive.
   7. A `run.sh` beside the dump starts the game, written as the precedent's.
   The plan names the exact command of each step from `build_ipa_from_nsp.py` and the precedent.
2. A profile `dm/games/0400C3F00006E000/` with its shader tables. Each profile key comes from this dump or this binary and has its reason in a comment, as in Donkey Kong Bananza's `game.tsv`.
3. The game plays on the Mac host:
   1. It reaches its frame loop and shows the title and the main menu.
   2. It plays the first two races of the Mushroom Cup in a 150cc Grand Prix, each with three laps, from the menu to the results screen.
   3. The picture is correct: the course, the ground, the karts and the HUD look as in a public video capture of the same spot.
4. Each fix is at its root cause. A general fix (Horizon, NVN2 or Ampere semantics in the host or the translator) comes before a profile key or a rewriter cut (AGENTS.md).
5. The documentation:
   1. The note `dm/games/0400C3F00006E000/README.md`, with the sections of Donkey Kong Bananza's note.
   2. The row in the games table `dm/games/README.md`.
   3. Each general fix gets an entry in its `docs/reference/<subsystem>.md`, and `dm/tools/docs_symptoms.py` updates the symptom index.
   4. The gallery entry with a screenshot (`docs/images/gallery.tsv`, then `dm/tools/docs_gallery.py`).
6. The prewarm seed list `pso_manifest.tsv`, recorded from the races, as Donkey Kong Bananza's.
7. `scripts/regress.py check --games` passes on five local games. Donkey Kong Bananza is always one of them. `regress.py impact` selects the other four. The report goes into the PR.

## Non-goals

- iOS: no device test and no IPA build in this run (the user's ruling, 2026-11-01: macOS only for now). AGENTS.md accepts a Mac gameplay test.
- Online play, local wireless, GameChat, the camera, amiibo.
- Free roam, the battle modes and the other cups.
- The base 1.0.0 alone and any version other than 1.8.0.
- Full frame rate in every scene. The note records the game's own frame-rate drops under Open, as Donkey Kong Bananza's note does.
- A graphics feature that the two races do not need. The note records it under Open. A feature that the two races need is in scope (for example the terrain, if its patches show).

## Anti-goals

- Code from #834. Its `nx2_lower` is GPL-3.0-or-later and the repository is MIT. The builder reads no `nx2_lower` source and copies no diff, no structure, no note text and no table rows. The builder writes each fact from #834 in new words, with the measurement on this dump.
- A workaround that hides a symptom without a known cause: a stub, a skipped call, a clamp, a profile key that turns a feature off. A workaround is permitted only when the cause lies where it cannot be fixed. Then the code comment and the note say so (AGENTS.md).
- Fixed addresses that run on a build they were not found in.
- Game files, keys or extracted data in the repository. Printed key values.
- Screenshots and logs in the repository. They stay in the work folder. Only the gallery screenshot goes into the repository, and it shows no profile name or other personal data.
- A regression on another game, Donkey Kong Bananza first.

## Decisions

- **Scope** (the user's answer): playable races on the Mac, then the games table row and the PR.
- **Branch and PR:** the branch `feature/2026-11-01-mkw-profile`, started from `origin/nx2` and rebased onto `origin/main` (`cd859c38`) after upstream merged `nx2` into `main` (the user's decision, 2026-11-01). One PR into `main` at the end. Commits as `legoxz` (a plain `git commit` with the checkout's identity; the user's decision, 2026-11-01: AGENTS.md's `seedds` identity is the upstream owner's own, not a contributor's, and the earlier commits were rewritten to `legoxz`). Push and the PR only on the user's command; the PR comes from the GitHub account `legoxz`.
- **Commit order:** each general fix (host, translator, tools) is its own commit, before the profile and docs commits that use it. Then a commit can be reverted alone if it regresses another game.
- **Folders:** the build folder `/Users/lego/github/switch/mkw/0400C3F00006E000_build` (the default of `pack_dump.py`) and the work folder `/Users/lego/github/switch/mkw/0400C3F00006E000-work`, beside the dump. Donkey Kong Bananza's are under `/Users/lego/github/switch/dkb/` the same way. The game's folder is the NSP folder of AGENTS.md.
- **NVN2 API version:** a general rule before a profile key. The task that meets the game's check reads the checks of both games in their binaries. If one answer satisfies both (major 231 and the larger minor), the host gives it for every Switch 2 SDK, with the comment at `hle_nvn.c:186-187` updated. The regression run checks Donkey Kong Bananza. `device_api` goes into the profile only if no general answer satisfies both games.
- **Memory keys:** the builder takes them from this NPDM and the heap check of the NSP script, not from #834.
- **Runtime-only shaders:** a run with `DM_NVN_SHDUMP` collects them. They go into the shader map through the existing inputs of the split, as for other games in nx2. No mechanism from #834.
- **Build order for the shaders:** full build, play, collect the runtime-only shaders, add them, build the changed shaders again, rebuild the host, then record `pso_manifest.tsv` last.
- **Bring-up is iterative.** The failures after boot are not known in advance. The plan cuts the work into milestones. Each milestone task finds and fixes what stops it, at its root cause. Each fix is recorded once: a general fix as a reference entry, a game fact in the note.
- **Input for races:** the autonomous run steers the game with the host's scripted input: `DM_KEYS`, `DM_CTL` and `DM_REC` (`docs/agents/06-dev-loop.md`, "Run with scripted input"). The user's own play sessions are on the manual test checklist that the run writes at its end (agentic-workflow).
- **Who judges the picture:** the user, from one screenshot per course in the work folder, compared with a public video capture of the same spot. The run compares first and reports what differs. No game footage goes into the repository.
- **Tests** (CLAUDE.md §2): one regression test per real bug fixed in host or tool code, where the existing harness (`dm/host/tests`, `dm/gfx/tests`, `dm/tools/tests`) can hold it. No tests for profile tables or docs. `dm/tools/tests/test_docs.py` must pass.
- **Disk:** the internal disk has 313 GB free (2026-11-01). The dump is 45 GB. Donkey Kong Bananza's 8.6 GB RomFS image suggests that the build folder needs about the size of the update's RomFS plus the shader cache. The first task measures the free space after the pack and stops if less than 100 GB remain.

## Invariants

- Donkey Kong Bananza boots, plays and passes `regress.py check` as before.
- `dm/tools/tests/test_docs.py` passes.
- `git status` shows no game file.

## Acceptance (gherkin)

```gherkin
Feature: Mario Kart World 1.8.0 on the Mac (nx2)

  Scenario: Pack and build
    Given the dump at /Users/lego/github/switch/mkw with base/ and updates/917504/
    When the pack, the rewriter, the shader split, the full shader build, the font step and the pipeline prebuild run with DM_GAME=0400C3F00006E000
    Then each step exits 0
    And the four build IDs read from the NSOs match the handover
    And the note lists each shader that does not translate

  Scenario: First boot to the title
    Given the packed build, the Mac host built with GAME=0400C3F00006E000 and an empty save folder
    When run.sh starts the game
    Then the game reaches its frame loop and shows the title and the main menu
    And each [RED] line in the log has its explanation in the note

  Scenario: The first race
    Given the main menu
    When scripted input starts a 150cc Grand Prix in the Mushroom Cup and drives the first race of three laps
    Then the race reaches its results screen
    And a screenshot of the course is in the work folder for the user to compare

  Scenario: The second race
    Given the results screen of the first race
    When the second race of the cup runs with three laps
    Then it reaches its results screen without a crash, a hang or a missing picture

  Scenario: Documentation and seed list
    When test_docs.py runs
    Then it passes with the note, the games table row, the gallery row and the symptom index
    And dm/games/0400C3F00006E000/pso_manifest.tsv holds the shaders and pipelines of the two races

  Scenario: No regression
    Given the changes on the feature branch
    When regress.py check --games runs on Donkey Kong Bananza and the four games regress.py impact selects
    Then each game is PASS, or CHANGED with the change explained in the PR
```

## Constraints

- AGENTS.md rules: per-game values in the profile; per-build knowledge keyed by the NSO build ID; the shader chain procedure (`dmsh_chain.py --header` after a chain tool change, a full rebuild of the profile's shaders, then the host); English everywhere; `[RED] ` for red log lines; no signing or installing on the user's devices.
- A full shader build of Donkey Kong Bananza took about 75 minutes. Mario Kart World's can take as long or longer.
- The game gets a 9.55 GiB pool (#834). The run measures the host's peak memory footprint and records it in the note.

## Known consequences

- No iOS device test. The note says iOS is untested.
- Free roam and the other cups stay untested and are named as such in the note.
- The shaders that do not translate and the graphics features that the races do not need are listed under Open in the note.
- The other Switch 2 game, Donkey Kong Bananza, gets the new NVN2 API answer if the general rule applies.

## Questions left to the builder

- Which facts from #834 hold on this dump, and which nx2 already covers (the movie abort).
- The root causes of what stops each milestone.
