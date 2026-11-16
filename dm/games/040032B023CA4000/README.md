# Orbitals (`040032B023CA4000`)

A Switch 2 game: the Deluxe Edition, update v327680 (1.1.2.0-178968 on the title screen), SDK 21.4.2 built for the Switch 2 (`Ounce`). Shapefarm's two-player co-op adventure on Unreal Engine 5.6.1 (`SDK MW+EpicGames+UnrealEngine-5_6_1` in `main`; `SF_GLSL_SWITCH2` / `SP_GLSL_SWITCH2` shaders) on NVN2, with Wwise sound and Bink 2 movies. Modules `main`, `subsdk0` (NintendoSDK_movie) and `sdk`.

**Plays on the Mac** (M6, 16 GB, macOS 27.2, 2026-11-14), two players, in a window: the first-start language, gamma and terms screens, the title, Play Local with both players joined, the character select, the opening anime (Bink) and the first area of the story, both characters walking, at the game's own 30 fps with no red line beyond one known one. The same route also plays in an iPhone's address space on the Mac (`DM_ALIAS_SIZE=0x118000000`, 2026-11-14). Not run on an iPhone or iPad.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000`, `app_pool 0x240000000` | [`game.tsv`](game.tsv) | Engage's 5 GB block for an iPhone (the Mac reserves 64 GB): see [Memory](#memory). The Switch 2's 9 GB pool |
| `sys_resource 0x1400000` | `game.tsv` | the NPDM declares 20 MB, so virtual address memory is on |
| `depth_mode zero` | `game.tsv` | the game sets near = 0 (`nvnDeviceSetDepthMode`), so pipeline bases made offline use `main0_z` |
| `pad_layout full` | `game.tsv` | a 3D field, both sticks in use |

There is no game-specific patch: the `sdk`'s node area is the SDK-wide rule's cut ([The SDK's address-region node area](../../../docs/reference/memory.md#the-sdks-address-region-node-area)), and Unreal's allocator and engine region are the rewriter's engine-wide rules ([Unreal's FMallocBinned3 replaced by the host allocator](../../../docs/reference/memory.md#unreals-fmallocbinned3-replaced-by-the-host-allocator)). Everything else it needed is general: see [Found on this game](#found-on-this-game).

## Two players

Orbitals cannot be played alone: Play Local waits for a second player to join, and each picks a character (Maki or Omura). The screen splits in two. On the Mac the keyboard plays with player 1, and each game controller is a player of its own, so a second player needs a controller. Scripted input reaches player 2 through a `DM_CTL` line `P2 <keys>` ([Scripted input](../../../docs/agents/05-debugging.md#scripted-input-and-thread-scheduling)).

## Files

- One [NUP container](../../../docs/development.md#the-packing-script-in-detail) of 17.9 GB: the base game, update v327680 and the Deluxe add-on (`040032B023CA5001`, a licence with no content; the main menu shows the Deluxe concept gallery and a "Bonus content unlocked" notice). No NACP; the name comes from the profile.
- RomFS (9.9 GB, 53 files): `Orbitals/Content/Paks/Orbitals-Switch2.utoc` / `.ucas` (6.8 GB), `Orbitals-Switch2.pak` (110 MB), `global.utoc`, and 35 Bink 2 movies (`.bk2`) under `Orbitals/Content/Movies`: the story's animatics (the opening, `BK2_Orbitals_S01_Animatics.bk2`, 366 MB), memories, UI previews and the startup logos. The game decodes them itself.

## Shaders

- 23,432 unique in [`shader_map.tsv`](shader_map.tsv) (3,556 vertex, 16,459 pixel, 3,415 compute, 2 geometry) from the container's two IoStore shader libraries (global 4,515, game 36,480 shaders), split in 57 s: [Unreal Engine 5.6 IoStore shader libraries for the Switch 2](../../../docs/reference/shaders.md#unreal-engine-56-iostore-shader-libraries-for-the-switch-2). All build: a full Mac build took 19 minutes on the M6 the first time, 6 minutes on chain `73b94575300e` with the Metal compile cache warm.
- Resolved: 5 global compute shaders (`3ea35c5a0f1ea749`, `45389d82b79dbf0c`, `50c768e8620cf18e`, `868f60d0648f0110`, `c2f9e9baad57e2d0`) stopped at `Ampere instruction 394 … is not implemented`: Unreal's ShaderPrint counter, an image atomic whose handle is in a register ([Ampere forms with a handle in a register](../../../docs/reference/shaders.md#ampere-forms-with-a-handle-in-a-register-and-sureds-other-operations)).

## Frame rate

- Current: 30 fps, the game's own: it sets present interval 2 at the title (`the game sets present interval 1 → 2`). In the first area: frame time p50 33.3 ms, p95 34.2 ms, an occasional long frame of 200 ms; CPU 113% of one core, 4.7 W.
- Not tried: 60 fps (`fps_target`).

## Memory

- Unreal's engine region: `nninitStartup` reserves 62 GB, and the host gives it what the alias region has room for. Its page allocator never gives address space back ([Unreal 5.6's page allocator never gives address space back](../../../docs/reference/memory.md#unreal-56s-page-allocator-never-gives-address-space-back)), and right at boot it hands out a 1 GB block that stays mostly unmapped (the high-water mark goes from the region's start to 1 GB past it with under 0.25 GB mapped).
- Mac (64 GB alias region): the region gets 62 GB at alias +0.06 GB. Before the node area cut it started at +1.00 GB, past the SDK's 1 GB node area, which no SDK-wide rule matched on this `sdk` build (SDK 21.4.2 for the Switch 2), and the first area took it to alias +5.00 GB with 2.56 GB mapped.
- iPhone (Engage's 5 GB block, a 4.375 GB alias region; run on the Mac with `DM_ALIAS_SIZE=0x118000000`, 2026-11-14): the region gets 4.06 GB at alias +0.06 GB. From the boot through the character select, the opening anime and about two minutes of both players walking in the settlement, the high-water mark rose to +4.00 GB of the region's end at +4.12 GB, with 2.51 GB mapped, and stayed there; no out-of-memory abort.
- Current: in the first area, 6.0 GB resident (footprint), 4.0 GB of Metal allocations, an Unreal heap of 1 GB.
- Open: the room left in the region is about 0.12 GB. Later chapters are not measured; if they run past it, Unreal calls its out-of-memory handler. An iPhone run.

## Saves

- `ProfileSave`, `SharedGameSettings` and `OrbInputUserSettings` after the first-start screens; the next start goes straight to the title. Story progress was not checked across a restart.

## Driving it on the Mac

The work folder has `boot.sh <log> [secs] [VAR=…]` (window + HUD, a renamed host copy, `DM_CTL` at `run/ctl.txt`), `ctl.sh <wait> <shot> [presses…]` and `route.sh` (boot to the character select, then the picks and both ready).

From a save past the first-start screens: at about 40 s the title, `A`; 22 s later the main menu, `A` (Play Local), `A` (player 1 joins), `P2 A` (player 2 joins), `A` (Start). At the character select wait until it takes input (the first presses after it appears are ignored; look with a shot): `Left` (player 1 to Maki), `P2 Right` (player 2 to Omura), then `A` and `P2 A`: the load and the opening anime. The anime shows `HOLD B TO SKIP`, but scripted holds (`B~4`, `B~5`) did not skip it on 2026-11-14 and `Plus` opens the pause menu (`B` closes it): wait it out, about 3 minutes, then the settlement. `LUp` / `P2 LUp` walk.

On a first start the language, gamma and terms screens come before the title: `A` (title), `Down` three times and `A` (Apply), `A` (gamma), `A` (terms).

## Found on this game

- [Unreal Engine 5.6 IoStore shader libraries for the Switch 2](../../../docs/reference/shaders.md#unreal-engine-56-iostore-shader-libraries-for-the-switch-2): the split stopped at `shader 0 is not an LSLG record`.
- [The SDK's address-region node area](../../../docs/reference/memory.md#the-sdks-address-region-node-area): the engine region started past a 1 GB node area and did not fit an iPhone's address space; the SDK-wide rule now has this SDK 21.4 build's constructor.
- [Ampere forms with a handle in a register](../../../docs/reference/shaders.md#ampere-forms-with-a-handle-in-a-register-and-sureds-other-operations): 5 compute shaders stopped at `Ampere instruction 394`.
- [A frame the Mac dropped stays queued in the frame history](../../../docs/reference/services.md#a-frame-the-mac-dropped-stays-queued-in-the-frame-history): after both players readied up, the load stopped presenting, and Unreal aborted with `GameThread timed out waiting for …`.

## Open

- 60 fps, later chapters (and the engine region's room on an iPhone there: [Memory](#memory)), online play (Gameshare, Play Online), the sound (Wwise opens with no red line; not listened to).
- An IPA (none built yet) and an iPhone or iPad run.
