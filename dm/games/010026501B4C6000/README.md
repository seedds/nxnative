# Cricket 24 (`010026501B4C6000`)

Base game 1.0.0 (v0), no update, SDK 16.2.3. Big Ant Studios' engine with Wwise. Modules `main` and `sdk`; `subsdk0` (NintendoSDK_movie) is not loaded, since `main` takes nothing from it that the host does not supply. The game loads one NRO, `nro/party.nro`, through `nn::ro`. The RomFS is 103,353 files: 103,349 of them under `data/<xx>/<hash>` with no extension, named in the plain-text listing `data/data-0.blobset.nx.log` (`<path> => …/data/<xx>/<hash>, File size: …`) and indexed by `data/data-0.blobset.nx` (magic `BLOB`). 54,177 of those files are chained zstd frames, 48,851 are RIFF (audio) and 266 Wwise banks.

Status: plays on the Mac (Apple M4 Max, macOS 27.2): from a fresh save through the title, the control scheme, the main menu, team selection, the match setup screen and the coin toss into a men's Test match at the Sydney Cricket Ground, with the HUD and scoreboard, at the game's 30 fps, with sound. The log's only red lines are the two of the kits' smallest levels (see [Rendering](#rendering)). Not checked: a whole match, other modes and grounds, the career, online play, iOS.

## What it needs

| Setting | Where | Why |
|---|---|---|
| Big Ant's chunked zstd files and shader pack | `dm/gfx/tools/shader_split.py` | the split found no shaders before; see [Shaders](#shaders) |
| A copy into a block encoder's integer texture in a `CPU_NO_ACCESS` pool is read back into its storage | `dm/gfx/nvn/nvn_mtl.m` | the players' kits were all zero and the players drew black; see [Rendering](#rendering) |
| `6af2b7263c5e4b61` | [`dual_want.tsv`](dual_want.tsv) | a pipeline blends with its output 1 as the second source (`[RED] shader 6af2b7263c5e4b61_b writes output 1 …`) |
| The 4 GB layout (`mem_reserve 0x100000000`, `mem_alias 0x10000000`, no system resource) | [`game.tsv`](game.tsv) | the NPDM declares no system resource; `main` sets a heap of at most the pool less 64 MB (`[mem] heap 3.13 GB`) |

## Shaders

The split finds 8,484 shaders (8,125 pixel, 231 vertex, 126 compute, 2 geometry), all in `shaders.blob.nx` (`data/e4/5e4095ee55f1e2`, 43.5 MB stored, 140 MB decoded), in 8,528 programs, 8,401 vertex / pixel pairs ([Big Ant's chunked zstd files and shader pack](../../../docs/reference/shaders.md#big-ants-chunked-zstd-files-and-shader-pack)); none in `main` or the other files. All of them translate. Of the RomFS files, 54,176 of the 54,177 chained files open; `data/9f/2786ce74485019` (4,227 bytes) does not and is read raw (`[RED] … a size-prefixed frame that did not open`, the one red line of the split).

## Rendering

The players' kits are built at run time. For each kit the game renders the picture, encodes BC3 blocks by compute into an `RGBA32Uint` texture (one texel per 4×4 block), and copies each level into a 128×128 `RGBA32Uint` texture at offset 0 of the kit's own 0x56000-byte pool (`CPU_NO_ACCESS | GPU_CACHED`); the character shaders sample the 512×512 BC3 texture made over the same storage (format 0x44, 10 levels). Before the fix that texture and its storage were all zero, and the players drew black with speckled clothing ([Block-encoder copies into a pool the CPU cannot access](../../../docs/reference/nvn-objects.md#block-encoder-copies-into-a-pool-the-cpu-cannot-access)).

Open: the two smallest levels of each kit (2×2 and 1×1) are not copied (`[RED] texture copy destination at level 8 of a texture with 8 levels: skipped`, and `texture 128×128×1 declares 10 levels, its size holds 8`): the integer texture declares 10 levels, which a 128×128 texture cannot hold.

## Mac routes

From a fresh save, step by step with `DM_CTL` and a shot after each step: "Press A to begin" shows within about 100 s of launch on the first run after the build; `A` there opens the control scheme screen; `A` (Arcade), `A` (Play Now), `A` (New Men's Match) reach team selection (Australia against England); `A` reaches the match setup screen with both captains in their kits; `A` on Start Match loads the match, `A` on Continue goes to the coin toss and the first balls. With `A` every 15 s from 105 s, this route is in the match by about 280 s.
