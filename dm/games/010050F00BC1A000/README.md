# Resident Evil (`010050F00BC1A000`)

The HD Remaster. Base game 1.0.0 (v0), SDK 5.4.0. Capcom's MT Framework on NVN. Modules `main`, `subsdk0` (the SDK's movie module, NintendoSDK_movie) and `sdk`. Plays on the Mac.

## What it needs

| Setting | Where | Why |
|---|---|---|
| 4 GB layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000`, `sys_resource 0x0` | [`game.tsv`](game.tsv) | the NPDM declares no system resource: `main` sets a heap of at most the pool − 64 MB (a 3,328 MB heap region), as the survey's provisional profile gave it |
| `tex_flush_upload on`, `tex_dirty_upload on`, `depth_mode zero` | `game.tsv` | taken from The Great Ace Attorney Chronicles ([010036E00FB20000](../010036E00FB20000/README.md)), the other MT Framework game; not tried without them |
| `romfs_marker nativeNXx64/sa/NX_64/root.arc` | `game.tsv` | the archive with the engine's root shader package, as in Ace Attorney |

## Files and shaders

- Everything is under `nativeNXx64/`: 3,899 ARC archives (`*.arc`) of **version 9**, beside Opus sound, `.stqr` sound tables and 64 H.264 `.mp4` movies. Version 9 has 64-byte paths and a 0x54-byte directory entry: version 7's fields with a u32 (2 in every entry) before the offset; the first member starts at 0x8000, and every member is a zlib stream, even a 0x10-byte one stored in 0x18 bytes (`shader_split.py` `mtarc_members`).
- Shaders: 19,104 in three shader packages, each the only member of its archive: `sc\NX_64\root` (1,726), `sc\NX_64\Title` (56) and `sc\NX_64\Game` (17,322) in `nativeNXx64/sa/NX_64/{root,Title,Game}.arc`; 19,060 unique (9,530 vertex, 9,530 pixel). The records are not back to back as in Ace Attorney: binding data, a 0x20-byte header naming the control and code lengths, the control section (0x850 bytes on SDK 5), then the code area (`shader_split.py` `mtspk_headed_shaders`). The package's own vertex / pixel combinations are not read, so the vertex shaders are paired with the single-stage cover. All build.

## Status on the Mac

- Boots through the intro and plays a good stretch of the game (rooms, doors, cutscenes) with no rendering or audio problems seen. Not tried on the iPhone.

## Found on this game

- **MT Framework ARC version 9 and its headed SPK records**: [SPK shader package in an MT Framework ARC](../../../docs/reference/shaders.md#spk-shader-package-in-an-mt-framework-arc).
