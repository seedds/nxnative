# Pokémon: Let's Go, Pikachu! (`010003F003A34000`)

Switch update 1.0.2 (`v131072`), Nintendo SDK 5.4.151. Game Freak's engine uses NintendoWare G3d, Ui2d and Vfx, NVN graphics, and Wwise audio. The profile loads `main`, `subsdk0` (GLSLC), `subsdk1` (NintendoSDK_movie), and `sdk`. The shared host and rewriter handle these modules with no game-specific patches.

## What it needs

The NPDM declares no system resource. Startup requests a 3.14 GB heap, which fits in the 3.25 GB heap region of the standard 4 GB layout. The game sets present interval 2. The Mac run uses the game's 30 fps cadence at 1920 × 1080.

The split finds 11,160 distinct shaders: 4,710 vertex, 6,411 pixel, and 39 compute. Sources include loose BNSH files, BNSH in GFPAK and SARC archives, and YEBIS effects in `main`. All 11,160 compile for Mac and iOS with Xcode 27.2, with zero unresolved bindings or lost variants. The Mac pipeline prebuild compiles all 4,404 bases. The registered profile packs an unsigned 1.0.2 IPA with the RomFS bundled.

## Mac verification

On Apple M6 with macOS 27.2.1, a fresh save reached controller selection, the VP9/Vorbis opening movie, language selection, Professor Oak's 3D introduction, and the playable bedroom in Pallet Town after choosing the trainer and rival names. The in-game menu wrote a 1 MiB save there. The run had no `[RED]` lines. In the bedroom, one 300-frame window measured 33.34 ms median and 33.83 ms p95, with 2,013 MB resident and 1,547 MB allocated by Metal in the same log interval. The 450 prewarm seeds cover this route and the save menu.

The game opened a six-channel audio output that the Mac folded to stereo. Its separate movie output reported startup underruns in the cold run.

In docked mode the game accepts the host's single right Joy-Con mapping for the Mac keyboard after the controller selection. Press J for A, K for B, and U for X to open the in-game menu. WASD moves the stick. The HUD can be enabled with `DM_METAL_HUD=1`.

## Found on this game

- [A comma in the build path stops the shader split](../../../docs/reference/packing.md#a-comma-in-the-build-path-stops-the-shader-split).

## Open

- Save reload, gameplay outside the bedroom, physical Joy-Con motion, Poké Ball Plus, Pokémon GO connectivity, and iPhone gameplay still need verification.
