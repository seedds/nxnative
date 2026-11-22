# Super Woden Rally Edge (`010066C027434000`)

Base game 1.0.0 (v0), SDK 21.4.7, the newest SDK so far. Unity 6000.3.17f1 IL2CPP with FMOD on NVN; NPLN 1.53 with gRPC linked in. Modules `main`, `subsdk0` (the SDK's movie library: the attract movies are H.264, decoded with VideoToolbox) and `sdk`. On the Mac: the logos, the title, the main map, Arcade, the route and car selects, the round info and Kenya SS1 under control (accelerating with ZR, steering), with FMOD sound. On an iPhone (iPhone19,4, A20 Pro, iOS 27.3 beta): played by hand; a short launch check logged 60 fps with a 1.0 GB footprint and no red line.

## What it needs

| Setting | Where | Why |
|---|---|---|
| Node area cut 1 GB → 62 MB | the SDK-wide node-area rule (`MATCH_CUTS` in `dm/tools/sdk_patches.py`), whose SDK 21.4.7 fingerprint matches this `sdk` build `ab3835b2…` (the constructor at `sdk+0x14fbe0`; the movs at `+0x14fbf0` and `+0x14fc5c`) | Unity 6 takes all its memory as address regions, after the node area; see [Memory](#memory) |
| `sys_resource 0x1000000` | [`game.tsv`](game.tsv) | the NPDM declares 16 MB of system resource, so `IsVirtualAddressMemoryEnabled` is true |
| 5 GB block: `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | `game.tsv` | the block of the other Unity games with virtual address memory (4.375 GB alias region) |
| `SHADER_6000_3`, `COMPUTE_6000_3` | `dm/gfx/tools/unityfs.py` | no bundle carries a type tree to borrow; see [Files and shaders](#files-and-shaders) |
| No `pad_layout` (the 2D default) | `game.tsv` | menus on the D-pad and A / B, the car on the left stick and ZR / ZL |

The provisional profile was right as generated; everything else was general host work ([Found on this game](#found-on-this-game)), not yet re-run on this tree's host.

## Memory

- As Hollow Knight: Silksong ([010013C00E930000](../010013C00E930000/README.md)) and BALL x PIT ([010086A022444000](../010086A022444000/README.md)): no heap, Unity 6 reserving and committing its memory itself through `nn::os::AllocateAddressRegion` and `AllocateMemoryPages` in the alias region, Engage's 5 GB block, and the node-area cut on this `sdk` build.
- Measured on the Mac over the menus and the first stage: the alias region's high-water mark +2.01 GB of 4.375 GB, 1.24 GB mapped, resident 1.8–2.0 GB, 0.8–1.0 GB of GPU memory.

## Rendering

- Docked, the game draws 1440x810 into 1920x1080 window textures and crops the window to 1440x810 (`nvnWindowSetCrop`), which the present scales to the screen. A Mac shot shows the crop only with `DM_NVN_SHOT_OUT`.
- **Open: the attract movie ("DEMO PLAY", after about 40 s on the title) is noise.** The movie itself decodes right (2560x1440 H.264; its YUV → RGB pass into a 2560x1440 RGBA8 target is correct in a texture dump). The game's own `VideoTexture` render texture (`sharedassets9.assets`) is `R32G32B32A32_SInt` (GraphicsFormat 44, NVN `RGBA32I`, 1920x1080): Unity's `Hidden/VideoComposite` (fragment `5a3af64137de2137`) writes float colours into it and `UI/Default` (`3cd99cd61d77d0cb`) samples it as float. NVIDIA's colour unit stores a float output's bits as they are in a 32-bit integer target and its texture unit hands them back unchanged, so the picture survives on a Switch; Metal converts on the way in and out. Stored as `RGBA32Float` (an experiment: NVN `RGBA32I` mapped to `RGBA32Float`) the movie plays right. A general fix needs the host to know which fragment outputs are integers (the profile's `fout` facts, which a translated library does not carry at run time) to give a 32-bit integer attachment a float view only for float outputs; not done, since several games write `RG32UI` / `RGBA32UI` targets with integer outputs.

## Frame rate

- The game paces itself on `vi::GetDisplayVsyncEvent`: a listener thread records each vsync's time and the main thread starts a frame only when 0.9 × 1/60 s have passed since the last one's vsync (`main+0xe38d60`), so every late vsync wake costs a whole frame.
- In the Mac app (a window, vsync from the display link): 60 fps on the title (frame interval p99 17.4 ms). Headless (`DM_WINDOW` unset, the host's own 60 Hz vsync thread) with the machine loaded by other builds (load average 25 on 12 cores): about 41 fps, the vsync wakes jittering by 10–24 ms. The guest work per frame is small (main thread 3–4% CPU, GPU 2–5 ms). Not measured on a phone yet.

## Files and shaders

- No Addressables and no AssetBundles: 384 `Shader` and 21 `ComputeShader` objects in the player's serialized files (`Data/*.assets`, `level*`, `Data/Resources`), none with type trees, under the type hashes `57f0dd2d…` (Shader) and `43a0948a…` (ComputeShader). The layouts are written out from AssetRipper's TypeTreeDumps for 6000.3.17f1 ([Unity Shader layouts written out for players without bundles](../../../docs/reference/shaders.md#unity-shader-layouts-written-out-for-players-without-bundles)). 12,833 stage shaders, 11 of them the player's own GLSLC outputs in `main`.
- Movies: `Data/StreamingAssets` H.264 MP4s (an intro at 1920x1080, the attract movie at 2560x1440).
- `romfs_marker` is `Data/sharedassets1.assets.resS`.

## Input

- Docked, the 2D pad layout. Menus: D-pad and A / B, Plus on the title. In a stage ZR accelerates and the left stick steers.

## Mac routes

- From a fresh save (`SV=` an empty folder with the work folder's `run/run.sh`), `DM_KEYS=28:Plus,34:Down,36:Right,38:A,42:Left,44:A,50:A,56:A,70:A,86:A,100:ZR~8,108:ZR+LLeft~1,110:ZR~6` goes from the title to the main map, Arcade, route A, the first car (automatic), the round info and Kenya SS1, and drives off. The prewarm list was recorded on this route.
- The main map's Competitions needs a car bought at the Dealer first ("You don't have any car in your garage").

## Found on this game

General host change first needed here; it applies to every game:

- **The node-area rule on SDK 21.4.7** (`sdk_patches.py` `MATCH_CUTS`, `sdk` build `ab3835b2…`): [The SDK's address-region node area](../../../docs/reference/memory.md#the-sdks-address-region-node-area).

It also needed general changes that upstream made for other games (the fork had made its own, which this tree does not carry):

- **Unity 6000.3 `Shader` / `ComputeShader` layouts** (`unityfs.py` `SHADER_6000_3`, `COMPUTE_6000_3`, the same type hashes): [Unity Shader layouts written out for players without bundles](../../../docs/reference/shaders.md#unity-shader-layouts-written-out-for-players-without-bundles).
- **The C library's monotonic clock through the steady clock** (`hle_time.c`; SDK 21.4.7 links the same `GetStandardSteadyClockCurrentTimespec`): [`clock_gettime` is `nn::time`'s steady clock (SDK 22)](../../../docs/reference/services.md#clock_gettime-is-nntimes-steady-clock-sdk-22).
- **Event descriptors** kept by the host for gRPC's poll engine (`hle_net.c`): [`EventFd` needs no network](../../../docs/reference/services.md#eventfd-needs-no-network).
- **`nvnTextureViewCompare`** (`hle_nvn.c`): [`nvnTextureBuilderGetViewOffset`, `nvnTextureViewCompare`, `nvnBlendStateSetAdvancedNormalizedDst`](../../../docs/reference/nvn-objects.md#nvntexturebuildergetviewoffset-nvntextureviewcompare-nvnblendstatesetadvancednormalizeddst).

## Open

- The attract movie is noise ([Rendering](#rendering)).
- Frame pacing on an iPhone ([Frame rate](#frame-rate)).
