# Super Mario 64 in Super Mario 3D All-Stars (`010049900F546001`)

Program 1 of the application `010049900F546000`, update 1.1.4 (v393216). SDK 10.8.1. Modules `main` and `sdk`.

Nintendo's N64 layer:

- the game's MIPS code is recompiled to arm64 in an NRO (`rom/Stardust_JP/05_UNSMJ3.002.nro`), which the game loads with `nn::ro`;
- whatever the recompiled code does not cover runs through a JIT plugin (`nro/EmuSystemJITPlugin.nro`) under `nn::jit`;
- it renders with Vulkan ([`hle_vk.m`](../../gfx/vk/hle_vk.m)).

It plays on the Mac.

## What it needs

| Setting | Where | Why |
|---|---|---|
| Packed one program at a time | NSP script `--program 1` (`PROGRAM_INDEX`), `nx_extract --program` | The application is 4 programs (launcher 0, Super Mario 64 1, Super Mario Sunshine 2, Super Mario Galaxy 3), each with its own ExeFS and RomFS. |
| `launch_param lang8 f32:1 f32:1` | [`game.tsv`](game.tsv), host `hle_oe.c` | The launcher (program 0) starts each game with `oe::ExecuteProgram` and a 16-byte parameter: the language code and two volumes. Without one, the game exits at boot. |
| `nn::jit` | host [`hle_jit.c`](../../host/hle_jit.c) | See [The JIT plugin](#the-jit-plugin). |
| `nn::web::OfflineWebSession` | host `hle_offline.c` | Each game starts the launcher's pause-menu page hidden at boot. The session starts, never exits on its own, takes every message and sends none. |
| `modules main sdk` | `game.tsv` | `subsdk0` (Vulkan), `subsdk1` (OpenGL) and `subsdk2` (GLSLC) are not loaded: `main` takes nothing from them that the host does not supply. |
| 3.5 GB layout: `mem_reserve 0xe0000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000`, `sys_resource 0` | `game.tsv` | Main sets a heap of at most the pool − 64 MB − 520 MB of bss. |

## The JIT plugin

On the Switch, the `jit:u` sysmodule loads the plugin NRO and calls its `nnjitplugin*` functions. The plugin writes code through a RW view of two code memories, which the game sees R-X and R--.

The host has no such sysmodule. Instead:

- The plugin is the rewritten copy that `nro_collect.py` collected by its SHA-256, loaded through `nro_find` / `nro_load`.
- It is called directly on a host thread with an 8 MB stack, as the sysmodule's thread would be.
- The game's work buffer stays the plugin's RW view. The game's view is a second mapping of the same pages (`mach_vm_remap`).
- The call order and argument layout follow [switchbrew's JIT services page](https://switchbrew.org/wiki/JIT_services).

The code the plugin writes is not rewritten, so it must need no rewrite. Super Mario 64's has none.

