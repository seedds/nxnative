# Resident Evil 4 (`0400569027BF4000`)

A Switch 2 game: update v131072, SDK 21.4.3 built for the Switch 2 (`Ounce`), NVN2. Capcom's RE Engine, with Havok and Wwise. Modules `main`, `subsdk2` (`NintendoSDK_movie`) and `sdk`; `subsdk0` (C / libc) and `subsdk1` (`NintendoSDK_DLSS`) are not loaded. It plays on the Mac: from an empty save through the first-start screens, the title, New Game (Standard), the opening movie and cutscene, to walking the forest path of the first chapter. Only v131072 has been run; the base game has not.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000`, `app_pool 0x240000000` | [`game.tsv`](game.tsv) | the Switch 2's 9 GB pool; `main` sets a heap, which the heap check sized at 0.12 GB |
| `sys_resource 0x1400000` | `game.tsv` | the NPDM declares 20 MB, so virtual address memory is on |
| `modules main subsdk2 sdk` | `game.tsv` | `main` takes nothing from `subsdk0` and `subsdk1` that the host does not supply; it calls the movie library in `subsdk2` |
| `romfs_marker re_chunk_000.pak`, `zip_store .pak:.metallib` | `game.tsv` | the RomFS is RE Engine PAKs (26.5 GB with the update's patch PAK) |
| `shader_groups re_chunk=sdf` | `game.tsv` | the shaders are [SDF definitions inside the PAKs](../../../docs/reference/shaders.md#sdf-definitions-in-re-engine-paks), in a third layout |
| `depth_mode zero` | `game.tsv` | the game calls `nvnDeviceSetDepthMode` with near = 0 (`depth mode: near=0` in the log) |

There is no game-specific patch and no rewriter cut. The profile is the NSP script's guess from the survey, with its comments rewritten; only v131072's build row exists.

## Shaders

42,313 in [`shader_map.tsv`](shader_map.tsv): 18,248 pixel, 15,705 vertex, 7,568 compute, 680 mesh, 49 domain, 33 hull, 30 geometry, from 23 SDF files and one OCIO file. On chain `6e1d6cb7b703` all build for the Mac; a full Mac build with nothing cached took about 35 minutes on the M6.

- Resolved: 7,225 pixel shaders read an attribute at each vertex of the triangle ([LDTRAM](../../../docs/reference/shaders.md#ldtram-an-attribute-at-each-vertex-of-the-triangle)); before, 7,246 shaders did not build and the draws with them were skipped. In the first minutes of play 112 of them were used, and the forest path draws whole.
- Resolved: 21 compute shaders with [YIELD](../../../docs/reference/shaders.md#yield).

## Driving it on the Mac

`python3 scripts/build_ipa_from_nsp.py <folder> --platform mac` builds the Mac shaders and the host and prints the run command; its last step, the pipeline prebuild, can be stopped, and `DM_PSO_PREPARE=0` skips the prepare step at boot. The first start asks for the brightness, an age (any adult age) and analytics consent ("I do not consent" works). The title then shows a series of notices of bonus content (The Mercenaries, Separate Ways, costumes, a deluxe weapon, the treasure map), each closed with A. Main Story → New Game → Standard starts the opening movie (H.264 1080p, decoded by VideoToolbox) and then a real-time cutscene, which + pauses and X skips. In play, the left stick walks and B crouches.

Measured on the M6 (16 GB), in a 1280x720 window: 60 fps at the title (frame time p50 16.67 ms) and on the forest path (p50 16.66 ms, GPU p50 12.75 ms), with one hitch of 1.1 s while a pipeline compiled; the host's footprint was 6.8 GB in play.

## Found on this game

- The rewriter could not place patch areas for `main`, 185 MB of text with 90 MB of data after it ([Hops for sites neither patch area reaches](../../../docs/reference/rewriter.md#hops-for-sites-neither-patch-area-reaches)).
- The shader split found no shaders: its SDF stage records have 0x60-byte headers ([SDF definitions in RE Engine PAKs](../../../docs/reference/shaders.md#sdf-definitions-in-re-engine-paks)).
- A crash after graphics initialize, in `nn::vi::InitializeWithVrrLfcEnabled` ([`vi::InitializeWithVrrLfcEnabled`](../../../docs/reference/services.md#viinitializewithvrrlfcenabled)).
- LDTRAM ([an attribute at each vertex of the triangle](../../../docs/reference/shaders.md#ldtram-an-attribute-at-each-vertex-of-the-triangle)); its shaders read the third perspective barycentric coordinate, which the translator had computed as 1 - x - y ([the barycentric coordinates](../../../docs/reference/shaders.md#ipa-at-an-offset-and-the-barycentric-coordinates)).
- [YIELD](../../../docs/reference/shaders.md#yield) in 21 compute shaders.
- `nvnCommandBufferSetRepresentativeFragmentTest` was unimplemented ([Representative fragment test](../../../docs/reference/nvn-objects.md#representative-fragment-test-nvncommandbuffersetrepresentativefragmenttest)).

## Open

- Gameplay beyond the first minutes of the forest path: the village, combat, the menus in play, sound by ear.
- The red lines left in play, each once: `nvnCommandBufferSetFloatBlendClamp 1` (as on [PRAGMATA](../0400C76024710000/README.md)), a texture view that starts at mip 2 of a one-level texture, a constant buffer 1 not bound for one shader at frame 0, a pushed buffer address in no memory pool, and textures read through a pool table of another type, which the host leaves out (a 64x64 one; 1x1 placeholders, nine times in the opening cutscenes).
- The iPhone: no IPA has been built. Its 7,225 LDTRAM pixel shaders compile as MSL 4.0, which needs iOS 26.
- The prewarm list (`pso_manifest.tsv`) is empty.
