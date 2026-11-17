# Twelve Minutes (`0100CD701434C000`)

Base game 1.0.1 (v0), SDK 12.3.7. Unity 2019.4.31f1 IL2CPP with FMOD Studio on NVN. Modules `main`, `subsdk0` and `sdk`. Plays to every ending on an iPhone (iPhone Duo, iOS 27) with no crash, hang or visible or audible problem. On the Mac: the title prompt and the opening credits; not played further.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `UserPresence::Initialize`, `SetDescription` and `Commit` succeed | `hle_friends.c`, for every game | a failed `Initialize` ended the start-up before `MountSaveData`: [Friends presence is accepted offline](../../../docs/reference/services.md#friends-presence-is-accepted-offline) |
| 5 GB guest layout: `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000`; the Switch's pool `app_pool 0xCD500000` with `stdalloc_cap 0x7C000000` | [`game.tsv`](game.tsv) | StandardAllocator's 3.14 GB region left Unity's regions too little of the iPhone's alias region; see [Memory](#memory) |
| `shader_groups /Data/=data`, `depth_mode zero` | `game.tsv` | from the NSP script's survey and heap check |

## Start-up

- **Resolved: the loading screen stayed at 22%** (seedds/nxnative#217). The game read `level0` and `Master Bank.bank`, then asked for nothing more; every guest thread waited with no work queued. It had called `account::TryOpenPreselectedUser`, `GetUserId`, `GetNickname` and `friends::UserPresence::Initialize` once each, and never `fs::MountSaveData`. With presence accepted it mounts `Save:`, reads `SaveData` and `GamePrefs`, and loads `level7` and `level8` within a second.

## Memory

- The NPDM declares 16 MB of system resource, so `nninitStartup` hands application pool − used memory to StandardAllocator (`[mem] address region … returning to main+0x299150`).
- With the Switch's 3285 MB pool that region is 3.14 GB at +0.06 GB, and Unity's own regions go above it. With `DM_ALIAS_SIZE=0x118000000` on the Mac, and on the iPhone, the high-water mark reached +4.26 GB of 4.38 GB at the end of the first load, then `CRASH sig=11 pc=nvn_sparse_pool+0x4 lr=mtl_reg_tex+…` on a texture whose pool word held floats (`0xbdc33c27ffffffff`), from `UnityGfxDeviceWorker` (`main+0x29b774`).
- With `stdalloc_cap 0x7C000000` StandardAllocator gets 1.94 GB of address space (`StandardAllocator::Initialize(nullptr, 3.14 GB) … 1.94 GB of address space, the profile's stdalloc_cap`) and the pool stays the Switch's 3285 MB, so the game reads the memory it would on the Switch. 130 s on the Mac with the iPhone's alias size (title prompt, opening credits) end at +3.51 GB with at most 2.28 GB mapped and no crash. On the iPhone Duo the game plays to every ending; an earlier build with a 2 GB `app_pool` instead of the cap gave the same figures and did too.
- If a later build maps more than 1.94 GB through StandardAllocator, raise `stdalloc_cap` and check the high-water mark with `DM_ALIAS_SIZE=0x118000000` again.

## Mac routes

- `DM_KEYS=50:A,55:A,60:A,70:A,80:A` from a fresh save: the title prompt at about frame 1,350, the opening credits by frame 3,500.
