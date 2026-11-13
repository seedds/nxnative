# Sonic Frontiers Definitive Edition (`0400921025B32000`)

A Switch 2 game: v65536 (1.0.0 on the title screen), SDK 21.4.1 built for the Switch 2 (`Ounce`), NVN2. SEGA / Sonic Team's Hedgehog Engine 2 (`hh::needle`, `hh::gfx`; its allocators log as `Rsdx`), with CRIWARE (CRI Atom / AtomEx sound, CRI Mana movies). Modules `main`, `sdk` and `subsdk0` (the SDK's movie module, not loaded: `main` takes nothing from it the host does not supply). **Plays on the Mac** (M6, 16 GB, macOS 27.2, 2026-11-13, scripted presses): the logos, the title, New Game, the intro cutscene drawn in real time, and the first Cyber Space stage, run through with rings picked up. The stage and the title run at 60 fps (frame time p50 16.67 ms, p95 16.7-16.8 ms, GPU about 4 ms a frame), the logos and the cutscene at 30 fps, which the game sets for them. No shader is missing from the map and the log has no `[RED]` line. The host's footprint was 1.4 GB in the stage. No IPA has been built.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `mem_reserve 0x270000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0x240000000` | [`game.tsv`](game.tsv) | the Switch 2's 9 GB pool: `main` sets a heap of the pool − 64 MB (8.92 GB) in a 9 GB heap region |
| `sys_resource 0x0` | `game.tsv` | the NPDM declares no system resource: no virtual address memory |
| `shader_groups /raw/=raw` | `game.tsv` | every RomFS shader is in `raw/NeedleShader.pac` |
| `depth_mode zero` | `game.tsv` | the game sets near = 0 at boot (`[nvn] depth mode near=0` in the log). Without the key the NSP script's prebuild made the pipeline bases for near = −w, and the first start built all 2,576 pairs again, 25 s before its first frame |

There is no game-specific patch and no cut: the rewriter needs only [a site for a compare and swap of the pair x18, x19](../../../docs/reference/rewriter.md#compare-and-swap-of-a-register-pair-that-is-x18-x19) (`main+0x19ce5c4`), which holds for any module.

## Files and shaders

- The dump holds the game as base and update v65536 with the same code modules; the update carries the RomFS: 26.4 GB under `raw/`, 1,435 PACx 403 archives (`.pac`), CRIWARE sound (`.acb`, `.awb`), 93 USM movies (6.05 GB) and texture streaming packages (`.ntsp`).
- 4,958 shaders: 1,554 from 233 [Needle containers with NVN2 records](../../../docs/reference/shaders.md#needle-containers-with-nvn2-records) in `main` (1,586 records), 3,404 more from `raw/NeedleShader.pac`, [an archive of LZ4 chunks](../../../docs/reference/shaders.md#shaders-in-hedgehog-engine-2s-pacx-403-archives) (3,626 records). Decompressed whole, the other 1,434 archives hold no Needle container and no control section. The split takes about a minute and the Mac shaders 3 minutes (none fails); the NSP script's pipeline prebuild, 2,576 pairs, 23 s.
- **Open:** the prewarm list (`pso_manifest.tsv`) is empty.

## On the Mac

- `python3 scripts/build_ipa_from_nsp.py <folder> --platform mac` builds the Mac shaders and the host and prints the run command; the first build takes about 7 minutes, 3 of them for the shaders and 1.5 for the 26 GB RomFS image.
- The game makes four allocators in its heap: system 128 MB, main 2,304 MB, nvn (CPU) 1,536 MB and nvn 2,048 MB.
- From an empty save, through `DM_CTL`: the title (New Game first) is up about 50 s after boot. `A` (New Game), `A` (Normal), `A` (Action style) start the intro cutscene; `A` held 2.5 s at its Skip prompt goes to the first Cyber Space stage, a few seconds of loading. `LUp` runs, `A` jumps.

## Found on this game

- The rewriter stopped on `caspal x18, x19, x4, x5, [x16]` ([Compare and swap of a register pair that is x18, x19](../../../docs/reference/rewriter.md#compare-and-swap-of-a-register-pair-that-is-x18-x19)).
- The shader split stopped on `main`'s Needle containers ([Needle containers with NVN2 records](../../../docs/reference/shaders.md#needle-containers-with-nvn2-records)) and could not read `raw/NeedleShader.pac` ([Shaders in Hedgehog Engine 2's PACx 403 archives](../../../docs/reference/shaders.md#shaders-in-hedgehog-engine-2s-pacx-403-archives)).
- `nvnDeviceGenerateDebugDomainId`, called once at boot, was unimplemented; the host now hands out a new number for each call.

## Open

- Only the start of the game was played, with scripted presses: the open zones, the Cyber Space stages after the first, combat and bosses, the USM movies, sound by ear, the other languages and the Definitive Edition's added content are not tested. The README's list of games that passed Mac gameplay testing waits for a longer run.
- The prewarm list ([Files and shaders](#files-and-shaders)).
- iPhone: no IPA built; the 9 GB heap region is the Switch 2's and has not been checked against an iPhone's memory.
