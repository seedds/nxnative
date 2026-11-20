# Fate/hollow ataraxia REMASTERED (`010070E022546000`)

The Switch version, update 1.0.2 (v131072) over the US base NSP (the JP NSP shares the `main` build ID), SDK 16, TYPE-MOON's Fuzz engine (`smash::fw`, the Smash-family engine line) rendering through Nintendo's OpenGL ES stack. Modules `main`, `subsdk3` and `sdk`; `subsdk0` (`NintendoSDK_OpenGL`), `subsdk1` (`GLSLC`) and `subsdk2` (`NintendoSDK_Vulkan`) are never read for anything the host must supply. RomFS: the game data under `obb/` (`pack11d.bin` marker) and `data/`, the KAG script engine compiled into `main`.

Brought up on the Mac. Boot, the opening, the title and the Entrance savedata screen, a 1.0.2 save loaded from slot 0 (all three slots render their stored thumbnails, dates and play time), in-game scenes play and the game autosaves (run 2026-11-20). The game is a visual novel: it renders fullscreen and its menus take the touchscreen — on the Mac the mouse click through the window, in handheld mode (`DM_OPMODE=0`); the docked layout shows the picture but the menu only answers touch.

## What it needs

| Setting | Where | Why |
|---|---|---|
| 4 GB layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000`, `sys_resource 0x0` | [`game.tsv`](game.tsv) | no virtual address memory (the NPDM declares 0 MB of system resource); main raises its heap within the pool |
| two `main` cuts of build `6c16aa06…` | [`patches.py`](patches.py) | the Entrance savedata-version gate and the config lookup's hollow default; see [docs/investigations/fate-ha-save-20261117.md](../../../docs/investigations/fate-ha-save-20261117.md) |
| `glGetShaderSource` returns the kept source | `dm/gfx/gl/hle_gl.m` | the game reads its shader source back after compiling; an empty reply made it branch to 0 |

## Savedata

- **Resolved: every slot rendered as an empty START, saved and imported alike** (Entrance screen). `setHollowSlotInfo` (`main+0xdf8750`) fills a slot only when the version string behind the `check_savedata_version` storage query reaches 438 (`atoi` `main+0xdf91e0`, compare `main+0xdf87ac`, branch to the skip `main+0xdf87b4`); the Switch 1.0.2 data carries `2` there — the 438 migration is a PC 1.0.4 feature (`data/.patch/dep.dat` reads `1.0.4.729` on the PC files, and the Switch NSP ships no such bump), so the gate filters valid saves. With the branch removed the slots render from the stored data.
- **Resolved: with the slots rendered, their START/COPY/DELETE buttons still ignored input.** The same storage query's default when `smash::fw::config`'s registry never filled is 1 (`mov w3,#1` at `main+0xdf9238`), which marks the slot hollow; the retail data sets the key, the host environment has no config source. With the default moved to 0 the buttons answer and slot 0 loads. Full A/B evidence in [docs/investigations/fate-ha-save-20261117.md](../../../docs/investigations/fate-ha-save-20261117.md).

## Rendering

- **Resolved: the game jumped to 0 soon after the shader programs linked.** The Fuzz engine calls `glGetShaderSource` on its compiled shaders and uses the returned text; the host had no implementation (the unimplemented stub left the buffer empty and reported length 0, and the engine then walked a null source's line table). `glGetShaderSource` now hands back the text `glShaderSource` kept (`dm/gfx/gl/hle_gl.m`).
