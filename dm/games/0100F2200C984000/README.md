# Mortal Kombat 11 (`0100F2200C984000`)

Update 1.0.26 (`v1703936`) over the base game. NetherRealm's Unreal Engine 3, Wwise and Havok on NVN API 53.313, SDK 9.3.1. The profile comes from YubiSmart Labs' Mac port, which reached fighter selection, local matches and story movies on an Apple M2. [PR #2799](https://github.com/seedds/nxnative/pull/2799)

## What it needs

| Profile setting | Why |
|---|---|
| `main subsdk0 sdk` | NintendoSDK_movie in `subsdk0` handles MP4 extraction and playback. |
| 4 GiB reservation, 256 MiB alias and stack regions, `sys_resource 0` | The NPDM declares no system resource. The SDK uses a normal heap with the Switch application pool. `mem_used` preserves the 2.99 GiB heap observed on the Mac. |
| `depth_mode zero` | The game selects near-zero depth in `nvnDeviceSetDepthMode`. The offline pipeline bases use the same depth mode. |

The profile uses the shared rewriter and host. Fragment output types come from the pipeline's attachments through the [typed-output entry point](../../../docs/reference/shaders.md#typed-outputs-for-every-pixel-shader).

## Files and shaders

- The merged RomFS has 4,621 files (25.92 GiB). Cooked packages are in `Asset/*.xxx`, the 207 MP4s in `Movies/`, and scripts in `MKScript/`.
- The current split reproduces 76,865 shader keys (11,875 vertex, 63,739 pixel, 972 compute, 152 domain and 127 hull). The [UE3 shader reader](../../../docs/reference/shaders.md#unreal-engine-3-shader-caches-oodle-packages) reads the wide package headers and control-first shader records. The 972 translation facts describe compute workgroups and shared memory from the file headers.
- The prewarm manifest and seen shader list come from the M2 menu, fighter-selection, match and story-movie runs. The Mac builder's `--seen-shaders` option uses those recorded keys for a shorter build. Use a full build for routes outside that coverage.

## Mac routes

- Build with `scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> --platform mac` and run the command it prints. Set `DM_METAL_HUD=1` for the performance HUD.
- From the title, go to the main menu, choose Fight and start a local match. Fighter selection exercises the compressed portrait textures. A story movie and its transition to a fight exercise the H.264 and AAC paths.

## Testing

- 2026-11-24, Apple M6, macOS 27.2.1, Xcode 27.2. The first Mac build compiled all 3,903 selected shaders including two pipeline cover shaders, and prepared 3,390 recorded render and compute pipelines with zero failures. The game played its opening movies, reached the title and passed Updating Game Data. A fresh save then presented the game's license prompt.
- The full Mac build compiled all 76,865 shaders, with no failures or lost variants. It wrote the library and matching uniform tables in 38 minutes 48 seconds on the same Mac. The launcher loaded the stamped library. The user's run reached fighter and arena selection, started a local Scorpion versus Sub-Zero match, and exited normally with no `[RED]` lines. The log confirms match startup and frame presentation. Story transitions still need a retest on this build.

## Found on this game

- [Writes into an uncompressed alias of a compressed texture](../../../docs/reference/rendering.md#writes-into-an-uncompressed-alias-of-a-compressed-texture)
- [Typed outputs for every pixel shader](../../../docs/reference/shaders.md#typed-outputs-for-every-pixel-shader)
- [LDC's 16-bit immediate](../../../docs/reference/shaders.md#ldcs-16-bit-immediate-signed-beside-a-register-the-address-alone)
- [Handles in uniform buffers written earlier in the same submission](../../../docs/reference/shaders.md#handles-in-uniform-buffers-written-earlier-in-the-same-submission)
- [Native texture buffers with zero capacity](../../../docs/reference/movies.md#native-texture-buffers-with-zero-capacity)
- [Movie extractor allocations need the SDK graphics allocator](../../../docs/reference/movies.md#movie-extractor-allocations-need-the-sdk-graphics-allocator)
- [Audio decoder history survives a movie seek](../../../docs/reference/movies.md#audio-decoder-history-survives-a-movie-seek)
- [A partial Mac build creates a database at the cache directory path](../../../docs/reference/packing.md#a-partial-mac-build-creates-a-database-at-the-cache-directory-path)

## Open

- iOS, online play, individual DLC, long sessions and full story completion are unverified. The supplied NSP folder has the base and update only.
