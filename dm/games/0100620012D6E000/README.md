# Big Brain Academy: Brain vs. Brain (`0100620012D6E000`)

Update 1.2.0 (v131072), SDK 11.4.3. NintendoWare Bezel Engine 1.56 on NVN. Modules `main`, `subsdk0` (GLSLC: imported, never called, as `main` holds no GLSL source), `subsdk1` (the movie module) and `sdk`. Plays on the iPhone, handheld with Boost; 60 fps (present interval 1). The README lists its IPA at about 0.8 GB.

Brought up on the Mac: the title, Solo, enrolling a new student (birthday, profession, appearance, online features, the creepy-crawly filter), the main menu (Practice, Ghost Clash, Test), the Practice categories and activities from each (Whack Match, Species Spotlight, Frame Filler, Fast Focus, Flash Memory, Cubegame, the photo quiz, Tick-Tock Turn, Shadow Shift) with their results, coins and unlocks, at 60 fps with sound, the H.264 activity previews and saves, by controller and by touch.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `sys_resource 0` | [`game.tsv`](game.tsv) | the NPDM declares no system resource, and `nninitStartup` chooses its memory setup by `IsVirtualAddressMemoryEnabled`; see [Memory](#memory) |
| `op_mode handheld` | `game.tsv` | handheld, the game reads the touch screen (menus, every activity, Party's two-player touch battles); docked it does not |
| `perf_mode boost` | `game.tsv` | handheld Normal draws 1280x720, handheld with Boost 1920x1080 (the window crop goes from 1280x720 to 1920x1080 once the game sees Boost) |
| 4 GB layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | `game.tsv` | a 3328 MB heap region: `SetHeapSize` asks for 3.14 GB |
| `depth_mode zero` | `game.tsv` | the game sets the near = 0 depth mode, so the pipeline bases made before it runs (the NSP script's Mac prebuild, and a [pipeline archive made offline](../../../docs/development.md#the-pipeline-archive-made-offline) for the iPhone when one is built with `DM_PSO_GPUS`) take its vertex shaders' `main0_z` entries (the host itself follows the mode the game sets) |

No `patches.py`. Its shared `sdk` build (`2d369d9999e3e10e665106f840ffff9b`) now has the node-area cut needed by [Pokémon Brilliant Diamond](../0100000011D90000/README.md); this game uses the heap path and does not need it.

## Memory

- **Resolved: `nn::diag` abort in `StandardAllocator::Initialize(void *, size_t, bool)` from `nninitStartup`** (`sdk+0x2f98e4` ← `main+0x5b69e0`). The game's `nninitStartup` (`main+0x5b6984`) asks `os::IsVirtualAddressMemoryEnabled()`: when true it gives StandardAllocator a 60 GB address region (`0xf00000000`), when false it sets a heap of pool − used (2 MB aligned) and hands that over. The SDK's answer is `GetInfo(TotalSystemResourceSize) != 0` (`VammManager::IsVirtualAddressMemoryEnabled`, `sdk+0x34e074`), and the host reported a 32 MB system resource to every game, while this game's NPDM declares none. The profile key `sys_resource` reports the NPDM's size (0 here, so the heap path, as on hardware); the survey writes it for every new game, and profiles without it keep the 32 MB answer.
- The game sets a 3.14 GB heap (pool − used) in the 4 GB layout. It creates 1.5 GB of NVN memory pools in its first 25 s (384 + 256 MB CPU-uncached, a 320 MB physical pool for sparse mappings, 192 + 64 MB of render targets), so about 1.3 GB is resident at the title and 1.5 GB in the first activity.
- Over 15 minutes of activities it rose to 1.86 GB in steps as each new activity loaded (pools 217 → 292, textures 444 → 1,173), flat in between.

## Rendering

- 2,125 shaders (955 vertex, 1,153 pixel, 12 geometry, 5 compute). The window origin is lower left, depth mode near = 0, the swapchain 1920x1080 RGBA8 sRGB (format 0x38; cropped to 1280x720 in handheld Normal), present interval 1 at 60 fps. Everything drew right at the first frame with shaders; no texture or pool key was needed.
- **Not done: Bezel's line renderer.** Its 12 geometry shaders (`_BezelSystemResources/line/shader/line.bfsha`) take a line strip with adjacency (primitive type 11) and emit up to 12 vertices per segment as triangle strips: thick lines with joins and caps (28 emit sites). Metal has no geometry stage, so they do not build (`unknown main0_out`) and their draws are skipped (`[RED] primitive type 11 not implemented`). From the main menu on, every frame draws one such line (width 10 in its uniform block) into the top-left 512x512 of the frame and copies that region into a texture, so the main menu's chart loses its outlines.
- **Not done: ten pixel shaders pick their texture at run time** (Ryujinx's fallback arrays, `fp_t2d_cb0_40000000`): six in `03GamePerception.bnbshpk`, two PBR, two in `main`. In `e10f24835bbaa5f6` the handle is c3 +0x130 or c12 +0x1d0 of the game's own uniform buffers, chosen by a flag in c12, packed as texture ID | sampler ID << 20. `tex_dyn_slots` covers only handles from the driver's c2 tables, so these do not load and their draws are skipped. Resolving them needs the translator to report such a choice of two uniform-buffer handles, a chain change for every profile. None of the activities checked uses them.
- **Conditional rendering:** the engine calls `SetRenderEnableConditional`; it is drawn unconditionally (the profile has no `render_cond`), and no screen played looked wrong.

## Audio

- Its `AudioRenderSystem::Update` lock is `SdkMutexType::Lock` at +0x1d8 (SDK 10.4–11.4); `update_lock` (`hle_audren.c`) was checked with sound on it ([The renderer's update lock](../../../docs/reference/audio.md#the-renderers-update-lock-update_lock)).
- **Resolved:** when queued catch-up frames still signalled the renderer event, every frame of this game became a catch-up frame (about 38,000 catch-up renders instead of 37). A queued frame no longer signals the event ([A sound driver that never waits on the renderer event](../../../docs/reference/audio.md#a-sound-driver-that-never-waits-on-the-renderer-event)).

## Movies

- Each activity's preview is an H.264 movie (`movie/Movie/*.mp4`) that the game loops with Flush and Start, from four threads at once.
- **Resolved: the process died when an activity started, 2 runs in 5, with no `CRASH` line** (exit 137, SIGKILL; `~/Library/Logs/DiagnosticReports/dmhost-*.ips` had `EXC_BREAKPOINT` in `_os_unfair_lock_corruption_abort` ← `pthread_mutex_destroy` ← `md_free`, the movie decoder's destructor). `DM_MOVIE_TRACE=1` (every decoder call with its thread): the event thread acquired an input index while the control thread's Flush waited for the packet being decoded; the flush then returned that index to the free list, it was handed out a second time, two packets shared one buffer (VideoToolbox -12909 on the overwritten one, in exactly the runs that died) and a fifth pending packet wrote past `pend[]` into the decoder's `npend` and `dmu`. Handing such an index out last was not enough: with three hosts running at once one run still sent an index twice. Fixed in `hle_movie.c` (see [Found on this game](#found-on-this-game)).
- **Resolved: the previews flashed black at the end of every loop.** The game reads the buffers of the end-of-stream output (no picture) and of an output it acquired before the flush, and the host painted its texture black for both.

## Services

SDK 11.4.3 calls it needed ([Hardware Opus decoders](../../../docs/reference/audio.md#hardware-opus-decoders-built-on-the-sdks-software-decoders), [Directory, file data cache and save-thumbnail calls](../../../docs/reference/services.md#directory-file-data-cache-and-save-thumbnail-calls), the rest in [services.md](../../../docs/reference/services.md)):

- **Abort in `sf::hipc::InitializeHipcServiceResolution` under `codec::GetHardwareOpusDecoderWorkBufferSize(int, int)`** (`main+0x31f8e8`, early in `nnMain`): SDK 11's form without the option argument; the host had only the three-argument one.
- From the import list before the next boot (`main`'s undefined `nn::` symbols no hook names): `fs::EnableIndividualFileDataCache` / `DisableIndividualFileDataCache`; `account::LoadCachedNintendoAccountInfo` (fails: no Nintendo Account; the game then skips the birthday and country getters); the IR camera (`irsensor`: every camera unconnected, processors cannot run; Bezel's input layer makes one per controller and asks its status, `main+0x497f84`); `ldn::GetNetworkConfig`; and SDK 11's socket overloads with plain `sockaddr` / `int` arguments (`Connect`, `Accept`, `GetSockName`, `Get` / `SetSockOpt`, `Fcntl`, `Recv`, `Send`).

## Input

- The profile plays it handheld with Boost. Handheld, the game reads the touch screen (every menu and activity, and Party's two-player touch battles); docked it does not. The buttons work as well; the virtual gamepad starts in the 2D layout, and touches outside the controls reach the game's screen.

## Files and shaders

- Nearly all of RomFS is in Bezel Engine archives: 603 `.bea` files (`Archive/<Permanent|Temporary>~<name>.nx[.<language>].bea`, plus the engine's own `System.nx.bea` at the RomFS root), 7,665 files in all, 2.6 GB unpacked from 0.38 GB. `romfs_marker` is `Archive/Permanent~01Boot.nx.bea`.
- A `.bea` is a NintendoWare binary file with magic `SCNE` (version 0x10100, BOM `FF FE`): +0x20 u64 entry count, +0x28 u64 offset of an array of u64 entry offsets, +0x30 the name dictionary (`_DIC`), +0x40 the archive name. Each entry is a 0x30-byte `ASST` record: +0x10 u16 kind (2; 258 for the one sound project), u16 alignment shift, +0x14 u32 stored length, +0x18 u32 length, +0x20 u64 data offset, +0x28 u64 name offset (u16 length, then the path). Every entry is one zstd frame, even the two whose stored length equals their length. Inside: layouts (`.lyt`, SARCs with their own `__ArchiveShader.bnsh`), BNTX textures, `.bfsha` shader archives, models (`.fmdb`), MSBT text and fonts. The sound is loose `stream/*.bfstm` and the movies loose `movie/Movie/*.mp4` (H.264). [01, Compressed RomFS content](../../../docs/agents/01-extract.md#compressed-romfs-content).
- Shaders: 2,125 BNSH in the archives' `.bfsha` entries, most of them the engine's own in `System.nx.bea` (`_BezelSystemResources/<system>/shader/*.bfsha`: PBR shadow maps 846, post effects 770, …), the rest in the layouts' `__ArchiveShader.bnsh` and the per-archive shader packs (`.bnbshpk`), plus one BNSH in `main`. `shader_split.py` scans each archive entry as a file of its own (`bea_members`): 2,125 shaders in about 90 s. The profile groups `System.nx.bea` as `system` and the game's archives as `archive`.

## Mac routes

- From a fresh save, `DM_KEYS="f1900:A,f2400:A*9/150,f3800:Right*14/150,f3875:A*14/150,f6000:A*4/150,f6900:Down*5/60,f7250:A,f7500:A*17/150,f10050:A"` enrolls a student (birthday 01/01/2000, the default appearance, no online features) and starts Whack Match from Practice at about frame 10,050 (`<work>/run/enroll.sh`).
- In handheld mode `T<x>x<y>` taps work too: `f1900:T0.5x0.88` taps Start on the title.

## Found on this game

General host changes first needed here; each applies to every game:

- **`sys_resource`**: the host reports the NPDM's system resource instead of 32 MB for every game: [dm/games/README.md, Keys](../README.md#keys), [Virtual address memory follows the NPDM's system resource](../../../docs/reference/memory.md#virtual-address-memory-follows-the-npdms-system-resource).
- **Movie decoder Flush from several threads** (a send claims an index a flush returned, `pend[]` cannot overflow, an index held across a flush is withheld for 250 ms; a flush keeps each slot's last picture, and in native-texture mode an output with no picture leaves the texture as it was; `hle_movie.c`): [Input indices returned by a flush while a client holds them](../../../docs/reference/movies.md#input-indices-returned-by-a-flush-while-a-client-holds-them).
- **SIGABRT caught by the crash handler**: before it was, this game died once with exit 134, nothing in the log and no report: [05, Reading a crash report](../../../docs/agents/05-debugging.md#reading-a-crash-report).
- **BNSH in Bezel Engine archives** (`bea_members`): [BNSH in Bezel Engine archives](../../../docs/reference/shaders.md#bnsh-in-bezel-engine-archives).
- The spin-wait search's store rule takes one more loop in its `main`, a real flag wait (`main+0x1734e0`): [development.md, Power](../../../docs/development.md#power).

## Open

- The 12 line-renderer geometry shaders and the ten run-time texture pixel shaders (above).
- One 15-minute run of activities met one of the ten (`e10f24835bbaa5f6`, `03GamePerception.bnbshpk`) and the process aborted moments later, before the host caught SIGABRT, so its cause is not known; a second 15-minute run was clean.
- The development notes and 04 say the line renderer's geometry shaders do not build and that the chain does not use Ryujinx's geometry-to-compute transforms yet; the geometry research (`docs/geometry-metal-research.md`) now describes a compute path that does, and names this game's line renderer as an end-to-end target. Whether these 12 build with it is not recorded.
