# Bloodstained: Ritual of the Night

[Games table](../README.md#games)

- **Build:** Switch title `0100BF500207C000`, update 1.60 (`v851968`), main build `461b14e505ab955525dc93e0e908c0bc`.
- **Engine:** Unreal Engine, Nintendo SDK 7.6.0, NVN 53. The v8 PAK holds two v1 shader archives with Zlib-compressed LSLG records.
- **Mac validation:** Played the early ship section on an M5 Max with macOS 27.2 using the NX2Apple provisional build. With both texture upload settings enabled, the minimap and the pink materials rendered correctly in the tested scene. Later areas and iOS are untested.

## Texture uploads

The minimap drew a solid magenta rectangle and character and enemy materials showed magenta. A captured 128×128 ASTC texture sampled by the minimap draw had 10,007 nonzero bytes in guest storage while all 484 blocks in its Metal base mip were zero. The texture carried the sparse flag and was backed by a CPU-visible pool. Enable `tex_flush_upload` to pick up flushed writes and `tex_dirty_upload` to detect writes without a flush. See [texture storage and CPU writes](../../../docs/reference/nvn-objects.md#block-compressed-storage-the-cpu-wrote-before-first-use).

To reproduce the tested scene, load the early ship save, check the minimap and Miriam in the save room, then enter the adjacent combat area to check enemies and impact effects.
