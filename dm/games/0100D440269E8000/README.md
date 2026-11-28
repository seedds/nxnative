# Farming Simulator 26: Nintendo Switch Edition (`0100D440269E8000`)

Update 1.0.1.0 (`v65536`), SDK 20.5.4, GIANTS Engine on NVN. Modules `main` and `sdk`. The [PR author](https://github.com/seedds/nxnative/pull/1962) tested the Mac build through the title screen and menus into 3D gameplay. iOS is unverified.

The full shader split and Mac shader build were repeated on macOS 27.2.1 with Xcode 27.2. All 2,209 Metal libraries built. The translator reported unresolved image slots in 46 compute shaders and unsupported `PrmtI` instructions in seven compute shaders. A focused Mac shader rebuild on an M4 Pro with [Maxwell PRMT implemented](../../../docs/reference/shaders.md#maxwell-prmt-byte-permutation) built all seven and reduced the unsupported `PrmtI` diagnostics from 72 to zero. Their runtime impact remains open.

## What it needs

- The 4 GB layout in [`game.tsv`](game.tsv) leaves room for the 3328 MB heap that `main` reserves. The NPDM declares zero system resource memory.
- `shared/shader_switch.sharc` holds 2,209 shader outputs (843 vertex, 1,181 pixel and 185 compute). Of the vertex shaders, 804 have VertexA/B parts. The current merger handles both `c1` layouts in those records.

## Found on this game

- [Maxwell PRMT byte permutation](../../../docs/reference/shaders.md#maxwell-prmt-byte-permutation)

- [SHARC archives in GIANTS Engine](../../../docs/reference/shaders.md#sharc-archives-in-giants-engine)
- [VertexA and VertexB parts merged into one vertex shader](../../../docs/reference/shaders.md#vertexa-and-vertexb-parts-merged-into-one-vertex-shader)
