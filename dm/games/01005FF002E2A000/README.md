# Rayman Legends: Definitive Edition (`01005FF002E2A000`)

Base game with update 1.2, SDK 1.3.1, the oldest SDK in the games table. Ubisoft's UbiArt Framework on NVN, with Pia 5.2.10 for its network play. Modules `main`, `subsdk0` (the SDK's NVN), `subsdk1` (`nn::sf`), `subsdk2` (libcurl), `subsdk3` (the SDK's movie library) and `sdk`. The profile is the NSP script's provisional one, unchanged apart from its names. The game needed three general changes, all for its SDK or its engine: the [audio renderer](#audio), the [Opus decoder](#audio) and its [shader archive](#files-and-shaders).

On the Mac (2026-11-06, M1, 16 GB, macOS 26.7), from a fresh save: the Ubisoft logo movie, the leaderboard country, the title, New game, the intro movie, the in-engine prologue and first control in the first level, running and jumping, with music and sound effects; a 5-minute run with no crash, no missing shader and one `[RED]` line ([Rendering](#rendering)). An earlier build of the same changes also reached the Main Gallery and the first world's gallery ("Teensies in Trouble"). The title runs at 54 to 58 fps, the first level at 42 to 45 (GPU 10 to 30 ms a frame at 1080p: the M1's GPU is the limit; CPU about 25% of one core); the footprint is 1.5 to 1.9 GB. The IPA is not built.

## What it needs

| Setting | Where | Why |
|---|---|---|
| 3.25 GB layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | [`game.tsv`](game.tsv) | the provisional layout, which holds: `main` sets a heap of at most the pool − 64 MB (3.14 GB) |
| `sys_resource 0x0` | `game.tsv` | the NPDM declares 0 MB of system resource: `IsVirtualAddressMemoryEnabled` is false, as on Horizon |
| `modules main subsdk0 subsdk1 subsdk2 subsdk3 sdk` | `game.tsv` | every module `main` imports from; `subsdk3`'s movie library plays the intro and the attract movies (`video/*.mp4`) |
| `shader_groups /video/=video /enginedata/=enginedata` | `game.tsv` | the shader split's groups; every shader is in `bundle_nn.ipk`'s `enginedata/` |
| no `pad_layout` | `game.tsv` | a 2D platformer: the default 2D layout |

## Audio

- **Resolved: the host stopped at boot, `[RED] sf: the game connects to service audren:u itself`.** SDK 1 has no audio renderer in the process: the host renders itself, after the patch in [issue #960](https://github.com/seedds/nxnative/issues/960) ([SDK 1: no audio renderer in the process](../../../docs/reference/audio.md#sdk-1-no-audio-renderer-in-the-process)). The game opens one renderer at 48 kHz, with 2 voices, 2 mix buffers and no effects, for its music and sound effects. Each movie plays its AAC sound through an `AudioOut` of its own.
- **Resolved: the host stopped about 4 s in, `[RED] sf: the game connects to service hwopus itself`.** The game calls `HardwareOpusDecoder`'s members by name ([SDK 1 calls the `HardwareOpusDecoder` members directly](../../../docs/reference/audio.md#sdk-1-calls-the-hardwareopusdecoder-members-directly)).
- **Current:** sound from the first title screen on: over the first minute, 7,566 of 12,000 renderer frames carry sound (the logo movie's sound goes through its `AudioOut`). The SDK 1 renderer's missing effects (biquad, reverb, delay) do not concern this game, which opens none.

## Files and shaders

- RomFS: 17 files, 2.90 GB. Seven in the root (`bundle_nn.ipk` 2.6 GB and two smaller IPK archives, `.gf`, `.metapreload`, `.preres`, `.txt`), nine movies in `video/` (0.27 GB), one localisation file in `enginedata/localisation/`.
- **Resolved: a black screen after boot, 27 programs `not in the shader map`.** The shaders are whole GLSLC outputs in the engine's `.fxb` effect files inside `bundle_nn.ipk` (zlib entries), where the split did not look: it found 8, all in `main` ([GLSLC outputs in UbiArt IPK archives](../../../docs/reference/shaders.md#glslc-outputs-in-ubiart-ipk-archives)).
- **Current:** 5,893 shaders (pixel 5,276, vertex 617) from `enginedata/shaders/compiled/nx/` (`renderpct`, `afterfx`, `font`, `movie`), all translated for the Mac. `enginedata/shaders_dev/` holds a development build of the same effects, which the game does not ask for, and is left out.
- **Open:** the prewarm list (`pso_manifest.tsv`) is empty.

## Rendering

- **Current:** `[RED] NVN address lookup of an unknown name: nvnDeviceWaitForError`, twice at boot: SDK 1's NVN asks for a function the host does not have, and the game runs on without it.
- **Open:** the frame rate on the M1 ([On the Mac](#on-the-mac)): GPU time per pass has not been looked at, nor a faster Mac.

## Input

- B jumps; jumping into a painting of a gallery enters its world or level. A confirms in the menus and wakes the heroes at the end of the prologue.
- **Open:** with the earlier build, skipping the intro movie (Plus, then A) once left a grey screen that took no input; letting it play to the end, or choosing Continue on the title, always worked.

## On the Mac

- After the NSP script's `--platform mac` build, its printed command runs the game. The game pauses when its window loses focus, as it does on the Switch when it goes to the background: `DM_FOCUS_KEEP=1` keeps it running for a host started from a background shell, and `DM_WINDOW_TOP=1` keeps a covered window from slowing it (the title ran at about 48 fps covered).
- `DM_SAVE` must be on an APFS volume (the host says so in a red line otherwise).
- From a fresh save: A on the title, A for the leaderboard country, A on New game; the intro movie (about a minute) and the prologue play by themselves, then A wakes the heroes and the first level starts.

## Found on this game

- **SDK 1's audio renderer** in the host (`hle_audren.c`): [SDK 1: no audio renderer in the process](../../../docs/reference/audio.md#sdk-1-no-audio-renderer-in-the-process).
- **SDK 1's `HardwareOpusDecoder` members** (`hle_codec.c`): [SDK 1 calls the `HardwareOpusDecoder` members directly](../../../docs/reference/audio.md#sdk-1-calls-the-hardwareopusdecoder-members-directly).
- **UbiArt IPK archives** in the shader split (`shader_split.py`): [GLSLC outputs in UbiArt IPK archives](../../../docs/reference/shaders.md#glslc-outputs-in-ubiart-ipk-archives).

## Open

- The frame rate on the M1 ([Rendering](#rendering)).
- iPhone: no IPA built yet.
- The grey screen after skipping the intro movie ([Input](#input)).
- Only the start was played: the other worlds, the four-player mode, the Murfy touch levels and the online challenges are untested.
