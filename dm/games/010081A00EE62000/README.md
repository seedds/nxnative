# Boomerang Fu (`010081A00EE62000`)

Update 1.3.7 (v1441792) over the base game, with the two licence-only DLC (Fresh Flavors Pack, Just Desserts), SDK 18.3.5. Unity 2022.3.54f1 IL2CPP with FMOD on NVN. Modules `main`, `subsdk0` (the movie module, which `main` imports from; the game plays no movie) and `sdk`; the `sdk` and the movie module are Monster Train 2's builds ([010051701FB46000](../010051701FB46000/README.md)). It needed no host change: the provisional profile was right as generated.

On the Mac: the title ("Press any button"), the main menu with both DLC listed, character select with a bot, the game mode and ready screens, the controls card and Free for All matches against the bot over several arenas with the round scoreboards between them, at 60 fps (the game asks for present interval 1), no red line; about 1.6 GB resident and 1.2 GB of Metal allocations, flat over 10 minutes. On an iPhone (iPhone19,4, A20 Pro, iOS 27.3 beta): played by hand, with the game files as a 619 MB `Documents/romfs.img` and both DLC in `Documents/aoc`; the logged session ran at 60 fps (p99 16.67 ms, no dropped frames after the first seconds) with a 1.67 GB footprint, 1.25 GB of Metal allocations, thermal state 0 and no red line.

## What it needs

| Setting | Where | Why |
|---|---|---|
| Node area cut 1 GB → 62 MB | `dm/tools/sdk_patches.py`, `sdk` build `60751516…` (Super Mario Galaxy's and Monster Train 2's) | the build already had it |
| `sys_resource 0x1000000` | [`game.tsv`](game.tsv) | the NPDM's 16 MB of system resource: virtual address memory |
| 5 GB guest layout: `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000`, `app_pool 0x40000000` | `game.tsv` | Monster Train 2's layout; the NSP script's heap check set the 1 GB pool; see [Memory](#memory) |
| `shader_groups /Data/=data` | `game.tsv` | everything is under `Data/` |
| No `pad_layout` (the 2D default) | `game.tsv` | left stick, A, B, X; see [Input](#input) |

## Memory

- As Monster Train 2: 16 MB of system resource, `nninitStartup` hands pool − used memory to StandardAllocator as virtual address memory and sets no heap. The heap check found the hardware pool's region taking the alias region to +4.26 GB in `nninitStartup` and set a 1 GB pool.
- Measured on the Mac over 10 minutes of matches: about 1.0 GB mapped, 1.6 GB resident, 1.2 GB of GPU memory, flat.
- **Current: the alias region's high-water mark reaches +4.27 GB of 4.375 GB within 4 minutes and stays there.** Not a leak: the SDK's AddressRegionManager puts a new region in the highest free gap first (`sdk+0x13bfd0`), and Unity's 16.25 MB pools come and go with every round; with probes on the allocation's success path (`sdk+0x13bf8c`) and `FreeAddressRegion` (`sdk+0x13c7e0`) 2.06 GB were live, StandardAllocator's 956 MB at +64 MB among them, with gaps of up to 742 MB below the top. Once the top was full the game played on for 6 more minutes with no failed allocation ([An alias high-water mark near the top is not exhaustion](../../../docs/reference/memory.md#an-alias-high-water-mark-near-the-top-is-not-exhaustion)).

## Frame rate

- 60 fps; the game asks for present interval 1. Under a heavy machine load (other builds, load average 46 on 12 cores) frames of 25–75 ms showed every few seconds in menus and matches, none with pipeline compiles; not measured on an idle machine.

## Input

- Controller only. Left stick moves, A slashes, B dashes, X throws the boomerang; in menus A joins and confirms, B goes back, left / right on the "Hold A to begin" bar adds bots, and A held starts. One player plays against bots (up to six players locally).
- It stays docked at 1920x1080 with the 2D pad layout, which has every control the game uses. Whether it reads touch has not been tried.

## Files and shaders

- 10,035 shaders in the player's serialized files under `Data/` and `main` (the player's own GLSLC outputs).
- The DLC are licence-only (no content): the host reports both as owned and the main menu lists them.

## Mac routes

- From a fresh save: `DM_KEYS="f2700:A,f2900:A,f3100:A,f3300:A,f3450:LRight~0.2,f3550:A~2,f3800:A,f4000:A,f4500:A,f4800:LUp~0.5*300/40,f4810:X*300/40,f4820:LRight~0.4*300/40,f4830:A*300/40,f4840:LDown~0.5*300/40,f4850:B*300/40,f4860:LLeft~0.4*300/40"` goes from the title through the menus (one player, one bot, Free for All) into the first match at about frame 4,600 and keeps playing rounds.

## Found on this game

No host or tool change was needed. The alias high-water mark under the SDK's region placement (above) is [An alias high-water mark near the top is not exhaustion](../../../docs/reference/memory.md#an-alias-high-water-mark-near-the-top-is-not-exhaustion), found on SUPER ROBOT WARS Y, which ships the same `sdk` build (`60751516…`, placement at `sdk+0x13bfd0`).

## Open

- The hitches above were seen only under heavy load; an idle-machine measurement is missing.
- The prewarm list (`pso_manifest.tsv`) is empty.
