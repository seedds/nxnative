# 13 Sentinels: Aegis Rim, Asian release (`01008D7016438000`)

Base game 1.00 (v0), SDK 11.4.3: the Japanese / Korean / Chinese cartridge and eShop release, under its own title ID. Vanillaware's own engine on NVN, as in the western release ([01003FC01670C000](../01003FC01670C000/README.md)), whose note covers the engine. Modules `main`, `subsdk0` (NintendoSDK_movie 11.4.3) and `sdk`. `subsdk0` and `sdk` are the western builds; `main` (`31194702…`) is not the western 1.10's (`4b632573…`), so the NSP script's lookup by main build ([`same_build`](../../../scripts/build_ipa_from_nsp.py)) does not find the western profile for it. Plays on the Mac from the base NSP alone (packed with `--platform mac`): 60 fps with sound, saving and loading (PR #161). Not tested on the iPhone.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `alloc_big_align on` | [`game.tsv`](game.tsv) | the engine's ring heaps are cut from a memory pool at 64 KB alignment; without it they do not fit the pool ([the western note, Memory](../01003FC01670C000/README.md#memory)) |
| 4 GB layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | `game.tsv` | as the western profile: no system resource in the NPDM, the game sets a heap |
| `sys_resource 0x0` | `game.tsv` | the NPDM declares no system resource (the NSP script writes it from the NPDM) |
| `depth_mode zero` | `game.tsv` | the game sets the near = 0 depth mode, so the pipeline bases made before it runs (the NSP script's Mac prebuild, and a [pipeline archive made offline](../../../docs/development.md#the-pipeline-archive-made-offline) for the iPhone when one is built with `DM_PSO_GPUS`) take its vertex shaders' `main0_z` entries (the host itself follows the mode the game sets) |

One cut in [`patches.py`](patches.py) (below), no `sdk_patches.py` cut for its `sdk` build (`2d369d99…`), no `pad_layout` (the iPhone pad starts in the 2D layout). The rest of the profile is the NSP script's provisional one.

## Files and shaders

- The data is in CRI CPK archives as in the western release; `ROBO_CN.CPK` is the `romfs_marker`.
- Shaders: GLSLC outputs in `main`, with the same keys as the western release's (all 751 `shader_map.tsv` rows; `shader_pairs.tsv`, `shader_facts.tsv` and `bf_want.tsv` are identical), at other offsets in `main`.

## Found on this game

No host change was needed. One cut of the game's own code:

- **A crash at the first frame when the start is slow** (`SIGSEGV` at `main+0x309128`, about 9 s after the start, before the logo). At the end of a frame the main thread waits for the render thread only as long as the frame's time lasts (`main+0x30c194`), then goes on without it: a skipped frame. The render thread's first submission waits on the host for the pipeline archive and the prepare step. From an internal disk that is 0.1 s. From a library folder on a USB 2.0 disk (20 to 40 MB/s, a 131 MB archive) it was 7 s and more, the main thread meanwhile ran on (reading the next screens' files from the same disk, which made the wait longer), and the render thread's first draws bound a texture entry of index −1. What the main thread did to that texture meanwhile was not traced. Measured on a Mac with an M2 and 8 GB: 13 of 13 starts with a wait of 7 s or more crashed, none of 8 below 6.5 s. [`patches.py`](patches.py) cuts the three branches that end the wait, so the main thread waits until the render thread is done: 9 of 9 starts ran, with waits of 2 to 3.5 s (no start with the cut had a wait of 7 s, since the main thread no longer reads from the disk meanwhile). A frame the render thread cannot finish in time now makes the game slower instead of being skipped.

## Open

- Not tested on the iPhone.
- `pso_manifest.tsv` is not recorded yet (no prewarm).
- Only the base game was packed; no update is known.
- Whether the western base game's own `main` (v0) is this build was not checked; the western profile registers only its 1.10.
