# Orbitals (`040004F027560000`)

A Switch 2 game: v327680, SDK 21.4.2 built for the Switch 2 (`Ounce`), NVN2 (Ampere shaders). Unreal Engine 5.6.1 with Niagara particles and GPUScene instance culling, Wwise 2024.1, Bink movies. Modules `main`, `subsdk0` (movies) and `sdk`. A co-op game for two players, played split-screen. **Plays on the Mac**: from the title through the lobby, the character select and the intro cutscene into the first area and stage 2, both players driven, at the game's own 30 fps. No IPA has been built.

## What it needs

On main as it is, the shader split stops at the first shader (`shader 0 is not an LSLG record`): Unreal Engine 5.6's Switch 2 shader code archives hold NVN2 records. The pull requests it needs:

- the Switch 2 shader code archives read with `uepak.sage_record` (the split; without it there is no library);
- PR #2406, pipelines with integer bits in float colour outputs built whole (without it the player ship goes dark while engaging);
- PR #2404, 32-bit integer buffer and linear textures with the format-view usage at creation (about 8,000 texture upgrades a session without it, and colour flashes in one view).

The rest of what the first port needed is on main: SDK 21's audio from Star Fox (without it Wwise aborts at boot with `Result 0xe15`), [GameChat at boot](../../../docs/reference/services.md#gamechat-at-boot), the [layer frame history](../../../docs/reference/services.md#layer-frame-history) Unreal's vsync thread waits on, [NVN2 compute's second push block](../../../docs/reference/shaders.md#nvn2-computes-second-push-block-holds-buffer-entries), [more than 16 vertex attributes](../../../docs/reference/rendering.md#more-than-16-vertex-attributes) (the game binds 17), [threadgroup stores in warp-synchronous code](../../../docs/reference/shaders.md#threadgroup-stores-in-warp-synchronous-code), [VOTEU's uniform predicate](../../../docs/reference/shaders.md#voteu-writes-the-uniform-predicate), the controller applet's [maximum](../../../docs/reference/services.md#the-controller-applet-leaves-out-the-players-past-the-maximum), and the Ampere forms of Yoshi and the Mysterious Book, PRAGMATA and Pokémon Pokopia.

| Setting | Where | Why |
|---|---|---|
| `mem_alias 0x1000000000`, `mem_reserve 0x1028000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | [`game.tsv`](game.tsv) | Unreal's 62 GB address region in `nninitStartup` and FMallocBinned3's 37 GB of it: a 64 GB alias region on the Mac ([memory](../../../docs/reference/memory.md)) |
| `sys_resource 0x1400000` | `game.tsv` | the NPDM declares 20 MB, so virtual address memory is on |
| `zip_store .bk2:.pak:.ttf:.txt:.ucas:.utoc:.metallib` | `game.tsv` | the RomFS's files are compressed already |
| `shader_groups ShaderArchive-Global-=global ShaderArchive-=game` | `game.tsv` | the engine's global shaders and the game's material shaders |
| `draw_barriers game` | `game.tsv` | Unreal orders its own passes with NVN barriers; full barriers serialised about 54 full-screen passes a frame (15 -> 18 fps at the character select) |
| `enc_thread on` | `game.tsv` | the 3D scenes draw about 10,000 times a frame |
| `fps_target 30` | `game.tsv` | the game's own rate, which the Switch 2 holds; see [Frame rate](#frame-rate) |

There is no game-specific patch and no rewriter cut.

## Files

- A dump folder (`exefs/`, `romfs/`, no NACP, so the app takes its name from the profile): a RomFS of 9.86 GB, 53 files. `Orbitals/Content/Paks/Orbitals-Switch2.{utoc,ucas}` (the 6.8 GB IoStore container), `Orbitals-Switch2.pak`, `global.{utoc,ucas}`, 35 Bink movies.
- `main` build `84dc546cd0896d09582f02293c4edbce`, `sdk` `daa31832ca979cdfbe07b3b09488d8e0`, `subsdk0` `caf9437bacb2aa161d837d5886f19ae8`.
- PR #2369 reports the same build (v327680, the same 23,432 unique shaders) as Orbitals Deluxe Edition, `040032B023CA4000`, which has no profile here.

## Shaders

- 23,432 in [`shader_map.tsv`](shader_map.tsv) (3,415 compute, 3,556 vertex, 16,459 pixel, 2 geometry), from the two shader code archives `ShaderArchive-{Global,Orbitals}-SF_GLSL_SWITCH2-SP_GLSL_SWITCH2` (40,995 shaders, Oodle, in the IoStore container). With the archive reading above, main's split gives the four tables here byte for byte.
- Main's translator: a random sample of 504 keys (353 pixel, 82 vertex, 69 compute, among them the four named below) all build, nothing lost; the only `[RED]` lines are 6 vertex shaders with an input at attribute 16 or above (no pulling or shadow-input library). PR #2369 reports 23,420 of the 23,432 built on main of 2026-11-11, with 13 bindless reads deleted; which ones has not been checked here.
- Shader keys map to Unreal shader names: a key's `#UE<n>` in `shader_map.tsv` is line n of the archive's `.usf|` strings. For example `a958e514cc3f4431` is `FBuildInstanceIdBufferAndCommandsFromPrimitiveIdsCs`, `d032ce5090303e01` the GPU radix sort's downsweep, `e97d65cf4844ecdb` the player ship's base pass (`M_Cel_Base_PlayerShip`), `2dab9fff04ddd58a` the homing missile's thruster mesh.
- The GPUScene upload shaders `f0db173d46f9cdc5` and `da5435fda42d9fb1` read `PrimitiveUploadIndices[PrimitiveIndex]`; an earlier translator loaded it at element 0, so every primitive went to one slot (black 3D backgrounds). Main's translator keeps the index.
- [`pso_manifest.tsv`](pso_manifest.tsv) is empty until it is recorded on this chain.

## Rendering

Measured on an M4 Pro, macOS 26, on our branch (main of 2026-11-12 with the pull requests above and others), a 215 s run from boot with both players into the settlement scenes, 2026-11-12:

- frame time p50 33.39 ms, p95 34.30 ms over the last 300 frames, GPU p50 14.56 ms;
- GPU allocated 3.9 GB, flat; process resident 6.3-6.5 GB; the residency set 2.2-2.3 GB; no growth over an 11,100-frame run either;
- earlier (2026-11-06), from a save at stage 2: gameplay 57 s after boot, 30.0 fps, 300 presents in 300 display link ticks.

- What the Switch 2 shows was checked against a published co-op walkthrough (YouTube `-a5HMPJueu8`, chapter 2 at 944 s): the stage 2 start matches, with the vertical light columns on the right. The burning ship's smoke and fire in the intro were checked by eye in a live run.
- One `[RED]` line a run: `vertex attribute format 0x27 (8 / 16-bit integer) feeds a float input`, converted on the CPU, with no visible effect.

## Frame rate

- **Current:** the game's own 30 fps. Its config sets `t.MaxFPS 30` and `rhi.SyncInterval 2`. The Switch 2 holds an even 30 here: in a 60 fps capture of it, 173 of 182 frames are 33.3 ms apart.
- On the Mac at 30: 1,198 of 1,199 presents are two refreshes apart, and the power line reads 30.0 fps.
- 60 is possible: the rewriter finds both variables by name (`t.MaxFPS` at `main+0x9cd50d0`, `rhi.SyncInterval` at `main+0x9b87290`, in main's fixups) and `hle_fpslock.c` writes 60 and interval 1. Unlocked, the M4 Pro gave 55-59 fps with missed refreshes and the game ran at 0.92 of real time, so the profile keeps 30.

## Driving it on the Mac

Both players are needed: each joins in the lobby with A, and START is B. From a save at stage 2: the logos, the title, the menu (PLAY LOCAL), the lobby and CONTINUE, then gameplay about 60 s after boot. The character select takes Left and A for player 1, Right and A for player 2. In the intro cutscene both players hold B at the same time to skip it (B alone is Back). Set `DM_FOCUS_KEEP=1` for scripted runs. Our scripted runs drove player 2 through a `DM_CTL` extension (`p2:<keys>`) that main does not have.

## Open

- Not run on main alone: the library on main's chain is a cold build (about 2.5 hours here), not made yet. The facts above come from our branch.
- The engagement scene in stage 2 is darker than on the Switch 2: the asteroids and the drone show as silhouettes.
- The game reports itself as a trial (Friend's Pass); the local co-op path is not gated by it.
- An IPA: the 64 GB alias region is over iOS's limit.
- About 1,060 buffer-texture views a frame are made and released (the game registers its buffer textures again each frame): 0.15-0.35 ms a frame of residency commits.
- Not checked: game speed at 60 over a fixed route.

## Found on this game

- [Threadgroup stores in warp-synchronous code](../../../docs/reference/shaders.md#threadgroup-stores-in-warp-synchronous-code): broken Niagara beams and one-frame view flashes.
- [VOTEU writes the uniform predicate](../../../docs/reference/shaders.md#voteu-writes-the-uniform-predicate): flat purple and cyan blobs at the ships (the thruster meshes read the wrong particles).
- [Compute threads that return before a barrier](../../../docs/reference/shaders.md#compute-threads-that-return-before-a-barrier).
