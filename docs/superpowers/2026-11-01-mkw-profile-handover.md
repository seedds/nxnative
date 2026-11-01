# Mario Kart World 1.8.0: handover

State on 2026-11-01 (evening). Plan: `docs/superpowers/2026-11-01-mkw-profile-plan.md`. PRD: `docs/superpowers/2026-11-01-mkw-profile.md`.

## Where the work is

- Branch `feature/2026-11-01-mkw-profile`, rebased onto `origin/main` (`cd859c38`, still the newest upstream main). Not pushed. The pull request goes to `seedds/nxnative` `main`, from the GitHub account `legoxz` (its fork).
- Checkout: the main checkout `/Users/lego/github/switch/nxnative` (moved from the worktree `nxnative-nx2` on 2026-11-01; that worktree is removed).
- Run ledger (git-ignored): `.superpowers/sdd/2026-11-01-mkw-profile-plan/progress.md`, with the task reports and the review reports in the same folder.
- Python for every tool: `.superpowers/sdd/2026-11-01-mkw-profile-plan/py`. It is CPython 3.14 with lz4, capstone and fonttools from the local uv cache. Homebrew `python3` 3.15 has none of them.
- Shader tools need `DOTNET_ROOT=/opt/homebrew/opt/dotnet/libexec`.
- Game folders: build `/Users/lego/github/switch/mkw/0400C3F00006E000_build`, work `/Users/lego/github/switch/mkw/0400C3F00006E000-work`, start script `/Users/lego/github/switch/mkw/run.sh` (`DM_AUDIO=0` mutes).

## Done

- Tasks 1–6, 7a and 7b are complete and reviewed. Fix rounds after review are in the ledger as "fix (unreviewed)".
- The race crash is fixed (commits `b09b1bc6`, `4571e7a2`, `0f8e9a56`, review fix `c531bd71`, unreviewed). Cause: the translator lacked Ampere BREV / UBREV, so 25 DrivePass shaders were skipped. Compute shader `18dd278d143adfd4` then wrote past its append buffer into heap objects. The boot crash is proven; the race crash probably (report: `.superpowers/sdd/2026-11-01-mkw-profile-plan/debug-race-crash-2.md`, review: `review-crash2.md`).
- The user drove race 1 by hand, all 3 laps, with sound (`user_check.log`): no crash, no broken textures seen. Not recorded.
- The game boots, shows the title and the menu, and plays race 1 of the 150cc Mushroom Cup at about 60 fps.
- Fixed:
  - the Switch 2 services the game opens (mouse, GameChat, camera, HDR metadata),
  - the 9 GB pool,
  - sparse textures as Metal placement sparse textures,
  - `render_cond`,
  - SDK 21 video decoder names,
  - sRGB / linear texture pairs,
  - compute shared memory past 32 KB in a device buffer,
  - the Ampere shared-memory stride (the rainbow bands),
  - Ampere BREV / UBREV (the race crash).

## Test tools in the work folder

- `save_menu/`: the save after the first boot, at the main menu.
- `run_until.sh <log> <target frame> [keys]`: a fresh copy of `save_menu`, muted, ends on a crash, at the target frame or on a stall.
- `keys_race_menu.txt`: from `save_menu` to race 1, A held 20 s after GO.
- `rec.sh [name]`: records the user's play with `DM_REC` and `DM_DETCLOCK` (not used: the user does not record).
- The range-check hook (storage stores against the bound size) was a local debug build only, not committed (design in `debug-race-crash-2.md`).

## Open, waiting for the user's choice

The user stopped the scripted-route author (Task 7c round 2, brief `task-7c2-brief.md`) before it started work: do nothing until the user chooses. The user will not record races with `DM_REC` (too long). Options given to the user:

1. Scripted route through races 1 and 2: Smart Steering (and auto-accelerate) turned on in the menu, A held, built with `DM_CTL` and shots (the brief above).
2. Skip the route: close Task 7 on the user's hand-driven race 1, go on to Tasks 9–11.
3. Stop here.

Also open:
- 25 shaders on the route still fail to translate (opcodes 0x883, 0x927, 0x992); 67 fail in the full build.
- `nvnCommandBufferDrawMeshTasks` and `nvnCommandBufferShadingRateImagePalette` are not implemented (thousands of calls a race).
- Task 8 (race 2), Task 9 Steps 4–6 (seed list, prebuild), Task 10 (note, games table, gallery), Task 11 (regression: Donkey Kong Bananza plus four games, also Monster Hunter Rise and OCTOPATH TRAVELER; no IPA). Then the final review and the checklist.

## Rules agreed in this session

- Boot no other game before Task 11.
- Runs are muted.
- Commits as `legoxz`: a plain `git commit` with the checkout's identity, not AGENTS.md's `seedds` identity.
- Push and the PR only on the user's command. Nothing goes upstream while the work is unfinished.
- Describe a new agent run to the user before it starts.
- A run the user plays: add `DM_REC` (free recording).

## Known outside the code

The external BenQ monitor on Thunderbolt drops its link during heavy game runs (WindowServer hotplug out/in). The Mac charges through the same cable. Suspected cause: power renegotiation under GPU and charging load. A MagSafe charger is the test.

## Resume

In a new session in `/Users/lego/github/switch/nxnative`: `implementuj docs/superpowers/2026-11-01-mkw-profile-plan.md`. The ledger marks done tasks. Ask the user which option of "Open, waiting for the user's choice" to take.
