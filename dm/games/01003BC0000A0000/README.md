# Splatoon 2

Base title ID `01003BC0000A0000`, American release 5.5.2 (`v3342336`),
SDK 3.4.1, Nintendo EPD's sead / agl / gsys engine, and NVN 53.313.
The application loads `main` and `sdk`. [Build IDs and profile settings](game.tsv).

## Current status

On an Apple M6 Mac mini (16 GB, macOS 27.2.1), the game renders character
creation and the opening tutorial and plays audio. The first shader subset
left the tutorial's 3D scene black behind the instructions. The complete
Mac library renders the alley, Inkling, reticle and balloon. A virtual
controller tilt raises the view and advances to the ZR shooting prompt.
Standing at the aiming prompt, frame time was 16.67 ms p50 and 16.73 ms p95
over a 300-frame interval. The Mac app carries the complete shader library.

The host logs one same-pass feedback warning for `Blitz_Proc` shader
`0be625d3c994c796`, which binds a 2D texture to an array input while drawing
to it. The warning also appears with the complete library and does not
prevent the aiming scene from rendering. Its effect on ink rendering, later
single-player areas, DLC gameplay, multiplayer and iOS runtime remain open.

## Profile and controls

The NPDM declares no system resource. Startup requests a `0xC9400000`-byte
heap in the profile's 4 GiB guest reservation. The full virtual controller
layout gives iPhone players a right stick for aiming. The Mac tutorial uses
gyro input. On a keyboard, enable **Controls → Right Stick Motion** with
⌘⇧G and use Shift–arrow keys to tilt the virtual controller. WASD moves,
J confirms, and C fires.

## Files and shaders

- The merged RomFS contains 10,141 game files, about 5.43 GiB. The supplied
  packages expose DLC indices 1, 2, 5, 100 and 101. Startup mounts the
  content image for index 101.
- The shader split finds 74,547 keys and 51,475 variation pairs, including
  19 compute and 15 geometry shaders. The three agl fallback archives
  contribute no Switch shaders.

## Found on this game

- [System gameplay recording with a work buffer](../../../docs/reference/services.md#system-gameplay-recording-with-a-work-buffer)
  prevented startup until the shared host recording hook handled the older SDK entry point.
- [SARC byte order and agl fallback archives](../../../docs/reference/shaders.md#sarc-byte-order-and-agl-fallback-archives)
  fixes the split without changing the generated shader tables or packages.
