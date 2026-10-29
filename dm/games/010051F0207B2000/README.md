# Tomodachi Life: Living the Dream (`010051F0207B2000`)

Update 1.0.4 (`v262144`) over the base game, SDK 20.5.3. Nintendo EPD engine (`sead`, `agl`, `gsys`, Havok / Phive physics and animation) on NVN. Modules `main` and `sdk`.

**Current:** plays on the Mac at 60 fps: title screen, island initialization, Mii creation/customization, apartment view, dialogue, sound effects, and background music; no red lines.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `mem_used 0x6200000` | [`game.tsv`](game.tsv) | `main` and `sdk` take 98 MB of mapped image memory; without `mem_used 0x6200000`, the default 64 MB used space causes `SetHeapSize` (`0xc9400000` + `0x6200000` = `0xcf600000`) to exceed `app_pool 0xcd500000` (error `0x10801`). Sizing `mem_used 0x6200000` scales the heap request down to `0xc7300000`, fitting the pool |
| SDK 20.5.3 AddressRegionManager Cut | `dm/tools/sdk_patches.py` | `AddressRegionManager` constructor sets its node area limit to 1022 MB (`+0x3fe00000`) at `sdk+0x145bfc` and `sdk+0x145c68`. Cut down to 62 MB (`+0x3e00000`) via `dm/tools/sdk_patches.py` (`651781821e2c2e0b2bdfe905e8676f56`), saving 960 MB of virtual address space |
| 4 GB layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000`, `sys_resource 0x0` | [`game.tsv`](game.tsv) | standard 4 GB physical layout without virtual address memory |
| `romfs_marker RSDB` | [`game.tsv`](game.tsv) | marker for the game's resource databases |

## Shaders

- 14,386 shaders: compiled from monolithic Zstandard (`.zs`) packages in RomFS.
- All 14,386 shaders translate and compile cleanly to Metal libraries (`.metallib`).
- Offline pipeline archive built for Apple Silicon GPUs (`applegpu_g13g`, 386.4 MB).

## Files

- The RomFS is 6.2 GB containing 66,343 assets, models, and audio streams (`BWAV`).
