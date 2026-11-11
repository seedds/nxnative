# Warhammer Survivors (`0100CAA026246000`)

Base game 1.0.0 (v0), SDK 21.4.7 (its own `sdk` build `ab3835b2a37b1916e296f27e82dd4e36`). Unity 6000.0.74f1 IL2CPP with FMOD, on NVN. Modules `main` (`8684ba349faec7f25c7f7b061dd46f30`), `subsdk0` (`92a5f1a1e8d50e4d05b2212f564bf226`, NintendoSDK_movie) and `sdk`. Plays on the Mac (Mac app 765.5 MB); 60 fps.

On the Mac: boot, the opening video and cinematics, the title screen, menus, character selection, and gameplay with FMOD audio. Runs at 60 fps (frame time p50 16.66 ms, p95 16.94 ms; GPU ~0.4 ms a frame, CPU ~38% on one core), memory flat at ~2.1 GB resident and ~1.9 GB GPU allocated; 0 audio underruns.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `sys_resource 0x1000000` | [`game.tsv`](game.tsv) | the NPDM declares 16 MB of system resource, so virtual address memory is enabled |
| 5 GB block: `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | [`game.tsv`](game.tsv) | Unity virtual address block (4.375 GB alias region) |
| `depth_mode zero` | [`game.tsv`](game.tsv) | the game calls `nvnDeviceSetDepthMode` with near = 0 |

## Memory

- 16 MB of system resource, no heap (`main` never sets one), Unity reserving and committing its memory in the alias region through `nn::os::AllocateAddressRegion` and `AllocateMemoryPages`.
- Measured on the Mac: alias region mapped up to +4.52 GB, application pool 3.21 GB, resident footprint 2,134 MiB, GPU allocated 1,923 MB, flat over 2,100+ frames.

## Rendering

- NVN device API 55.15 (SDK 21). Window size 1920x1080 format 0x25 with 3 swapchain textures, depth mode near=0.
- Shaders are UnityFS bundles under `Data/` (`shader_groups /Data/=data`).

## Audio and Video

- FMOD audio output (stereo 48000 Hz) via `nn::audio`.
- Video playback handled through `subsdk0` (`NintendoSDK_movie`).

## Mac route

Build with `scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> --platform mac`.
At the title screen, press A (or Space / Enter) to enter the main menu. Select Play / Start, choose a character, and start the run. In gameplay: left stick / WASD / arrow keys to move, Space / A to confirm or interact. Weapons fire automatically in survivor mode.

## Found on this game

- `nn::oe::SetPerformanceConfiguration(int)` hooked in `hle_oe.c`: [SetPerformanceConfiguration without an apm session](../../../docs/reference/services.md#setperformanceconfiguration-without-an-apm-session).

## Open

- iOS validation: unsigned IPA builds, but on-device testing on an iPhone or iPad remains to be verified.
- Long-session endurance, later chapters, and weapon progression beyond the initial stages.

