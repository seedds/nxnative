# CHRONO CROSS: THE RADICAL DREAMERS EDITION (`0100AC20128AC000`)

Update 1.0.2 (v131072), SDK 13.3.0, NVN. One game of three programs, packed as one: one NSP script run makes one Mac app (or IPA, or Mac run command), and the game-selection screen starts the games inside it.

| Program | What it is | Here |
|---|---|---|
| 0, the selection screen (this profile) | the title and game-selection menu, modules `main` and `sdk` | runs |
| 1, [Chrono Cross](../0100AC20128AC001/README.md) | the PlayStation game, remastered | starts from the selection screen |
| 2, [Radical Dreamers](../0100AC20128AC002/README.md) | the Satellaview game | starts from the selection screen |

Tested on the Mac (Apple M3 Max): the selection screen, Chrono Cross and Radical Dreamers each started from it, and the games' reset combination (L + ZL + R + ZR + Plus + Minus, the PlayStation soft reset) brings back the selection screen. The iPhone path has not been built.

## How it is one game

- **Profiles.** The key `programs` lists the others (`1=0100AC20128AC001 2=0100AC20128AC002`); each says `program_of`, has its own tables and no row in the games table.
- **Switching.** The selection screen calls `oe::ExecuteProgram(1)` or `(2)` with no parameters, and the games' soft reset calls `oe::ExecuteProgram(0)`: [Multi-program applications: `oe::ExecuteProgram`](../../../docs/reference/services.md#multi-program-applications-oeexecuteprogram). No `launch_param` is needed for a run of a game alone.
- **Packing.** A build of program 0 packs all three (`build_ipa_from_nsp.py` `other_programs`); `--program N` packs one alone, for bring-up. Without `programs` the host had only this program, and choosing a game ended the app (`[RED] oe::ExecuteProgram(1, 0 bytes of parameters:): the host has only this program; it ends`).
- **Versions.** The update patches all three programs. The base game's selection screen (v0) is build `b65acd7716a27cded6d7fc3a5865e977`; it is not registered, so a build of v0 is packed from a derived profile.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `programs 1=0100AC20128AC001 2=0100AC20128AC002` | [`game.tsv`](game.tsv) | the games are separate programs; the selection screen starts them |
| 4 GB layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000`, `sys_resource 0x0` | `game.tsv` | the survey's rules for a game with no system resource; `main` sets a heap of the pool − 64 MB |
| `modules main sdk` | `game.tsv` | every module `main` imports from |

No rewriter patch: nothing here depends on a build.

## Found on this game

- [Multi-program applications: `oe::ExecuteProgram`](../../../docs/reference/services.md#multi-program-applications-oeexecuteprogram): a third game that switches programs, with no launch parameter.

## Open

- The games were started, not played through; the iPhone build has not been made.
