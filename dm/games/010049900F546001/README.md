# Super Mario 64 in Super Mario 3D All-Stars (`010049900F546001`)

Program 1 of [Super Mario 3D All-Stars](../010049900F546000/README.md), packed with it, update 1.1.4 (v393216), SDK 10.8.1. Modules `main` and `sdk`.

Nintendo's N64 layer ("EmuAppNX64") on the Japanese Shindou ROM with English text patched in by Lua scripts:

- the MIPS code is recompiled to arm64 in an NRO (`rom/Stardust_JP/05_UNSMJ3.002.nro`), which the game loads with `nn::ro`;
- a precompiled block list goes through a JIT plugin (`nro/EmuSystemJITPlugin.nro`) under `nn::jit`, 14 MB of code at boot, and a few blocks are compiled in play;
- it renders with Vulkan ([`hle_vk.m`](../../gfx/vk/hle_vk.m)).

Tested on the Mac: the Mario head, the file select, a new file through Peach's letter and Lakitu to the castle grounds, walking to the castle, at 30 fps; 0.64 GB resident there. The pause page and Return to title selection work.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `launch_param lang8 f32:1.12 f32:2.24` | [`game.tsv`](game.tsv) | what the launcher hands over, for a run of this program alone; without one the game exits at boot |
| `jit_key_data 4:0-7,24-27`, `jit_out_code 4:entries=4,count=0x20004,size=0x20000` | `game.tsv` | the image an iPhone serves `nn::jit` from: command 4's request leaves 4 padding bytes uninitialised, and its output lists the entry points the game computes from the address it asked for ([`nn::jit` served from an image on iOS](../../../docs/reference/services.md#nnjit-served-from-an-image-on-ios)) |
| `modules main sdk` | `game.tsv` | `subsdk0` (Vulkan), `subsdk1` (OpenGL) and `subsdk2` (GLSLC) are not loaded: the host supplies what `main` takes from them |
| 3.5 GB layout: `mem_reserve 0xe0000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000`, `sys_resource 0` | `game.tsv` | `main` sets a heap of the pool − 64 MB − 520 MB of bss, then cuts it into fixed heaps: with a smaller pool the last size goes negative and the boot crashes |

## Controls

A closes text, B jumps, Y attacks; Plus starts at the head, Minus opens the launcher's pause page.

## Found on this game

- [`nn::jit` taken over by the host](../../../docs/reference/services.md#nnjit-taken-over-by-the-host), [`nn::jit` served from an image on iOS](../../../docs/reference/services.md#nnjit-served-from-an-image-on-ios).
- Vulkan layer: [`VK_EXT_debug_report`](../../../docs/reference/rendering.md#vk_ext_debug_report), [Specialization constants](../../../docs/reference/rendering.md#specialization-constants), [Packed 16-bit and 8-bit formats](../../../docs/reference/rendering.md#packed-16-bit-and-8-bit-formats), [Image clears, dynamic stencil and line width](../../../docs/reference/rendering.md#image-clears-dynamic-stencil-and-line-width), [What a game tears down that it should not](../../../docs/reference/rendering.md#what-a-game-tears-down-that-it-should-not).

## Open

- `[vk] [RED] shader 9da92c000ddbc3c8: set 1 is not bound`: untextured draws of its one fragment shader, which declares two samplers; the picture is right on the routes tried. Not looked into further.
- A course was not played through. An `nn::jit` request the iPhone's image lacks stops the app: play that part in a Mac build (its run command writes `jit_capture/play`) and pack again.
