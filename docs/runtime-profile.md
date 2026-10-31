# Proposal: a game profile the host reads at run time

Status: proposal for [#388](https://github.com/seedds/nxnative/issues/388) ("Fewer patches for forks"), for the maintainer to decide. This page is the design; the only code with it is step 1 below, which changes no behaviour. Nothing here is measured on a device: numbers come from reading the sources and from the fork's experiment, and say so.

## The problem

Each game's constants reach the host as macros. `dm/games/game.py header` turns `game.tsv` into `dm/host/gen/<id>/game_profile.h`, `make GAME=<id>` compiles the host against it, and 37 `DM_GAME_*` macros are read in 15 files in `dm/host`, `dm/gfx/nvn` and `dm/ios`. The consequences:

- **One host binary per game.** Changing `GAME` regenerates the header and rebuilds the whole host (`gen/game.stamp` in `dm/host/Makefile`), nvn_mtl.m included. An app or a front end that offers several games ships or builds one host for each.
- **A fork cannot add a game's layout without a build.** NX2Apple wants one host for every game (below) and carries a patch over those files; every upstream change near a `DM_GAME_*` use (the address-space work in `d8b8d642` is the latest) can conflict with it or silently drop it.
- **No array is sized by a profile value**, and the only `#ifdef` on one tests whether an optional key (`shared_font`, `device_api`) is set: the macros are plain constants in ordinary branches, the modules list and display-version table are static arrays with a fixed bound. So nothing forces them to be compile-time.

## What the fork did, and why

The fork's commit `73a89fb0` ("experiment(stage2): runtime game profile") replaces `DM_GAME_X` with `dm_profile()->x` in 20 files. `dm_profile()` fills one struct from the compiled header, then from the file `DM_PROFILE` names (a key/value file `game.py profile <id>` writes), then from `DM_PROF_<KEY>` variables. Its reasons, from the commit, `docs/launcher-c-design.md` and the README: one host binary for every game, as the foundation of a universal Mac launcher and an iOS app that holds several games, whose profiles are data in `Documents/games/<id>/` or the bundle and change without a rebuild; and no per-game signing entitlements, because the memory layout is no longer baked in.

What it proved (its test and commit message): the host builds, an iOS compile check passes (135 of 135), the host test suite is green, and `t_layout_runtime.c` boots the production `GetInfo` dispatcher four times, once per process, with four different layouts (5 GB with a 2.375 GB alias region, 4 GB with 256 MB, 1 GB in total, 5 GB with the pool limit on) and checks what the guest is told.

What it did not prove: no game was run from a runtime profile in that commit; nothing was measured on a hot path; one process still serves one game (`mem_init` runs once, and the host's state is process-wide), so "an app holding several games" means one binary that starts one game per run; and the file parser re-implements the key spellings of `game.py` in C (word and number forms), a second place for them to drift. It also predates `DM_GAME_STDALLOC_CAP` and the region table.

## Proposal

**Where values come from.** `game.py` stays the only place that parses `game.tsv`, applies defaults and validates. A new `game.py profile <id> <out>` writes the values *resolved*: the same numbers the header would carry (`present_display 1`, `pad_layout 2`, hex for sizes), nothing to interpret. The host reads that file once, in `host_preload` before anything else reads a value, from `DM_PROFILE` or, in an app bundle, `dm/profile.tsv` beside `env.txt`. Without a file it uses the compiled values, so a game-specific build behaves as today. The existing per-knob switches (`DM_FPS_TARGET`, `DM_PREEMPT`, ...) keep winning at their sites, which is the precedence of [#388](https://github.com/seedds/nxnative/issues/388) (launch environment first, tested defaults last). No new `DM_PROF_*` family: a value to override has its switch, or gets one.

**Run-time fields** (all read once at start or per SVC; most are already copied into a static at init, for example `g_core_preempt` in `hle_thread.c` and `g_sa_big_align` in `hle_os.c`):

| Group | Values | Where read |
|---|---|---|
| Identity | `id`, `name`, `romfs_marker`, `program_id`, `modules`, display versions | `main.c`, `front_mac.m`, `main.m`, `hle_fs.c`, `hle_sdk16.c`: once |
| Guest memory | `mem_reserve`, `mem_alias`, `mem_stack`, `app_pool`, `mem_used`, `sys_resource`, `mem_map_limit`, `stdalloc_cap` | `hle_svc.c`: `mem_init_once` (the region bitmaps are already `calloc`ed from these at run time), `GetInfo`, `SetHeapSize`, `MapPhysicalMemory` (a branch beside a lock and a bitmap update) |
| Switches | `fps_target`, `fps_hold`, `present_display`, `opmode`, `perf_boost`, `core_preempt`, `alloc_big_align`, `shared_font`, `device_api`, `pad_layout`, `game_args`, `game_env` | init, or once per frame |
| Metal | `pso_mtl4`, `present_snap`, `feedback_snap`, `gpu_ts_segment`, `tex_dyn_slots`, `vconv_range` | `nvn_mtl.m` at device or pipeline setup |

**Compile-time, for now**, because they sit on per-call paths in the graphics layer: `tex_flush_upload` (4 sites in `hle_nvn.c`), `tex_dirty_upload` (5 sites in `hle_nvn.c` and `nvn_mtl.m`, one on the texture-use path) and `render_cond` (4 sites: two in command recording, two in report handling). Today the compiler folds each to nothing or to the one live branch. At run time each costs a load from a read-only struct and a branch that is the same on every call, so the expected cost is below the work around it (a hash lookup, a record write), but that is a reading of the code, not a measurement. Step 4 measures it before moving them; if it shows, they keep a compile-time default and the profile file can only confirm it.

**What stays compile-time regardless:** the shader chain version (`DMSH_CHAIN`, already one for every game), and per-build fixed addresses, which are already run-time checks of `module_t.build_id`. The data a game needs (patched NSOs, shader libraries and tables, RomFS) is already a set of files; the host finds the shader map through `DM_GAME_ID` (`nvn_mtl.m`, where the library stamp also names the game), which moves to `dm_profile()->id`.

## Migration

Each step is its own pull request and keeps every game working.

1. **Accessor (this pull request).** `dm/host/hle_profile.{c,h}`: `dm_profile()` returns the compiled constants in one struct; `t_profile.c` checks every field against its macro. No call site changes. Behaviour-neutral.
2. **Resolved file.** `game.py profile`, the reader in `hle_profile.c` for `DM_PROFILE`, and a Python test that, for every profile in `dm/games`, the file and the header agree. The Mac app bundle carries the file. Still no call site changes; the `DM_PROFILE` row goes into `docs/env_switches.tsv`.
3. **Cold call sites, one subsystem per pull request:** identity and arguments, then guest memory (with a synthetic-layout test like the fork's `t_layout_runtime.c`, plus `stdalloc_cap` and the region table), then switches, then Metal setup. Each runs `scripts/regress.py` and shows no log difference.
4. **Hot graphics flags,** after a micro-benchmark of the flush and recording paths with a load against the folded constant.
5. **One host.** `GAME` becomes optional in the Makefile and `build_ipa.sh`; `test_architecture.py` (#422) gains the rule "no `DM_GAME_*` outside `hle_profile.c`". Whether packaged apps then share a host is a packaging choice for the front ends.

## Risks

- **Hot paths:** above; step 4 decides, nothing before it touches them.
- **Missing keys:** a profile file that lacks a key must not fall back to another game's compiled value or to zero; `game.py` writes every key, and the reader treats a short file as an error.
- **Wrong profile for the files:** the host would check the file's `id` against the library stamp (`st_game`) and the module build IDs it loads, so game A's profile cannot start game B silently.
- **Per-build addresses:** unchanged, keyed by build ID at run time; a profile never carries an address.
- **Two readers of one format:** avoided by writing resolved values (above); the fork's parser accepts both spellings, and the fork would drop that part.
- **Test cost:** one binary means one binary to test per change, `regress.py` per game as now.

## Decision

Adopt in steps (1 to 5), adopt partly (steps 1 to 3 and keep the graphics flags and the per-game build), or keep compile-time. In the last case the fork keeps its patch and its sync patchers; in the first two the fork drops its parser and its call-site edits as steps land, and keeps only the launcher on top.
