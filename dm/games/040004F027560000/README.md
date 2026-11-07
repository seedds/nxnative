# Orbitals (`040004F027560000`)

A Switch 2 game: v327680, SDK 21.4.2 built for the Switch 2 (`Ounce`), NVN2 (Ampere shaders). Unreal Engine 5.6.1 with Niagara particles and GPUScene instance culling, Wwise 2024.1, Bink movies. Modules `main`, `subsdk0` (movies) and `sdk`. A co-op game for two players, played split-screen. **Plays on the Mac**: from the title through the lobby, the character select and the intro cutscene into the first area and stage 2, both players driven, at the game's own 30 fps. No IPA has been built.

## What it needs

SDK 21's audio, which came with Star Fox (#1259): Wwise 2024.1 uses SDK 21's queued `HardwareOpusDecoderManager` and `nn::audio` OutputStream ([SDK 21's hardware Opus decoder manager](../../../docs/reference/audio.md#sdk-21s-hardware-opus-decoder-manager), [SDK 21's OutputStream on a virtual output device](../../../docs/reference/audio.md#sdk-21s-outputstream-on-a-virtual-output-device)). Without them the game aborts at boot (`nn::diag abort: Result 0xe15`, sdk+0x322660). With them it plays to stage 2 with sound: 0 underruns after the first seconds.


| Setting | Where | Why |
|---|---|---|
| `mem_alias 0x1000000000`, `mem_reserve 0x1028000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | [`game.tsv`](game.tsv) | Unreal's 62 GB address region in `nninitStartup` and FMallocBinned3's 37 GB of it: a 64 GB alias region on the Mac ([memory](../../../docs/reference/memory.md)) |
| `sys_resource 0x1400000` | `game.tsv` | the NPDM declares 20 MB, so virtual address memory is on |
| `zip_store .bk2:.pak:.ttf:.txt:.ucas:.utoc:.metallib` | `game.tsv` | the RomFS's files are compressed already |
| `shader_groups ShaderArchive-Global-=global ShaderArchive-=game` | `game.tsv` | the engine's global shaders and the game's material shaders |
| `draw_barriers game` | `game.tsv` | Unreal orders its own passes with NVN barriers; full barriers serialised about 54 full-screen passes a frame (15 -> 18 fps at the character select) |
| `enc_thread on` | `game.tsv` | the 3D scenes draw about 10,000 times a frame |
| `fps_target 30` | `game.tsv` | the game's own rate, which the Switch 2 holds; see [Frame rate](#frame-rate) |

## Files

- A dump folder (`exefs/`, `romfs/`, no NACP, so the app takes its name from the profile): a RomFS of 9.86 GB, 53 files. `Orbitals/Content/Paks/Orbitals-Switch2.{utoc,ucas}` (the 6.8 GB IoStore container), `Orbitals-Switch2.pak`, `global.{utoc,ucas}`, 35 Bink movies.
- `main` build `84dc546cd0896d09582f02293c4edbce`, `sdk` `daa31832ca979cdfbe07b3b09488d8e0`, `subsdk0` `caf9437bacb2aa161d837d5886f19ae8`.

## Shaders

- 23,432 in [`shader_map.tsv`](shader_map.tsv) (3,415 compute, 3,556 vertex, 16,459 pixel, 2 geometry), from the two IoStore shader libraries `ShaderArchive-{Global,Orbitals}-SF_GLSL_SWITCH2-SP_GLSL_SWITCH2` (Oodle). All 23,432 translate (current; checked with this tree's translator on 2026-11-05).
- Shader keys map to Unreal shader names: a key's `#UE<n>` in `shader_map.tsv` is line n of the archive's `.usf|` strings. For example `a958e514cc3f4431` is `FBuildInstanceIdBufferAndCommandsFromPrimitiveIdsCs`, `d032ce5090303e01` the GPU radix sort's downsweep, `e97d65cf4844ecdb` the player ship's base pass (`M_Cel_Base_PlayerShip`), `2dab9fff04ddd58a` the homing missile's thruster mesh.
- The prewarm seed list, [`pso_manifest.tsv`](pso_manifest.tsv) (3,078 lines: 1,344 shaders, 1,523 render pipelines, 211 compute pipelines), is the Mac's device list of the play sessions of 2026-11-04.

## Rendering

- Frame time in stage 2 with both players flying and firing: 33.3 ms at 30 fps, GPU about 21 ms, submit and present about 10 ms (M4 Pro, 2026-11-04 and 2026-11-05).
- What the Switch 2 shows was checked against a published co-op walkthrough (YouTube `-a5HMPJueu8`, chapter 2 at 944 s): the stage 2 start matches, with the vertical light columns on the right.
- One `[RED]` line a run: `vertex attribute format 0x27 (8 / 8-bit integer) feeds a float input`, converted on the CPU, with no visible effect.

## Frame rate

- **Current:** the game's own 30 fps. Its config sets `t.MaxFPS 30` and `rhi.SyncInterval 2`. The Switch 2 holds an even 30 here: in a 60 fps capture of it, 173 of 182 frames are 33.3 ms apart.
- On the Mac at 30: 1,198 of 1,199 presents are two refreshes apart, and the power line reads 30.0 fps.
- 60 is possible: the rewriter finds both variables by name (`t.MaxFPS` at `main+0x9cd50d0`, `rhi.SyncInterval` at `main+0x9b87290`, in main's fixups) and `hle_fpslock.c` writes 60 and interval 1. Unlocked, the M4 Pro gave 55-59 fps with missed refreshes and the game ran at 0.92 of real time, so the profile keeps 30.

## Driving it on the Mac

Both players are needed: `DM_CTL` lines `p2:<keys>` drive player 2. Pressing `A` then `p2:A` every 0.7 s from 3 s goes through the logos, the title, the menu (PLAY LOCAL), the lobby (each player joins with A; START is B) and CONTINUE; gameplay starts about 60 s after boot from a save at stage 2. The character select takes Left, A, `p2:Right`, `p2:A`. In the intro cutscene both players hold B at the same time to skip it. Set `DM_FOCUS_KEEP=1` for scripted runs.

## Open

- The engagement scene in stage 2 is darker than on the Switch 2: the asteroids and the drone show as silhouettes.
- The game reports itself as a trial (Friend's Pass); the local co-op path is not gated by it.
- An IPA: the 64 GB alias region is over iOS's limit.
- Not checked: game speed at 60 over a fixed route, memory over a long session.

## Found on this game

- [Unreal Engine 5.6 Switch 2 shader records](../../../docs/reference/shaders.md#unreal-engine-56-switch-2-shader-records): the shader split.
- [VOTEU writes the uniform predicate](../../../docs/reference/shaders.md#voteu-writes-the-uniform-predicate): flat purple and cyan blobs at the ships (the thruster meshes read the wrong particles).
- [Warp-synchronous shared memory without a barrier](../../../docs/reference/shaders.md#warp-synchronous-shared-memory-without-a-barrier): broken Niagara beams and one-frame view flashes.
- [Integer bits in a float colour output in a specialized Metal 4 pipeline](../../../docs/reference/rendering.md#integer-bits-in-a-float-colour-output-in-a-specialized-metal-4-pipeline): the player ship dark while engaging, loading dark then popping in.
- [32-bit integer buffer and linear textures re-created every frame](../../../docs/reference/rendering.md#32-bit-integer-buffer-and-linear-textures-re-created-every-frame): a texture upgrade every frame.
- [A rescue's display link is made only once the window is visible](../../../docs/reference/frame-rate.md#a-rescues-display-link-is-made-only-once-the-window-is-visible).
- [Ampere instructions of Unreal Engine 5.6's Switch 2 shaders](../../../docs/reference/shaders.md#ampere-instructions-of-unreal-engine-56s-switch-2-shaders): 19,720 of 23,432 shaders left out at first; black backgrounds.
- [A scalar storage-buffer load that lost its index](../../../docs/reference/shaders.md#a-scalar-storage-buffer-load-that-lost-its-index): the title's black 3D background.
- [Compute threads that return before a barrier](../../../docs/reference/shaders.md#compute-threads-that-return-before-a-barrier): GPU hangs.
- [NVN2 compute `c0` with two push blocks](../../../docs/reference/shaders.md#nvn2-compute-c0-with-two-push-blocks).
- [The GPU watchdog and GPU-written dispatch counts](../../../docs/reference/nvn-objects.md#the-gpu-watchdog-and-gpu-written-dispatch-counts).
- [More controllers than the controller applet's maximum](../../../docs/reference/services.md#more-controllers-than-the-controller-applets-maximum): the lobby reopened the applet forever.
- [Unreal Engine 5.6's Switch 2 platform layer at start](../../../docs/reference/services.md#unreal-engine-56s-switch-2-platform-layer-at-start): the abort after `nn::chat::Initialize`. Its three frames then a wait in `FFrameEndSync` are main's [Layer frame history](../../../docs/reference/services.md#layer-frame-history).
