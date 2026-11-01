# Mario Kart World 1.8.0: handover

State on 2026-11-01. Plan: `docs/superpowers/2026-11-01-mkw-profile-plan.md`. PRD: `docs/superpowers/2026-11-01-mkw-profile.md`.

## Where the work is

- Branch `feature/2026-11-01-mkw-profile`, rebased onto `origin/main` (`cd859c38`). Not pushed. The pull request goes to `main`.
- Worktree: `/Users/lego/github/switch/nxnative-nx2`. The name is historical: the branch is not on `nx2`.
- Run ledger (git-ignored): `.superpowers/sdd/2026-11-01-mkw-profile-plan/progress.md`, with the task reports and the review reports in the same folder.
- Python for every tool: `.superpowers/sdd/2026-11-01-mkw-profile-plan/py`. It is CPython 3.14 with lz4, capstone and fonttools from the local uv cache. Homebrew `python3` 3.15 has none of them.
- Shader tools need `DOTNET_ROOT=/opt/homebrew/opt/dotnet/libexec`.
- Game folders: build `/Users/lego/github/switch/mkw/0400C3F00006E000_build`, work `/Users/lego/github/switch/mkw/0400C3F00006E000-work`, start script `/Users/lego/github/switch/mkw/run.sh` (`DM_AUDIO=0` mutes).

## Done

- Tasks 1–6, 7a and 7b are complete and reviewed. Fix rounds after review are in the ledger as "fix (unreviewed)".
- The game boots, shows the title and the menu, loads race 1 of the 150cc Mushroom Cup and runs at about 60 fps.
- Fixed:
  - the Switch 2 services the game opens (mouse, GameChat, camera, HDR metadata),
  - the 9 GB pool,
  - sparse textures as Metal placement sparse textures,
  - `render_cond`,
  - SDK 21 video decoder names,
  - sRGB / linear texture pairs,
  - compute shared memory past 32 KB in a device buffer,
  - the Ampere shared-memory stride (the rainbow bands).

## Open, in order

1. **Race crash** at `main+0x1a43e4`, around frame 14,700 of race 1. It is new since `93aca024` (the 48 KB compute shaders run).
   - Findings: `.superpowers/sdd/2026-11-01-mkw-profile-plan/debug-race-crash.md`.
   - A 3x4 bone matrix from a 48 KB DrivePass skeleton shader (for example `2a89b6d4df731b8d`) lands on a heap object.
   - Cause (a) or (b), not yet told apart: (a) an unbounded, data-driven output index; (b) a GPU write after the pool memory was freed and reused as heap.
   - Next step: one diagnostic run to frame ~16,000 that logs each 48 KB dispatch's storage buffer and the pool frees near the object.
   - Uncommitted: `dm/host/probe.c` (`heap+<offset>` for `DM_PROBE_WATCH` / `DM_PROBE_POLL`).
2. **Test runs, the new way** (the user's decision):
   - a save after the first boot (`save_menu`, from a run stopped at about frame 4700, not the current `run/save`),
   - `run_until.sh <log> <target frame> [keys]`, which ends a run on a crash, at the target frame or on a stall,
   - static analysis before a run.
3. **Race input:** after the crash fix, the user drives races 1 and 2 once with `DM_REC` (with `DM_DETCLOCK`, from the saved copy). The recording becomes the route.
4. Task 7c (race 1 to the results screen, picture check), Task 8 (race 2), Task 9 Steps 4–6 (seed list, prebuild; no shader was missing from the map), Task 10 (note, games table, gallery), Task 11 (regression: Donkey Kong Bananza plus four games, also Monster Hunter Rise and OCTOPATH TRAVELER for the sparse change; no IPA). Then the final review and the checklist.

## Rules agreed in this session

- Boot no other game before Task 11.
- Runs are muted.
- Commits as `seedds`.
- Push and the PR only on the user's command.

## Known outside the code

The external BenQ monitor on Thunderbolt drops its link during heavy game runs (WindowServer hotplug out/in). The Mac charges through the same cable. Suspected cause: power renegotiation under GPU and charging load. A MagSafe charger is the test.

## Resume

In a new session in the worktree: `implementuj docs/superpowers/2026-11-01-mkw-profile-plan.md`. The ledger marks done tasks. Start with open item 1.
