# Fire Emblem Engage (`0100A6301214E000`)

Update 2.0.0 (v327680) over the base game, SDK 12.3.7. Unity 2020.3.18f1 IL2CPP with Wwise, on NVN. Modules `main`, `subsdk0` (NintendoSDK_movie) and `sdk`. Plays on the iPhone; 60 fps with 2.0.0 (30 on the Switch) through the game's own 60 fps path. The README lists its IPA at about 15 GB.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `QualitySettings.set_vSyncCount` argument → 1 | [`patches.py`](patches.py) `CUTS`, `main` `8c08b971…` at `main+0x32f92d4` | the game's own 60 fps path; see [Frame rate](#frame-rate) |
| Node area cut 1 GB → 62 MB | `dm/tools/sdk_patches.py`, `sdk` build `56ec2474…` (`sdk+0x346430`, `sdk+0x3464cc`) | the alias region ran out of address space minutes into play; see [Memory](#memory) |
| 5 GB guest layout: `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000`, `app_pool 0xA0000000` | [`game.tsv`](game.tsv) | 256 MB in front + 4.375 GB alias region + 256 MB stack region + 128 MB heap region, with a 2.5 GB pool reported. 5 GB is the total proven on a phone |
| `shader_groups` `engine` / `assets` | `game.tsv` | `data.unity3d` and `Resources/` are the engine's built-ins, `StreamingAssets/` the Addressables bundles |
| `depth_mode zero` | `game.tsv` | the game sets the near = 0 depth mode, so the pipeline bases made before it runs (the NSP script's Mac prebuild, and a [pipeline archive made offline](../../../docs/development.md#the-pipeline-archive-made-offline) for the iPhone when one is built with `DM_PSO_GPUS`) take its vertex shaders' `main0_z` entries (the host itself follows the mode the game sets) |

No `pad_layout`: the virtual gamepad starts in the 2D layout. `present_interval` stays `game`.

## Frame rate

- **Current: 60 fps from the game's own path.** The game's time class `App.GameTime` picks a vertical sync count per scene (Slow = 2, i.e. 30 fps: system, movies, demos, combat cutscenes, the hub; Normal is 1 only when `GameConfig.IsPerformance`) and hands it to Unity through `GameTime.SetVsyncCount(int)` (`QualitySettings.vSyncCount` → NVN present interval). About 96 pieces of per-frame logic (cameras, map cursor, unit actors, menu scrolling and cursors, hub camera, minigames) step by `GameTime.VsyncFixedCount` (= vSyncCount, how many 60 Hz ticks a frame advances): with a count of 2 and 60 frames on screen they all run twice as fast, with a count of 1 at 60 fps they are right. The real-time `VsyncDeltaCount` / `VsyncDeltaTime` (hub character movement, fishing, dragon ride, about 120 places) do not depend on the frame rate.
- The cut is in Unity's managed wrapper `QualitySettings.set_vSyncCount` (`main+0x32f92c0`): both paths use w19 as the argument, `mov w19,w0` → `mov w19,#1`, so every caller (`GameTime`, `QualitySettingsStack.PopImpl`, `XRSystem`) can only set 1; `get_vSyncCount` follows at 1. No code calls `SetQualityLevel`. The game then sets interval 1 itself and `game` mode gives 60 fps.
- Measured on the Mac (holding Down for 0.45 s in the save list): 60 fps with the cut and the original 30 fps both move 6 slots; without the cut, 60 fps forced by `DM_PRESENT_INTERVAL=display` reaches the end (9 or more): frame-counted logic speeds up.

## Memory

- The NPDM declares 16 MB of system resource, so `IsVirtualAddressMemoryEnabled` is true: `nninitStartup` hands all free memory (application pool − 64 MB) to StandardAllocator as one address region in the alias region (1 GB aligned). Unity's low-level virtual allocator (vtable at `main+0x61ffeb0`; reserve `main+0x9aa0f4` → `nn::os::AllocateAddressRegion`) also takes regions itself, each aligned by size (2.25 MB takes 4 MB, 16.25 MB takes 20 MB): about 1.05 GB in the prologue battle. Engage never calls `SetHeapSize`; thread stacks are placed at random in the stack region.
- Measured (prologue to the battle, 12 minutes): StandardAllocator commits only 0.58 GB and Unity's own regions 0.97 GB, 1.55 GB in all. So the pool is reported as 2.5 GB (StandardAllocator about 2.44 GB, four times its use). Reserved address space peaks at 3.47 GB of 4.31 GB.
- The former 6 GB block left no margin on a phone: without the extended virtual addressing entitlement iOS allows one reservation of at most 6 GB, and Metal needs address space too (Engage allocates about 1 GB of GPU memory on the Mac).
- **Resolved: a crash a few minutes into the prologue battle (scripted run, about 357 s).** A write to `NULL + 0x1fffe0` at `main+0x9a4664`: Unity's TLSF heap allocator (`main+0x9a45d0`) added a pool and got no address region for it. The alias region was out of address space, not memory: at the crash Engage held 4.17 of the 4.375 GB left after the SDK's ~1 GB node area (StandardAllocator 3.14 GB, Unity about 1.05 GB, the rest fragments). On hardware the alias region is 64 GB. The node area cut (the two `mov w8/w9, #0x3fe00000` in the AddressRegionManager constructor, `sdk+0x346420`, → 62 MB, about 1.35 million 0x30-byte nodes; a few hundred regions are live in measurements) fixed it; a 12-minute scripted run now plays through the battle.
- **Resolved: the idle attract demo crashed after about 220 s.** Unity's particle geometry jobs wrote through a NULL index pointer on several job threads at once (`main+0xdb0ad4`, `0xdaea58`, `0xdadb5c`). The scheduler (`main+0xd91e0c`) takes its vertex and index chunks from a fallback that creates and maps a fresh buffer each time (`main+0x7b9c64`; Unity's Switch renderer always takes it, as on hardware), and from about 180 s 32-bit mesh indices also turned up inside Unity's texture objects (the replay's red lines on rejected texture objects showed their first bytes as 32-bit triangle indices; they stopped once the alias region had address space left). Same cause: past 2 MB the index buffers got no mapping. With the cut an 850 s idle run is clean; with the old SDK the same run dies at 159 s in Unity's heap allocator. To reproduce the old state: `boot.sh <log> 245` with no input on an SDK rewritten without the cut.
- How it was measured: probe `VammManager::AllocateAddressRegion` at its success path (`ReleaseWriteLock` return: x21 = size, x22 = address) and `FreeAddressRegion` after its prologue (x1 = address) and sum live regions by caller; probe `VammManager::AllocateMemoryPages` / `FreeMemoryPages` (x1 address, x2 size) for commits. The `[mem]` log lines report the alias high-water mark and the mapped total ([Measuring address regions and commits](../../../docs/reference/memory.md#measuring-address-regions-and-commits)).

## Rendering

- **Resolved: shaders reported missing at random, their dumped control sections garbage.** Unity frees a program's control section once `nvnProgramSetShaders` returns; the keys are now computed inside that call (`prog_key`).
- **Resolved: GPU memory grew with every scene change** (the idle title demo reached 5.4 GB of GPU memory and 3.6 GB resident in 850 s; it now stays at 1.2 to 1.4 GB). Unity frees memory pools on scene changes and builds new ones. The residency set still held each finalized pool's no-copy `MTLBuffer`, and the storage-sharing texture table (`g_alias`) never evicted, so old pools' render targets and pool-backed linear textures stayed alive. Finalizing a pool now evicts its entries (`mtl_alias_drop_pool`), and pool buffers are retired through deferred release.
- **Probably resolved: about one run in four stalled early** (around the opening movie or the title; no log output). Since the real thread pause for IL2CPP's GC, 12 runs (eight of 200 s through the movie and title, four of 330 to 850 s) have not stalled. A second candidate, fixed later: a race in the dirty-page tracker (`nvn_dirty.c`) could leave a page read-only while marked writable; Engage's movie decoder writes frames into texture storage from a host thread while the replay thread protects pages (`tests/t_dirty_race.c`). If a stall comes back, `sample <pid> 3` when the log stops growing.
- Unity binds only separate textures and samplers (`c2[0x168 + 8 × slot]`; texture word 0x5A, sampler 0x15A).
- Video: Unity's `Hidden/VideoDecode` samples luma as `.w` of an `R8` texture with the swizzle (0, 0, 0, R) (Alpha8) and chroma from an `RG8` texture at half size. The frames are block-linear textures written with `nvnTextureWriteTexels` every frame (Y `R8`, UV `RG8`), uploaded from guest memory and re-uploaded when dirtied. To check the movie path apart from the decoder, fill the frames with a gradient in `hle_movie.c`.
- One geometry shader (in `unity_builtin_extra`) fails to compile (`unknown main0_out`); nothing in the opening scenes uses it.
- The game sets depth mode near = 0.
- Uniform updates inside a render pass no longer end it: its battles went from about 1,850 to 360 MB of attachment loads and stores a frame (88 → 38 passes).

## Movies

- **Resolved: the game waited forever at its 94 s opening movie `S01F.webm`** (frames kept presenting). Unity demuxes the WebM itself and feeds VP9 packets to `movie::Decoder`, which the host implements (four events, input and output indices, one NV12 frame per packet, w × h × 1.5); libvpx decodes on one host thread per decoder. Unity reads `width`, `height`, `crop`, `nv12-y-stride`, `nv12-y-offset`, `nv12-uv-offset` and `slice-height`. When the movie ends the game goes on to its title screen.
- **Resolved:** `movie::Extractor` (libwebm in `subsdk0`) runs natively; the host used to force `SetDataSource` to fail, and the game crashed on a null right after the failed open.

## Audio

- Wwise drives AudioOut directly: one output opened at `main+0x177d924`, with no release event (the game polls). On the phone it opened a second one later; the Mac runs never did. Unity's FMOD also has an output (`main+0x4aec0c` / `0x4aec4c`). The log shows each as `audio #N`.
- Known issue (README): after switching away from the app and back, some sounds may stay silent until a restart.

## Services

- **Resolved: a long black screen at boot** (about 17 s on the Mac, 32 s on an iPhone). The game walks RomFS directories of 38,000 files, and every path component scanned its whole parent directory with `getxattr` on each entry. A per-directory name index (`dir_index_get` in `hle_fs.c`), which also speeds up every file Wwise opens, brought the first frame to 5.7 s; a lighter `ReadDirectory` to 1.8 s on the Mac (9.9 s → expected about 3 s on an iPhone), and the opening movie starts at 7 s instead of 49 s.
- **Resolved: a null read in `sdk` under `nn::applet::CreateLibraryApplet`** (title menu → DLC → `nn::ec::ShowShopAddOnContentList`). Every `Show…` overload the game imports is hooked; the shop calls act as if the player closed the shop (`hle_misc.c`).
- **Resolved: an abort in `nn::sm::Initialize` under `nn::pctl::IsFreeCommunicationAvailable`** entering the first battle map: answered "not available".
- Add-on content: its two DLC packages each hold one empty marker file (`e0`, `h0`); the content is already in update 2.0.0. `DLCManager.RefreshHasList` marks a content owned when its mount succeeded (probe `main+0x29f3c88`).
- It calls `GetSharedFontAddress` without importing `ScalableFontEngineHelper::Decode` (a Unity player), so it gets the plain TTF.
- `SignalToAddress` type 2 now subtracts 2 as Horizon does; it did not change Engage's demo crash (the address-space shortage above).
- The appearance choice opens the system keyboard (name entry): with no window the host answers cancel and the screen goes back; pass `DM_KBD=<text>` in a scripted run.

## Files and shaders

- The survey found Unity 2020.3 IL2CPP on SDK 12.3.7 with NVN, an extra `subsdk0` (the movie player), 27,152 UnityFS bundles holding the shaders, and 101 `nn::hid` imports plus `irsensor`, `ro` and `swkbd`.
- Shaders: 209 `Shader` objects in 188 bundles (`StreamingAssets/aa`) plus the engine files (`data.unity3d`, `Resources/unity default resources`), read by `unityfs.py`: 9,269 stage shaders (4,634 vertex / pixel pairs and one geometry shader), no compute shaders; 9,268 compile. Unity's player has 11 GLSLC outputs of its own in `main`.
- Rewriter: 7 offset jump tables, 1 indirect call and 1 `sp` write through `x18` in `main`.
- Prewarm list: boot, the opening movie, the title, a save load and the prologue battle.

## Found on this game

General host changes first needed here; each applies to every game:

- **Unity's x18 shapes** (`site_br_tab`, `site_blr`, `site_sp`): [Offset jump tables through x18 read at rewrite time](../../../docs/reference/rewriter.md#offset-jump-tables-through-x18-read-at-rewrite-time), [Indirect calls through x18](../../../docs/reference/rewriter.md#indirect-calls-through-x18), [Stack pointer writes through x18](../../../docs/reference/rewriter.md#stack-pointer-writes-through-x18).
- **Node area cut by `sdk` build** (`dm/tools/sdk_patches.py`) and **sizing a virtual-address-memory game's reservation**: [The SDK's address-region node area](../../../docs/reference/memory.md#the-sdks-address-region-node-area), [Sizing the pool and alias region for virtual address memory](../../../docs/reference/memory.md#sizing-the-pool-and-alias-region-for-virtual-address-memory).
- **C++ exception unwinding through `svcQueryMemory`** (IL2CPP throws for every managed exception), **`movie::Decoder` on MediaCodec's protocol**, **applet `Show…` hooks**, **RomFS directory index** (`dir_index_get`), **add-on content from `DM_AOC_DIR`**, **program keys in `SetShaders`** (`prog_key`), **CPU-written block-linear textures** (`mtl_tex_cpu_written`): [`svcQueryMemory` must report each module's real layout](../../../docs/reference/memory.md#svcquerymemory-must-report-each-modules-real-layout), [`movie::Decoder` taken over as a MediaCodec-style protocol](../../../docs/reference/movies.md#moviedecoder-taken-over-as-a-mediacodec-style-protocol), [System applets opened by `Show…` calls](../../../docs/reference/services.md#system-applets-opened-by-show-calls), [RomFS lookups on large directories](../../../docs/reference/services.md#romfs-lookups-on-large-directories), [Add-on content from `DM_AOC_DIR`](../../../docs/reference/services.md#add-on-content-from-dm_aoc_dir), [Program keys computed inside `nvnProgramSetShaders`](../../../docs/reference/shaders.md#program-keys-computed-inside-nvnprogramsetshaders), [Textures written with `nvnTextureWriteTexels`](../../../docs/reference/nvn-objects.md#textures-written-with-nvntexturewritetexels).
- **Texture objects copied into records** (`REC_SNAP` / `tex_snap`): [Texture objects copied into command records](../../../docs/reference/rendering.md#texture-objects-copied-into-command-records).
- **Several audio outputs logged apart** (`audio #N`): [Several outputs at once (`audio #N`)](../../../docs/reference/audio.md#several-outputs-at-once-audio-n).
- **Pool finalize evicting shared textures** (`mtl_alias_drop_pool`), deferred release of pool buffers, the memory line's `serial objects` / `freed but alive` / `shared textures`: [Finalized pools kept alive by residency and the sharing table](../../../docs/reference/nvn-objects.md#finalized-pools-kept-alive-by-residency-and-the-sharing-table).
- **Unity `Shader` objects** (`unityfs.py`): [Unity 2020.3 Shader objects in UnityFS bundles](../../../docs/reference/shaders.md#unity-20203-shader-objects-in-unityfs-bundles); **separate textures** (`NVN_SEPTEX_SLOT0`): [04, section 3](../../../docs/agents/04-graphics.md#3-find-the-binding-layout-by-census).
- **Prewarm lists**: [development.md](../../../docs/development.md#prewarm-lists).

## Open

- After a trip to the background some sounds may stay silent (README known issue).
- The early stalls are "probably fixed", not confirmed.
- 03's entry on `StandardAllocator::Initialize` still gives Engage's layout as "5.5 / 8 GB on the Mac"; the profile has 4.375 / 5 GB.
- `game.tsv`'s header still says the game "reaches the title screen on the Mac" and that the memory partition is "set for Mac bring-up".
