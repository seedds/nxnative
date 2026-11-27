# Pokémon: Let's Go, Pikachu! (`010003F003A34000`)

Switch update 1.0.2 (`v131072`), Nintendo SDK 5.4.151. Game Freak's engine uses NintendoWare G3d, Ui2d and Vfx, NVN graphics, and Wwise audio. The profile loads `main`, `subsdk0` (GLSLC), `subsdk1` (NintendoSDK_movie), and `sdk`.

## What it needs

The NPDM declares no system resource. Startup requests a 3.14 GB heap, which fits in the 3.25 GB heap region of the standard 4 GB layout.

The split finds 11,160 distinct shaders: 4,710 vertex, 6,411 pixel, and 39 compute. Sources include loose BNSH files, BNSH in GFPAK and SARC archives, and YEBIS effects in `main`. All 11,160 compile for Mac and iOS with Xcode 27.2, with zero unresolved bindings or lost variants. The Mac pipeline prebuild compiles all 4,404 bases. The registered profile packs an unsigned 1.0.2 IPA with the RomFS bundled.

## Frame rate

The framework constructor initializes two frame periods from a table of nanoseconds per frame. At `main+0x38ba8` on 1.0.2 it takes the two-tick entry (33,333,334 ns). The renderer turns the first period into the NVN present interval at `main+0x29ea0`. The application's override at `main+0x7af0` writes the two-tick entry into the simulation period on every update. Timing consumers read that period, including the animation setup at `main+0x66351c`. In a render-only 60 fps test the simulation setter still supplied 33,333,334 ns per update.

The profile's matched cuts select the one-tick entry (16,666,667 ns) in the constructor and the simulation override. The game then requests its own one-tick interval. Both functions must match uniquely and the constructor's relocated two entries must agree before either cut is applied. Pikachu and [Eevee](../0100187003A36000/README.md#frame-rate) share this rule. Both editions' 1.0.0 and 1.0.2 executables match the code and table offline. Each profile opts in because its application's override supplies the simulation period. Builds outside the match retain the game's own timing.

## Mac verification

On Apple M6 with macOS 27.2.1, a fresh save reached controller selection, the VP9/Vorbis opening movie, language selection, Professor Oak's 3D introduction, and the playable bedroom in Pallet Town after choosing the trainer and rival names. The in-game menu wrote a 1 MiB save there. The run had no `[RED]` lines. In the bedroom, one 300-frame window measured 33.34 ms median and 33.83 ms p95, with 2,013 MB resident and 1,547 MB allocated by Metal in the same log interval. The 450 prewarm seeds cover this route and the save menu.

On the same Mac at 1920 × 1080, 1.0.2 reloaded the bedroom save and opened the save menu with both timing cuts active. The game requested interval 1 and supplied 16,666,667 ns to its simulation setter once per update. In the bedroom timing samples the original ran at 30.0 fps (33.34 ms median, 33.73 ms p95) and the patched run averaged 56.5 fps (16.67 ms median, 31.33 ms p95), with 60 fps sustained for 15 seconds in that window. A second patched run with the probes removed averaged 51.6 fps over 14 seconds in the bedroom (16.87 ms median, 34.51 ms p95). These runs had no `[RED]` lines. The cause of the missed ticks is open. Consumers of the fixed step can fall behind wall time during drops. The unsigned 1.0.2 IPA contains both cuts and passed its ZIP integrity check.

The game opened a six-channel audio output that the Mac folded to stereo. Its separate movie output reported startup underruns in the cold run.

In docked mode the game accepts the host's single right Joy-Con mapping for the Mac keyboard after the controller selection. Press J for A, K for B, and U for X to open the in-game menu. WASD moves the stick. The HUD can be enabled with `DM_METAL_HUD=1`.

## Found on this game

- [A comma in the build path stops the shader split](../../../docs/reference/packing.md#a-comma-in-the-build-path-stops-the-shader-split).

## Open

- Gameplay outside the bedroom, physical Joy-Con motion, Poké Ball Plus, Pokémon GO connectivity, and iPhone gameplay still need verification.
