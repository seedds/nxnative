# Pokémon: Let's Go, Eevee! (`0100187003A36000`)

Switch update 1.0.2 (`v131072`). Game Freak's engine uses NintendoWare G3d,
Ui2d and Vfx, NVN graphics, and Wwise audio. It shares the Nintendo SDK 5.4.151,
GLSLC and movie modules with [Let's Go, Pikachu!](../010003F003A34000/README.md).
Eevee has its own main executable and shader tables.

## What it needs

The NPDM declares no system resource. Startup requests a 3.14 GiB heap, which
fits the 3.25 GiB heap region of the standard 4 GB layout. The profile loads
`main`, `subsdk0` (GLSLC), `subsdk1` (NintendoSDK_movie), and `sdk`.

The split finds 11,156 distinct shaders (4,708 vertex, 6,409 pixel, and 39
compute). Sources include loose BNSH files, BNSH in GFPAK and SARC archives,
and shaders embedded in `main`. Xcode 27.2 builds all 11,156 libraries for
both Mac and iOS with zero unresolved bindings or lost variants. The Mac
prebuild compiles all 4,401 pipeline bases.

## Mac verification

On Apple M6 with macOS 27.2.1, a fresh save reached controller selection, the
VP9/Vorbis opening movie, the 3D introduction, and the playable bedroom in
Pallet Town after trainer and rival setup. The in-game menu committed a 1 MiB
save, and a second launch with the registered profile reloaded it into the
bedroom. Both logs had zero `[RED]` lines. The 446 prewarm seeds cover the
fresh-save route and the save menu. The reload resolved all its prewarm keys.

At the original 30 fps target, standing in the bedroom for about a minute at
1920 × 1080, Metal allocated 1,551 MB and the host settled to about 2,020 MB
resident. The game opened six-channel audio, which the host folded to stereo
for the Mac's output device.

The docked controller screen accepts the Mac keyboard as a single right
Joy-Con. J is A, K is B, U opens the in-game menu, and WASD moves the stick.

## Frame rate

The profile selects 60 fps through the
[timing rule shared with Let's Go, Pikachu!](../010003F003A34000/README.md#frame-rate).
The rule changes the game's presentation and simulation periods together from
33,333,334 ns to 16,666,667 ns. Both editions' 1.0.0 and 1.0.2 executables match
both complete functions and the relocated period table offline. Builds that
fail those checks retain the original timing.

On Eevee 1.0.2, a presentation-only override left the simulation setter at
33,333,334 ns per update. With both cuts, the game requested a one-tick present
interval and the setter received 16,666,667 ns once per update.

On Apple M6 with macOS 27.2.1, the same Pallet Town bedroom save at 1920 × 1080
ran at 30.0 fps with stock timing (33.34 ms median and 33.74 ms p95). The
patched run delivered 900 presents over 900 display ticks (60 fps over 15
seconds), with 16.68 ms median and 17.11 ms p95 guest frame time. The
comparison runs had zero `[RED]` lines. The simulation uses a fixed step,
so it can fall behind real time if a device cannot sustain the frame rate.

## iOS package

The registered profile builds an unsigned 1.0.2 IPA with bundled RomFS, the
iOS shader library, and the prewarm list. The 60 fps IPA passed its archive
integrity check, and both timing instructions were verified in the embedded
game executable.

## Open

Later gameplay, physical Joy-Con motion, Poké Ball Plus, Pokémon GO
connectivity, and iPhone gameplay still need verification.
