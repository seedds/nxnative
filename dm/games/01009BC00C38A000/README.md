# The Binding of Isaac: Rebirth (`01009BC00C38A000`)

Base game 1.0 (v0), SDK 5.5.0, its own engine on NVN. Modules `main`, `subsdk0` (GLSLC), `subsdk1` (Nintendo's movie player) and `sdk`.

On the Mac: the opening movie, title, file selection, New Run as Isaac, Basement I, movement and shooting with both sticks, a room transition, a room clear with a key reward, and exit to the main menu with a saved run. Continue restores the room, health, reward and player. The initial three-minute windowed run used the Metal HUD and real audio, with no red diagnostic or crash. Steady gameplay frame p50 was 16.66–16.69 ms (60 fps), GPU p50 about 1.1 ms. Not run on the iPhone yet.

## What it needs

The provisional profile's memory and rendering defaults held. No host change, game-specific rewriter cut, or SDK cut was needed. An iOS compiler error in the shared shader chain needed one fix (see [Found on this game](#found-on-this-game)).

| Setting | Where | Why |
|---|---|---|
| `modules main subsdk0 subsdk1 sdk` | [`game.tsv`](game.tsv) | the game compiles its embedded shaders with `subsdk0`; `subsdk1` supplies the movie player |
| `sys_resource 0x0` | `game.tsv` | the NPDM declares no system resource: no virtual address memory, as on Horizon |
| `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | `game.tsv` | the ordinary 4 GB layout; the game takes a 3.14 GB heap within its 3.25 GB heap region |
| `pad_layout full` | `game.tsv` | a right stick for directional shooting beside the left stick for movement; face buttons also shoot |

The window origin is upper left. The presented picture is 1920x1080. Audio uses the SDK 5 renderer (48 kHz, 240 samples per frame, 24 voices); the initial run's device callback reports had no underrun.

## Shaders

`main` contains 20 GLSL source strings. At boot its own GLSLC (API 17.17, GPU code 1.15) produces 19 single-stage outputs: 9 vertex and 10 pixel shaders. All 19 translate and build for both the Mac and iOS. The NSP script captures them automatically into the extraction's `glslc_capture` folder, using the existing [runtime GLSLC capture](../../../docs/reference/shaders.md#built-in-shaders-compiled-with-the-games-own-glslc) path. The default translator binding layout works.

The map, pairs and facts come from that capture. [`pso_manifest.tsv`](pso_manifest.tsv) records the initial Mac route through the opening, menus and first room clear.

## Memory

A separate 90-second fresh-save boot followed the smoke route and then stood in the starting room for over a minute: the host's resident-memory report moved from 561 MB at frame 900 to 564 MB at frame 3900, and Metal allocations stayed at 370 MB. There was no continued resource allocation in the last minute's residency reports. The initial three-minute interactive route ended at 586 MB resident and 482 MB of Metal allocations after movie/menu/room transitions and diagnostic screenshots.

## Mac routes

The build and work folders are `<NSP folder>/01009BC00C38A000_build` and `<NSP folder>/01009BC00C38A000-work`. Run the NSP script with `--platform mac`; the work folder's `run/boot.py <log> [seconds] [KEY=VALUE ...]` runs an isolated `isaachost` copy with the window, HUD and audio, and accepts input through `run/ctl.txt`.

From a fresh save: A skips the opening, A on the title, A on file 1, A on New Run, A on Isaac. [`smoke.tsv`](smoke.tsv) spaces these by 120 presented frames from frame 300, then stands in the safe starting room. Left stick moves, right stick shoots, Plus pauses. Exit Game in the pause menu saves the run and returns to the main menu, where Continue is selected.

## Found on this game

- **The blend helper's output reference on iOS:** the shared shader helper now names the `thread` address space of its output, and its compiler test covers the packing script's iOS target as well as the Mac: [reference](../../../docs/reference/shaders.md#the-blend-helpers-output-reference-on-ios).

## Open

- iPhone gameplay and touch controls remain untested.
- Only the supplied base version was checked; later floors, bosses and local co-op were not tested.
- Longer sessions and memory behaviour across many rooms remain unverified.
