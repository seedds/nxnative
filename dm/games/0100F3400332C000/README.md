# Xenoblade Chronicles 2

Japanese/Asian base title ID `0100F3400332C000`, update 3.0.0 (`v983040`), SDK
3.5.0, Monolith's engine, NVN. [game.tsv](game.tsv) lists the three modules and
their build IDs. Opening gameplay was tested on Mac and iPhone. Later gameplay
and DLC are outside these checks.

## Choose the 3D world resolution

| Mode | Launcher settings | Effect |
| --- | --- | --- |
| Game's own (default) | Render scale: Game's own | Keeps the game's sizing and ignores the launcher DRS overrides. |
| Fixed 720p | Render scale: 720p; Dynamic resolution off | Keeps the world at 1280×720. |
| 720p with bounded DRS | Render scale: 720p; Dynamic resolution on | The game's GPU-timing controller chooses the world size inside the selected limits (80–100% by default). |

The limits are fractions of the selected target. The lower size rounds up and
the upper size rounds down to the game's eight-pixel grid. If no aligned size
fits, the first aligned size at or above the lower limit is used. Only the 3D
world changes; the 1280×720 presentation, HUD and frame-rate behavior stay as
they were. MetalFX, if used, scales the presented image separately.

## Measured in the opening battle

Start a new game and advance past the opening movies to the first battle.
Measurements used the geometry viewport and matching color and depth attachments.

| Mac (M4, macOS 27.2) | 3D world |
| --- | --- |
| Game's own, handheld | 960×544, matching stock, even with DRS overrides set |
| Fixed 720p, handheld | 1280×720 |
| DRS 80–85%, handheld, normal GPU timing | 1088×608; controller stayed at stage 0 |
| DRS 80–85%, handheld, simulated over-budget timing | 1024×576; controller reached stage 5 |

The simulated timing used a separate diagnostic build. The normal run reached
the configured ceiling, not the floor. A separate 80–80% docked Mac run measured
a 1024×576 world while MetalFX scaled the 1280×720 surface to 1920×1080.

| iPhone 15 Pro (iOS 27.0.1), docked mode | 3D world |
| --- | --- |
| Game's own | 1280×720 |
| Fixed 720p | 1280×720 |
| DRS 80–100% | 1280×720; the bounds were applied, but the scene did not reduce resolution |

## Profile and rewriter

- The profile keeps the Switch `0xCD500000`-byte application pool and the tested
  memory settings: a 4 GiB guest reservation, 256 MiB alias and 256 MiB stack.
  The NPDM declares no system resource. The render patch does not change these
  settings.
- `bf2.ard` identifies the RomFS. The NSP packer checks its shader split against
  the profile's shader map, translation facts and vertex/pixel pairs. Shader
  programs and compiled libraries are produced locally from the user's game files.
- [patches.py](patches.py) checks main build ID
  `feee7afa8a1541bd67aed1a0061fbb57` and the original instructions at four sites.
  The hooks choose the native world path, set the bounds after the stock mode
  factors and enforce them on the resize grid. They use the existing render-only
  state page, leaving the native timing controller in charge. The code gate is
  the exact build and instructions, not the release's region name.
