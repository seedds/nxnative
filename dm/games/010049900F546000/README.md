# Super Mario 3D All-Stars (`010049900F546000`)

Update 1.1.4 (v393216). One game of four programs, packed as one, as on the cartridge: one NSP script run makes one IPA, one Mac app or one Mac run command, and the launcher starts the games inside it.

| Program | What it is | Here |
|---|---|---|
| 0, the launcher (this profile) | the menu and the soundtrack player: sead / agl / gsys with NintendoWare layouts on NVN, SDK 10.8.1, H.264 movies through the SDK's movie library | runs |
| 1, [Super Mario 64](../010049900F546001/README.md) | Nintendo's N64 layer: the MIPS code recompiled in an NRO, the rest through a JIT plugin under `nn::jit`, on Vulkan | runs |
| 2, [Super Mario Sunshine](../010049900F546002/README.md) | NERD's Hagi GameCube layer: the PowerPC code recompiled in an LZ4-packed NRO, on Vulkan with shaders generated at run time | runs |
| 3, Super Mario Galaxy | the Wii game, built for **32-bit ARM** (its NPDM's 64-bit flag clear) | cannot run: Apple silicon has no AArch32 mode and the host runs a game's own code. Its tile, artwork and soundtrack stay in the menu. [Super Mario Galaxy](../010099C022B96000/README.md), the later 64-bit release, has its own profile |

Tested on the Mac: the opening, the title, every tile with its movie; A on Super Mario 64 or Sunshine starts it with the launcher's parameters, and each game's Return to title selection comes back. The iPhone path compiles and packs; it has not run on a device.

## How it is one game

- **Profiles.** The key `programs` of this profile lists the others (`1=010049900F546001 2=010049900F546002`); each of those says `program_of` and has its own modules, memory layout and tables, and no row in the games table.
- **One host.** A host built for this profile holds a table of the three (`game.py` `header_multi`), and `DM_PROGRAM` picks the entry at start (`main.c` `dm_game_cur`). A host for one of the others alone still builds, for bring-up and the shader capture.
- **Packing.** A build of program 0 packs every program in `programs`, each into its own `<id>_build`, then one package (`build_ipa_from_nsp.py` `other_programs`). `--program N` packs one alone, for bring-up.
- **Switching.** [Multi-program applications: `oe::ExecuteProgram`](../../../docs/reference/services.md#multi-program-applications-oeexecuteprogram). The Mac starts the host again in place. iOS cannot start a process: the app names the next program, schedules a notification that opens it again, and ends; the next start runs that program. Opening the app any other way starts the launcher. The launcher's settings screen does not come back on a switch.
- **Launch parameters.** The launcher hands each game 16 bytes: its language code and two volumes, which differ by game (Super Mario 64 1.12 and 2.24, Sunshine 0.5 and 1, Galaxy 0.35 and 0.7). A game's `launch_param` gives the same when it is run alone.
- **Saves.** One save folder, as on Horizon; each program keeps its own files in it.

## What it needs

| Setting | Where | Why |
|---|---|---|
| 4 GB layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000`, `sys_resource 0x0` | `game.tsv` | what the survey's rules give for a game with no system resource; `main` sets a heap of the pool − 64 MB |
| `modules main subsdk0 sdk` | `game.tsv` | `subsdk0` plays the menu's movies |

Render conditions, which the host evaluates for every game ([Conditional rendering evaluated at replay](../../../docs/reference/rendering.md#conditional-rendering-evaluated-at-replay); until 2026-11-12 the profile key `render_cond`): the menu wraps its clears in conditional rendering; without evaluating it every frame is black ([Conditional rendering evaluated at replay](../../../docs/reference/rendering.md#conditional-rendering-evaluated-at-replay)).

No rewriter patch: nothing here depends on a build.

## Galaxy's tile

- **Current:** A on the tile calls `oe::ExecuteProgram(3)`. No program 3 is packed, so the launcher starts again (a red line in the log, [the rule](../../../docs/reference/services.md#multi-program-applications-oeexecuteprogram)): the screen fades, and the title comes back. A build-bound cut could make the press do nothing (PR #193 and issue #1135 carry one, in the launcher's tile-decide function, `main+0xc270` on 1.1.4); the host rule needs no patch per build, so the launcher has none.

## Found on this game

General host changes first needed here; each applies to every game:

- [Every undrawn extension code point is a ring](../../../docs/reference/services.md#every-undrawn-extension-code-point-is-a-ring): the crash at boot, `main+0x63780` on 1.1.4, U+E06B, which issue #206 reported at `main+0x62d20` on 1.0.0.
- [Multi-program applications: `oe::ExecuteProgram`](../../../docs/reference/services.md#multi-program-applications-oeexecuteprogram).
- [Offline HTML pages in a web view](../../../docs/reference/services.md#offline-html-pages-in-a-web-view).
- [Under `--work-dir` the programs of one game overwrote each other's RomFS](../../../docs/reference/packing.md#under---work-dir-the-programs-of-one-game-overwrote-each-others-romfs): the app build stopped at the launcher's RomFS marker.

Not needed here: the same-pass gamma draw over each movie tile (agl's `021631192bc452b6`) that earlier patches gave a `feedback_snap` for: main's general snapshot of an attachment a draw samples (`fb_prepare`, `nvn_mtl.m`) already handles it.

## Mac input

With `DM_CTL`: A at the title; A starts the tile shown; Right moves between tiles. Each game's page opens with Minus, and `webshot <path>` saves it, since a game shot has only the game's picture.

## Open

- iPhone: no device run yet. Everything for it is built and packed (one IPA over the three programs).
- A switch on iOS quits the app and asks for a tap. PR #193's in-process switch would avoid that, at the cost of every host static living in a section that is saved and restored; it is not in.
- `--platform mac-app`: the app holds all three programs; its settings window is the launcher's.
- `--romfs separate` is refused: `Documents` holds one RomFS image.
- No pipeline archive is packed (no program sets `pso_mtl4`).
