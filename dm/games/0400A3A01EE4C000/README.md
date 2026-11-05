# Pokémon Pokopia (`0400A3A01EE4C000`)

A Switch 2 game: update v655360 (the title screen says Ver. 2.0.1), SDK 20.5.25 built for the Switch 2 (`Ounce`). Koei Tecmo's KTGL engine on NVN2, modules `main`, `subsdk0` (WebM movies) and `sdk`. **Plays on the Mac**: the opening, the character choice and the first area's tutorial at about 60 fps, including 31 minutes of the user's playtest with no GPU error and no crash. A save from an earlier run loads and continues. No IPA has been built.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `mem_reserve 0x270000000`, `app_pool 0x240000000` | [`game.tsv`](game.tsv) | Donkey Kong Bananza's 9 GB layout, not measured here. The NPDM declares no system resource |
| `mem_used 0x5600000` | `game.tsv` | the rewritten modules take 82 MB, more than the default 64 MB that `SetHeapSize` checks the game's request against |
| `pad_layout full` | `game.tsv` | |
| `depth_mode zero` | `game.tsv` | the game sets near = 0 (the log's `depth mode` line). Without the key the pipeline archive made offline holds the other entry point, and the host refuses it |

## Files

- A dump folder whose `updates/<version>/` holds the whole RomFS; [`pack_dump.py`](../../../scripts/pack_dump.py) packs it without a base RomFS. RomFS: the newer RDB (as ROMANCE OF THE THREE KINGDOMS 8 REMAKE's) under `motor_asset_re/KIDS_PLATFORM_OUNCE`, and a `LD_C0_R.BIN` / `LI_C0_R.IDX` LINKDATA pair.
- Shaders: 131,426 in [`shader_map.tsv`](shader_map.tsv) ([KTGL on NVN2](../../../docs/reference/shaders.md#ktgl-on-nvn2-switch-2-glslc-outputs-and-ld_--li_-linkdata)). The Mac playtest's library held only the 8,157 that its runs asked for (with their pixel shaders), listed in [`shader_seen.tsv`](shader_seen.tsv); the NSP script's `--seen-shaders` builds those alone ([Building selected Mac shaders](../../../docs/development.md#building-selected-mac-shaders)). Such a library has no stamp and no `c1_full.tsv`, and each start logs that in red. On chain `20b9b1c2e4e4` an M4 Pro with other jobs running took 18 minutes for the 8,158 shaders of the list and the last logs (Metal's compile cache held nearly all of its libraries: 113 compiled), or 16 minutes with all of them cached, then about 5 minutes for the 7.9 GB Mac app. That app boots to walking at about 16.7 ms a frame with no GPU error and no shader reported missing, and later areas are untested on it. The NSP script builds all 131,426: an estimated 2.5 hours on an M4 Pro (from 1,001 shaders; no full build has run) since [the builder stopped keeping the 4.4 GB of packages in memory](../../../docs/reference/shaders.md#a-shader-build-that-keeps-its-packages-in-memory-runs-one-worker-at-a-time) and [its compiles stopped instantiating Metal's texture classes again](../../../docs/reference/shaders.md#each-metal-compile-instantiates-metals-texture-classes-again) and [its Python stopped limiting it](../../../docs/reference/shaders.md#rewrites-that-try-every-character-of-a-large-source) (before all of these, players saw about one shader a second and ETAs of 20 to 200 hours: GitHub issue #1036). The map's 105,534 pixel shaders each have `_ba` and `_bl` libraries, 57% of its compiles before those whose MSL matches another's, while the runs on the 8,157-shader library opened none of them (file access times; 7,985 of its 8,157 `_b` libraries were opened). Those runs only reached the first area (open).
- No `pso_manifest.tsv` yet, so each start also logs a red line for the missing seed list. In the runs here no draw waited on it.

## Shaders

- Resolved: `cc679c7b4d3e703c` did not translate (`LDC with a bindless bank or an address mode`): an indexed read of c1 with the bank in the address register, found on Hyrule Warriors: Age of Imprisonment, which ships the same shader ([LDC with the bank in its address register](../../../docs/reference/shaders.md#ldc-with-the-bank-in-its-address-register-is)); all 8,158 shaders of the playtest library now translate, and the walk below logged no shader failing to load. The census also lists f55 three times, two more of that LDC and 16 BRX with no target: whether they are the forms since translated ([Barrier moves around warp shuffles](../../../docs/reference/shaders.md#barrier-moves-around-warp-shuffles)) is not checked (open).
- A `3D binding fallback` line fires once at about frame 2100: a 2D 8x8 texture in a 3D slot. The shader is not identified (open).

## Rendering

- Frames take about 16.7 ms. A cold file cache, as after a reboot, keeps the game on the loading icon for over a minute, and a clean run there says nothing: check that `[nvn] frame N: … calls` reached about 3,500.
- The lit HDR target is RG11B10, exposure a 2x1 RGBA32F image, tone map `075fd7285c64b798` with a 32³ LUT, final `ed5f4c6386ca621d`, the luminance histogram compute `2118ab534fac88c6`.
- `texraw` pads rows: an RG11B10 dump has a 16-byte stride with 4-byte pixels at the head of each row. It returns one slice of a 3D texture, and serials shift between runs.

## Driving it on the Mac

The first boot shows the language list: `A`, `LUp`, `A` picks English. The character choice takes `LLeft` before `A`. From a warm cache, `A A LLeft A A Plus` reaches walking in about 300 s. On later boots, `A` at the title screen a few times continues the save and reaches walking in about 110 s. The work folder has `run/boot.sh`, `run/hdrive.sh` (no window, timed control lines) and `run/spam.sh`. The packed app takes about a minute to reach the language list (14 s of it the prepare step on a second start), so the fixed times in `run/spam.sh` miss it: press the keys once the list shows.

## Open

- The picture has not been compared with a Switch 2. The user's playtest found it fine. Sunlit ground matches published iPhone shots of the game. Shaded ground and walls are lavender. The player carries fine contour lines, already present in colour target 5435, which may be the material. In some moving shots far blocks showed through near ones; this has not been rechecked since the fixes below.
- Only the first area has been played on this build. A new save written through the game has not been checked: the 31-minute playtest wrote none.
- The pipeline archive saved nothing on the walk from the language list to the first area, on the partial library: archive lookups found 0 and found none 655 times, and 216 bases were compiled, though the host's probe found the archive's bases. The draws' pairs seem not to be those of `shader_pairs.tsv`, which come from BNSH variations. Whether a full library differs is not checked.
- An IPA has not been built. A full iOS shader build would be needed first.

## Found on this game

- [One compile for a pixel shader's side entry points](../../../docs/reference/shaders.md#one-compile-for-a-pixel-shaders-side-entry-points): 57% of the map's compiles were `_ba` and `_bl` libraries; a cold build of 1,021 shaders now takes 48% of the CPU on the Mac's flags and 47% on the iPhone's.
- [KTGL on NVN2: Switch 2 GLSLC outputs and LD_ / LI_ LINKDATA](../../../docs/reference/shaders.md#ktgl-on-nvn2-switch-2-glslc-outputs-and-ld_--li_-linkdata): the split found almost none of its shaders.
- [HADD2 and HMUL2 with an immediate](../../../docs/reference/shaders.md#hadd2-and-hmul2-with-an-immediate): the GPU hang and 88 ms frames.
- [Scaled shared memory addresses](../../../docs/reference/shaders.md#scaled-shared-memory-addresses): stride 3 in use, on accesses of every size.
- [HFMA2 .H0_NH1](../../../docs/reference/shaders.md#hfma2-h0_nh1).
- [Ampere instructions new in Pokémon Pokopia](../../../docs/reference/shaders.md#ampere-instructions-new-in-pokémon-pokopia).
- [A shader build that keeps its packages in memory runs one worker at a time](../../../docs/reference/shaders.md#a-shader-build-that-keeps-its-packages-in-memory-runs-one-worker-at-a-time): the full build at one or two shaders a second (resolved).
- [Each Metal compile instantiates Metal's texture classes again](../../../docs/reference/shaders.md#each-metal-compile-instantiates-metals-texture-classes-again): the full build's remaining hours.
- [Rewrites that try every character of a large source](../../../docs/reference/shaders.md#rewrites-that-try-every-character-of-a-large-source): the builder's Python limited the full build at about 0.12 s a shader, 0.05 s after.
- [A library that depends on its source's file name](../../../docs/reference/shaders.md#a-library-that-depends-on-its-sources-file-name): two `_bl` files differed between cold builds of the same code.
- [Integer buffer textures read through float slots](../../../docs/reference/rendering.md#integer-buffer-textures-read-through-float-slots).
- [The Joy-Con mouse pointer](../../../docs/reference/services.md#the-joy-con-mouse-pointer) and [GameShare guest displays](../../../docs/reference/services.md#gameshare-guest-displays): boot aborts.
- [Block-compressed storage the CPU wrote before first use](../../../docs/reference/nvn-objects.md#block-compressed-storage-the-cpu-wrote-before-first-use): its `IMAGE` BC1 / BC7 icons showed other items' pictures once their storage was reused (resolved).
