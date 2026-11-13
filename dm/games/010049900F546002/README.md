# Super Mario Sunshine in Super Mario 3D All-Stars (`010049900F546002`)

Program 2 of [Super Mario 3D All-Stars](../010049900F546000/README.md), packed with it, update 1.1.4 (v393216), SDK 9.5.1. Modules `main`, `subsdk0` (the movie library) and `sdk`.

NERD's Hagi layer, as in Super Mario Galaxy ([010099C022B96000](../010099C022B96000/README.md)):

- the GameCube's PowerPC code is recompiled to arm64 in an LZ4-packed NRO (`data/gekko/MarioSunshine/nx/MarioSunshine.nrolz`), which Hagi unpacks and loads with `nn::ro`;
- it renders with Vulkan, with GX shaders generated at run time and captured on the Mac (1,154 modules).

Tested on the Mac, 30 fps: the opening, the file select, a new file's plane scene and control at Delfino Airstrip, 0.95 GB resident there. FLUDD was not reached. The pause page and Return to title selection work.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `launch_param lang8 f32:0.5 f32:1` | [`game.tsv`](game.tsv) | what the launcher hands over, for a run of this program alone; without one the game exits at boot |
| 6 GB layout: `mem_reserve 0x180000000`, `mem_alias 0x110000000`, `mem_stack 0x10000000`, `app_pool 0x50000000`, `sys_resource 0x800000` | `game.tsv` | see [Memory](#memory) |
| Coroutine stack region 1 GB → 64 MB | [`patches.py`](patches.py), `main` build `b21e3733…` | see [Memory](#memory). Binding `build`: a size in Hagi's own code. 1.1.4 is the last update |
| `vk_capture.tsv` | profile | the keys of the SPIR-V modules Hagi generates, which end the NSP script's capture |
| `.nrolz` collected | [`nro_collect.py`](../../tools/nro_collect.py) | a big-endian u32 size, then one LZ4 block; the collector matches the unpacked image against the NRRs |
| `pad_layout full`, `op_mode handheld` | `game.tsv` | as Galaxy: a 3D pad, and Hagi's preset (`hagi.config`) has its pointer on the touch screen in handheld mode. Sunshine reads only the pad |

## Memory

Hagi maps the GameCube's 32-bit address space 1:1 in one 4 GB + 2 MB address region, then reserves its coroutine stacks as a second region of 1 GB (`main+0x151218`). The SDK-wide cuts ([The SDK's address-region node area](../../../docs/reference/memory.md#the-sdks-address-region-node-area)) already apply to this `sdk`, so the node area is 62 MB and regions align to 2 MB. Both regions and a heap do not fit the 6 GB iOS allows one reservation unless the second shrinks: Hagi hands its stacks out with a bump allocator and PR #193 measured under 2 MB of them on 1.0.0, so 64 MB is ample. The Mac runs the same layout. The heap region is 1.25 GB; about 450 MB of it was touched.

## Controls

A talks and picks a block at the file select (stand under one: Mario starts between A and B, where A only jumps), Plus skips scenes; B closes a talk; Minus opens the launcher's pause page.

## Found on this game

- [Loading NROs with `ro::LoadModule`](../../../docs/reference/services.md#loading-nros-with-roloadmodule) (`.nrolz`).
- [Occlusion queries that span render encoders](../../../docs/reference/rendering.md#occlusion-queries-that-span-render-encoders).

## Open

- The two GX geometry shaders (`93df6946e1d3efc0`, `c48083b2a5c2f72e`: points and lines expanded to quads) are not translated, the same two as both Galaxies: those draws come out one pixel wide. Whether FLUDD's spray is one was not seen.
- Logic op pipelines (`VK_LOGIC_OP_INVERT`, Hagi creates about a dozen at boot) did not draw on the route tried; a draw logs a red line.
- PR #193 saw Hagi crash once in about 12 runs as the plane movie started (a trap at `main+0xe00d4` on 1.0.0, a race inside Hagi's video replacement); not seen here.
