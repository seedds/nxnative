# Red Dead Redemption (`01007820196A6000`)

Base game 1.0.1, SDK 15.3.2. Rockstar's RAGE engine as Double Eleven ported it to the Switch (the engine's classes are in the `d11` namespace; threads are named `[RAGE] …` and `[RDR2] Render Thread`), on NVN. Modules `main` and `sdk`. The profile has no `patches.py`: the rewriter runs with its defaults.

On the Mac: the Rockstar legal screens, the title and its scenes, the new-game intro and the Ricketts farm, where John walks around with the audio, the foliage and the characters drawn as on the Switch. Not played further than the farm and its surroundings; not tried on the iPhone; no IPA built (the README gallery's ~11.5 GB is the Mac app's size, almost all of it the RomFS stored as it is).

## What it needs

| Setting | Where | Why |
|---|---|---|
| `romfs_marker audio_switch.rpf` | [`game.tsv`](game.tsv) | the largest RomFS file (6.3 GB) |
| `zip_store .bk2:.pkdswitch:.rpf:.metallib` | `game.tsv` | big files that do not compress |
| `mem_reserve`, `mem_alias`, `mem_stack`, `app_pool`, `mem_used` | `game.tsv` | the NPDM declares 0 MB of system resource, so `IsVirtualAddressMemoryEnabled` is false as on Horizon; `main` sets a heap of at most the pool − 64 MB − 1207 MB of bss. The values are the survey's guess (`profile_guess.py`), and the game boots and plays with them |
| `shader_groups` | `game.tsv` | the survey's guess from the RomFS top-level folders (`/redemption/`, `/cero/`, `/d11extra/`). The shaders are not in files of those names, so the groups label little. `shader_groups` and `shader_note` are inputs of the shader libraries: changing either restamps them |

No game-specific host code: every fix below is general.

## Rendering

- **Current: the scene depth sampled while attached.** The lighting pass and three more draw with depth test on and write off while sampling the same depth texture (`ser=277` in the dumps) through a handle in a uniform buffer. Metal returns whole zero blocks there: [Depth sampled while it is the pass's depth attachment](../../../docs/reference/rendering.md#depth-sampled-while-it-is-the-passs-depth-attachment). The profile needs no `feedback_snap` entry for it.
- **Current: occlusion queries.** About 108 samples-passed reports a frame and no `ResetCounter`: [Samples-passed counter read as a running total](../../../docs/reference/rendering.md#samples-passed-counter-read-as-a-running-total). Before the fix every query read 0 and characters and horses were left out except their hats and guns.
- The one `[RED]` line in a run is `SetRenderEnableConditional: the host draws anyway for now`: the game's conditional rendering is not evaluated, so the host draws more than the Switch does, never less.
- The pass order of a frame in the farm: the G-buffer passes (normals `ser=278`, albedo `3392` / `3393`, depth `277`), then the lighting pass into `3355` (411 to 450 draws), then post effects at 1/2, 1/4 and 1/8 resolution. Fragment shader `3e5ae8050239cfe0_b` draws the foliage cards.

## Files and shaders

- RomFS: 134 RPF6 archives (`.rpf`), the `.bk2` videos and a Double Eleven package, `d11extra/langselect.pkdswitch`. RPF6's table of contents is AES-256 encrypted, the cipher applied 16 times; the key is a 32-byte window of `main` that `rpf6.py` finds by decrypting the first records (it is never printed). Names are hashes. 26,892 records in 128 archives are resources of other packings than the plain files `rpf6.py` reads.
- 3,583 shaders, none from GLSLC files in the RomFS: 3,413 in `shaders.rpf` (134 `rgxd` resources, each with reflection names and the code and control section of its shaders, in the reverse of GLSLC order), 168 unaligned `rgxd` records in `main` and 2 in `langselect.pkdswitch`: [Rockstar RAGE shaders in RPF6 archives (rgxd)](../../../docs/reference/shaders.md#rockstar-rage-shaders-in-rpf6-archives-rgxd). 2,134 are vertex and 1,449 pixel shaders.
- Single-stage programs: `shader_split.py` pairs them as a cover of two pixel shaders for every vertex shader; the real pairing happens at draw time. The engine pairs by name (`GfxDevice_c::GetShaderProgram`); the names in the `rgxd` headers are not used yet.
- `main` holds no glslc, no `#version` and no `NVNS` / `BNSH`.
- The shader tables are the split's output, `shader_pairs.tsv` and `pso_pairs.tsv` for the pairs. `pso_manifest.tsv` is the host's `pso_user.tsv` of a play-through on the Mac (boot, legal screens, title, New Game, the intro, the farm) with the duplicates taken out: 792 shaders and 629 render pipelines. The pipeline archive's prebuild covers 4,268 pairs and takes 18 s on the M4.

## Audio

- The game opens two `nn::audio` renderers at once; the host drove one, the second one's system event never fired and a thread waiting for it held a mutex the main thread needed (a freeze at about frame 650 and 85 audible frames out of 36,000): [Several renderers open at once](../../../docs/reference/audio.md#several-renderers-open-at-once). The host keeps up to four, each with its own output.

## Mac routes

- The save used is in the cabin after the intro. Launch `Contents/MacOS/nxnative` with its own `HOME` (a copy of `~/Library/Application Support/nxnative/01007820196A6000`, so a second instance does not share the save, the log or the pipeline archive), `DM_CTL=<file>` and `DM_AUDIO_MUTE=1`, and drive it with the control file ([05, Scripted input](../../../docs/agents/05-debugging.md#scripted-input-and-thread-scheduling)).
- From frame 600 on: `Plus` then `A` six times (about 8 s apart) leaves the legal screens and the title, `A` until the minimap ring shows the game is playable, then `LUp~4` and `LUp~2` walk John out of the cabin to the tree in front of the barn. More `Plus` presses open the pause menu and the map (`B` backs out).
- The blacksmith at his grindstone, east past the tree: five `LUp+LRight~1.2` steps 2 s apart, then `LRight~1.3` and `LUp~1.5`. The NPCs wander, so a hat-only character is not a fixed test; the blacksmith stands still.
- The title scenes show the foliage too, but they cycle and differ from run to run.
- The window has to be visible: a window another window covers is throttled and `shot` requests stall.
- `smoke.tsv` is the same route for `scripts/regress.py` (`DM_KEYS`: `22:Plus*6/8,25:A*6/8,75:A*3/6,100:LUp~4,108:LUp~2`, shots at 118 s and 128 s). It needs a save from the cabin after the intro in `<NSP folder>/saves/farm`; the game's own save folder is enough (`RDR2SAVE0.SAV` and the other `.SAV` files). The frame it must reach is 2,900 by 115 s (29.9 fps from a first frame at about 12 s).
- `regress.py check` of this branch against `main` (Apple M4): the baseline host hangs at frame 904 on the second renderer (`[RED] audren: a second renderer (the host drives only one)`), the candidate reaches frame 3,767 at 29.9 fps (p99 33.7 ms, peak 1,581 MB) with the one `[RED]` line of Rendering. 3,583 of 3,583 shaders build in both, none newly failing, no MSL changed.

## Found on this game

General host and tool changes first needed here; each applies to every game:

- **Shaders in RPF6 archives** (`rpf6.py`, `shader_split.py`): [Rockstar RAGE shaders in RPF6 archives (rgxd)](../../../docs/reference/shaders.md#rockstar-rage-shaders-in-rpf6-archives-rgxd).
- **Several audio renderers**: [Several renderers open at once](../../../docs/reference/audio.md#several-renderers-open-at-once).
- **Depth feedback**: [Depth sampled while it is the pass's depth attachment](../../../docs/reference/rendering.md#depth-sampled-while-it-is-the-passs-depth-attachment).
- **The samples-passed counter as a running total**: [Samples-passed counter read as a running total](../../../docs/reference/rendering.md#samples-passed-counter-read-as-a-running-total).
- **The pointer in fullscreen** (`front_mac.m`): it hides after 2 s without moving and shows again on a move; see [development.md](../../../docs/development.md).

## Open

- `SetRenderEnableConditional` is not evaluated (see Rendering).
- Vertex and pixel shaders are paired as a cover, not by the names in the `rgxd` headers.
- The 26,892 RPF6 records of other packings are not read; they may hold more shaders.
- The depth snapshot splits each pass that samples depth: GPU time in the farm went from about 6 to about 10 ms a frame. Taking the snapshot once at the start of the pass would cut that.
- `pso_manifest.tsv` is empty (see Files and shaders).
- No benchmark route (`bench.tsv`); not tried on the iPhone; play past the farm is untested. `regress.py` has only this game and Balatro to run on this Mac, and Balatro has no route (and the chain check does not cover OpenGL games): other games' runs are unchecked.
