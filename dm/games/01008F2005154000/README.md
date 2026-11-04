# South Park: The Fractured But Whole (`01008F2005154000`)

Update 1.05 (v327680) over the base game, SDK 3.5.1, Ubisoft's Snowdrop engine on NVN, with Havok and Wwise: the same engine generation as [Mario + Rabbids Kingdom Battle](../010067300059A000/README.md). The ExeFS loads `main`, `subsdk0` (GLSLC) and `sdk`; the six supplied DLC NSPs are indices 1–6, each holding only a licence file (2 entries). **Mac: plays from boot through the title and main menus, the opening cinematics, character creation and the prologue's cutscenes to the first interactive scene. iOS remains unvalidated.**

## Memory

- The NPDM declares 16 MB of system resource and `main` sets no heap: the engine takes its memory through address regions in the 4.375 GB alias region.
- **Resolved: 12 GB reservation.** [Separate virtual ranges in Snowdrop's page allocator](../../../docs/reference/memory.md#separate-virtual-ranges-in-snowdrops-page-allocator). The allocator's initializer (`main+0x17fb29c` onwards) reserves 12 GB and splits it at fixed offsets: large allocations at +0 (65,536 pages), 512 KB pages at +4 GB, 64 KB pages at +8 GB. The NSP script's region check measured alias +13.16 GB in nnMain's first 15 s. `patches.py` keeps the layout in 4 GB: 1.5 GB large (24,576 pages), 512 MB of 512 KB pages, 2 GB of 64 KB pages. `free` and the slab paths read the stored range starts, not fixed offsets. The SDK build `8bb65519bc6399ab432e0ba7cdb16e2b` takes Kingdom Battle's node-area cut (1 GB → 64 MB) and 32 MB alignment for regions of 1 GB and more (`dm/tools/sdk_patches.py`).
- **Resolved: 64 KB pool exhausted.** A first 3 GB cut with 512 MB of 64 KB pages aborted 2 s into nnMain: when that pool's bump pointer reaches its end (`main+0x17fb600`), it asks for another 4 GB at the range's end (`+0x17fb640`), a fixed-address reservation the SDK aborts on (Break at `sdk+0xa9dec`). The pool sizes come from a run of the original layout with a 64 GB alias region (`DM_ALIAS_SIZE`), with the pools' fields read by `DM_CTL` memdump at `main+0x4968900 + 0xdea80`: after 90 s (title, logos), 64 KB pages 848 MB (735 MB at 30 s), 512 KB pages 236 MB, large allocations 16,384 pages.
- **Current measurements; later capacity open.** With the 4 GB layout, the regions reach alias +4.23 GB of 4.375 GB; the prologue up to the first interactive scene ran without a growth request. Pool use over sustained play, later areas and battles is not measured. A pool that fills still takes the abort above.

## Files and shaders

- The merged RomFS holds 3,010 files under `camel/sdf/nx/data`: 172 `BERG` data slices (`.sdfdata`) and 2,828 five-byte placeholders, a `.sdftoc` (SDF version 22), a `.sdfver`, two DDS boot images, and the build's own `_FileList.txt` and `_DataPartList.txt`, which list every packed file with its slice, offset and sizes (the offsets are the base game's: the update's slices are shifted).
- **Resolved: no shaders found by the split.** The shaders are GLSLC outputs in `camel/shadercache/nx/*.mcache`, stored as bare zlib streams (a 1 KB window, header `28 15`) without the 20-byte record header of the Mario + Rabbids games: [Shader records in Snowdrop SDF data slices](../../../docs/reference/shaders.md#shader-records-in-snowdrop-sdf-data-slices). The split finds 4,614 stages: 4,072 vertex, 467 fragment and 75 compute. All compile for the Mac.

## Rendering

- `depth_mode` and `pad_layout` follow Kingdom Battle. Menus, text, 2D cinematics, the in-engine cutscenes (the town, the opening street scene, the bathroom) and the character creator draw correctly in the Mac test; no wrong picture was seen.
- `nvnCommandBufferGetControlMemoryFree` is reported unimplemented (red line) on every frame; it has shown no effect so far. Open.

## Mac testing

Tested headless on 2026-10-31 with the registered update and all six DLC packages, the Mac shader libraries of the NSP script's Mac app and `DM_CTL` input: title screen (A), main menu (PLAY, NEW GAME), the disclaimer and opening cinematic (hold B to skip), character creation (NEXT, Choose Difficulty, CREATE), the arrival and opening cutscenes, and the first interactive scene, whose L / R tutorial prompt reacts to input.

## Found on this game

- [Shader records in Snowdrop SDF data slices](../../../docs/reference/shaders.md#shader-records-in-snowdrop-sdf-data-slices): bare zlib cache files.
- [Separate virtual ranges in Snowdrop's page allocator](../../../docs/reference/memory.md#separate-virtual-ranges-in-snowdrops-page-allocator): pool bounds as separate immediates, and a 64 KB pool that needs 2 GB.

## Open

- Play into the first battle and beyond; measure the pools over sustained play.
- Audio was silent in the headless test (no audio device requested); check it with a window.
- iOS testing is performed by the user.
