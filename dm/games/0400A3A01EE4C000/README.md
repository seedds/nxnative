# Pokémon Pokopia (`0400A3A01EE4C000`)

A Switch 2 game: update v327680, SDK 20.5.25 built for the Switch 2 (`Ounce`). Koei Tecmo's KTGL engine on NVN2, modules `main`, `subsdk0` (NintendoSDK_movie: WebM movies) and `sdk`. **Plays on the Mac** (M4 Pro, macOS 27): the opening, the character choice and the first area's tutorial, at about 60 fps. Not yet built as an IPA.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `shader_key_skip 0x8+0x28` | [`game.tsv`](game.tsv) | KTGL rewrites +0x08 and GLSLC's compile hash at +0x20 of a shader's NVN2 control section when it loads it: without the skip, 13 of the language screen's 27 programs named no packaged shader ([NVN2 shaders keyed without the bytes KTGL rewrites](../../../docs/reference/shaders.md#nvn2-shaders-keyed-without-the-bytes-ktgl-rewrites)) |
| `mem_used 0x6000000` | `game.tsv` | the rewritten modules take 82 MB, more than the default 64 MB of used memory that `SetHeapSize` checks the game's request against |
| `mem_reserve 0x270000000`, `app_pool 0x240000000` | `game.tsv` | Donkey Kong Bananza's 9 GB layout (not measured here); the NPDM declares no system resource |
| `pad_layout full` | `game.tsv` | |

## Files

- Built from a dump folder whose `base/` held only the update: the update's RomFS is the whole patched tree. RomFS: the newer RDB (as ROMANCE OF THE THREE KINGDOMS 8 REMAKE's): `motor_asset_re/KIDS_PLATFORM_OUNCE/root.rdb` and `system.rdb` over `.fdata` files named by KTID; movies in `movie_asset/ounce/*.webm`.
- Shaders: 99,207 in [`shader_map.tsv`](shader_map.tsv): 95,015 from one RDB asset (`0x38f972af`), 3,343 from `0x05d525ae`, 430 from the `LD_C0_R` LINKDATA, 153 from `main` ([KTGL on NVN2](../../../docs/reference/shaders.md#ktgl-on-nvn2-switch-2-glslc-outputs-and-ld_--li_-linkdata)). The play sessions so far asked for 6,004 of them, 2,129 from `0x38f972af`.
- A full shader build: the IPA script's single run needs about 45 GB of temporary SPIR-V, so it was run in chunks of 10,000 keys (`own_compilers.py <pkg> <cache> <keys…>`), about 1.4 hours a chunk on an M4 Pro for the iOS target.

## Shaders

With the Ampere instructions of [Ampere instructions new in Pokémon Pokopia and Mario Kart World](../../../docs/reference/shaders.md#ampere-instructions-new-in-pokémon-pokopia-and-mario-kart-world), all but one of the 6,004 shaders the sessions used translate, and 9,997 of the first 10,000 keys of the full build. The ones that do not: an LDC with a bindless bank (`Ampere b82 … LDC with a bindless bank or an address mode`, two) and a BRX with no target in its c1 jump table (two). 112 compute shaders have 35-48 KB of shared memory, past Metal's 32 KB: they need threadgroup memory in a device buffer (a change of its own).

## Driving it on the Mac

The character choice takes `LLeft` before `A` (A alone does nothing there). The first boot shows the language list: `A`, `LUp`, `A` picks English.

## Open

- An IPA and the iPad: not tried yet.
- LDC with a bindless bank, and BRX with no target in the c1 jump table (a few shaders).
- Nmouse and GameShare answer that nothing is there ([The Joy-Con mouse pointer](../../../docs/reference/services.md#the-joy-con-mouse-pointer), [GameShare (streamplay) without guests](../../../docs/reference/services.md#gameshare-streamplay-without-guests)).

## Found on this game

- [KTGL on NVN2: Switch 2 GLSLC outputs and LD_ / LI_ LINKDATA](../../../docs/reference/shaders.md#ktgl-on-nvn2-switch-2-glslc-outputs-and-ld_--li_-linkdata): the split found almost none of its shaders.
- [NVN2 shaders keyed without the bytes KTGL rewrites](../../../docs/reference/shaders.md#nvn2-shaders-keyed-without-the-bytes-ktgl-rewrites).
- [Integer buffer textures read through float slots](../../../docs/reference/rendering.md#integer-buffer-textures-read-through-float-slots) and [HADD2 and HMUL2 with an immediate](../../../docs/reference/shaders.md#hadd2-and-hmul2-with-an-immediate): the GPU hang from the first 3D scene on.
- [Scaled shared memory addresses](../../../docs/reference/shaders.md#scaled-shared-memory-addresses): its luminance histogram.
- [The Joy-Con mouse pointer](../../../docs/reference/services.md#the-joy-con-mouse-pointer), [GameShare (streamplay) without guests](../../../docs/reference/services.md#gameshare-streamplay-without-guests): boot aborts.
