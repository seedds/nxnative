# Hello Kitty Party Land (`01001340271EC000`)

Update 1.0.2 (v131072), SDK 21.4.4 (its own `sdk` build). Unity 6000.3.11f1 IL2CPP with CRIWARE movies (USM), Addressables and Pia, on NVN. Modules `main`, `subsdk0` (the SDK's movie library) and `sdk`. Plays on the Mac and on the iPhone at 60 fps, where the Switch runs gameplay at 30 (IPA 2.7 GB, 2,936,235,769 bytes).

On the Mac (M3 Pro, macOS 27.0, 2026-11-01): about an hour of play, the menus, the tutorial and minigames, with sound (six-channel AudioOut, no underruns). 60 fps in menus and gameplay (frame time p50 16.66 ms, p95 17.2 ms; GPU under 2 ms a frame), the minigames at their intended speed. About 2 GB resident and 0.5 GB of Metal allocations.

On the iPhone (iPhone 15 Pro Max, iOS 27.0.1, 2026-11-01): about 30 minutes, a few minigames, 60 fps with the cut, on par with the Mac.

It needs two tool changes, each in a PR of its own: the rewriter's split of two patch areas within the back area's reach (`main` has 151.8 MB of text; [PR #856](https://github.com/seedds/nxnative/pull/856)) and Unity 6000.3's `ComputeShader` layout in `unityfs.py` (the GPU skinning kernels; [PR #857](https://github.com/seedds/nxnative/pull/857)). See [Found on this game](#found-on-this-game).

## What it needs

| Setting | Where | Why |
|---|---|---|
| 60 fps: the game's 30 fps mode → its 60 fps mode | [`patches.py`](patches.py), `main` build `ca82f3ab…`, `main+0x863b318` / `+0x863b31c` / `+0x863b4ec` / `+0x863b4f8` | gameplay switches to 30 fps; see [Frame rate](#frame-rate) |
| `sys_resource 0x1000000` | [`game.tsv`](game.tsv) | the NPDM declares 16 MB of system resource, so `IsVirtualAddressMemoryEnabled` is true |
| 5 GB block: `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | `game.tsv` | the block of the other Unity games with virtual address memory (4.375 GB alias region) |

## Frame rate

- The game's frame-rate class has `SetMode(int)` at `main+0x863b2a0`: 1 calls `QualitySettings.set_vSyncCount(1)` and sets a mode field (its statics +0) to 1, 0 calls `set_vSyncCount(2)` and sets it to 0, and 2 does as 1 with one more call (`main+0x863b580`). Each gameplay section calls `SetMode(0)` when it starts and `SetMode(1)` when it returns to the menus (nine pairs), so on the Switch the menus run at 60 fps and gameplay at 30, as an emulator shows.
- **Current: 60 fps everywhere.** `patches.py` makes the 30 fps paths of `SetMode(0)` and of the setter at `main+0x863b4a0` the 60 fps ones: `vSyncCount` 1 and the field 1, so the field stays consistent with the rate. Nothing in `main` calls the field's getter (`main+0x863b6a0`) directly. Over about an hour on the Mac and 30 minutes on the iPhone the minigames kept their intended speed at 60: they step by elapsed time.
- Unity's `Application.set_targetFrameRate` is called only from engine code, not the game's; no engine limit holds gameplay at 30 once the count is 1.

## Memory

- As BALL x PIT ([010086A022444000](../010086A022444000/README.md)): 16 MB of system resource, no heap (`main` never sets one), Unity 6 reserving and committing its memory itself in the alias region, and the 5 GB block.
- **Current: no SDK cut.** The SDK-wide node-area cut ([The SDK's address-region node area](../../../docs/reference/memory.md#the-sdks-address-region-node-area)) and the 2 MB alignment rule match no function in this SDK 21.4.4 `sdk` build (`148394139d9d26ca8afd5c9ba5d30601`; `sdk.match.tsv`), and neither platform needs them: the Mac and the iPhone 15 Pro Max ran the menus, the tutorial and minigames without them.
- Measured on the Mac (menus, the tutorial and minigames): the alias region's high-water mark +2.75 GB of 4.375 GB, 1.26 GB mapped.

## Rendering

- The game draws into 1920x1080 window textures and crops the window (`nvnWindowSetCrop`) to 1280x720 at boot and 1920x1080 after.
- **Open:** `nvnTextureViewCompare` is unimplemented (`[RED] NVN unimplemented function called`, a few times a frame at most; 42 lines in one run). No wrong picture has been traced to it.

## Files and shaders

- 12,739 shaders (6,321 vertex, 6,325 pixel, 92 compute, one geometry), 11 of them the player's own GLSLC outputs in `main`; everything is under `Data/` (`shader_groups /Data/=data`). The Addressables bundles are in `StreamingAssets/aa/Switch`, the movies are CRIWARE USM files.
- The loose serialized files (`globalgamemanagers.assets`, `resources.assets`, `Resources/unity default resources`) have no type trees. Their `Shader` objects borrow the tree the bundles carry ([Unity 6 Shader objects](../../../docs/reference/shaders.md#unity-6-shader-objects)); their `ComputeShader` objects, Unity's `Internal-Skinning` and `Internal-BlendShape` among them, need `unityfs.py`'s Unity 6000.3 layout. Without it the characters drew as huge stretched triangles.
- `romfs_marker` is the opening movie, `Data/StreamingAssets/aa/Switch/movieassets_assets_assets/app/movieresources/opening.usm_data`.
- No prewarm list is recorded yet (`pso_manifest.tsv` is empty); the pipeline prebuild covers first sights.

## Found on this game

- `main`'s 151.8 MB of text and 98 MB of ro, data and bss after it: the split of the [two patch areas](../../../docs/reference/rewriter.md#two-patch-areas-for-a-text-longer-than-a-branch-reaches) has to stay within the back area's reach ([PR #856](https://github.com/seedds/nxnative/pull/856)).
- Stripped Unity 6000.3 `ComputeShader` objects, hash `43a0948a…`: [Unity ComputeShader objects](../../../docs/reference/shaders.md#unity-computeshader-objects) ([PR #857](https://github.com/seedds/nxnative/pull/857)).
