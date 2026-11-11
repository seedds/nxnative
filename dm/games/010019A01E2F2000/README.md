# Metroid Prime 4: Beyond (`010019A01E2F2000`)

The Switch version, update 1.1.0 (v131072) over a cartridge dump of the base game, SDK 19.3.15, Retro Studios' own engine (little-endian RFRM forms, the engine of [Metroid Prime Remastered](../010012101468C000/README.md)), NVN (API 55.15). Modules `main`, `subsdk0` (the SDK's movie library) and `sdk`. RomFS is 445 files, 26.2 GB: `MaterialArchive.arc`, `Kernel.pak`, `MiscData.pak`, `StartupUI.pak`, `Preload/DuplicateData.pak` (3.6 GB), the UI packs under `UI/MPR4/`, `Worlds/FrontEnd/FrontEnd.pak`, 432 packs under `Worlds/Suits/` and the update's `Patch/Z_198_NXPatch.pak.patch`.

Brought up on the Mac with the provisional profile's values and no host change: boot, the opening movie, the title, the menu, the save slot and difficulty screens, a new game's text and real-time opening (the Federation base under attack, Samus's gunship landing) and the first area of Tanamaar. Played for about a minute there: walking, turning and firing, with the visor HUD, the radar, the map and the hint texts. 60 fps (frame time p50 16.67 ms, GPU p50 about 5 ms at 1600×900 on an Apple M6); the audio renderer produces sound (70,478 of the first 72,000 rendered frames not silent; not listened to); memory levels off at about 4.0 GB resident and 1.9 GB of Metal allocations in the first area (frames 18,000 to 23,000). Not yet tested on the iPhone.

## What it needs

| Setting | Where | Why |
|---|---|---|
| 4 GB layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | [`game.tsv`](game.tsv) | no virtual address memory: the game raises its heap step by step and settles on 3.16 GB in the 3.25 GB heap region, as Metroid Prime Remastered does ([memory.md](../../../docs/reference/memory.md#a-heap-request-beyond-the-heap-region-returns-resultoutofmemory)) |
| `sys_resource 0x0` | `game.tsv` | the NPDM declares 0 MB of system resource |
| `modules main subsdk0 sdk` | `game.tsv` | every module `main` imports from |
| `shader_groups` (`/Worlds/`, `/Preload/`, `/UI/`, `/Patch/`) | `game.tsv` | the shader split's groups ([Shaders](#shaders)) |
| [`dual_want.tsv`](dual_want.tsv) | profile | `af84ccd3191eaf89` writes colour output 1 that a pipeline blends as the second source (met in the first area; the host's red line names it) |

No rendering key, no rewriter patch and no host row: beyond that table, the profile is the provisional one the NSP script wrote.

## Shaders

- **Resolved: `the shader split found no shaders in main or the RomFS`**, after `ADIR of 13 entries in 0x30c bytes, neither 36 nor 52 bytes each` for 442 files. The packs' resource entries are 60 bytes here, and the program records and the packs' `MTRL` forms are zstd streams where Metroid Prime Remastered's are zlib: [SNVN program records in Retro Studios' RFRM forms](../../../docs/reference/shaders.md#snvn-program-records-in-retro-studios-rfrm-forms). 259,989 shaders: 129,992 vertex, 129,972 pixel, 19 geometry and 6 compute, first seen in the world packs (124,094), the preload packs (110,994), the patch pack (21,360), the root's files (3,445; `MaterialArchive.arc` has 3,473 stages in 1,740 programs) and the UI packs (96); 129,972 vertex / pixel pairs. `main` holds none. The split takes 2 minutes 19 s, the Mac libraries 3 hours 43 minutes and the pipeline prebuild 51 minutes (Apple M6); the libraries are 68 GB and the Mac pipeline archive 16 GB.
- `MaterialArchive.arc` also holds an `NVN2` chunk with the same table and record layout as `SNVN` (1,740 programs, other code); this build never asked for a shader outside the map, so it reads `SNVN`. What `NVN2` is for was not looked into (the Switch 2 Edition's programs, by the name).

## Current

- `vertex attribute format 0x27 (8 / 16-bit integer) feeds a float input` (RGBA8UI): converted on the CPU at replay, as for [Metroid Prime Remastered](../010012101468C000/README.md); no visible effect.
- `NVN address lookup of an unknown name: nvnInstallGlobalDebugCallback`: the game asks for a debug entry point by name and goes on without it.
- **Open: runs of 32 ms frames after a long frame.** A long frame in play (a first use of a pipeline, 28 to 111 ms in submit; a screenshot, 80 to 90 ms) is followed by 5 to 20 frames of 32 ms each whose submit and GPU times are normal (1 ms, 3 to 5 ms); the game never sets a present interval other than 1. Whether the game's own frame limiter halves its rate to catch up, as it would on the Switch, or the host's pacing does, was not traced. 21 pipelines were first built in 30 s of play although the prebuild had run; the prewarm list is empty ([Open](#open)).

## Mac routes

The work folder is `<NSP folder>/010019A01E2F2000-work`: `host.sh` copies the worktree's `dm/host/dmhost` to `build/mp4host`; `run/boot.sh <log> [secs] VAR=value…` runs it on the NSP script's build (`010019A01E2F2000_build`: the RomFS image, the rewritten modules, the shader libraries in `shader/cache`) with `DM_CTL=run/ctl.txt`; `run/ctl.sh press|snap` drives it, and `run/step.sh <name> [key [wait]]…` presses keys and takes a shot. `run/pso_archive` is a link to the NSP script's (`010019A01E2F2000_build/run/pso_archive`): without it the host prebuilds 129,972 pairs before the first frame, about 50 minutes. `tools/rget.py <path> <out>` copies a file out of the RomFS image and `tools/pak.py <pack> [type]` lists a pack's resources.

- From a fresh save, in seconds after the start: the title's `PRESS A` answers from about 60 s (`A~0.4`; a press at 52 s did nothing); then `A~0.3` four seconds apart on Play, save slot 1 (New Game) and Normal. A loading screen, the text of the opening and the real-time opening follow unskipped (skipping was not tried); the game gives control on Tanamaar within 2 minutes 30 s of the last press, about 3 minutes 45 s into the run (energy 99, missiles 30). The left stick walks, the right stick turns and A fires; ZL locks on by the game's hint (not tried). The new file's `save.bin` exists from the difficulty screen on; there is no save past the first area.

## Found on this game

General tool changes first needed here; each applies to every game:

- **Joy-Con vibration handles on a full controller's npad:** the game asks only for JoyDual vibration handles, which the host refused on a Pro Controller's (or DualSense's) npad, so nothing vibrated: [services.md](../../../docs/reference/services.md#joy-con-vibration-handles-on-a-full-controllers-npad).

- **Version-validated Mac runtime code choices:** [runtime patch manifests](../../../docs/reference/rewriter.md#validated-mac-runtime-patch-manifests), introduced for the experimental internal scene size below.

- **60-byte pack entries, zstd program records and `MTRL` forms, and program records whose control length is not a multiple of 0x40**, which the reader had skipped without a word in Metroid Prime Remastered too (28,479 of its shaders) (`rfrm_shaders`, `snvn_records`): [shaders.md](../../../docs/reference/shaders.md#snvn-program-records-in-retro-studios-rfrm-forms).

## Open

- The iPhone: no IPA built or tested. The game needs a 3.16 GB heap in a 4 GB layout, 4.0 GB resident on the Mac, and 68 GB of Mac shader libraries; the pipeline prewarm list `pso_manifest.tsv` is still empty.
- Only the opening and the first minute of Tanamaar have been played: combat, scanning, the Morph Ball, the psychic abilities, Sol Valley and Vi-O-La, bosses, saving at a station and loading a save are untested.
- The base game 1.0.0 alone was not run (the NSP script packs it from a derived profile).
- The runs of 32 ms frames ([Current](#current)).


## Image quality investigation, 2026-11-11 (open)

Investigation baseline: main `084f5aa5c59bbfbebfcab7ad1b1a3b9bc196a582`, also the upstream tip checked on this date. Experimental worktree: branch `codex/mp4-image-quality`. This section records an investigation, not a new gameplay milestone. At that investigation checkpoint, no game source, profile settings or shader-chain tool had changed and no local full shader build or gameplay run had been performed. The implementation below supersedes that checkpoint.

### Local package and executable verification

The local package folder contains a compressed base Application `010019a01e2f2000`, version 0, and a compressed Patch `010019a01e2f2800`, version 131072. These identities were read with `nx_extract --contents`, not inferred from filenames. The update was decompressed into the game's work folder; its control data reports **1.1.0**. Its complete ExeFS gives the following build IDs (the 20 nonzero bytes; the NSO's 32-byte fields end in 12 zero bytes):

| Module | Build ID |
|---|---|
| main | `8275fbe1ec5191c279f3a943ad8524f4bab087a3` |
| subsdk0 | `9c8b2c03854ffda4e33614d6f42d4a648aa53c8e` |
| sdk | `0cc95d4d30133833d7df439a890414de87026609` |
| rtld | `232335b2f5212e30d90415810cbdd276fade2fe1` |

The first 16 bytes of main, subsdk0 and sdk match the registered profile. Only update executables and control data were extracted: the update-only command subsequently failed when trying to mount patch RomFS without its base. That failure is not evidence that the package is damaged; a complete merged RomFS and its integrity checks are still required. The base NSZ is not directly usable as an extracted Program NCA: the normal build must decompress it first. Metadata and logs are in `<NSP folder>/010019A01E2F2000-work/metadata/`; original dumps and installed apps were not changed.

### PR disposition and dependencies

GitHub open/closed state alone does not indicate whether main has the functionality:

| PR | State on inspection | Current main and remaining action |
|---|---|---|
| [512](https://github.com/seedds/nxnative/pull/512) | open, unmerged | The required 60-byte pack directory and zstd SNVN/MTRL support is already in the common reader, from `430ace12`; retain it rather than add a parallel reader. |
| [513](https://github.com/seedds/nxnative/pull/513) | closed, unmerged | Ported as `7c23cc03`: 16-byte aligned handles and normalization of backslashes across mounts, with broader coverage than the PR. |
| [514](https://github.com/seedds/nxnative/pull/514) | closed, unmerged | Ported as `45a3c987`: indexed RomFS overloads and texture-builder swizzle getter. |
| [515](https://github.com/seedds/nxnative/pull/515) | closed, superseded | General native-texture slot handling in `4e37a225` and `8bfc3a60`; do not restore the movie-specific profile key. See [movie slot ownership](../../../docs/reference/movies.md#getoutputbuffer-returns-the-output-index-in-native-texture-mode). |
| [516](https://github.com/seedds/nxnative/pull/516) | closed, superseded | Main's `dual_want.tsv` / `main0_d` implementation (`ada029c5`) replaces `_bds` / `dsb.tsv`; this profile already lists `af84ccd3191eaf89`. See [secondary blend source](../../../docs/reference/rendering.md#a-written-secondary-blend-source-nvn-dual-source-blending). |
| [517](https://github.com/seedds/nxnative/pull/517) | closed, superseded | `bcb18804` preserves writable-image vertex side effects under rasterizer discard. No chain change is needed to restore this fix. |
| [518](https://github.com/seedds/nxnative/pull/518) | closed, ported | `148cc076` supports cloning a single RomFS image and probes each source independently. |
| [519](https://github.com/seedds/nxnative/pull/519) | open, unmerged | Launch-time patch mechanism remains absent; it contains no Prime 4 resolution/FPS data. Reuse existing verified rewriter stubs where possible. If this mechanism is needed, review parsing, build gating, rewritten-site checks and rewrite-cache inputs before integrating. |
| [520](https://github.com/seedds/nxnative/pull/520) | open, unmerged | Main already has the 1.1.0 profile and full shader tables from `430ace12`. The PR additionally enables CPU-written texture streaming keys; reproduce the reported magenta mip levels on current main before changing the profile. Its obsolete movie key and shader tables must not replace main's. |

PR 520 declared dependencies on 515 and, for correct rendering, 512/513/514/516/517. Their underlying mechanisms are now present, though their implementations differ. The optional resolution/FPS choices additionally need 519 or a verified alternative plus game-specific code not included in these PRs. [Combined PR 579](https://github.com/seedds/nxnative/pull/579), closed and unmerged, fixed 519's unconditional access to `DM_GAMES_DIR` and added the generator to the rewrite cache key; those fixes matter if adapting 519. It did not establish gameplay validation of its combined branch.

### Community patches and compatibility

Inspected [Fl4sh9174's Prime 4 archive](https://github.com/Fl4sh9174/Switch-Emulator-Ultrawide-FPS-Mods/blob/main/Metroid%20Prime%204%20Beyond%20%5B010019A01E2F2000%5D%5Bmods%5D.zip), Git blob `010b786dfc68719da0bc300dd1836bfea76903bd`. The main patch targets the local main's 20-byte build ID; the FPS companion targets the local sdk's. This establishes identity compatibility, not compatibility with rewritten instructions or the native host's timing.

The inspected archive offers 1920x1080, 3840x2160, 7680x4320, ultrawide variants, dynamic-resolution disable, LOD, and dynamic 120/240 FPS. It contains no dedicated 2560x1440 16:9 preset. Its instructions require the included input-dependent cheat for resolution mods: the scanning path changes dimensions while ZL is held. Applying only the code writes would leave that dependency unsatisfied. The FPS modification changes both game code and SDK behavior; a display-rate override alone cannot reproduce its timing behavior. PR 519 reports 1440p/120 and 4K/60 on the author's own branch, not on this baseline or this Mac.

No LICENSE/COPYING file or grant was found inside the archive, and the repository README supplies no reuse license. Free downloads and author attribution do not establish permission to incorporate or adapt the patch code. No third-party patch was applied, committed or packaged. Integration needs a verified license grant, or an implementation derived from the game's own code and measured behavior. The latter must not be described as clean-room work: the community patch was inspected during this investigation.

### Implementation scope and validation gates

- **Low complexity, already available:** build and package a self-contained Mac app at the game's own resolution and 60 FPS; use a new output name and build folder.
- **Medium complexity:** validate the texture-streaming report and the long-frame pacing issue on current main. Record a route through the cave, including the cannon, before changing upload policy.
- **Medium to high complexity:** selectable 1920x1080, 2560x1440 and 3840x2160 internal sizes. Static inspection of local main confirms separate size initialization, viewport, screen-space constants and scan-related calculations. A window size setting cannot cover them. Verify original bytes and build IDs at every rewritten site and preserve the 16:9 layout.
- **High complexity:** 120 FPS. Trace the frame limiter and game-time calculation, then compare the same timed movement, turning, combat and cutscene inputs at 60 and 120. Check behavior below target FPS and on a 60 Hz display before enabling `fps_max 120`.
- **Optional, medium complexity:** anti-aliasing improvements. Start with higher internal resolution and compare still and moving edges. Forcing MSAA or temporal AA globally is unsupported by the present evidence; establish the game's buffers and sampling requirements first.

Existing [present MetalFX scaling](../../../docs/reference/rendering.md#the-present-upscales-through-metalfx) can enlarge the completed image, but it does not raise internal render-target resolution or provide temporal anti-aliasing. The separate NVN2 DLSS and agl upscaler replacements have no established use in this Switch build. Keep present scaling off for native-size comparisons; assess it as an optional performance/quality tradeoff when rendering below the display size.

Prove each internal mode with pass/attachment sizes and viewport logs, alongside game-frame captures and screen captures. Compare HUD placement, scan targeting, bloom, fog, shadows, depth-based effects, transparent geometry, movies and real-time cutscenes. Use fixed saves and identical inputs for timing. Later areas, bosses, Morph Ball, saving/loading and prolonged streaming remain untested locally. No large renderer refactor is justified by this investigation.

### Local checks and build resources

Hardware verified: M4 Max, 48 GB RAM, macOS 27.0; Xcode 27.2 and the Metal compiler match the chain lock. Baseline checks passed: 23 tests across `test_snvn_records`, `test_discard`, `test_game_profile`, `test_settings`, `test_docs`, using Python 3.14; `make -C dm/gfx/tests GAME=010019A01E2F2000 check_t_rast_discard check_t_missing_src1` on the M4 Max. These are synthetic tests, not game validation. The system Python 3.9 cannot import current shader helpers; use a recent Python. No regression-harness comparison was run because no implementation change or merged game build exists yet.

The drive had approximately 258 GiB free after the user freed space. Plan for at least 140 GB of additional working storage, plus intermediate files and headroom: decompressed packages, merged RomFS, the shader library and pipeline data. The existing shader measurements above come from another Mac, not this M4 Max. PR 520 reports offline `metal-tt` memory above 79 GB for all pairs: prefer an initial `--gpu none` build on this 48 GB machine, with `DM_PSO_PREBUILD=0` in its build env file to avoid the long first-start prebuild. This incurs first-use pipeline compilation and may expose the long-frame pacing issue; do not promise a stutter-free app.

A full baseline app build is reproducible with the following command once full compilation is wanted. It uses every shader, keeps game data beside the source packages, and gives the experimental app its own name. It does not implement the enhanced resolution/FPS choices described above. Create `<NSP folder>/010019A01E2F2000-work/quality.env` with `DM_PSO_PREBUILD=0` first. Put the installed NSZ executable (5.0 or newer, with `--keys`) on PATH if it is not installed in the selected Python.

```sh
/opt/homebrew/bin/python3.14 scripts/build_ipa_from_nsp.py \
  "<NSP folder>" -k "<prod.keys>" \
  --package v131072 --platform mac-app --gpu none \
  --work-dir "<NSP folder>/010019A01E2F2000_build-experimental" \
  --env-file "<NSP folder>/010019A01E2F2000-work/quality.env" \
  --out "<NSP folder>/Metroid Prime 4 Beyond Experimental.app"
```

Full compilation remains deferred. No enhanced app has been built or claimed validated; subsequent runtime experiments are recorded below.


## Experimental 1440p internal scene rendering (open)

### Progress (2026-11-11)

- **Implemented:** checkpoint A, the DMRT1 runtime manifest (PR 519's idea, rewritten: full build-ID check, original-byte check against the original and the rewritten image, all-or-nothing writes before the code is sealed, unknown choice or missing manifest rejected); choice `1440p` raises the scene's colour/depth targets and the second pooled target to 2560x1440. Display output, HUD and the 1600x900 composition targets stay the game's.
- **Last verified:** `t_runtime_patch` 29 cases (gcc ASan/UBSan, Linux) and `test_runtime_patch` 7 tests, `test_docs`, 2026-11-11. Allocation evidence from the earlier scripted runs below; those runs lacked 2,975 shaders (GPU 0.45 ms a frame), so they show no picture worth judging.
- **Current experiment:** checkpoint B, the first manual test of `1440p` with a usable partial shader library: [`manual_test.sh`](manual_test.sh) builds the host and a `--seen-shaders` library from [`shader_seen.tsv`](shader_seen.tsv) (2,984 keys the earlier runs drew or missed, grown each run by the missing keys in `run/host.log`), runs the game with its save in `<build>/run/save`, and writes `summary.txt` under `<NSP folder>/010019A01E2F2000-work/manual-test/`.
- **Blockers:** a whole-map shader build (259,989 shaders) is not approved; areas past the tested route draw incomplete until the script runs again. 1440p output (HUD and final image) is not implemented, see below.
- **Next after feedback:** fix what the 1440p scene shows wrong; then the output size (below), then 4K.

### White scene in the 1440p manual test (current, fix awaiting a manual run)

First manual test of `1440p` (2026-11-11, M4 Max, `dmhost` in a window, about 8 minutes from a new game into the Galactic Federation landing pad): the world turned almost white at times, with coloured edges, while the HUD, visor and subtitles drew. Evidence from that run's `run/host.log` (pass dumps every 1,800 frames):

- **Cause (found in the log, the fix not yet seen on screen):** the geometry shaders `3296ad37262ccdaa` and `fdef95d012529950` (`MaterialArchive.arc`) were not in the partial library. They pick the face of a cube map: a 36-index cube drawn into a 128x128 RGBA16F cube (`array=6 target=3`, 8 mips, `ser=13585`) as each mip is filtered. The host draws a program whose geometry shader has no library without that stage (`draw_gs_check`, `nvn_mtl.m`), so only face 0 was written. The main 2560x1440 scene pass samples that cube (`tex reg4`, passes 261903 / 261904 at F13500), and the other five faces held stale or uninitialised data. Which face a pixel samples follows where the camera looks, which fits a blown-out world that comes and goes. It has nothing to do with the scene size: the stock path draws the same way with this library.
- **Ruled out by the same dumps:** every pass of a gameplay frame at 1440p matches the stock frame (`runtime-test/run/verify-default.log`): the scene, half-size AO (1280x720), luminance chain (2048x1024 down to 1x1, 1024x512 in the stock frame), bloom at a quarter of the scene (640x360 down to 40x22 inside the scene-sized target, as the stock frame's 324x182 inside 1296x728), then the scene and bloom composited into the 1600x900 output. No pass reads a target at a size it did not write. Draws skipped for want of a pipeline: 1 to 3 per dumped frame, outside the scene's lighting.
- **Also missing:** 42 other shaders (universe room, the landing pad, the pause menu, preload data). `c398aed35ef28ff5` (`zone_galFed.pak`, a 6-index quad in the scene pass) blends with a second source and had no dual-source entry point, so its second source counted as zero ([A written secondary blend source](../../../docs/reference/rendering.md#a-written-secondary-blend-source-nvn-dual-source-blending)).
- **Change:** the 44 keys are in [`shader_seen.tsv`](shader_seen.tsv) and `c398aed35ef28ff5` is in [`dual_want.tsv`](dual_want.tsv). `test_nsp_mac_app` `SeenShaders.test_missing_geometry_shader` checks that a geometry shader the log reports missing enters the next partial build.

### Scanning does not complete in the 1440p test (candidate fix)

Second 1440p manual test (2026-11-11, `manual-test/1440p-1111-154657`): the white scene was gone (no draw without its geometry shader), and the warm start built 3,608 prewarm lines in 0.28 s with no long frame from a pipeline compile. But the tutorial door in the opening Federation facility could not be scanned: with ZL held for seconds the reticle narrowed on the door, no scan progress appeared and the door stayed shut, every time.

- **Not settled by that run:** no pass dump fell on a frame while ZL was held (dumps every 1,800 frames), and the input was not recorded.
- **Static evidence that points at the resolution writes, unproven:** the scene-size code (`main+0x331474`–`0x3317fc`) reads a list of requests (entries of type 0xf, `0x331540`); it copies one into `x22+0xbd0` (types 1 and 2 ramp the floats at `+0xbd8` / `+0xbe4`), and the size it computes from them (`0x331654`–`0x33170c`) is what the `1440p` write at `0x33170c` replaces with a constant. `w27` sends the size to the fixed 1600x900 descriptor (`0x331670`, `0x3316f0`), which the writes at `0x3317ec` / `0x3317f8` also change. The community patch archive (not used) says its resolution codes need a cheat because the scanning path changes dimensions while ZL is held. Which request scanning makes, and what reads the size it asked for, is not traced yet.
- **Not the cause:** the two shaders still missing (`01e51066c1f1afde`, `16527609f1d61dbc`, `Preload/DuplicateData.pak`, a pair whose draws were dropped: `program has no vertex shader`) are now in [`shader_seen.tsv`](shader_seen.tsv); they first drew about 105 s into the run, in a 1600x900 pass. The other red lines are expected with a partial library and change no picture: no stamp and `c1_full.tsv` (each c1 gets its own 64 KiB, memory only), `nvnInstallGlobalDebugCallback` (a debug hook the game looks up), and the 8 / 16-bit integer attribute the host converts on the CPU.
- **Default works:** `manual-test/default-1111-155819` scanned the door (ZL held 1.5 to 2.8 s, no gap in any hold, `input.rec`). So ZL reaches the game as held, and the visor works without the writes.
- **Scanning makes no size request of its own:** in that run's 61 dumps (every 150 frames), the scene was 1244x700 to 1436x808 with ZL held or not, as dynamic resolution chose, and no pass had attachments of different sizes in any run.
- **The second descriptor is the suspect:** `w27` (`0x331490`, `0x331d10`) is 1 only without a dynamic-resolution controller, so the fixed 1600x900 descriptor at `0x3317ec` is not a fallback: it is requested every frame after the scene's. Its target's size goes to `+0x618` of the renderer (`0x331890`) and is compared with the 1600x900 display rectangle (`0x3318b8`), setting bit 0 at `+0x620` when they differ: never in stock, always with the old writes. No dump shows that target bound as an attachment in either mode, so raising it added nothing to the 1440p picture.
- **Candidate (awaiting the door test):** choice `1440p` now makes only the scene write (`0x33170c`); [`runtime_patches.py`](runtime_patches.py) `scene_writes`, checked by `test_runtime_patch` `test_1440p_changes_only_the_scene_descriptor`. A replay of the default run's input in this mode drew the scene at 2560x1440 with no new red line. The replay leaves the recorded route before the door, so it cannot test the scan itself.

### Vibration with a DualSense (current)

- The game asks once, at boot, for the JoyDual vibration handles of each npad (style `0x4`), then runs the SDK's vibration graph at 200 values a second per handle. The host refused JoyDual handles on a Pro Controller's npad, so all values went to handle 0 and nothing was felt: [Joy-Con vibration handles on a full controller's npad](../../../docs/reference/services.md#joy-con-vibration-handles-on-a-full-controllers-npad), fixed in the host.
- **Game side, from the code:** the input object's init (`main+0xaab6f8`, vtable `0x1772650`) asks every npad, connected or not, for JoyDual handles (constant style 4 at `0xaab7a0`; a connected npad's path `0xaab868` sets up its six-axis sensors and then joins the same call), so JoyDual handles are the game's handles for every controller. Each handle gets a graph VibrationWriter → VibrationMixer (mode 3) → VibrationTarget. Each frame the writer (`0xd34f64`) writes the npad's requested low and high amplitudes (object `+0x3cc8` / `+0x3cd0`, stride 0x158), clamped to 0..1, then clears them. The requests come through virtual methods (`0xa4e00` amplitudes, `0xd350b8` effect files through a VibrationPlayer), and `0xa4e70` mixes a request without any gate of its own. So the zeros mean game logic upstream made no request: the game's own vibration setting, or another condition, not traced.
- **Measured:** `manual-test/1440p-1111-163318` (108 s of play with scans): handles `0x5` / `0x10005` on npad 0, 84,966 values, 0 seconds with a nonzero amplitude. Nothing was lost in the host; the game requested no vibration.
- **Host output check without the game (passed 2026-11-11):** [`scripts/rumble_check.sh`](../../../scripts/rumble_check.sh) builds `dm/host/rumble_check.m` with the host's haptics code and plays a fixed sequence on the first controller; on the user's DualSense every step was felt, the side and strength steps included. The host's output works; the game sends no vibration.
- **Open, the remaining lead:** the rumble system's setup (`main+0x936640`) looks up a resource named `RumbleCategories` by name (`0x936670`, the resource manager's vtable `+0x28`) and skips its whole setup when the lookup returns nothing (`cbz x0, 0x936748` at `0x936698`). A missing rumble category table would leave every request at 0, as measured. Whether the lookup fails here needs a probe on `0x936698` in a game run (`x0` there); the game's own vibration option is the other candidate. Not pursued further for now.
- **What the game is told (log of `manual-test/1440p-1111-165259`):** it declares FullKey, Handheld and JoyDual as supported (`hid supported controller styles 0x7`). The host answers FullKey (a Pro Controller, style `0x1`) for a connected pad when the game supports it (`kind_style`, `hle_hid.c`), and the earlier handle log showed npad 0 at `0x1`. The DualSense was reported 46 s after the game set up vibration; npad 0 already had the keyboard's player then, so the handle request was answered. The game's options have no vibration setting (user, 2026-11-11). That the game draws Joy-Con button prompts is not proof of a Joy-Con style: which prompts it picks, and from what, is not traced.
- **Probe results (`manual-test/1440p-1111-170322`, `RUMBLE_PROBE=1`):** the rumble setup runs (`main+0x936628`, at boot and again at 17.5 s) and finds `RumbleCategories` (`0x9366a0` each time). From frame 508 the rumble mixer calls the amplitude request (`0xa4e00`, through vtable `+0x58` from `0x2829e0`) for npad 0 every frame. It passes its sums at `[x21]` / `[x20]` times a category gain (`[entry+0xf8]` normalised); a gain of 0 skips the call (`0x282950`), so the gain is positive and the sums are 0. No effect file plays (`0xd350b8` never hit). The host side is complete: handles, routing and output all work, and the zeros come from the game's rumble sources, which no gameplay event fed in 63 s of play.
- **Blocker (deferred, 2026-11-11):** why the game's rumble sources stay at 0 is in its gameplay rumble system (what fills the sums the mixer reads), not traced. Not a host fault found so far; possibly a condition of the controller or session the game checks that the host answers differently from a console.

### 2160p scene (current: candidate, not run)

Choice `2160p` (`DM_INTERNAL_RESOLUTION=2160p`) is the same single write at `0x33170c` as 1440p, branching to a second stub that packs 3840x2160 (`patches.py` `SCENE_SIZES`, `RUNTIME_SCENE_2160`); 1440p and default are unchanged. Expected cost, an estimate: 2.25 times the 1440p scene's pixels, whose GPU time was 6 to 9 ms a frame in the landing pad, so roughly 13 to 20 ms, at or past the 16.7 ms budget in busy scenes on an M4 Max; the scene's targets grow by about 130 MB. The HUD and output stay 1600x900, so this is supersampling into the original output, as 1440p is.

### Texture sharpness (current: an A/B candidate)

From the pass dumps of the 1440p and stock runs and the host's sampler code, without a run of its own:

- **As on the console:** every one of the game's samplers in the dumps has anisotropy 1 and LOD bias 0, which `samp_by_id` (`nvn_mtl.m`) passes to Metal unchanged. 14 of about 2,500 sampler bindings clamp the minimum LOD to 0.5 or 0.75; that is the game's choice. Bound textures carry their whole mip chains (for example 1024x1024 with 11 levels, 64x512 with 10), and a re-upload after the CPU rewrites a texture's pool pages sends every level again, so no top mip is lost.
- **Already sharper at 1440p:** the scene is 2560x1440 instead of the stock 1244x700 to 1436x808, so the hardware picks mips about 0.8 to 1 level finer with no bias. A negative bias would add shimmer, not detail.
- **The candidate:** anisotropic filtering, which the game never asks for, sharpens floors and walls seen at a grazing angle. `DM_NVN_ANISO=<2..16>` (`nvn_aniso.h`, `t_aniso`) sets a floor for the samplers that filter linearly across mips, leaving out nearest, comparison (shadow), min / max reduction and single-LOD samplers. Off by default. Its GPU cost is expected to be small next to the 6 to 9 ms the 1440p scene takes; there is no memory cost. Not seen on screen yet.

### Red lines of a partial-library run (current)

`manual-test/1440p-1111-165259`, read once:
- `shader library … has no stamp (inputs.tsv)` and `c1_full.tsv is stale or missing`: expected with `--seen-shaders`. A partial library is left unstamped on purpose, since shaders it does not hold are not proven safe, so the host does not take the tables a full build writes, and every c1 gets its own 64 KiB (memory, not a picture). A packaged app needs the full, stamped library and its `c1_full.tsv` / `cb_extent.tsv`, which only an approved full build makes.
- `NVN address lookup of an unknown name: nvnInstallGlobalDebugCallback`: benign. The game asks for a debug-layer hook the host does not provide and gets null, as without the debug layer.
- `vertex attribute format 0x27 (8 / 16-bit integer) feeds a float input`: benign compatibility. The host converts those attributes on the CPU at replay, as the line says; no draw was seen wrong.
None needs a fix.

### Stutter at area transitions (open)

- From `manual-test/default-1111-155819` and replays of its input (`<NSP>/010019A01E2F2000-work/replay/`): with the warm prewarm list, pipeline compiles remain only on first visits (55 past the door on the first run, 1 in a replay after it). Area loads keep a long frame: frame 1976-1980, 113-153 ms, with 172 first texture uploads (309-312 MB, 29-34 ms) and 3.2-3.7 GB entering the residency set in that window (155 commits, 62-67 ms). Six-texture re-uploads of about 130 MB (20-28 ms) recur at transitions; a ring of six 1920x1080 video frames rewritten one a frame (8.4 MB, all pages) is not one of them. Frames of 60-150 ms on the submit thread with no compile or upload (frames 2232-2285) did not recur in every replay; a CPU sample of the host across them is the next step.

### Frame pacing in the same run (current)

- Outside compile bursts the game holds 60 fps at 1440p: 52 clean 300-frame windows with frame-time p50 16.67 ms, interval p99 at most 19.8 ms, 0 to 6 frames dropped per window; GPU p95 at most 10.7 ms; the submit thread's p95 2 to 5 ms.
- **Stutters are first-use Metal pipeline compiles:** 104 of the 213 long frames, with 405 pipelines and 8.9 s of compiling between them, up to 1.78 s in one frame (frame 20545: 15 pipelines in 326 ms; frame 20700's window: 2.4 s), at scene and cutscene starts. Texture uploads caused one long frame (39 ms). The run started with an empty profile seed list (`pso_manifest.tsv`, 0 lines) and no `pso_user.tsv`, so it was a cold start for everything past the archive's 1,489 earlier pipelines.
- The other long frames (about 100, 34 or 134 ms each) fall while the front end reported `window covered` (its display link fires about once a second); they measure nothing.
- 1440p adds no pass, reallocation or synchronisation of its own in the dumps; its cost is GPU time (p50 6 to 9 ms in the landing pad).
- **Expected next run:** the run wrote `run/pso_user.tsv`, which the prepare step builds before the first frame, so the places already visited should start without these compiles. If that holds, record a seed list for `pso_manifest.tsv` (docs/development.md, Prewarm lists) so a first start gets the same.

### Output resolution: findings for the next step (static, unverified at run time)

- The render-target pool `0x134470` (and `0x134850` / `0x134ddc`) keys targets by descriptor `+0` width, `+4` height, `+0x18` format. About 14 call sites build fixed 1600x900 descriptors (`mov x8,#1600; movk x8,#900,lsl #32`): `0xa1cd8`, `0x12e3a8`, `0x12f9a0`, `0x132458`, `0x133568`, `0x136edc`, `0x1f5990`, `0x3317ec`, `0x335d54`, `0x3e5060`, `0xa1e6e4`, `0xd315a4`, `0x137f4b0`; these are the 1600x900 composition/HUD targets of the pass logs (RGBA8 fmt 71 with D24S8).
- `0xda3fd8` initializes the resolution policy table at `main+0x1832940`, `[mode][kind]` of 16 bytes: kind 0 1024x576, kind 1 1600x900. Dynamic resolution (`0x980118`) scales an entry by a float and clamps it to the size at `0x1826938`; `0x99fd10` / `0x99fd30` read it.
- `0x80be60` (via `0x80be90` / `0x80be98`) returns the rectangle 0,0,1600,900 and `0xda3af4` the size 1600x900; `0xda3b04` / `0xda3b10` pair that size with a target's real size. This reads as a fixed 1600x900 virtual layout space mapped onto each target, so the HUD's layout should keep 1600x900 while the targets grow. `0xda2928` picks 1600x900 or 800x450 by the flag at `0x1832938`.
- Open: where the window (present) textures, 1600x900 (`ser=2`), are sized. All targets one pass draws into must change together.

Source is on `codex/mp4-image-quality`, based on `084f5aa5`. This is a Mac-only development option, not a validated launcher choice. `DM_INTERNAL_RESOLUTION=default` (or unset) applies **zero runtime writes**. `1440p` selects the manifest made from the registered 1.1.0 executable. No 1080p, 4K, 120 FPS, presets, new anti-aliasing or shadow changes are exposed. The game keeps its own 60 FPS timing. The profile's texture upload policy is unchanged.

### How the internal size changes

Derived from the local executable, not converted from community patch payloads:

- `main+0x331474` loads input dimensions, and the code through `0x331708` computes an adaptive scene size. At `0x33170c`, `orr x21,x9,x8,lsl #32` packs width and height into a scene allocation descriptor; `0x3317e4` stores it at `sp+0x58`, and `0x3317e8` passes it to `0x134850`.
- [`patches.py`](patches.py) reserves a three-instruction stub using the existing `STUBS` interface: construct `x21 = 2560 | (1440 << 32)` and branch back to `0x331710`. It changes no original site by itself. Only the selected runtime manifest replaces the original pack instruction with a branch to this stub. It preserves every other register and NZCV.
- A second descriptor at `0x3317ec` / `0x3317f8`, stored at `sp+0x58` by `0x3317fc` after the scene target's pool request at `0x3317e8`, is a fixed 1600x900 target (descriptor `+0x18` = 30) that the same pool (`0x134470`) serves next. It is not a fallback for the adaptive path, as an earlier note here said: both run every time. The manifest raises it to 2560x1440 with the scene.
- Both overrides occur at the allocation requests, after the dynamic-resolution calculation. The controller and its lifetime management still run; its output cannot lower these two requested sizes. This deliberately overrides the game's internal-resolution policy, as a requested enhancement. There is no skipped dynamic-resolution function, replacement frame limiter, scan-input cheat or SDK patch.
- The display framebuffer and HUD coordinate system remain the game's own. This implementation is internal supersampling into the original output, not a claim of 1440p output or a larger window. Scanning, input targeting and every post effect still require visual/gameplay checks.

[`runtime_patches.py`](runtime_patches.py) checks the full 32-byte NSO build ID, the rewriter layout, the new stub and all three original instruction sites against the rewritten image. Unknown builds, changed sites, missing stubs and unsupported layouts reject generation. The shared [runtime manifest loader](../../../docs/reference/rewriter.md#validated-mac-runtime-patch-manifests) validates the complete manifest before any write. Only default and experimental 1440p are accepted; a requested non-default mode without a manifest also rejects loading. The Mac packer carries the manifest; iOS signed code is not modified.

### Local allocation evidence

The first scripted run used only eight compiled boot stages, subsequently expanded to 44; its missing shaders make its picture unsuitable for visual validation. It nevertheless exercised the real game and Metal allocations. `runtime-test/run/route-1440.log` contains:

```text
[pass] F5580 O 21207   vp[0] = 0,0,2560,1440
[pass] F5580 P 21208 c0 ser=3292 fmt=92 2560x1440 ms1 lv0 sl0 load mtl=0x7987db8c80
[pass] F5580 P 21208 d ser=3734 fmt=260 2560x1440 load
```

`TEXINFO` also records 2560x1440 NVN allocations, including colour formats `0x38` / `0x3f` and depth/stencil `0x35`, with matching storage. This proves a change to internal colour/depth targets and scene viewport, independently of window scaling. The game-frame screenshot was still 1600x900. It does **not** establish correct shading, scanning, cutscenes or performance. The default comparison and the rerun using the complete captured subset are recorded separately below when complete.

### Reproduce and verify

All game-derived outputs stay under the NSP folder. The single reusable build is `<NSP folder>/010019A01E2F2000_build`; diagnostics and isolated test saves are under `<NSP folder>/010019A01E2F2000-work/runtime-test`. No installed app or original dump is changed. Merged extraction passed; the split's map, facts and pairs match the repository profile row for row. Only the stages requested by the route were compiled, into `shader/partial`, sharing `shader/cache/cas.db` with a future full build. The chain and shader inputs are unchanged. A partial library intentionally has no full-library stamp or c1/extent tables and logs that limitation.

1. Build the host from this worktree: `make -C dm/host GAME=010019A01E2F2000`. Run the targeted tests below before any expensive build.
2. Rewrite `main`, `subsdk0`, `sdk` from `extract/v131072/ExeFS` with `DM_GAME=010019A01E2F2000` and `dm/tools/patch_nso.py`, into `patched_v131072`. The existing NSP build's rewrite step now does this and generates `main.runtime.tsv` automatically. To regenerate just the manifest after rewriting:

   ```sh
   python3 dm/games/010019A01E2F2000/runtime_patches.py \
     "<build>/extract/v131072/ExeFS/main" \
     "<build>/patched_v131072/main.patched.bin" \
     "<build>/patched_v131072/main.runtime.tsv"
   ```

3. In an isolated test-run folder, with an empty test save directory and a temporary directory, run the same route twice, first with `MODE=default`, then with `MODE=1440p`. `MODE` below is a shell variable. Use the compatible partial cache already prepared; this command does not compile shaders:

   ```sh
   DM_INTERNAL_RESOLUTION="$MODE" \
   DM_ROMFS="<build>/extract/v131072/RomFS.img" \
   DM_MSL_DIR="<build>/shader/partial" \
   DM_NVN_SHMAP="<worktree>/dm/games/010019A01E2F2000/shader_map.tsv" \
   DM_SAVE="<test-run>/save" DM_TEMP="<test-run>/temp" \
   DM_PSO_PREBUILD=0 DM_METAL_HUD=1 DM_PRESENT_UPSCALE=off \
   DM_NVN_PASSDUMP=every:600 DM_NVN_TEXINFO=2560x1440 \
   DM_CTL="<test-run>/ctl.txt" \
   DM_KEYS='65:A~0.4,70:A~0.3,75:A~0.3,80:A~0.3,230:LUp~1,234:A~0.3' \
   "<worktree>/dm/host/dmhost" "<build>/patched_v131072" main subsdk0 sdk \
     > "<test-run>/log.txt" 2>&1
   ```

4. Let the run reach the opening/first area. Append `shot <absolute PNG path>` to `ctl.txt` to capture the game's own frame. Compare `P` colour and depth attachment dimensions and `O` viewport dimensions in **scene** frames; a screenshot's pixel size or a `present` line alone is insufficient. Confirm actual draws have shaders/pipelines and are not skipped. Expected 1440p scene attachments and viewport are 2560x1440; the original output may remain 1600x900.
5. Compare the same stationary scene, HUD proportions, depth/fog/bloom, shadows, transparency and cutscene framing. Exercise targeting/scanning and dynamic-resolution pressure, and check saving/loading and transitions. Missing-library lines invalidate visual conclusions. Capture frame-time/GPU distributions on an idle Mac with diagnostics off before claiming performance parity. Do not promote the setting to the launcher until these checks pass.

Automated checks: `make -C dm/host/tests runtime-patch` (29 C cases, also passed with AddressSanitizer/UBSan); `python3 -m unittest dm.tools.tests.test_runtime_patch dm.tools.tests.test_nro_pack dm.tools.tests.test_patches dm.tools.tests.test_docs`; `test_nsp_mac_app` and `test_nsp_chain`. Use the shared recent-Python tool environment with Capstone installed. The Mac host builds. Focused clang-tidy on the loader and parser test has no findings above the existing baseline. Python lint has the same unrelated `dmsh_entry.py` unused import and dependency-related baseline discrepancies on main and this branch; no new finding from these changes remains.

The regression harness was attempted with `check --games 010019A01E2F2000 --quick`, but refused discovery because this manually prepared partial build has no managed `tools/dmhost` Mac-build record. No harness comparison or cross-game gameplay claim is made. Its temporary source checkouts were removed. This change has one game-specific active path; other games without a runtime manifest take the no-op loader path, covered by focused tests. Full game shader compilation remains deferred.


Actual host rejection checks also passed for five cases before `nnMain`: unsupported `4K` selection, wrong build ID even with default selected, a `SIZE_MAX` offset, mismatched expected instructions, and a missing manifest with 1440p requested. The tests linked the existing rewritten binaries into a small diagnostic folder; no code or original module file was modified. Results: `<NSP folder>/010019A01E2F2000-work/runtime-test/rejections/results.json`.

Front-ends: new: Mac module bundles can contain `*.runtime.tsv`; preserve these files with the rewritten modules. Their presence is not evidence that a graphics choice is validated or should appear in the launcher.
