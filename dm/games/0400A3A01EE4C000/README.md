# Pokémon Pokopia (`0400A3A01EE4C000`)

A Switch 2 game: update v655360 (the title screen says Ver. 2.0.1), SDK 20.5.25 built for the Switch 2 (`Ounce`). Koei Tecmo's KTGL engine on NVN2, modules `main`, `subsdk0` (WebM movies) and `sdk`. **Plays on the Mac**: the opening, the character choice and the first area's tutorial at about 60 fps, including 31 minutes of the user's playtest with no GPU error and no crash. A save from an earlier run loads and continues. No IPA has been built.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `mem_reserve 0x270000000`, `app_pool 0x240000000` | [`game.tsv`](game.tsv) | Donkey Kong Bananza's 9 GB layout, not measured here. The NPDM declares no system resource |
| `mem_used 0x5600000` | `game.tsv` | the rewritten modules take 82 MB, more than the default 64 MB that `SetHeapSize` checks the game's request against |
| `pad_layout full` | `game.tsv` | |

## Files

- A dump folder whose `updates/<version>/` holds the whole RomFS; [`pack_dump.py`](../../../scripts/pack_dump.py) packs it without a base RomFS. RomFS: the newer RDB (as ROMANCE OF THE THREE KINGDOMS 8 REMAKE's) under `motor_asset_re/KIDS_PLATFORM_OUNCE`, and a `LD_C0_R.BIN` / `LI_C0_R.IDX` LINKDATA pair.
- Shaders: 131,426 in [`shader_map.tsv`](shader_map.tsv) ([KTGL on NVN2](../../../docs/reference/shaders.md#ktgl-on-nvn2-switch-2-glslc-outputs-and-ld_--li_-linkdata)). The Mac playtest's library held only the 8,157 that its runs asked for (with their pixel shaders), listed in [`shader_seen.tsv`](shader_seen.tsv); [Building selected Mac shaders](../../../docs/development.md#building-selected-mac-shaders) builds those alone. Such a library has no stamp and no `c1_full.tsv`, and each start logs that in red. The NSP script builds all 131,426: an estimated 9 hours on an M4 Pro (from 1,001 shaders) since [the builder stopped keeping the 4.4 GB of packages in memory](../../../docs/reference/shaders.md#a-shader-build-that-keeps-its-packages-in-memory-runs-one-worker-at-a-time) (before, players saw about one shader a second and ETAs of 20 to 200 hours: GitHub issue #1036).
- No `pso_manifest.tsv` yet, so each start also logs a red line for the missing seed list. In the runs here no draw waited on it.

## Shaders

- `cc679c7b4d3e703c` does not translate (`LDC with a bindless bank`). The boot loads it and logs a red line; it has not been seen drawing (open). The census also lists f55 three times, two more of that LDC and 16 BRX with no target (open).
- A `3D binding fallback` line fires once at about frame 2100: a 2D 8x8 texture in a 3D slot. The shader is not identified (open).

## Rendering

- Frames take about 16.7 ms. A cold file cache, as after a reboot, keeps the game on the loading icon for over a minute, and a clean run there says nothing: check that `[nvn] frame N: … calls` reached about 3,500.
- The lit HDR target is RG11B10, exposure a 2x1 RGBA32F image, tone map `075fd7285c64b798` with a 32³ LUT, final `ed5f4c6386ca621d`, the luminance histogram compute `2118ab534fac88c6`.
- `texraw` pads rows: an RG11B10 dump has a 16-byte stride with 4-byte pixels at the head of each row. It returns one slice of a 3D texture, and serials shift between runs.

## Driving it on the Mac

The first boot shows the language list: `A`, `LUp`, `A` picks English. The character choice takes `LLeft` before `A`. From a warm cache, `A A LLeft A A Plus` reaches walking in about 300 s. On later boots, `A` at the title screen a few times continues the save and reaches walking in about 110 s. The work folder has `run/boot.sh`, `run/hdrive.sh` (no window, timed control lines) and `run/spam.sh`.

## Open

- The picture has not been compared with a Switch 2. The user's playtest found it fine. Sunlit ground matches published iPhone shots of the game. Shaded ground and walls are lavender. The player carries fine contour lines, already present in colour target 5435, which may be the material. In some moving shots far blocks showed through near ones; this has not been rechecked since the fixes below.
- Only the first area has been played on this build. A new save written through the game has not been checked: the 31-minute playtest wrote none.
- An IPA has not been built. A full iOS shader build would be needed first.

## Found on this game

- [KTGL on NVN2: Switch 2 GLSLC outputs and LD_ / LI_ LINKDATA](../../../docs/reference/shaders.md#ktgl-on-nvn2-switch-2-glslc-outputs-and-ld_--li_-linkdata): the split found almost none of its shaders.
- [HADD2 and HMUL2 with an immediate](../../../docs/reference/shaders.md#hadd2-and-hmul2-with-an-immediate): the GPU hang and 88 ms frames.
- [Scaled shared memory addresses](../../../docs/reference/shaders.md#scaled-shared-memory-addresses): stride 3 in use, on accesses of every size.
- [HFMA2 .H0_NH1](../../../docs/reference/shaders.md#hfma2-h0_nh1).
- [Ampere instructions new in Pokémon Pokopia](../../../docs/reference/shaders.md#ampere-instructions-new-in-pokémon-pokopia).
- [A shader build that keeps its packages in memory runs one worker at a time](../../../docs/reference/shaders.md#a-shader-build-that-keeps-its-packages-in-memory-runs-one-worker-at-a-time): the full build at one or two shaders a second (resolved).
- [Integer buffer textures read through float slots](../../../docs/reference/rendering.md#integer-buffer-textures-read-through-float-slots).
- [The Joy-Con mouse pointer](../../../docs/reference/services.md#the-joy-con-mouse-pointer) and [GameShare guest displays](../../../docs/reference/services.md#gameshare-guest-displays): boot aborts.
