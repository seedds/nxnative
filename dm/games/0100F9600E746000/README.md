# ESP Ra.De. Ψ (`0100F9600E746000`)

エスプレイドΨ, M2 ShotTriggers, 2019; sold in Japan only. Update 1.4.1 (v327680) over the base game, SDK 7.4.0. M2's engine `m2engage` on NVN, which emulates Cave's 1998 arcade board (68000, YMZ280B) and draws M2's own frontend around it. Modules `main`, `subsdk0` (NintendoSDK_movie) and `sdk`, plus five NROs that the game loads from its RomFS through `nn::ro`. Brought up on the Mac on 2026-11-09: boot, M2's logos, the attract mode, the game mode menu, player select and play through stage 1's boss into stage 2, at 60 fps, with sound, no red line and no change to the host or the rewriter.

Build IDs: `main` `4485f4ff5482b4f6e4c83fa16e868c8e`, `subsdk0` `baee264d3658fc6b6b581e5cbb662e3a`, `sdk` `e30d2170165e907aba457c79fd83383d`.

## What it needs

| Setting | Where | Why |
|---|---|---|
| the profile's keys | [`game.tsv`](game.tsv) | the provisional profile's static guesses (`build_ipa_from_nsp.py` / `profile_guess.py`), unchanged: they boot and play |
| no address region | none | the NPDM declares no system resource, so virtual address memory is off as on Horizon; `main` sets a 3.14 GB heap (heap region 3.25 GB) |

## The NROs

The RomFS's `nro/` folder holds five modules and one `.nrr` with their five hashes; the host maps all five and loads each when the game asks (`ro::LoadModule`, [Services](../../../docs/reference/services.md)):

| Module | Size | What |
|---|---|---|
| `emu_integration_01500.nro` | 1.6 MB | M2's arcade frontend glue, loaded first |
| `esprade_01000.nro`, `esprade_01010.nro` | 48.5 MB each | arcade drivers (`EspradeDriver_*`) with the board's ROM data |
| `esprade_01020.nro`, `esprade_01030.nro` | 48.9 MB each | arcade drivers |

Every Mac run so far loaded `emu_integration_01500` and then `esprade_01030` twice before the first menu, unloading it in between; the game and the mode chosen later load nothing more (seven runs: ARCADE in ESP Ra.De. and in ESP Ra.De. Ψ, ARCADE CHALLENGE, ARCADE OSARAI, and the hand-played session). What makes the game load one of the other three drivers was not found.

## Mac routes

From an empty save (the first boot writes the save and shows the controls screen). The title screen comes up at a slightly different frame from run to run, so the menu steps are timed with a margin. In the arcade game X inserts a coin and Plus starts; without a credit it shows its attract mode (DEMONSTRATION) for ever:

- `DM_KEYS="f1428:A,f1536:A,f1788:A,f1975:A,45:A,f3600:X,f3660:Plus,65:A*10/2"` passes the logos and the title, picks ESP Ra.De. (left) on the game menu and ARCADE (the top entry) at SELECT GAME MODE, inserts a coin once the board has booted (CREDIT 1 at frame 3,600), starts, takes the player select, and plays stage 1 from frame 4,200 (left alone, the ship is lost and CONTINUE? shows at 6,300).
- `f1960:Right,f2000:A` in place of `f1975:A` picks ESP Ra.De. Ψ (right) instead; its own mode menu follows, then its arcade board boots.
- `f1975:A,f2150:Down,f2250:A` reaches ARCADE CHALLENGE's stage and area grid; `f1975:A,f2150:Down,f2190:Down,f2250:A` reaches ARCADE OSARAI's replay list (empty on a new save).
- `DM_NVN_SHOT=2100,3600,4200:shot_%d.png` gives the mode menu, the arcade title with its credit and stage 1.

## Mac play

A windowed session played by hand with a controller (the recorded input has analog stick values) and sound, 17,700 frames (about 5 minutes) from an empty save through the menus, stage 1 with Irori and its boss into stage 2: frame time p50 16.66–16.78 ms in every 300-frame report, 0 `[RED]`, 0 `UNIMPL`, no crash, 0 audio underruns in 62,000 device callbacks, footprint up to 2.59 GB. The player heard correct sound, and the slowdown under heavy fire that the arcade board has. The pause menu (RETURN TO GAME, RESTART, OPTION SETTING, QUICK SAVE / QUICK LOAD, GAME RESET, RANKING VIEWER, MATERIALS ROOM) opens and closes. The save is written at the first boot (four transactional commits).

Recorded with `DM_PSO_RECORD` from a fresh root, that session is this folder's [`pso_manifest.tsv`](pso_manifest.tsv) (15 shaders, 9 render pipelines). The shader tables come from the NSP script's split of the shaders in `main` and the RomFS, unchanged.

## Open

- Not tried: an iPad or iPhone, a controller on iOS, the other menu pages (OPTION SETTING, RANKING VIEWER, MATERIALS ROOM), QUICK SAVE / QUICK LOAD, TATE (M2's vertical screen setting), play past stage 2, and online ranking (NEX).
