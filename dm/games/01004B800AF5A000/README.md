# Bloodstained: Curse of the Moon (`01004B800AF5A000`)

Update 1.1.1 (v131072), Nintendo SDK 4.5.0, Inti Creates' engine on NVN. The game loads `main`, `subsdk0` (GLSLC) and `sdk`. On the Mac, initial setup and Stage 1 render at 1280×720 with a lower-left window origin. The game uses the SDK 4 audio renderer at 48 kHz.

## What it needs

- [`game.tsv`](game.tsv) registers update 1.1.1. The NPDM declares no system resource, so virtual address memory is disabled. The game takes a 3.14 GB heap within the standard 3.25 GB heap region for titles without virtual address memory.
- [Runtime GLSLC capture](../../../docs/reference/shaders.md#built-in-shaders-compiled-with-the-games-own-glslc) finds six compiler outputs, with six vertex and five pixel shader keys. The NSP script captures them automatically and generates the shader tables. [`pso_manifest.tsv`](pso_manifest.tsv) records the pipelines seen on the Mac from setup into Stage 1.
- The game uses the standard NSO rewrite and has no game-specific code patches.

## Mac validation

Build with `scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> --platform mac` and use the run command it prints. Set `DM_METAL_HUD=1` to show the HUD. On a fresh save, press A at the initial setup prompt and confirm the title and new game menus to enter Stage 1.

On an Apple M6 (Mac18,5, macOS 27.2.1), Stage 1 held a median frame time of 16.67 ms and p95 of 16.69–16.70 ms while standing still. From frame 1,800 to 4,800, resident memory went from 435 to 433 MB and Metal allocated memory stayed at 230 MB. The real audio output reported zero underruns across the first 16,000 device callbacks.

## Found on this game

- **Resolved:** the host stopped before its first frame. The game's sound thread and the host audio renderer deadlocked on the audio catch-up queue. [A host lock held across a same-core handoff](../../../docs/reference/threads.md#a-host-lock-held-across-a-same-core-handoff) gives the cause and shared host fix.

## Open

- iPhone gameplay, later stages and the base build without the update await testing.
