# Pokémon Violet (`01008F6008C5E000`)

Update 4.0.0 (v786432) over the base game, SDK 13.3.13. Game Freak's Trinity engine on NVN, with Wwise audio; every file lives in the Trinity packs of `arc/data.trpfs` (a 9.24 GB RomFS), as in Pokémon Legends: Z-A. Modules `main` and `sdk` (`subsdk0`, NintendoSDK_movie, is not loaded). Started from the provisional profile the NSP script wrote from its survey (`dm/games/profile_guess.py`). On the Mac it runs through the language select, character creation, the opening and into play, the player's room and the house's ground floor, at a steady 30 fps (2026-10-26); the IPA (10.3 GB) is built but not yet tested on the iPhone.

## What it needs

| Setting | Where | Why |
|---|---|---|
| Allocator address space 12 GB → 4 GB | [`patches.py`](patches.py), `main` `709bfd66…` at `main+0x167d54c` | `StandardAllocator::Initialize(nullptr, 0x300000000, false)` does not fit a phone; see [Memory](#memory) |
| Node area cut 1 GB → 62 MB | `dm/tools/sdk_patches.py`, `sdk` build `bb560bdf…` (`sdk+0x37c760` / `+0x37c7fc`) | the alias region must hold the node area and the allocator's region |
| Regions of 1 GB and more aligned to 32 MB instead of 1 GB | `dm/tools/sdk_patches.py`, `sdk+0x37c95c` | at 1 GB alignment the 4 GB region did not fit behind the node area; see [Memory](#memory) |
| 5 GB guest layout: `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000`, `sys_resource 0x1000000` | [`game.tsv`](game.tsv) | the guesser's layout for a game with a 16 MB system resource (virtual address memory) that never sets a heap; the 4.375 GB alias region holds 62 MB + 4 GB |
| `mem_map_limit on` | `game.tsv` | as Pokémon Legends: Z-A: `nninitStartup` allocates and frees all it can, and the SDK keeps those pages mapped until a map fails (one map of 512 KB refused at 3.15 GB in the first minute; in play the alias region holds 0.6 to 2.3 GB) |
| Bluetooth LE answered without a Bluetooth service | `dm/host/hle_bluetooth.c` (general) | the game initializes BLE for its Pokémon GO link in `nnMain` and aborts without the `bt` service; see [Services](#services) |
| `depth_mode zero` | `game.tsv` | the game sets the near = 0 depth mode at start (`nvnDeviceSetDepthMode`), which the pipeline bases made before it runs (the Mac prebuild, the iPhone's offline archive) have to know |
| `pad_layout full` | `game.tsv` | the camera is on the right stick |

## Memory

- With virtual address memory (a 16 MB system resource), `nninitStartup` (`main+0x2b7da90`, through its helper at `main+0x167d510`) initializes its StandardAllocator over 12 GB of address space, `StandardAllocator::Initialize(nullptr, 0x300000000, false)` (`main+0x167d554`), and asks `GetAllocatableSize` right after, as Pokémon Legends: Z-A's does.
- **Resolved: `StandardAllocator::Initialize` aborted in `nninitStartup`** with the provisional profile: an `nn::diag` abort at `sdk+0x3251a0` (`Initialize+0xd4`) from `main+0x167d558`, the host exiting with 42. Two causes, one behind the other. The alias region had to hold the SDK's 1 GB node area and the 12 GB region: the first two cuts above are Pokémon Legends: Z-A's on this game's builds ([StandardAllocators over 12 to 62 GB of address space](../../../docs/reference/memory.md#standardallocators-over-12-to-62-gb-of-address-space), [The SDK's address-region node area](../../../docs/reference/memory.md#the-sdks-address-region-node-area); the constructor is at `sdk+0x37c750`). With them the abort stayed: a probe on `VammManager::AllocateAddressRegion` (`sdk+0x37c860`) showed its first request, the 4 GB, returning 0x1803 (no address space), because this SDK build's placement helper (`sdk+0x37c8f0`) aligns regions of 1 GB and more to 1 GB, and 62 MB then 4 GB at that alignment do not fit the 4.375 GB alias region: the third cut, [1 GB alignment of regions of 1 GB and more](../../../docs/reference/memory.md#1-gb-alignment-of-regions-of-1-gb-and-more). The log then shows `alias region mapped up to +4.06 GB (alias region 4.38 GB)` and the game reaches `nnMain`.

## Services

- **Resolved: abort in `nn::bluetooth::InitializeBluetoothUserInterface` early in `nnMain`**, after `[RED] sf: the game connects to service bt itself`: the game initializes Bluetooth LE for its Pokémon GO link (`main+0xa0aa7c`: `InitializeBle` at `+0xa0b02c`, then the five `AcquireBle…Event` calls, no result checked), and with the initialization answered, `btm::user::AcquireBleScanEvent` aborted next. The host answers them without Bluetooth: [Bluetooth LE events bound without a Bluetooth service](../../../docs/reference/services.md#bluetooth-le-events-bound-without-a-bluetooth-service). The other 30 `nn::bluetooth` imports (scan, connect, GATT) stay unhooked; they are reached from the GO link's menu only.
- The game's own HIPC client is not used for anything else: no other `sf:` line.

## Rendering

- 30 fps (the game's own cap) from the first frame through the opening and in the house: frame time p50 33.3 ms, CPU energy 7 to 9 mJ per frame, 47 to 50 % of one core. 150 `[longframe]` lines in a 15-minute run, at scene changes.
- **Resolved: `[RED] NVN unimplemented function called: nvnCommandBufferSetConservativeRasterDilate`**: the game sets the dilation beside `SetConservativeRasterEnable on` (ignored by the host, Metal has no conservative rasterization); the setter is now a quiet stub, [NVN calls that were red stubs](../../../docs/reference/nvn-objects.md#nvn-calls-that-were-red-stubs).
- **Current: `[RED] image atomics on a block-linear texture (texture ID 302 target 1)`**, once per run during the opening: the atomics go to a scratch buffer ([Image atomics](../../../docs/reference/shaders.md#image-atomics)). Nothing wrong was seen in the picture so far; whether the result is read back later is open.
- The game's command memory runs short about twice a frame, and the host lends 256 KB segments ([Host-lent command memory segments](../../../docs/reference/nvn-objects.md#host-lent-command-memory-segments)): about 25,600 lends by frame 11,600, a segment pool of 289 segments (74 MB), still growing slowly.

## Mac routes

- The Mac app: `scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> --platform mac-app`; its host log is `~/Library/Logs/nxnative/01008F6008C5E000.log`. The first launch after a build shows nothing for about 70 s before its first frame (seen with the pipeline archive already in place, so not its writing; the cause is not traced); later launches reach it in about 2 s.
- A fresh save into the player's room, driven through `DM_CTL` with `DM_KBD=<name>` answering the name keyboard: the language select at about 30 s (`A`), the look picker at about 50 s (it takes the left stick: `LLeft~0.3`, then `A`), the enrollment application (`A`), then `A` every 3 to 4 s through the opening (about 4 minutes, past the autosave) until "Get Up" (`LUp~1.5`). From there the stick walks the player around the room and `X` opens the main menu. A later launch loads that save from the title screen (`A`, about 25 s into the room). The stairs are entered from the green mat at the top-left of the stairwell: from the loaded save's position `LLeft~3`, `LUp~5`, `LLeft~4` reaches their top and `LDown~5` the house's ground floor.

## Found on this game

General host changes first needed here; each applies to every game:

- **Bluetooth LE answered without a Bluetooth service** (`hle_bluetooth.c`): [Bluetooth LE events bound without a Bluetooth service](../../../docs/reference/services.md#bluetooth-le-events-bound-without-a-bluetooth-service).
- **`nvnCommandBufferSetConservativeRasterDilate` as a quiet stub** (`hle_nvn.c`): [NVN calls that were red stubs](../../../docs/reference/nvn-objects.md#nvn-calls-that-were-red-stubs).

Per-build cuts on this game's SDK 13.3.13 build (`sdk_patches.py`): [The SDK's address-region node area](../../../docs/reference/memory.md#the-sdks-address-region-node-area), [1 GB alignment of regions of 1 GB and more](../../../docs/reference/memory.md#1-gb-alignment-of-regions-of-1-gb-and-more).

## Open

- Play beyond the house is untested: Cabo Poco, the open world, battles, the menus past the main menu.
- The iPhone: the IPA is built (the 35,937 shaders take about 27 minutes per platform on the Mac; the RomFS is 9.24 GB) but not yet played there.
- The 70 s first launch after a build (above).
- The Pokémon GO link's other `nn::bluetooth` calls are unhooked; opening that menu would abort in the SDK's client.
- The block-linear image atomics and the command memory lends above.
