# L.A. Noire (`0100830004FB6000`)

Update 1.2 (v131072), Nintendo SDK 1.3.1, NVN. The Mac bring-up tested English on an Apple M4 Pro with macOS 27.2.1.

## What it needs

- [The profile](game.tsv) loads `main`, `sdk` and the GLSLC, NVN and `nn::sf` modules it imports. The NPDM declares zero system resource. The game opens 3.14 GiB of the 3.25 GiB guest heap region.
- The shader map includes the WAD arrays, the other loose GLSLC files, the boot-time GLSLC captures and shaders in `main`. The WAD reader extracts shader stages without guessing the original vertex and fragment pairings.

## Found on this game

- **Resolved** — [Direct `irs` connection and status memory](../../../docs/reference/services.md#direct-irs-connection-and-status-memory).
- **Resolved** — [WAD and VFP shader arrays](../../../docs/reference/shaders.md#la-noire-wad-and-vfp-shader-arrays).

## Mac validation

Build with `scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> --platform mac-app --gpu none --no-prebake` and open the resulting app. From a fresh save, start a new game and play to the alley in “Upon Reflection.” Inspect the two officers' faces and uniforms, then open the notebook and check the page text.

In the Mac bring-up, the opening patrol-car scene and first intersection rendered the officers' faces, uniforms and badges. The current profile rebuilt all 8,968 shaders on chain `ee9727a4d1d5` without a shader regression. On this build the player confirmed that the alley characters' materials rendered correctly and that the notebook pages were readable in “Upon Reflection.” The windowed test presented at 1920×1080 with a 33.33 ms median in steady sections. First-use pipeline compilation caused some long frames. The packaged host also booted from the app's bundled RomFS and shader image.

## Open

- The unsigned iOS IPA built with `--romfs separate` and passed archive validation. It needs the 20.4 GiB game image copied once per version after sideloading.
- The base build, later cases, other languages and gameplay on iOS await testing.
