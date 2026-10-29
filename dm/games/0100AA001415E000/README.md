# FINAL FANTASY VI (`0100AA001415E000`)

Update 1.2.1 (v458752) over the base game, SDK 14.3.0. Unity 2020.3.37f1 IL2CPP on NVN. Modules `main`, `subsdk0` (the movie module) and `sdk`. Plays on the iPhone: the opening and title on an iPhone 15 Pro (iOS 27.0.1, 90 s with no crash) and play on an iPhone of the iPhone19,4 class (iOS 27.2). On the Mac (M2 Pro), two minutes from boot through the opening at 60 fps with no crash, the alias region used up to +3.25 GB with 1.00 GB mapped; that run had no Mac shader libraries, so some draws were missing.

## What it needs

| Setting | Where | Why |
|---|---|---|
| Node area cut 1 GB → 62 MB | `dm/tools/sdk_patches.py`, `sdk` build `2aef2cff…` (Batman: Arkham Knight's, [0100ACD0163D0000](../0100ACD0163D0000/README.md); the constructor at `sdk+0x12d8a0`, the movs at `+0x12d8b0` and `+0x12d94c`) | `StandardAllocator::Initialize` aborted in `nninitStartup`; see [Memory](#memory) |
| `sys_resource 0x1000000` | [`game.tsv`](game.tsv) | the NPDM's 16 MB of system resource: virtual address memory |
| 5 GB guest layout: `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000`, `app_pool 0x60000000` | `game.tsv` | a 1.5 GB pool in place of the provisional profile's hardware pool (0xCD500000), which used up the alias region; see [Memory](#memory) |
| `shader_groups /Data/=data` | `game.tsv` | the provisional profile's split, unchanged |

## Memory

- The NPDM declares 16 MB of system resource, so `IsVirtualAddressMemoryEnabled` is true: `nninitStartup` (`main+0x345cd0`) hands application pool − used memory to StandardAllocator as virtual address memory (`Initialize(nullptr, pool − used)` at `main+0x345d3c`) and never calls `SetMemoryHeapSize`, as in Patrick's Parabox and Ace Attorney Investigations Collection.
- **Resolved: `StandardAllocator::Initialize` aborted** at `sdk+0x11f308`, called from `nninitStartup` at `main+0x345d40` (`Break#1 reason=0` at `sdk+0x158b40`; the NSP script's heap check reported "the host stopped before nnMain"). The `sdk` build is Batman: Arkham Knight's, and its node area cut, registered for that game, applies here too.
- **Resolved: a null write 41 s into the opening on the iPhone** (`CRASH sig=11 addr=0xffffe0 pc=main+0x839d5c`, the pool size minus 0x20 with a 16 MB pool in x22). With the hardware pool StandardAllocator reserved 3.2 GB of address space as one region; on the Mac the alias region was used up to +4.25 GB of 4.375 GB within seconds, and Unity's next 16 MB region came back empty, as in [Size-aligned regions exhaust the alias address space](../../../docs/reference/memory.md#size-aligned-regions-exhaust-the-alias-address-space). With a 1.5 GB pool StandardAllocator's region is 1.44 GB at alias +0.75 GB, and the game maps 1.00 to 1.26 GB through the opening.
- Not measured: whether a 1 GB pool (the heap check's current starting point for Unity 2020) leaves its malloc enough, as it did not for Advance Wars 1+2: Re-Boot Camp. 1.5 GB is what was played.

## Open

- Played only through the opening and early play; long sessions, battles, menus and saves were not checked on the Mac.
- The Mac runs here had no Mac shader libraries; the shaders were not checked on the Mac.
