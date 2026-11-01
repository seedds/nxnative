# Mario Kart World (`0400C3F00006E000`)

A Switch 2 game: update 1.8.0 (v917504) over the base game, SDK 21.4.0 built for the Switch 2 (`sdk` names its spec `Ounce`), NVN2. Nintendo EPD's engine (sead / agl / gsys, Phive), modules `main` + `subsdk0` (NintendoSDK movie) + `subsdk1` (C library) + `sdk`. **In bring-up**: the game runs its frame loop on the Mac (the boot's 60 s watch window: 1,073 frames, 57 fps between frames 600 and 900; the picture is not yet checked, [Open](#open)). iOS is untested.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `sys_resource 0x4000000` | [`game.tsv`](game.tsv) | the NPDM declares 64 MB of system resource: virtual address memory on, and `main` sets a heap |
| `app_pool 0x240000000` | `game.tsv` | the Switch 2's 9 GB, as Donkey Kong Bananza's (not measured). `nnMain` allocates a fixed 7.04 GB memory block from its heap (`main+0xca866c`); with the Switch 1 pool (3,285 MiB) the heap was 1.68 GB, the block null, and the game crashed at `main+0x5dd3ac`. `nninitStartup` sizes the heap as the pool less about 1.53 GB: 7.47 GB |
| `mem_reserve 0x2d0000000`, `mem_alias 0xb0000000`, `mem_stack 0x10000000` | `game.tsv` | the heap check with the 9 GB pool (`SetHeapSize OOM: req 0x1de400000`): an 8 GB heap region. The alias region keeps 2.75 GB: the sdk's address region node area takes its first 1 GB |
| `romfs_marker Movie/StaffRoll_00.webm` | `game.tsv` | a file of the 1.8.0 RomFS |
| NVN2 API 231.39 | `hle_nvn.c` `nvn_api` | the game requires major 231 and minor ≥ 39 (`main+0xdd0890`, after `nvnDeviceGetInteger` items 0 and 1); its own `sdk` answers 231.39 (`DeviceGetInteger` at `sdk+0x65b680`; Donkey Kong Bananza's sdk answers 231.30). The host gives 231.39 to every Switch 2 SDK, which also meets Donkey Kong Bananza's minor ≥ 30 |

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

## Boot (Mac)

Each step below stopped the boot before its fix:

1. A null memory block in `nnMain` (`CRASH sig=11 addr=0x10 pc=main+0x5dd3ac`): the game allocates a fixed 7.04 GB block from its heap, which the Switch 1 pool could not hold. The Switch 2 pool, 9 GB (`app_pool`, [What it needs](#what-it-needs)).
2. `nn::diag abort: Result 0xe15` in `hid::InitializeNmouse`, then in `chat::Initialize` (GameChat), then `CheckGraphicsSystemInitialized` before the camera context: Switch 2 services the host does not serve. Answered with nothing connected ([Switch 2 mouse, GameChat and camera with nothing connected](../../../docs/reference/services.md#switch-2-mouse-gamechat-and-camera-with-nothing-connected)). The chat camera storage needed NVN2 native images ([NVN2 native images](../../../docs/reference/nvn-objects.md#nvn2-native-images)); the camera session fails "open camera" with the result the game takes as "no camera".
3. `CRASH pc=0` after `vi::SetLayerNextHdrAdjustableBezierMetadata` (`main+0x959ae4`), in the first frames ([Switch 2 HDR metadata on a layer the host never made](../../../docs/reference/services.md#switch-2-hdr-metadata-on-a-layer-the-host-never-made)).
4. GPU out of memory after about 90 frames (`kIOGPUCommandBufferCallbackErrorOutOfMemory`, then the present watchdog; "GPU allocated" 21 to 24 GB): six sparse terrain arrays of 8192x8192x68 with 12 levels (two of 6.1 GB, format 0x50; four of 1.5 GB, ASTC 8x8) in virtual pools the game maps only in part, made as whole textures. Now Metal placement sparse textures, mapped and filled tile by tile from the page table ([Sparse textures in sparse virtual pools as Metal placement sparse textures](../../../docs/reference/nvn-objects.md#sparse-textures-in-sparse-virtual-pools-as-metal-placement-sparse-textures)): after 60 s 26,593 Metal tiles (1.7 GB) in 7 heaps, GPU allocated 14.2 GB, process footprint 12.6 GiB (with the 9 GB pool).

Memory after 60 s (`run11.log`): GPU allocated 14.2 GB, footprint 12.6 GiB, 7 placement heaps of 256 MB.

## Open

- The picture: not yet compared with a capture (the title and the menus are the next milestone).
- `[RED] failed to create compute pipeline: Threadgroup memory size (49152) exceeds the maximum threadgroup memory allowed (32768)`, 3,138 times in 60 s: a compute shader declares 48 KB of shared memory, which Ampere has and an Apple GPU's threadgroup does not (32 KB). Its dispatches are skipped. Which shader, and what it computes, is not identified yet (the pipeline failure does not name its key).
- 63 shaders missing from the cache (`failed to load shader …: cannot open …/b-3c66991eb439/….metallib`, 16 compute and 2 pixel programs with no shader): none of their keys is in the full build's 11,522 libraries, so they are the runtime-only shaders (about 240 by #834) that a `DM_NVN_SHDUMP` run collects (Goal: build order).
- The NVN2 calls reported as unimplemented: `CommandBufferDrawMeshTasks` (302 times in 60 s), `CommandBufferBindShadingRateImage`, `CommandBufferShadingRateImagePalette`; unknown names `nvn*CooperativeVectorMatrix*`, `nvn*PoolWriteDescriptorTableData`.
- `SetRenderEnableConditional: the host draws anyway for now`: `render_cond` is not set yet (the clears are conditional, #834).
- The display link stalled 7 times in the first 50 s (`display link recovered: stalled … ms`), with 253 dropped frames up to frame 600; after that 2 dropped in 300 frames.
- The camera's other commands (29 at the first frames) fail as "no camera"; their meaning is not known.
- `pad_layout` and `app_category` are judgement, not measured.

## Found on this game

- [Sparse textures in sparse virtual pools as Metal placement sparse textures](../../../docs/reference/nvn-objects.md#sparse-textures-in-sparse-virtual-pools-as-metal-placement-sparse-textures)
- [Switch 2 mouse, GameChat and camera with nothing connected](../../../docs/reference/services.md#switch-2-mouse-gamechat-and-camera-with-nothing-connected)
- [Switch 2 HDR metadata on a layer the host never made](../../../docs/reference/services.md#switch-2-hdr-metadata-on-a-layer-the-host-never-made)
- [NVN2 native images](../../../docs/reference/nvn-objects.md#nvn2-native-images)
