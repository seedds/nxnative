# Tetris Effect: Connected (`0100DE501433A000`)

Update 2.0.2.2 (`v1310720`), SDK 13.3.2. Unreal Engine 4.26 / 4.27 (`FMallocBinned3`) with Wwise on NVN. Modules `main`, `subsdk0` (`NintendoSDK_movie`) and `sdk`.

Single player is playable on the Mac; selecting Multiplayer reaches the multiplayer main screen, where user input does nothing (see [Open](#open)).

## What it needs

| Setting | Where | Why |
|---|---|---|
| `RedpointEOS` null check at `main+0xb64b8c` | [`patches.py`](patches.py) | `EOSSDK-Switch-Shipping.nro` is refused, so `IOnlineSubsystem::Get("RedpointEOS")` returns null; see [Online / RedpointEOS](#online--redpointeos) |
| `sys_resource 0x5000000` | [`game.tsv`](game.tsv) | the NPDM declares 80 MB of system resource (virtual address memory); `main+0xd0b81c` allocates a 62.00 GB address region |
| 5 GB guest layout: `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | `game.tsv` | from the survey's heap/memory configuration |
| `depth_mode zero` | `game.tsv` | the game calls `nvnDeviceSetDepthMode` with near = 0; pipeline prebuilding uses `main0_z` |
| `shader_groups ShaderArchive-Global-=global ShaderArchive-=game` | `game.tsv` | Unreal shader code archives decoded at build time |

## Online / RedpointEOS

- `main` imports `EOS_*` functions from `nro/EOSSDK-Switch-Shipping.nro`, which the host refuses in `ro::RegisterModuleInfo` (`dm/host/hle_ro.c`, `dm/tools/nro_collect.py`) so the game plays offline.
- Because `EOS_Platform_Create` (`main+0x410600`) stays a stub returning `0`, `OnlineSubsystemRedpointEOS` does not register `"RedpointEOS"`.
- **Workaround in `patches.py`:** `main+0xb64b18` (an Actor's `BeginPlay`, called from `AActor::DispatchBeginPlay` at `main+0x2742aec`) calls `IOnlineSubsystem::Get("RedpointEOS")` at `main+0xb64b88` and dereferences the returned pointer in `x20` at `main+0xb64bec` (`ldr x8, [x20]`, `sig=11 addr=0`) without a null check. `eos_null_guard` branches to the function's epilogue (`main+0xb64c3c`) when `x0 == 0`.
- Static inspection of `main` shows other unchecked `GetOnlineSubsystem("RedpointEOS")` call sites (such as `main+0xb64fe8`, `main+0xb652d8`, and `main+0xb691e0`) that were not hit during single-player testing.

## Current verification

- 2026-11-18, Apple M4 Mac (`mac-app`, `DM_PSO_MTL4=1`), build `v1310720` (`2.0.2.2`): with `eos_null_guard`, the game boots past the spinning logo and single player is playable.

## Open

- **Multiplayer screen unresponsive to input:** selecting Multiplayer from the main screen navigates to the multiplayer main screen, after which the game does not hang but user input does nothing (cannot select anything or go back to the main screen), requiring the user to quit the game.
- **iOS / iPhone:** not yet tested.
