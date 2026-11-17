# Kirby's Dream Buffet (`0100A8E016236000`)

Base game 1.0.0 (v0), SDK 13.3.0, HAL Laboratory's engine and Basil scripts on NintendoWare, NVN. Modules `main`, `subsdk0` (NintendoSDK_movie) and `sdk`. Plays on the Mac through the intro, title and the standing-still hub scene. Races and iOS are untested.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | [`game.tsv`](game.tsv) | no system resource; the game sets a 3.14 GB heap in the 3.25 GB heap region |
| `modules main subsdk0 sdk` | `game.tsv` | the game demuxes its WebM with NintendoSDK_movie; the host decodes VP9 video and Vorbis audio |
| BaseSystem initialization before dependent Info setup | [`patches.py`](patches.py) | the game's parallel initialization caches a manager before the job that creates it finishes; see below |

The profile contains 4,503 shaders, all built for the Mac. No frame-step patch or shared-host behavior switch is needed.

## Intro transition: initialization dependency

**Resolved ([issue #1569](https://github.com/seedds/nxnative/issues/1569)).** Both skipping the intro and letting it finish crashed at `main+0x7f23ec`, address `0x8`, LR `main+0x7f14d8`. On an M4 Pro the original code failed after 7.6 s with `DM_KEYS=5:Plus`, and after 38.4 s without input. The font errors were separately resolved before these runs.

The fault is Basil's integer member-load instruction: `Scn.Step.Info.RewardGet.MasterImpl.OnUpdate()` reads `_gamePause.__bf_IsPause`, but `_gamePause` is null. Its `OnSetup()` caches `Scn.Step.Modules.GamePauseManager`. `Modules.Initialize()` queues `InitializeBaseSystem`, `InitializeInfo` and `InitializeDecor` in one parallel batch; BaseSystem creates the manager while Info calls the dependent UI setup. A targeted trace captured the null cache assignment at 6.9021 s on one worker, then the source manager's initialization at 6.9029 s on another, followed by the exact crash. Both workers belong to that same batch. The native queue claims tasks under its mutex and waits for every worker's completion LightEvent before returning; both events start unsignalled and clear automatically.

This is a dependency in the game's own code. The existing movie Stop/destructor fences do not order these jobs, and `DM_PREEMPT=1` still reproduced the crash. Shared decoder/event code is left alone. Logging before the source read can hide the race by changing timing; it is not a fix.

The function-matched game patch wraps the native `Parallel.Manager.Add` binding. After the original Add, only the exact `Scn.Step.Modules` / `InitializeBaseSystem` task invokes the game's own `ExecuteAndWait`, using its original VM-call context. The caller then queues Info and Decor for its existing parallel batch. Other classes and methods retain their original queue behavior. Complete-function fingerprints locate Add, ExecuteAndWait and the task runner that proves the instance/type-name layout; a missing or ambiguous function installs no patch. There are no fixed addresses or game checks in the host, no sleep, movie skip or null guard. The binding is **game**, at its loosest correct scope: other Basil games do not establish this initialization dependency.

The extra queue wait occurs during module initialization. The short name predicate also runs for other Add calls; its CPU cost is estimated negligible, while the change in scene-loading time from ordering the dependent jobs has not been measured.

For the inspected v0, native Add is `main+0x76b2f0`, ExecuteAndWait `main+0x76b3e8`. In `basil/ScnCommon.bin`, Modules.Initialize queues the three jobs at `0x3ae28`, `0x3ae34`, `0x3ae40` and waits at `0x3ae48`; BaseSystem assigns GamePauseManager at `0x3bb8c`; RewardGet.OnSetup caches it at `0x45faac`; OnUpdate reads it at `0x45fc74` and its IsPause field at `0x45fc7c`. These offsets describe this build; patch placement uses the reviewed function fingerprints.

## Mac routes

- Build the base with `scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> --package v0 --platform mac`; the last command runs it. The tested input folder contains only the base NSP.
- From an empty save, no input lets the intro finish; `DM_KEYS=5:Plus` skips it. Both routes must reach the scene after the movie without the null read.
- `A` at the title enters the hub. The tested route is `DM_KEYS=5:Plus,10:A`; it leaves Kirby standing on the starting plate, facing Main Menu.

## Testing

- 2026-11-17, Apple M4 Pro (Mac16,7): original natural and skip routes reproduce the same crash. An ARM64 instruction replay verifies the inserted dependency boundary, unchanged queuing of Info/Decor and unrelated tasks, exact class/method matching, original context arguments, aligned stacks and preserved callee-saved registers.
- The fixed game, with no probes: the skip route reaches the title and continues presenting through 30 s; natural completion reaches the same title and continues through 60 s. A visible HUD-enabled 180 s title run also returns from two further attract-movie cycles without the crash. Another visible run enters the hub with `A`; its screenshot shows Kirby standing on the starting plate. These runs verify the transition and initial scene, not a completed race.
- All 4,503 Mac shaders are ready; the NSP script builds from the registered profile. The registry/matcher tests and Python lint/type ratchet pass. Final review added the task-runner matcher precondition; rewriting the real module with it is byte-identical to the live-tested code.

- 2026-11-17, MacBook Pro M4 Pro, macOS 27.2.1: full `regress.py check` of the [shared audio output-route rework](../../../docs/reference/audio.md#stereo-only-on-a-51-system), against main `2925d7af`, PASS over the 60 s intro/title route with real Core Audio enabled and muted (`DM_AUDIO=1 DM_AUDIO_MUTE=1`). The six-input DeviceSink queues six channels and its original stereo fold together, and the built-in stereo AudioUnit selects the fold. Its 10,000 logged callbacks had 0 underruns and 0 zero padding in both revisions. The separate movie AudioOut stays two-channel. All 4,503 shaders built in both revisions, no changed MSL or new interface/red finding; 29.9 to 29.9 fps, frame p99 33.36 to 33.37 ms, peak 3,679 to 3,690 MB. Physical six-channel route changes and iOS remain untested.

## Open

- The supplied v65536 update fails to layer over this base with `ResultFsInvalidArgument (2002-6001)` in `OpenRawStorageWithPatch`; no update was tested. Its extraction failure also occurred in the original report and is separate from the base game's crash.
- The transition still logs a texture-type mismatch for shader `6927d8e65dd8ffe2_b`, slot 1. The captured title and hub look correct; other uses of that shader are unchecked. We should leave it for this fix.
- iOS, multiplayer and later gameplay are untested.
