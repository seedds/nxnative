# Mega Man Legacy Collection (`01002D4007AE0000`)

Update 1.0.3 (v196608), SDK 3.7.0. Digital Eclipse's collection of the six NES Mega Man games, played on its own NES emulation in `main`. It renders with **OpenGL**, not NVN: NVIDIA's desktop GL through EGL, which the host implements on Metal ([OpenGL layer](../../../docs/reference/rendering.md#opengl-layer)); `subsdk0` (NVIDIA's GL driver) is not loaded.

On the Mac (M6, macOS 27.2): the photosensitivity warning, the title, the collection menu, Mega Man 1's title and stage select, and play in Cut Man's stage (running, jumping, shooting), with sound, at 60 fps (300 presents per 300 display ticks; 2 to 6 empty ticks per 300 with the TV filter on); the in-game Options/Controls menu and its TV filter (scanlines and persistence, the CRT and composite shaders). About 0.8 GB footprint and 0.17 GB of Metal allocations. Played for about 5 minutes in all: no other game of the six, no boss, no save or load yet. Not run on an iPhone.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `modules main sdk` | `game.tsv` | `subsdk0` is NVIDIA's OpenGL driver, which programs the GPU through NvRm: never loaded, the GL layer answers `main`'s EGL and GL calls |
| `sys_resource 0x0`; `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | `game.tsv` | no system resource, so no virtual address memory; `main` sets a heap of at most the pool − 64 MB (3,328 MB) |
| No NVN shader tables | `game.tsv` `shader_note` | no NVN shaders: `gl_shaders.py` writes the GL library and its table, not the profile |

No rewriter cut, build-keyed host row, capture table or profile switch: everything below is general (host, GL layer and shader tools).

## Audio

- **Resolved: a stop before the first frame,** `FATAL AudioOut sample rate 44100 not supported (48 kHz only)`. `main+0x9a3c8` opens its AudioOut at 44.1 kHz; when that fails, `main+0x9a3cc` opens it again with a zeroed parameter (the default rate) and reads the output's properties back for its buffer sizes. The host now refuses the rate as audout does (0x699), and the game plays at 48 kHz: 0 underruns over 14,000 device callbacks of 240 frames. [An AudioOut opened at a rate other than 48 kHz](../../../docs/reference/audio.md#an-audioout-opened-at-a-rate-other-than-48-khz).

## Rendering (OpenGL)

- **Resolved: no draws.** Its shaders are six GLSL ES 3.20 / 3.10 files in `shaders/nx/` (`default`, `crt` and `composite`, `_vs` and `_ps`; `default_vs` and `composite_vs` have the same text) and two strings in `main`; the NSP script read only the strings ([GLSL source files named `.glsl`](../../../docs/reference/rendering.md#glsl-source-files-named-glsl)). A capture run compiled the same six files, byte for byte.
- **Resolved:** three of the files end `main` with `};` ([A function body closed with `};`](../../../docs/reference/rendering.md#a-function-body-closed-with-)); `default_ps.glsl` declares a sampler it never reads ([A sampler the shader never reads](../../../docs/reference/rendering.md#a-sampler-the-shader-never-reads)).
- **Resolved:** it samples through sampler objects, one per unit ([GL sampler objects](../../../docs/reference/rendering.md#gl-sampler-objects)).
- **Resolved: pictures upside down.** It skips setting `u_flip` when the uniform's location is 0 (`main+0x77b28`) ([No uniform at location 0](../../../docs/reference/rendering.md#no-uniform-at-location-0)).
- `eglBindAPI(EGL_OPENGL_ES_API)` logs a red line once at boot; the context is desktop GL, as the game's GLSL ES text compiles on it.

## Services

- `time::StandardSteadyClock::now()` returned 0 on every call (six call sites in `main`), now the host's monotonic seconds ([Steady clock, clock snapshots and user clocks](../../../docs/reference/services.md#steady-clock-clock-snapshots-and-user-clocks)). Nothing visible was shown to change.

## Mac routes

Work folder `<NSP folder>/01002D4007AE0000-work`: `host.sh` builds the host from a worktree into `mmhost` (`REPO=`), `boot.sh <run> [seconds] [VAR=value …]` boots with window, HUD and sound (`SAVE=<dir>` copies a save in, `MMHOST=` another host), `ctl.sh <run> <line> …` drives it through `DM_CTL` (`shot <name>`, `wait <s>`).

- From a fresh save: the warning and the logos take about 24 s; A at the title, A Games (Mega Man 1 is first), A Play, A at Mega Man 1's title, Plus at the stage select picks Cut Man (top left).
- Minus in a game opens Options/Controls (Save Game, Screen, Border, Filter, CPU Speed …); the d-pad moves, Right changes an option, B goes back to the game.
- Play: the d-pad moves, A jumps, B shoots.

## Found on this game

General changes first needed here; each applies to every game:

- **An AudioOut refused at another rate than 48 kHz**: [An AudioOut opened at a rate other than 48 kHz](../../../docs/reference/audio.md#an-audioout-opened-at-a-rate-other-than-48-khz).
- **`.glsl` files read by `gl_shaders.py` and `profile_guess.py`**: [GLSL source files named `.glsl`](../../../docs/reference/rendering.md#glsl-source-files-named-glsl).
- **`};` after a function body**: [A function body closed with `};`](../../../docs/reference/rendering.md#a-function-body-closed-with-).
- **Only the samplers the MSL takes in the table**: [A sampler the shader never reads](../../../docs/reference/rendering.md#a-sampler-the-shader-never-reads).
- **GL sampler objects**: [GL sampler objects](../../../docs/reference/rendering.md#gl-sampler-objects).
- **No uniform at location 0**: [No uniform at location 0](../../../docs/reference/rendering.md#no-uniform-at-location-0).
- **`StandardSteadyClock::now()`**: [Steady clock, clock snapshots and user clocks](../../../docs/reference/services.md#steady-clock-clock-snapshots-and-user-clocks).

PRs [#2723](https://github.com/seedds/nxnative/pull/2723) to [#2729](https://github.com/seedds/nxnative/pull/2729) reported this game first; their findings were checked against the game here and implemented anew (no code from them). Two differ: the AudioOut rate is refused as audout does, not accepted (#2723), and the shaders are RomFS files the tools now read, not shaders put together at run time that need a capture key (#2725).

## Open

- The other five games, bosses, saving and loading, the Challenges, Museum and Music Player are not played yet.
- iPhone: not run. The GL layer is the same.
