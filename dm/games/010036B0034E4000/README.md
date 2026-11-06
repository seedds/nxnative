# Super Mario Party

Nintendo (NDcube), 2018. Title ID `010036B0034E4000`, update 1.1.0 (`v131072`). NintendoWare Bezel Engine 1.21.1 on NVN, SDK 9.4.0.

![Super Mario Party Status](https://img.shields.io/badge/status-in--game-green)

## What it needs

| Module | Build ID | Role |
|---|---|---|
| `main` | `fb321c86b35aa9755d6e42baa5e5d540` | Bezel Engine 1.21.1 runtime and core game loop |
| `subsdk0` | `f92af17ed915be68a70ed0b46ed256cd` | GLSLC compiler library |
| `subsdk1` | `8bdabfabc6964cd9e55b7079659f51f1` | `NintendoSDK_movie` (multimedia framework) |
| `sdk` | `014256a5d58180219805a7d05fba02e4` | NintendoSDK 9.4.0 |
| `nro/NX_Master/*.nro` | 20 NROs (1 NRR) | Dynamically loaded mode and minigame modules (`scene_common`, `main_menu`, `sugoroku`, `minigame`, `rhythm`, `river`, `sticker`, `toy`, `parade`, `ending`, etc.) |

The game calls `nn::ro::Initialize`, `nn::ro::RegisterModuleInfo`, and `nn::ro::LoadModule` at startup to load 18 of its 20 NRO modules into the process before reaching the title screen.

## Rendering

11,201 shaders across RomFS and `main` (4,284 vertex, 6,859 pixel, 58 compute):

- 10,624 in `Archive/*.bea` (Bezel Engine archives with embedded `bnsh` containers)
- 535 in `System.nxonnx32.bea`
- 42 embedded in `main`

All 11,201 shaders build cleanly for the Mac with 0 lost bindless accesses, producing 39,111 vertex+fragment shader pairs.

## Memory

Runs at 60 fps in Party Plaza and board selection with ~2.8 GB host resident memory and ~1.6 GB GPU memory allocated.

## Files

737 files in RomFS (2.74 GB total):

- 458 `.bea` Bezel Engine archives (`Archive/*.bea`, 2.35 GB)
- 257 `.bfstm` streamed audio tracks (`sound/stream/*.bfstm`, 0.38 GB)
- 20 `.nro` relocatable modules (`nro/NX_Master/*.nro`, 14.3 MB) and 1 `.nrr` registration file (`.nrr/NX_Master.nrr`)
- `Pack.nxonnx32.bin`, `Scene.nxonnx32.bin`, `System.nxonnx32.bea`

## Mac routes

`dm/games/010036B0034E4000/` carries the shader map (`shader_map.tsv`, `shader_facts.tsv`, `shader_pairs.tsv`, `pso_manifest.tsv`, `pso_pairs.tsv`) and `bf_want.tsv` for the 11,201 shaders and 22 dynamic modules (`main`, `subsdk0`, `subsdk1`, `sdk`, and the 18 loaded NROs).

Although the game calls `SetSupportedNpadStyleSet(0x18)` (`STYLE_JOYLEFT | STYLE_JOYRIGHT`, single horizontal Joy-Con), Bezel Engine's input polling loop checks `GetNpadStyleSet` and reads `NpadFullKeyState` when the host reports `STYLE_FULLKEY`, mapping `L + R` to `SL + SR` at the Joy-Con strap and title prompts.

## Found on this game

- [Forced vibration sessions](../../../docs/reference/services.md#forced-vibration-sessions) — `main+0x5d503c` calls `nn::hid::EndForcedVibrationSession()` during boot initialization; when unhooked, the SDK falls back to `nn::hid::detail::EndPermitVibrationSession`, attempts to connect to the `hid` service directly, and dereferences a null session pointer at `sdk+0x3526bc`.

## Open

- Single-Joy-Con motion minigames that rely on horizontal SixAxis sensor orientation have not been tested yet.
