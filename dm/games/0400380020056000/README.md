# Splatoon Raiders (`0400380020056000`)

A Switch 2 game: update v65536 over the base game, SDK 21.4.4 built for the Switch 2 (`Ounce`), NVN2 API 231.39. Nintendo EPD's engine (sead / agl / gsys, Phive, Havok), modules `main`, `subsdk0` and `sdk`. **Plays on the Mac** (M4 Pro): the opening, character creation, the first level's beach, inking, grates and the first Salmonids, about 4.5 minutes in the last session with no crash. No IPA has been built.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `mem_alias 0x400000000`, `mem_reserve 0x550000000` | [`game.tsv`](game.tsv) | with the 4.5 GB of an earlier profile the region ran out at the first level: [An SDK 21 Switch 2 game with a 4.5 GB alias region](../../../docs/reference/memory.md#an-sdk-21-switch-2-game-with-a-45-gb-alias-region) |
| `mem_stack 0x10000000`, `app_pool 0x240000000` | `game.tsv` | as Donkey Kong Bananza, the Switch 2's 9 GB |
| `sys_resource 0x2000000` | `game.tsv` | the NPDM declares 32 MB, so virtual address memory is on |
| `render_cond on` | `game.tsv` | agl wraps its clears in render conditions; when those clears ran, the lit scene was wiped: the black dialogue scene ([Report tables that dropped entries](../../../docs/reference/rendering.md#report-tables-that-dropped-entries)) |
| `pad_layout full` | `game.tsv` | the game asks for most of the controller at once |

## Files

- Built from a dump folder (`base/`, `updates/65536/`). The display version `v65536` is a placeholder: no control data gave a better one.
- Update RomFS: 46,354 files, 13 GB.
- Shaders: 27,195 in [`shader_map.tsv`](shader_map.tsv), from `Shader/*.Nin_Ounce_NVN2.bfsha.zs`, `Sample.Nin_Ounce_NVN2.release.sarc.zs` and `Lib/agl/*`. A full build takes about 34 minutes from scratch, 13 with the Metal cache. 22,147 pipeline pairs prebuild with none failing, in about 200 s.

## Shaders

27,155 of 27,195 build. The 40 that do not (current): 12 compute shaders on instruction 0x2be, 10 on 0x301, 8 on 0xf8c, 6 on 0x38c, 1 on 0x481, one vertex shader on 0x927 (NigoProc), and 2 compute shaders of `agl_technique_pfx` that Metal refuses (`bracket nesting level exceeded`). The game asks for one of them at boot, `fa072659664b2d38` in `agl_common`, and the dispatch is skipped, with no effect seen so far.

## Rendering

- The first level renders at 1707x960 (R11G11B10F), with what the pass inputs suggest is a spatial anti-aliasing step, and one compute program of agl's upscaler (`c1b8edc09e3313b1`, one texture in, no earlier frame) takes it to 2560x1440; that is copied to the 3840x2160 window. The dialogue scene renders at 2560x1440.
- agl's `agl_ndsr.pck` is the same file in the debug and the release library (header word `0x40100`). Its 20 compute variants read colour that is encoded already and come in twos: no decode at the end, or a half-float sRGB decode clamped to 0-1. All past logs name only the 10 that decode; the first level dispatches `c1b8edc09e3313b1` (1707x960 → 2560x1440) and `30d8ee33de9dee85`. With `DM_UPSCALER=metalfx` its GPU p50 was 8.05 ms against 9.1 ms with the network on the beach (M4 Pro, windows of 300 frames, a loaded Mac; the frame rate stays 60 either way), and the picture matches ([An upscaler's colour read from its code](../../../docs/reference/rendering.md#an-upscalers-colour-read-from-its-code)).
- Ink is a 6400x6400 RG8 map with a Depth32F-S8 (880x880 in the dialogue scene). A stamp writes depth and stencil in its rectangle, then paints with a barrier before each draw, sampling the map; then about 150-285 occlusion queries a frame measure the map, each its own `SetRenderTargets`, uniform updates and report. These drew the costs on this page.
- Frame and GPU time, M4 Pro, windows of 300 frames: the still beach about 15 ms of GPU at 60 fps (150-190 ms, about 6 fps, before the fixes below). The last play session: a median frame of 16.6-16.9 ms; the slowest 5 % at 17.1-17.5 ms in its last stretch and up to 22 ms in the heavier moments; GPU median 10-16 ms, slowest 5 % 13-22 ms. So 60 fps is not held everywhere (current), and that session shared the Mac with a regression run. The ink map's passes are a large part in inked scenes: 0.32 ms each ([A render area for passes on very large targets](../../../docs/reference/rendering.md#a-render-area-for-passes-on-very-large-targets)). Each stamp also took a whole-map snapshot (0.76 ms) and a pass of its own; it now reads its pixel of the map from the tile (resolved, [#1063](https://github.com/seedds/nxnative/issues/1063)): on the route to the first beach, 4.2 → 0.6 snapshots a frame and GPU p50 12.8 → 9.5 ms, M4 Pro. Inked stages are not measured here (the reporter's M2: 22–26 → 13–16 ms with their first patch). A few single frames take 130-535 ms; the worst show little submit time and no new pipelines, and one of 373 ms is nearly all submit time; not traced (open).
- Focus: the game stops drawing its 3D scene when the window loses focus (10 passes, GPU 2 ms), so a frame rate read then says nothing.

## Driving it on the Mac

The work folder's `run/drive.sh <name> <secs> "<sec> <control line>"…` boots in a window with the HUD, raises it and appends control lines (`A~0.1`, `LUp~4`, `shot <path>`, `passdump 1`, `texdump <path with %d>`). With the save at the start of the dialogue, four presses of A get to the beach at about 75 s. `DM_NVN_SHOT=<frames>:<relative path>` takes consecutive frames: the path must have no space, as the driver's environment is split on them.

## Open

- `texture type mismatch: shader d0ce73e71074df85_b (stage 1) declares slot 0 as Metal type 6, texture ID 428 is type 5`, once.
- An IPA: the 16 GB alias region is over iOS's limit until the SDK 21 cuts are ported.
- A crash dump once hung in `flockfile` under `shader_load`, with the log contended.
- `dm/games/profile_guess.py` does not recognise `Generic_21_4_4`. `pl::IsRunningOnOunce` answers false.

## Found on this game

- [Storage words read one by one](../../../docs/reference/shaders.md#storage-words-read-one-by-one): the lit pass on an M1, 70 → 16 ms.
- [A blocked audio device start freezes the game](../../../docs/reference/audio.md#a-blocked-audio-device-start-freezes-the-game) when the selected HDMI device refuses to play.

- [Goto elimination forcing loops around a whole program](../../../docs/reference/shaders.md#goto-elimination-forcing-loops-around-a-whole-program): the first shader build stalled for 5 h 40.
- [Ampere jump tables read past their end](../../../docs/reference/shaders.md#ampere-jump-tables-read-past-their-end).
- [MUFU.F16](../../../docs/reference/shaders.md#mufuf16) and [The NDSR pack's record start](../../../docs/reference/shaders.md#the-ndsr-packs-record-start): the black 3D scene in the opening.
- [The Switch 2's device values](../../../docs/reference/nvn-objects.md#the-switch-2s-device-values): the NVN versions read from the game's `sdk`.
- [Built-in fonts and the audio device notification of SDK 21](../../../docs/reference/services.md#built-in-fonts-and-the-audio-device-notification-of-sdk-21).
- [An SDK 21 Switch 2 game with a 4.5 GB alias region](../../../docs/reference/memory.md#an-sdk-21-switch-2-game-with-a-45-gb-alias-region).
- [Report tables that dropped entries](../../../docs/reference/rendering.md#report-tables-that-dropped-entries): the black dialogue scene.
- [Query slots running out inside a pass](../../../docs/reference/rendering.md#query-slots-running-out-inside-a-pass).
- [Setting the same targets again continues the pass](../../../docs/reference/rendering.md#setting-the-same-targets-again-continues-the-pass), [Inline updates of a running submission refused uniform shadows](../../../docs/reference/rendering.md#inline-updates-of-a-running-submission-refused-uniform-shadows) and [A render area for passes on very large targets](../../../docs/reference/rendering.md#a-render-area-for-passes-on-very-large-targets): about 6 fps in the first level, and the water's grey blocks on the way.
- [A store only a dispatch's first invocation makes](../../../docs/reference/shaders.md#a-store-only-a-dispatchs-first-invocation-makes): the flickering plants.
- [Same-pass feedback read from the tile](../../../docs/reference/rendering.md#same-pass-feedback-read-from-the-tile): each ink stamp's one-draw pass and whole-map snapshot ([#1063](https://github.com/seedds/nxnative/issues/1063)).
- [An upscaler's colour read from its code](../../../docs/reference/rendering.md#an-upscalers-colour-read-from-its-code): the washed-out picture with `DM_UPSCALER=metalfx` ([#1156](https://github.com/seedds/nxnative/issues/1156)).
