# Mario Kart World (`0400C3F00006E000`)

A Switch 2 game: update 1.8.0 (v917504) over the base game, SDK 21.4.0 built for the Switch 2 (`sdk` names its spec `Ounce`), NVN2. Nintendo EPD's engine (sead / agl / gsys, Phive), modules `main` + `subsdk0` (NintendoSDK movie) + `subsdk1` (C library) + `sdk`. **In bring-up**: on the Mac the game shows its title ("Press L + R to start" over a 3D drive through Mario Bros. Circuit) and its main menu (Single Player, Multiplayer, Online Play, Wireless Play); the 3D scene has rainbow bands where it is bright ([Open](#open)). No race is tested yet; iOS is untested.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `sys_resource 0x4000000` | [`game.tsv`](game.tsv) | the NPDM declares 64 MB of system resource: virtual address memory on, and `main` sets a heap |
| `app_pool 0x240000000` | `game.tsv` | the Switch 2's 9 GB, as Donkey Kong Bananza's (not measured). `nnMain` allocates a fixed 7.04 GB memory block from its heap (`main+0xca866c`); with the Switch 1 pool (3,285 MiB) the heap was 1.68 GB, the block null, and the game crashed at `main+0x5dd3ac`. `nninitStartup` sizes the heap as the pool less about 1.53 GB: 7.47 GB |
| `mem_reserve 0x2d0000000`, `mem_alias 0xb0000000`, `mem_stack 0x10000000` | `game.tsv` | the heap check with the 9 GB pool (`SetHeapSize OOM: req 0x1de400000`): an 8 GB heap region. The alias region keeps 2.75 GB: the sdk's address region node area takes its first 1 GB |
| `romfs_marker Movie/StaffRoll_00.webm` | `game.tsv` | a file of the 1.8.0 RomFS |
| `render_cond on` | `game.tsv` | agl's render buffer binding wraps its clears in render conditions, as in Donkey Kong Bananza: `SetRenderEnableConditional(RENDER_IF_NOT_EQUAL)` at `main+0x49e194`, then `ClearColor` at `main+0x49e1c0`. Without the key every clear ran, and the title's 3D scene was black but for a few specks under the 2D layout |
| NVN2 API 231.39 | `hle_nvn.c` `nvn_api` | the game requires major 231 and minor ≥ 39 (`main+0xdd0890`, after `nvnDeviceGetInteger` items 0 and 1); its own `sdk` answers 231.39 (`DeviceGetInteger` at `sdk+0x65b680`; Donkey Kong Bananza's sdk answers 231.30). The host reads that answer from the game's `sdk` (`loader.c` `dm_sdk_nvn2_item`) |

## Files

- Packed from a dump folder (`base/`, `updates/917504/`) with [`scripts/pack_dump.py`](../../../scripts/pack_dump.py): the update's RomFS is the patched one (206,450 entries, 21.9 GB), packed on its own.
- Build IDs of 1.8.0: `main` `d00e4f399e411d0613aa8d26b1c02fef`, `subsdk0` `746302f49f095cfca3d2188423c2bd77`, `subsdk1` `8dec9c0b1bed821364e77b10a07b9a28`, `sdk` `7a68b13d88183767fb9c7babffe89f3d`.
- The rewriter (`dm/tools/patch_nso.py`) rewrites the four modules with no cut. The SDK-wide address region rules of `dm/tools/sdk_patches.py` (the node area cut and the 2 MB alignment) match no function of this `sdk`, so its address region node area keeps 1 GB of the alias region (the heap check: alias region mapped up to +1.00 GB).
- Shaders: `Shader/*.Nin_Ounce_NVN2.bfsha.zs` (six files) and `Shader/Drive.Nin_Ounce_NVN2.release.sarc.zs`, the effects in `Effect/static.Nin_Ounce_NVN2.esetb.byml.zs`, the layouts' shaders in 90 `Layout/*.blarc.zs`, agl's and gsys's archives in `Lib/`.

## Shaders

- The split (`dm/gfx/tools/shader_split.py`, no new reader) finds 11,604 shaders (compute 730, domain 13, pixel 7,301, vertex 3,560): BNSH in the `bfsha` files, the effects and the layouts (and 40 in `main`), agl's BAHS archives (`sharcb`, in `Lib/` and in `Shader/Drive…sarc.zs`) and agl's NDSR pack (36). Groups: shader 4,252, other 4,071, effect 3,136, layout 105, main 40; 5,180 variation pairs.
- Full Mac build (chain 3c66991eb439, about 25 minutes from an empty cache): 11,522 of 11,604 translate. The 82 failures are Ampere instructions the translator does not decode ([Ampere shader code](../../../docs/reference/shaders.md#ampere-shader-code-switch-2)). Open: which of them the races need.

  | Opcode | Shaders | Stages | Source |
  |---|---|---|---|
  | 0x883 | 29 | pixel | `DrivePassShaderDefault.sharcb` |
  | 0x2be | 25 | compute 13, vertex 7, domain 5 | `DrivePassShaderDefault.sharcb` |
  | 0x927 | 14 | domain 8, vertex 6 | `DrivePassShaderDefault.sharcb` |
  | 0x992 | 10 | compute | `DrivePassShaderDefault.sharcb` |
  | 0x593 | 2 | compute | agl `agl_technique_pfx.sharcb` (as in Donkey Kong Bananza) |
  | 0x481 | 1 | compute | `DrivePassShaderDefault.sharcb` |
  | 0x59f | 1 | compute | `DrivePassShaderDefault.sharcb` |

- The translator warns `Attribute offset 0x5C is not valid.` in 2,838 shaders (Donkey Kong Bananza's build has none). Open: not analysed.

## Rendering

The scene renders into 2048x1152 targets (RGBA16F colour). The window textures are 3840x2160, and the game draws its picture into their top left 2048x1152 and crops the window to it (`nvnWindowSetCrop`, `WindowCrop 0,0 2048x1152` in the log; 2560x1440 at the first frames). The window shows the crop. A `DM_NVN_SHOT` or `DM_CTL` `shot` without `DM_NVN_SHOT_OUT` saves the whole window texture, so the picture fills only its top left: take screenshots with `DM_NVN_SHOT_OUT=1920x1080`.

On the way to the title:

1. Black 3D scene: only the 2D layout ("Press L + R to start", "Ver. 1.8.0") over black with a few specks. The clears under render conditions ran unconditionally (`render_cond`, [What it needs](#what-it-needs)).

The title and the main menu match the game as it is known: the title drives Mario's kart through Mario Bros. Circuit (the canyon, the tunnel with the course's name) with "Press L + R to start" and "Ver. 1.8.0"; L + R opens the main menu over the same drive, with Single Player (selected, the 1P marker), Multiplayer, Online Play, Wireless Play, three round buttons and "Free Roam" on the + button. What differs is in [Open](#open).

## Boot (Mac)

Each step below stopped the boot before its fix:

1. A null memory block in `nnMain` (`CRASH sig=11 addr=0x10 pc=main+0x5dd3ac`): the game allocates a fixed 7.04 GB block from its heap, which the Switch 1 pool could not hold. The Switch 2 pool, 9 GB (`app_pool`, [What it needs](#what-it-needs)).
2. `nn::diag abort: Result 0xe15` in `hid::InitializeNmouse`, then in `chat::Initialize` (GameChat), then `CheckGraphicsSystemInitialized` before the camera context: Switch 2 services the host does not serve. Answered with nothing connected ([Switch 2 mouse, GameChat and camera with nothing connected](../../../docs/reference/services.md#switch-2-mouse-gamechat-and-camera-with-nothing-connected)). The chat camera storage needed NVN2 native images ([NVN2 native images](../../../docs/reference/nvn-objects.md#nvn2-native-images)); the camera session fails "open camera" with the result the game takes as "no camera".
3. `CRASH pc=0` after `vi::SetLayerNextHdrAdjustableBezierMetadata` (`main+0x959ae4`), in the first frames ([Switch 2 HDR metadata on a layer the host never made](../../../docs/reference/services.md#switch-2-hdr-metadata-on-a-layer-the-host-never-made)).
4. GPU out of memory after about 90 frames (`kIOGPUCommandBufferCallbackErrorOutOfMemory`, then the present watchdog; "GPU allocated" 21 to 24 GB): six sparse terrain arrays of 8192x8192x68 with 12 levels (two of 6.1 GB, format 0x50; four of 1.5 GB, ASTC 8x8) in virtual pools the game maps only in part, made as whole textures. Now Metal placement sparse textures, mapped and filled tile by tile from the page table ([Sparse textures in sparse virtual pools as Metal placement sparse textures](../../../docs/reference/nvn-objects.md#sparse-textures-in-sparse-virtual-pools-as-metal-placement-sparse-textures)): after 60 s 26,593 Metal tiles (1.7 GB) in 7 heaps, GPU allocated 14.2 GB, process footprint 12.6 GiB (with the 9 GB pool).

To the main menu (Mac): `DM_KEYS=f4000:L+R~0.5` from a fresh save. The title asks for L + R from about frame 3400 and the main menu shows by frame 4300 (with `DM_PRESENT_LINK=0`, so a window behind others still presents every frame). No movie started and no `nv::InitCheck` abort came in 3 minutes on the title without input (#834 saw one when the attract movie started).

Memory after 60 s (`run11.log`): GPU allocated 14.2 GB, footprint 12.6 GiB, 7 placement heaps of 256 MB.

## Open

- In the title's sky, at frame 3400 of the route: a dark band with rainbow streaks over the left half and a flat grey column from the horizon to the top of the picture. Not analysed; likely the same bloom fault (below) or the sky.
- Rainbow bands in the bright parts of the 3D scene: vertical colour bands in the sky near the horizon, on the water and on lit floors (in the tunnel the floor turns white with cyan bands). They come from the bloom: its bright pass (pixel shader `c44cb8e46dbcfd4f`, the 512x288 `RGBX16_SNORM` target, pass 876646 in the menu's pass dump) colours what it extracts by its brightness, and its output already holds the bands; the composite (`3d81a859ffbb99f5`) adds them to the scene. The scene's lit areas are much brighter than they should be (the tunnel floor is white). Suspected, not checked: the exposure, which a shader among the 16 runtime-only compute shaders that do not load (below) or the six 48 KB threadgroup compute shaders computes.
- `[RED] failed to create compute pipeline for <key>: Threadgroup memory size (49152) exceeds the maximum threadgroup memory allowed (32768)`: six compute shaders up to the main menu (`04dedc705a7ef88b`, `2a89b6d4df731b8d`, `7caf175d0e2ada58`, `a8310cf7c7acd0fe`, `c37db932f3076403`, `e2a5bd13907993e3`), each declares 48 KB of shared memory (the control section's +0x64), which Ampere has and an Apple GPU's threadgroup does not (32 KB). Their dispatches are skipped. All 168 shaders with 48 KB come from `DrivePassShaderDefault.sharcb`; the six run 64 threads, write storage buffers and use their shared memory with no barrier, 48-byte records indexed from the thread's number. What they compute is not known.
- 63 shaders missing from the cache (`failed to load shader …: cannot open …/b-3c66991eb439/….metallib`, 16 compute and 2 pixel programs with no shader): none of their keys is in the full build's 11,522 libraries, so they are the runtime-only shaders (about 240 by #834) that a `DM_NVN_SHDUMP` run collects (Goal: build order).
- The NVN2 calls reported as unimplemented: `CommandBufferDrawMeshTasks` (302 times in 60 s), `CommandBufferBindShadingRateImage`, `CommandBufferShadingRateImagePalette`; unknown names `nvn*CooperativeVectorMatrix*`, `nvn*PoolWriteDescriptorTableData`.
- The display link stalled 7 times in the first 50 s (`display link recovered: stalled … ms`), with 253 dropped frames up to frame 600; after that 2 dropped in 300 frames.
- The camera's other commands (29 at the first frames) fail as "no camera"; their meaning is not known.
- `pad_layout` and `app_category` are judgement, not measured.

## Found on this game

- [Sparse textures in sparse virtual pools as Metal placement sparse textures](../../../docs/reference/nvn-objects.md#sparse-textures-in-sparse-virtual-pools-as-metal-placement-sparse-textures)
- [Switch 2 mouse, GameChat and camera with nothing connected](../../../docs/reference/services.md#switch-2-mouse-gamechat-and-camera-with-nothing-connected)
- [Switch 2 HDR metadata on a layer the host never made](../../../docs/reference/services.md#switch-2-hdr-metadata-on-a-layer-the-host-never-made)
- [NVN2 native images](../../../docs/reference/nvn-objects.md#nvn2-native-images)
