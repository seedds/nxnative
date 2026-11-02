# Fire Emblem Fortune's Weave (`04007EF01F8AE000`)

A Switch 2 game: update v131072, SDK 22.2.0 built for the Switch 2 (`nnSdk-Generic_22_2_0`, `Ounce`), NVN2 API 231.40. Intelligent Systems' engine on NintendoWare (G3d, Ui2d, Vfx, Atk), modules `main`, `subsdk2` (movie) and `sdk`. **Plays on the Mac** (M3 Pro): the opening movie, the first map scenes, the world map, the Sothis scenes, character creation, the first battle maps and, in a 30-minute play session, the first dungeon (Heretics' Cave). No IPA has been built.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `mem_alias 0x260000000`, `mem_reserve 0x290000000` | [`game.tsv`](game.tsv) | the game sets no heap and takes its memory from a StandardAllocator in the alias region; with a Switch 1 game's 4.375 GB region the second movie's buffers failed and the screen went black: [Sizing the pool and alias region for virtual address memory](../../../docs/reference/memory.md#sizing-the-pool-and-alias-region-for-virtual-address-memory) |
| `app_pool 0x240000000` | `game.tsv` | the Switch 2's 9 GB, as Donkey Kong Bananza |
| `sys_resource 0x4000000` | `game.tsv` | the NPDM declares 64 MB, so virtual address memory is on |
| `depth_mode zero` | `game.tsv` | the game sets near = 0 at startup; pipeline bases made before that are made for it |
| `pad_layout full` | `game.tsv` | the game uses most of the controller: + (menu), Y (attack), R (dash), ZL and ZR on the battle maps, besides A and the sticks |

## Files

- Built from a dump folder holding `updates/131072/` only: the update is the whole game ([The NSP script on a dump with only an update](#found-on-this-game)). A dump has no control data, so the display version is unknown and the title shows "Ver. ?" ([The display version of a game packed from a dump](../../../docs/reference/services.md#the-display-version-of-a-game-packed-from-a-dump)).
- RomFS: 27 GB. 91 movies, all AV1 in WebM (`Movie/Product`, `Movie/CutScene`).
- Shaders: 16,888 in [`shader_map.tsv`](shader_map.tsv), all of which build. The shader split takes about 11 minutes from scratch, a full Mac shader build 6 to 10 minutes on an M3 Pro.

## Shaders

All 16,888 translate. The Ampere instructions its shaders brought to the translator, and the multiply-add carries that broke its auto exposure, are in [Ampere shader code (Switch 2)](../../../docs/reference/shaders.md#ampere-shader-code-switch-2); its light culling reads its dispatch's workgroup counts at the start of `c0` ([NVN2 compute `c0 + 4`](../../../docs/reference/shaders.md#nvn2-compute-c0--4-the-dispatchs-workgroup-counts)).

## Rendering

- The scene renders below the screen's size and NVN2's DLSS upscales it, served through MetalFX's temporal scaler ([NVN2's DLSS evaluation through MetalFX's temporal scaler](../../../docs/reference/rendering.md#nvn2s-dlss-evaluation-through-metalfxs-temporal-scaler)). The game sizes both from the display it is told it has ([The Switch 2's largest display buffer](../../../docs/reference/services.md#the-switch-2s-largest-display-buffer)): docked on a 3456x2234 MacBook Pro screen, 2560x1440, so it renders at 1024x576 into a 2048x1152 DLSS output (640x360 into 1280x720 before the host answered the query). It creates a 3840x2160 network too and has not evaluated it so far.
- The auto exposure averages a luminance histogram with an inlined u64 division; a scene change clears the exposure image before that runs ([Pending clears land before a compute dispatch](../../../docs/reference/rendering.md#pending-clears-land-before-a-compute-dispatch)).
- The battle maps hold the game's 30 fps on the M3 Pro (p95 33.8 ms over 300 frames).
- Movies decode through VideoToolbox on chips with an AV1 decoder (M3 and A17 Pro on) and through dav1d on the others ([SDK 22's video decoders: AV1 and the GPU route](../../../docs/reference/movies.md#sdk-22s-video-decoders-av1-and-the-gpu-route)).

## Driving it on the Mac

`<build>/tools/dmhost <build>/patched_v131072` with `DM_ROOT`, `DM_ROMFS=<build>/extract/v131072/RomFS.img`, `DM_SAVE`, `DM_TEMP`, `DM_MSL_DIR=<build>/shader/cache`, `DM_FONT_DIR` and `DM_NVN_SHMAP=<build>/profile/04007EF01F8AE000/shader_map.tsv`, as the NSP script prints it. From an empty save, `DM_KEYS=f1000:A*400/120` (A every 4 s) goes through the opening, the first map scenes and the world map to character creation, where it stays: A alone does not get past the body choice. With a save past the opening, `f1000:A*30/120` takes the title's Continue into the first battle map at about frame 1,280. `DM_NVN_SHOT_OUT=1920x1080` keeps screenshots at a size where the scene's sharpness can be compared.

## Open

- **AV1 decoding through dav1d**, for Macs and iPads before M3: checked here with `DM_AV1_DAV1D=1` only. A tester with an M1 Pro is trying it ([seedds/nxnative#927](https://github.com/seedds/nxnative/issues/927), where the AV1 change's scope is being settled).
- **iOS:** the 10.25 GB block is more than an iPhone allows without the extended-virtual-addressing entitlement; no IPA has been built.
- **Prewarm list:** [`pso_manifest.tsv`](pso_manifest.tsv) is empty, so the first visit to each scene compiles its pipelines.
- **DLSS:** the exposure texture, the network's method and preset, and its flag bit `0x20000000` are kept but unused; the 3840x2160 network has never run.
- Red lines: `nvnCommandBufferSetZCullZF32CompressionEnable` is unimplemented (a few times a run), and four `nvnWindow…AcquireTimeout` names are unknown; no effect seen.
- Nothing past the first dungeon has been played.

## Found on this game

- [The SDK's address-region node area](../../../docs/reference/memory.md#the-sdks-address-region-node-area): SDK 22.2.0's constructor, the rule's seventh implementation.
- [Sizing the pool and alias region for virtual address memory](../../../docs/reference/memory.md#sizing-the-pool-and-alias-region-for-virtual-address-memory): the second movie that never started.
- [The Switch 2's mouse pointer (Nmouse) with no mouse](../../../docs/reference/services.md#the-switch-2s-mouse-pointer-nmouse-with-no-mouse) and [The output device notification under its SDK 22 name](../../../docs/reference/audio.md#the-output-device-notification-under-its-sdk-22-name): the aborts at boot.
- [SDK 22's video decoders: AV1 and the GPU route](../../../docs/reference/movies.md#sdk-22s-video-decoders-av1-and-the-gpu-route): the movies.
- [NVN2's DLSS evaluation as a scaled copy](../../../docs/reference/rendering.md#nvn2s-dlss-evaluation-as-a-scaled-copy), then [through MetalFX's temporal scaler](../../../docs/reference/rendering.md#nvn2s-dlss-evaluation-through-metalfxs-temporal-scaler): the black 3D scene, then the shaking battle maps.
- [Ampere shader code (Switch 2)](../../../docs/reference/shaders.md#ampere-shader-code-switch-2): bindless textures, shared-memory atomics, 64-bit conversions, ERRBAR, and the IMAD.X and IMAD.HI carries behind the dark map.
- [Fragment shaders that read the primitive ID get whole pipelines](../../../docs/reference/shaders.md#fragment-shaders-that-read-the-primitive-id-get-whole-pipelines): the 2x2 dot grid of the lit scene.
- [NVN2 compute `c0 + 4`: the dispatch's workgroup counts](../../../docs/reference/shaders.md#nvn2-compute-c0--4-the-dispatchs-workgroup-counts): the dark title.
- [Pending clears land before a compute dispatch](../../../docs/reference/rendering.md#pending-clears-land-before-a-compute-dispatch): scenes that opened washed out.
- [The Switch 2's largest display buffer](../../../docs/reference/services.md#the-switch-2s-largest-display-buffer): the blurry picture.
- [The display version of a game packed from a dump](../../../docs/reference/services.md#the-display-version-of-a-game-packed-from-a-dump): "Ver. ?".
- Packing: the NSP script builds from a dump with only an update and surveys a dump with no profile; the shader split reads each SARC member's name up to its own NUL (12 minutes to 2 for this game's split).
