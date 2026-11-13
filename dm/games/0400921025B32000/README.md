# Sonic Frontiers Definitive Edition (`0400921025B32000`)

The Switch 2 edition, v65536 (1.0.0 on the title screen), SDK 21.4.1 (Ounce), NVN2. The NUP holds the whole game as that update, with no base. Modules `main`, `subsdk0` (NintendoSDK_movie, not loaded: the USM movies go through CRI Mana's own decoder) and `sdk`. SEGA / Sonic Team's Hedgehog Engine 2 (its allocators log as `Rsdx`), with CRIWARE (ADX2 sound, USM movies).

On the Mac (2026-11-13, M5 Pro, macOS 27.2, from the NSP script's provisional profile and then this one): the boot, the title, New Game, the intro cutscene drawn in real time, and the first Cyber Space stage with Sonic, rings and the whole stage drawn. The title and the stage run at 60 fps (frame time p50 16.7 ms), the cutscene at the game's 30 fps. No shader is missing from the map, and the only `[RED]` line is `nvnDeviceGenerateDebugDomainId` (unimplemented, called once at boot). Played with scripted presses only.

## What it needs

| Setting | Where | Why |
|---|---|---|
| 9 GB layout: `mem_reserve 0x270000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0x240000000` | [`game.tsv`](game.tsv) | the provisional layout for an Ounce SDK: `main` sets a heap of the pool − 64 MB (8.92 GB) |
| `sys_resource 0x0` | `game.tsv` | the NPDM declares no system resource: no virtual address memory |
| `depth_mode zero` | `game.tsv` | the game sets near = 0 depth at boot; without the key the app's pipeline archive was made for near = −w, refused, and the first start compiled 844 shader pairs before its first frame |

No code of the game is cut. The rewriter needs [a site for a compare and swap of the pair x18, x19](../../../docs/reference/rewriter.md#compare-and-swap-of-a-register-pair-that-starts-at-x18) (`main+0x19ce5c4`).

## Memory

- The game makes four allocators in its heap: system 128 MB, main 2304 MB, nvn (CPU) 1536 MB and nvn 2048 MB.
- About 1.7 GB resident at the title and 0.9 GB of Metal allocations.

## Files and shaders

- RomFS: 1,862 files, 26.41 GB, under `raw/`: 1,435 PACx 403 archives (`.pac`; 11.16 GB under `raw/stage`), 155 `.acb` / 51 `.awb` / 2 `.acf` CRIWARE sound files, 93 USM movies (6.05 GB) and 13 texture streaming packages (`.ntsp`, 3.57 GB).
- 4,958 shaders: 3,318 pixel, 1,288 vertex and 352 compute. 1,554 come from 233 [Needle containers with NVN2 records](../../../docs/reference/shaders.md#needle-containers-with-nvn2-records) in `main`, 3,404 from `raw/NeedleShader.pac`, [an archive of LZ4 blocks](../../../docs/reference/shaders.md#shaders-in-hedgehog-engine-2s-pacx-403-archives) (3,626 records, 10 of them shared with `main`). The other 1,434 archives hold none. All built for the Mac with no failure.
- Without `NeedleShader.pac` the title and the cutscene drew, but the first stage drew its geometry black: 74 shaders it asked for were missing, all in that archive.
- **Open:** the prewarm list (`pso_manifest.tsv`) is empty.

## Frame rate

- 60 fps at the title and in the first stage; 30 fps in the logos and the cutscene, as the game sets them.

## On the Mac

- After the NSP script's `--platform mac` build, its printed command runs the game.
- From a fresh save, `DM_KEYS=35:A,38:A*6/3,60:A~2*20/8,70:A*40/5` takes New Game at the title, goes through the screens before the game, skips the intro cutscene (A held 2 s, at its "Skip" prompt) and starts the first stage. Plus pauses a cutscene.

## Found on this game

- The rewriter stopped on `caspal x18, x19, x4, x5, [x16]` ([Compare and swap of a register pair that starts at x18](../../../docs/reference/rewriter.md#compare-and-swap-of-a-register-pair-that-starts-at-x18)).
- The shader split stopped on `main`'s Needle containers ([Needle containers with NVN2 records](../../../docs/reference/shaders.md#needle-containers-with-nvn2-records)) and could not read `raw/NeedleShader.pac` ([Shaders in Hedgehog Engine 2's PACx 403 archives](../../../docs/reference/shaders.md#shaders-in-hedgehog-engine-2s-pacx-403-archives)).

## Open

- iPhone: no IPA built yet; the 9 GB heap region is the Ounce layout and is not checked against an iPhone's memory.
- The empty prewarm list ([Files and shaders](#files-and-shaders)).
- Only the start of the game was played, with scripted presses: movement in the open zones, Cyber Space stages after the first, bosses, the USM movies, sound by ear, the other languages and the Definitive Edition's added content are untested.
- `nvnDeviceGenerateDebugDomainId` is unimplemented; the game goes on without it.
