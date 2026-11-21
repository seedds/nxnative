# Mortal Kombat 11 gameplay validation

Status: pending on the integrated upstream candidate. The owner reported the older local NX Play build working on 2026-11-15; that report does not validate this branch. No `validated.tsv` is supplied.

## Candidate and build

Use [codex/mk11-integration](https://github.com/yubismartlabs/nxnative/tree/codex/mk11-integration) in NX Play. This combines the profile and draft PRs [#2798](https://github.com/seedds/nxnative/pull/2798), [#2800](https://github.com/seedds/nxnative/pull/2800), [#2801](https://github.com/seedds/nxnative/pull/2801), [#2802](https://github.com/seedds/nxnative/pull/2802), [#2807](https://github.com/seedds/nxnative/pull/2807) and [#2808](https://github.com/seedds/nxnative/pull/2808). Base: upstream `23790f8d`. The candidate shader chain is `238c2c3c6acb`, with profile inputs `da4c4f927b27`.

Build the host and the full game shader libraries through NX Play. The older local libraries have a different chain stamp; a host-only rebuild cannot validate these shader fixes. The full shader build generates matching `c1_full.tsv` and `cb_extent.tsv`. The standalone Metal compiler is missing on the preparation machine; offline library compilation has not been checked here. Focused GPU tests compiled their synthetic MSL at runtime instead.

Record the exact integration commit (`git rev-parse HEAD`), NX Play version, app build time, chain/input stamps from the build log, game version, device, macOS version and active environment overrides. Keep the previous working app available for comparison.

## Manual route

For every step record pass/fail, visible defects, approximate duration and relevant log timestamps. Preserve the complete build and run logs.

1. Start the game and pass Updating Game Data into the main menu. Record whether networking is available and any wait or hang.
2. Open fighter selection. Inspect portraits, switch several fighters including Scorpion, Sub-Zero, Fujin and Joker when installed, and inspect their material previews for static or wrong colors.
3. Play a short local match through its end. Check fighter materials, stage textures, effects, transparency and HUD. Note stutters and whether they recur on a second run of the same scene.
4. Play a story movie. Check lip sync near the start, middle and end, audio continuity, image corruption and frame pacing. Exercise pause/resume and skip when available; test replay or seek if the mode offers it. Distinguish an observed sync problem from a measured drift.
5. Continue from a story movie into a fight, then return to the menus and repeat the transition. Look for crashes, stale audio and allocation failures.
6. Quit and relaunch. Confirm normal save/load behavior. Record which fighters, stages, story chapters and installed DLC were actually exercised.

Current upstream tessellation is present, but this route does not prove coverage of every hull/domain effect. Online play, all modes, full story completion, long sessions and iOS remain separate unchecked coverage unless explicitly exercised.

## Evidence and release gate

Review new `[RED]` lines, shader-build losses, crashes and stalls against the previous working app. Attach text logs and screenshots when reporting a failure; do not attach game files, keys, saves or extracted shaders.

The focused component checks are documented in the linked PRs. They do not replace integrated gameplay or the shared-fix cross-game regression report. Once a deterministic input/save route is available, add a `smoke.tsv` and run `scripts/regress.py check --record --games 0100F2200C984000` against the repository profile. Commit only a stamp produced by a successful harness run; a manual gameplay report must stay labeled as manual.
