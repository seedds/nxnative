# Twelve Minutes (`0100CD701434C000`)

Base game 1.0.1 (v0), SDK 12.3.7. Unity 2019.4.31f1 IL2CPP with FMOD Studio on NVN. Modules `main`, `subsdk0` and `sdk`. The uncapped layout reaches the opening credits and reloads the save on the Mac. Its 1.78 GiB IPA still needs a first-load and save-relaunch check on the affected iPhone. An earlier capped build passed the first load on an iPhone Duo running iOS 27. The older 2 GiB-pool build reached every ending on that device.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `UserPresence::Initialize`, `SetDescription` and `Commit` succeed | `hle_friends.c`, for every game | a failed `Initialize` ended the start-up before `MountSaveData`: [Friends presence is accepted offline](../../../docs/reference/services.md#friends-presence-is-accepted-offline) |
| 5.5 GiB guest layout with a 4.875 GiB alias region and the Switch's 3285 MiB pool | [`game.tsv`](game.tsv) | Both StandardAllocator's original request and Unity's later reservations fit during the first load. See [Memory](#memory). |
| `shader_groups /Data/=data`, `depth_mode zero` | `game.tsv` | from the NSP script's survey and heap check |

## Start-up

- **Resolved: the loading screen stayed at 22%** (seedds/nxnative#217). The game read `level0` and `Master Bank.bank`, then asked for nothing more; every guest thread waited with no work queued. It had called `account::TryOpenPreselectedUser`, `GetUserId`, `GetNickname` and `friends::UserPresence::Initialize` once each, and never `fs::MountSaveData`. With presence accepted it mounts `Save:`, reads `SaveData` and `GamePrefs`, and loads `level7` and `level8` within a second.

## Memory

- The NPDM declares 16 MiB of system resource, so `nninitStartup` gives StandardAllocator 3.14 GiB at alias +0.06 GiB (pool minus used memory, `main+0x299150`). Unity reserves its own regions beside it.
- With `DM_ALIAS_SIZE=0x118000000`, SDK 12.3.7's region search returned 2 (no fitting hole) at frame 71 for Unity's `0x103f000`-byte request. `VammManager::AllocateAddressRegion` converts it to `0x1803` at `sdk+0x346578`. The caller at `main+0x6624d8` receives that failure, and Unity retries with `0x83f000` bytes. That request fails on the same graphics worker just before `nvn_sparse_pool` dereferences a texture pool pointer containing float data.
- Reassigning the 128 MiB heap to the alias region inside the same 5 GiB block still reproduced the reservation failure. A diagnostic SDK with 64 KiB alignment for large regions also reproduced it at 4.375 GiB. The 4.875 GiB alias region passed startup with the original 3.14 GiB allocator reservation and the shared SDK alignment rules. The heap and stack keep their original sizes.
- On an M4 Pro with macOS 27.2.1, a 170-second run with `DM_ALIAS_SIZE=0x138000000` reached alias +4.77 GiB with 2.28 GiB mapped and no failed SDK region reservations. The host committed `SaveData` and `GamePrefs`. Relaunch read matching bytes from both files.

## Mac routes

- `DM_KEYS=50:A,55:A,60:A,70:A,80:A` from a fresh save: the title prompt at about frame 1,350, the opening credits by frame 3,500.
