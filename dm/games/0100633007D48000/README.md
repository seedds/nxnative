# Hollow Knight (`0100633007D48000`)

Update 1.5.12620 (v589824) over the base game. SDK 20.5.21. Unity IL2CPP on NVN, with FMOD. Modules `main`, `subsdk0` (the movie module, `NintendoSDK_movie`) and `sdk`.

It plays on the Mac. It was brought up with no host change: the NSP script's provisional profile ([dm/games/profile_guess.py](../profile_guess.py)) booted it as written, and that profile is this one. Every value was checked by play:

- the title;
- a save loaded from the menu, with play at a bench.

## What it needs

| Setting | Where | Why |
|---|---|---|
| 5 GB layout: `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | [`game.tsv`](game.tsv) | Main never sets a heap, and the game maps its memory itself in a 4.375 GB alias region. In its first 15 s its regions reach alias +1.30 GB. |
| `sys_resource 0x1000000` | `game.tsv` | The NPDM declares 16 MB of system resource, so `IsVirtualAddressMemoryEnabled` is true. |
| `shader_groups /Data/=data` | `game.tsv` | |

## Files and shaders

- 1,812 shaders: the `Shader` objects of `data.unity3d` and of the UnityFS bundles in `StreamingAssets`, read through `unityfs.py`.
- `main` holds 11 GLSLC outputs of Unity's player.

## Mac routes

From a save in slot 1, the title and the slot are passed with A, and play is under way by frame 5000: `DM_KEYS="f1200:A~0.3,f1350:A~0.3,…,f3450:A~0.3"` (A every 150 frames from frame 1200 to 3450).
