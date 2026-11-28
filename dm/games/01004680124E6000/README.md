# Bloodstained: Curse of the Moon 2 (`01004680124E6000`)

Update 1.3.2 (v327680), Nintendo SDK 10.6.0, Inti Creates' engine on NVN. The Mac run reached the title, menus and Stage 1 in a 1280×720 window with a lower-left origin. Audio goes directly through stereo `nn::audio::AudioOut`.

## What it needs

- [`game.tsv`](game.tsv) registers update 1.3.2 and loads `main`, `subsdk0` (GLSLC) and `sdk`. The NPDM declares no system resource, so virtual address memory is disabled. The game takes a 3.14 GB heap within the standard 3.25 GB heap region.
- [Runtime GLSLC capture](../../../docs/reference/shaders.md#built-in-shaders-compiled-with-the-games-own-glslc) records four compiler outputs from the game, two vertex and two pixel stages. The NSP script captures them at boot. The shader map and [prewarm list](pso_manifest.tsv) cover the Mac route through Stage 1.
- The port uses the existing NSO rewrite, NVN renderer and AudioOut implementation.

## Mac validation

Build with `scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> --platform mac` and use the run command it prints with `DM_METAL_HUD=1`. From a fresh save, confirm the setup prompts with A, choose Game Start on the title screen and start a new file to reach Stage 1.

On Apple M6 (Mac18,5, macOS 27.2.1), Stage 1 held a median frame time of 16.67 ms. From frame 1,500 to 4,500, resident memory went from 447 to 444 MB and Metal allocations stayed at 229 MB. Real stereo output reported zero underruns across the first 14,000 device callbacks.

## Open

- iPhone gameplay, later stages and the base build await testing.
