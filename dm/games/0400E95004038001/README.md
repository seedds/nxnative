# Xenoblade Chronicles 2 – Nintendo Switch 2 Edition (`0400E95004038001`)

Program ID `0400E95004038001`, display version 3.0.1 (`v0`), SDK 21.4.4 built for the Switch 2 (`Ounce`),
Monolith's engine, NVN2 with DLSS. Modules: `main`, `subsdk0` and `subsdk3` (two builds of the SDK's
movie module), `sdk`; `subsdk1` (`NintendoSDK_DLSS`) and `subsdk2` (the C library) are shipped but
`main` imports nothing from them the host does not supply. The `sdk`, `subsdk0` and `subsdk3` are
Xenoblade Chronicles: Definitive Edition's Switch 2 Edition's (`0400FF500E34A001`) builds.
The Switch 1 game is [its own profile](../0100E95004038000/README.md).

## Current status

Mac gameplay tested (2026-11-18, Apple M6, macOS 27.0.1, a Mac app from
`scripts/build_ipa_from_nsp.py --platform mac-app`): title screen, New Game on Normal, the
opening cutscenes, the salvage dive, the King Crustip battle with its tutorials, Chapter One's
cutscenes and the field in Argentum (Goldmouth Return Port), with sound; walking was not tried. About 20 minutes from
launch with no `[RED]` line, no crash and no shader missing from the map. Every 300-frame window ran
at 60 fps (frame time p50 16.62 ms), cutscenes included, and none was dropped on screen in
Argentum. Memory in Argentum: 4.6 GB resident, 4.1 GB on the GPU. The profile as committed boots
the rebuilt app to the title with none either. Two failures seen while the game window was
covered are open (below). Later chapters, saving and reloading, the add-on's
Torna story and the iPhone remain open; no IPA has been built.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `mem_reserve 0x2028000000`, `mem_alias 0x2000000000`, `mem_stack 0x10000000`, `app_pool 0x240000000` | [`game.tsv`](game.tsv) | the NPDM declares 96 MB of system resource (`sys_resource 0x6000000`), so virtual address memory is on, and `main` never sets a heap. `nninitStartup` sets up two 60 GB address regions. In the 64 GB alias region the second (`0xf00000000`, asked by `main+0xb7a540`) found its limit 0: its first arena was null and `memset` crashed at `sdk+0x798ce8`. The 128 GB alias region holds both, as for Xenoblade DE's Switch 2 Edition. The pool is the Switch 2's 9 GB |
| `mem_map_limit on` | `game.tsv` | Horizon's physical-pool limit: the engine probes its capacity by mapping until a map fails. Without it the probe mapped all 60 GB of its region, and the arena after it was null (the same `memset`, from `main+0xb7a650`) |
| `romfs_marker bf2.ard` | `game.tsv` | unlike Xenoblade DE's Switch 2 Edition, the Switch 2 program's own RomFS holds the game data |
| `pad_layout full` | `game.tsv` | a 3D game, as the Switch 1 profile |

There is no game-specific patch and no rewriter cut. The SDK takes the existing
function-matched `AddressRegionManager` cut, as Xenoblade DE's Switch 2 Edition's.

## Files

- The Switch 2 program's RomFS: 28,446 files, 25.63 GB: `bf2.arh` / `bf2.ard` (14.07 GB at the
  root), 214 WebM movies and 8 Wwise packs; and the add-on content, which the host mounts.

## Shaders

22,561 in [`shader_map.tsv`](shader_map.tsv): 16,330 pixel, 6,195 vertex, 33 compute, 3 geometry;
by source 17,370 from `bf2.ard`, 5,185 from the add-on and 6 from `main`'s standalone SLCT
(`main+0xc34b20`); 16,956 pairs.

`bf2.ard` keeps the upscaler's NDSR pack (`/monolib/shader/ndsr.pck`) as an XBC1 member, which
the split did not read: the title screen drew no scene, with 9 `[RED] shader … is not in the shader
map` keys and `the dispatched program has no compute shader`. Resolved by the general fix in the
split ([Monolith containers](../../../docs/reference/shaders.md#monolith-arh1-xbc1-and-hcpsslct-shader-containers)):
22,533 → 22,561.

## Driving it on the Mac

The Mac app's first launch after each pack spends about 70 s in macOS's launch check before
the launcher writes anything; the log is `~/Library/Logs/nxnative/0400E95004038001.log`. Headless:
`<app>/Contents/MacOS/nxnative --no-launcher` with `DM_CTL=<file>`; `DM_FOCUS_KEEP=1` keeps the game
in focus whatever the desktop does. The title screen ignored presses of 0.15 s and some of 0.5 s:
send `A~0.5` several times, 4 s apart (title, New Game, Normal). From there `A` every 5 s plays
the opening, wins the King Crustip battle (the party attacks by itself) and reaches Argentum in
about 15 minutes.

## Found on this game

- [SDK 21's OutputStream on a virtual output device](../../../docs/reference/audio.md#sdk-21s-outputstream-on-a-virtual-output-device) and [SDK 21's hardware Opus decoder manager](../../../docs/reference/audio.md#sdk-21s-hardware-opus-decoder-manager): its sound.
- [The Switch 2's order inside a GOB](../../../docs/reference/nvn-objects.md#the-switch-2s-order-inside-a-gob): its device flags `0x10400`.
- [Monolith containers](../../../docs/reference/shaders.md#monolith-arh1-xbc1-and-hcpsslct-shader-containers):
  an NDSR pack inside an ARD archive's XBC1 stream.

## Open

- **A GPU timeout while the window was covered (current, cause unknown):** 3 of about 25 boots
  ended with `kIOGPUCommandBufferCallbackErrorTimeout` on a game submission, then
  `SubmissionsIgnored` and `GPU access revoked`. One came about a second after confirming Normal, and two
  came at the title (frames 446 and 1085). Each time the window had been covered, and in the two logs kept the
  failed submission was the one of the frame on which `display link: rescue ended` was logged.
  The failed command buffer's shaders all ran in about 115 others with no failure. Not reproduced
  by `DM_TEST_LINK_STALL` (3 rescues ended), by covering the window with an opaque window 15 times
  (5 rescues ended), or in 8 plain boots.
- **A crash after the window was covered (current, cause unknown):** about 13 minutes in, during
  Chapter One's cutscenes, the window was covered for 4.4 s and the game went out of focus and
  back. Then `[RED] vertex stream 0's buffer 0x2f is in no memory pool; draw skipped` and
  `CRASH sig=10` at `main+0x8e6054` (called from `main+0x8797b8`), a load through a pointer
  ending in `…29f` the game read from its own data. Focus changes from `DM_OE_SCRIPT` at the title
  did not reproduce it.
- The title screen drops some presses (above). Not looked into.
- A `pso_manifest.tsv` has not been recorded.
- iPhone: the two 60 GB address regions do not fit the iPhone's alias region.
- Walking and the field, later chapters, saving and reloading, the Torna add-on.
