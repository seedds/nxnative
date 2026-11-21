# South Park: The Fractured But Whole (`01008F2005154000`)

Update 1.05 (v327680) over the base game, SDK 3.5.1, Ubisoft's Snowdrop engine on NVN, with Havok and Wwise. The ExeFS loads `main`, `subsdk0` (GLSLC) and `sdk`. The six supplied DLC NSPs are licence packages at indices 1–6. The current Mac build reaches character creation with the preview rendered. The current iOS build needs a device retest.

## Memory

- The NPDM declares 16 MB of system resource and `main` sets no heap: the engine takes its memory through address regions in the 4.375 GB alias region.
- **Resolved: 12 GB reservation.** The unmodified game reached alias +13.69 GB in the NSP script's first 15 s in `nnMain`. Its allocator initializer reserves three adjacent 4 GB ranges and sets the large allocator to 65,536 pages. `patches.py` matches the initializer and its callees in both 1.0 and 1.05. It moves the bounds and caps the large allocator together to fit inside 4 GB: 1.5 GB large (24,576 pages), 512 MB for 512 KB pages and 2 GB for 64 KB pages. The free and slab paths read the stored range starts. The older SDK VammManager matches the shared rule for a 64 MB node area and 32 MB alignment for regions of 1 GB or more. [Separate virtual ranges in Snowdrop's page allocator](../../../docs/reference/memory.md#separate-virtual-ranges-in-snowdrops-page-allocator).
- **Resolved: 64 KB pool exhausted.** A first 3 GB cut with 512 MB of 64 KB pages aborted 2 s into nnMain: when that pool's bump pointer reaches its end (`main+0x17fb600`), it asks for another 4 GB at the range's end (`+0x17fb640`), a fixed-address reservation the SDK aborts on (Break at `sdk+0xa9dec`). The pool sizes come from a run of the original layout with a 64 GB alias region (`DM_ALIAS_SIZE`), with the pools' fields read by `DM_CTL` memdump at `main+0x4968900 + 0xdea80`: after 90 s (title, logos), 64 KB pages 848 MB (735 MB at 30 s), 512 KB pages 236 MB, large allocations 16,384 pages.
- **Current measurements.** The compact Mac run reached alias +4.23 GB of 4.375 GB. PR #843's earlier prologue run reached the first interactive scene with this layout. A full pool can still hit the original 4 GB growth request. Peak use during later areas and battles remains open.

## Files and shaders

- The merged RomFS holds 3,010 files under `camel/sdf/nx/data`: 172 `BERG` data slices (`.sdfdata`) and 2,828 five-byte placeholders, a `.sdftoc` (SDF version 22), a `.sdfver`, two DDS boot images, and the build's own `_FileList.txt` and `_DataPartList.txt`, which list every packed file with its slice, offset and sizes (the offsets are the base game's: the update's slices are shifted).
- **Resolved: no shaders found by the split.** The shaders are GLSLC outputs in `camel/shadercache/nx/*.mcache`, stored as bare zlib streams (a 1 KB window, header `28 15`) without the 20-byte record header of the Mario + Rabbids games: [Shader records in Snowdrop SDF data slices](../../../docs/reference/shaders.md#shader-records-in-snowdrop-sdf-data-slices). The split finds 4,614 stages: 4,072 vertex, 467 fragment and 75 compute. All compile for the Mac.

## Rendering

- The game sets NVN near depth to zero and uses the full pad layout.
- **Resolved: missing character preview.** The preview's vertex streams declared a range beyond their pool, and the host skipped the draws even though their fetches fit. The rebuilt renderer drew the character in the same menu. [A vertex stream bound past its memory pool](../../../docs/reference/rendering.md#a-vertex-stream-bound-past-its-memory-pool).
- **Resolved on Mac: black loading screen and flickering title.** The same oversized binding affected the indexed UI draws and caused the startup draw warning. Captures show the game submitting the UI draws even on black frames. The renderer now checks the mapped vertex range using the CPU-readable indices. The loading indicator and title render again. [A vertex stream bound past its memory pool](../../../docs/reference/rendering.md#a-vertex-stream-bound-past-its-memory-pool).

## Mac testing

On 2026-11-21 the Apple M6 Mac mini (macOS 27.2.1) passed the title and opening cutscenes in a window with Metal HUD, audio output and the iPhone-sized alias region. The title held about 30 fps, all six DLC licences mounted and the game wrote `profile.sys` to the test save folder. The rebuilt renderer reached character creation and showed the preview. The earlier Mac run also reached the first interactive scene. [PR #843](https://github.com/seedds/nxnative/pull/843).

Press A on the title to reach Play and New Game. Hold B to skip the opening cinematic. In the character creator, choose Next, a difficulty and Create.

## Frame rate

Snowdrop requests NVN present interval 2 (30 fps at 60 Hz). The profile uses the host's existing display pacing to default to 60 fps. The launcher's **Frame rate** setting can restore 30 fps. [Present interval in display mode](../../../docs/reference/frame-rate.md#a-present-interval-that-is-the-games-only-limit-in-display-mode).

On the Apple M6 Mac mini, 1.05 reached about 60 fps through most of the opening, with a brief dip before returning to 60 fps. The original timing held about 30 fps. The visible Mac playtest reported no timing problems. The updated Mac bundle was checked at startup with both frame-rate settings. iOS has not been tested at 60 fps.

## iOS build

The unsigned separate-RomFS IPA builds all 4,614 shaders and includes the six DLC licence images. The IPA's RomFS identity matches the image from the 1.05 build. An earlier port ran on an iPhone 15 Pro Max. [PR #662](https://github.com/seedds/nxnative/pull/662).

## Found on this game

- [Shader records in Snowdrop SDF data slices](../../../docs/reference/shaders.md#shader-records-in-snowdrop-sdf-data-slices): bare zlib cache files.
- [Separate virtual ranges in Snowdrop's page allocator](../../../docs/reference/memory.md#separate-virtual-ranges-in-snowdrops-page-allocator).
- [NVN command buffer control memory](../../../docs/reference/nvn-objects.md#control-memory-capacity-queries).
- [A vertex stream bound past its memory pool](../../../docs/reference/rendering.md#a-vertex-stream-bound-past-its-memory-pool).
- [A save folder that cannot clone directories](../../../docs/reference/services.md#a-save-folder-that-cannot-clone-directories).
- [A hand-copied RomFS.img is not found on iOS](../../../docs/reference/packing.md#a-hand-copied-romfsimg-is-not-found-on-ios).

## Open

- Play into the first battle and beyond; measure the pools over sustained play.
- Retest the current IPA on an iPhone.
