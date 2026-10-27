# Mario Strikers: Battle League (`010019401051C000`)

Update 1.3.2 (v393216) over the base game, SDK 13.3.0 (NVN). Next Level Games' engine, the successor of Luigi's Mansion 3's, with Havok, Wwise and NEX / Pia. Modules `main`, `subsdk0` (the movie module, which plays the intro and the menus' VP9 movies) and `sdk`. On the Mac (Apple M5): the intro, the menus (30 fps, about 2.0 GB resident) and exhibition matches (Quick Battle) with a DualSense as player 1, at about 60 fps in a 2560×1440 window (frame time p50 16.2 to 17.8 ms over 300-frame windows, about 600 draws per frame, 2.5 to 2.8 GB resident); in one 20-minute run two matches were played to the end (the game saved after each). Another run went through 33 menu changes after the movie cut below. Not tested on the iPhone.

## What it needs

| Setting | Where | Why |
|---|---|---|
| Index revision 4 reader (`nlg4_index`, `nlg4_shaders`) | `dm/gfx/tools/shader_split.py` (every game) | the shader split stopped with an `IndexError` in `nlg_shaders`; see [Files and shaders](#files-and-shaders) |
| Allocator address space 10 GB → 3 GB | [`patches.py`](patches.py), `main` build `2e90f081…` (`main+0x620684`, `+0x620688`) | `StandardAllocator::Initialize` aborted in `nninitStartup`; see [Memory](#memory) |
| Node area cut 1 GB → 62 MB, 1 GB region alignment → 32 MB | `dm/tools/sdk_patches.py`, `sdk` build `7f771063…` (`sdk+0x37bcf0`, `+0x37bd8c`, `+0x37beec`) | the same abort; see [Memory](#memory) |
| Movie event thread: no read of a torn-down player | `patches.py`, `main+0xab4df8` | the game closed on the way into some menus; see [Movies](#movies) |
| `sys_resource 0x2000000` | [`game.tsv`](game.tsv) | the NPDM's 32 MB of system resource: virtual address memory |
| 5 GB guest layout: `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | `game.tsv` | Engage's 5 GB block with the hardware pool |
| `depth_mode zero` | `game.tsv` | the game sets the near = 0 depth mode (log: `depth mode: near=0, vertex shaders use the main0_z entry`), for the pipeline bases made before it runs |

## Memory

- The NPDM declares 32 MB of system resource, so `IsVirtualAddressMemoryEnabled` is true. `nninitStartup` (`main+0x620420`) initializes its StandardAllocator over 10 GB of address space (`StandardAllocator::Initialize(nullptr, 0x280000000)` at `main+0x620690`; the size is `mov x1, #0x80000000` / `movk x1, #2, lsl #32` at `+0x620684` / `+0x620688`).
- **Resolved: `StandardAllocator::Initialize` aborted** (`nn::diag` abort at `sdk+0x32472c`, `Break#1 reason=0` in `nninitStartup`) on the first provisional boot: 10 GB does not fit the 4.375 GB alias region. As for Pokémon Legends: Z-A and Paper Mario: The Thousand-Year Door, a cut on `main` gives it 3 GB (a workaround in the game's own code; the pool, 3.2 GB, bounds what it maps), with the node area cut and the 1 GB alignment cut on this `sdk` build (the AddressRegionManager constructor at `sdk+0x37bce0`, `VammManager::AllocateAddressRegion`'s placement at `sdk+0x37be80`, as Sea of Stars's SDK 16.2.0 build). The SDK-wide rules in `sdk_patches.py` (`MATCH_CUTS`) give this build the same three cuts; they are registered because the profile names the build. At boot the alias region is mapped up to +3.06 GB of its 4.38 GB.

## Files and shaders

- Every RomFS file but the Wwise banks (`wwiseaudio/*.pck`) and the two WebM movies is an archive of the engine's own, as in [Luigi's Mansion 3](../0100DCA0064A6000/README.md), but its index is revision 4: 879 `.dict` files, each indexing `<name>.data` (zstd chunks), and 49 `.patch` files from update 1.3.2 (revision 5). The layouts are in [01, Compressed RomFS content](../../../docs/agents/01-extract.md#compressed-romfs-content).
- **Resolved: the shader split stopped** with `IndexError: list index out of range` in `nlg_shaders` (`chunks[ci][3]`): it read the revision 4 index as revision 1's. `nlg4_index` and `nlg4_shaders` read revision 4 and its revision 5 patch ([Next Level Games archives, index revision 4](../../../docs/reference/shaders.md#next-level-games-archives-index-revision-4)).
- 3,230 shaders: 3,149 in `init` (2,648 in the base game's `init.dict`; the patch's entry table lists 3,149, the new ones in its data), 41 in the UI layouts' BNSH files (entries of type 65: 6 in `global`, 18 in `FrontEnd/Persistent`, 2 in `ClubStadium/Tutorial_TutorialCommon`, the same 41 in each) and 40 in `main` (BNSH): 1,971 pixel, 1,247 vertex, 9 geometry and 3 compute, every one built for the Mac. In `init` a shader is a type 1 entry (its code area, section 1) followed by a type 2 entry (its 0x880-byte control section, section 2); the next two entries are the same in sections 3 and 4, which point into `.nxpc`, which is not shipped. The other 875 archives hold no shaders; all 879 are read in about 5 s.
- **Resolved: menu elements not drawn.** Before the BNSH files were read, the way into the menus logged `[RED] shader … is not in the shader map` for 8 keys (`6862a25cc3c154e5`, `7e325e9be8a957ba`, `cc962df75a8d174a`, `77921ade312f1915`, `663724cf7e49669d`, `11b7fd357ef8f760`, `7840d623fcab928a`, `454623171bdcb2ac`) and `program has no vertex shader (or it is not mapped)`, and those draws were skipped. All 8 are in `global`'s BNSH files (variations 3, 15, 27, 42 and 45).

## Movies

- The intro and the menus' backgrounds are VP9 movies (1600×900 in the menus) through `subsdk0`'s `movie::Decoder` (the host's, `hle_movie.c`). A menu loops its movie (end of stream, `Flush`, `Start`) and tears its player down when the menu changes (`Stop`, the decoder destroyed): one run made 33 of them.
- **Resolved: the game closed on the way into some menus** (`CRASH sig=11 addr=0 pc=main+0xab4e18 lr=main+0xab4df8`, the host exited with 139; right after `[movie] Stop` and `decoder … destroyed`). The movies' event thread (`main+0xab4d84`) waits on the video decoder's four events and the player's (one MultiWait in a manager that `main+0x14e56c0` points to). After a video "output ready" event (its holder at manager `+0x3b0`) that is not for the current player, it reads the current player (manager `+0x20`) without checking it; the code for every other event checks first or does not read the player. Leaving a menu, the main thread (`main+0xab55a4`) parks the event thread, zeroes every holder's owner, frees the player and then nulls the pointer; an output-ready event the event thread took after that read null. `patches.py` sends such an event back to the wait (`main+0xab4df8`: `cbz w23, #0xab4e10` → `cbz w23, #0xab4dac`), as the other events do; on the console the timing presumably hides the window. After the cut, the run with 33 menu changes ended without a crash.

## Found on this game

General tool changes first needed here; each applies to every game:

- **Next Level Games' archives, index revision 4, their revision 5 patches and the BNSH files in them** (`nlg4_index`, `nlg4_shaders`): [Next Level Games archives, index revision 4](../../../docs/reference/shaders.md#next-level-games-archives-index-revision-4), [01, Compressed RomFS content](../../../docs/agents/01-extract.md#compressed-romfs-content).
- **The node area and 1 GB alignment cuts on another `sdk` build**: [The SDK's address-region node area](../../../docs/reference/memory.md#the-sdks-address-region-node-area), [1 GB alignment of regions of 1 GB and more](../../../docs/reference/memory.md#1-gb-alignment-of-regions-of-1-gb-and-more).
- **A StandardAllocator over 10 GB of address space**: [StandardAllocators over 12 to 62 GB of address space](../../../docs/reference/memory.md#standardallocators-over-12-to-62-gb-of-address-space).

## Open

- Online modes (Strikers Club, online matches): the host has no online services. A help page the game opens (`web::ShowOfflineHtmlPage (cut)`) shows nothing.
- `[RED] texture type mismatch: shader a3887f1ecfc6557c_b (stage 1) declares slot 2 as Metal type 7, texture ID 8632 is type 2`, once in a match; nothing wrong was seen on screen.
- Rumble: `[RED] haptics: controller left handle create player: Couldn't communicate with a helper application` in matches.
- Two or more players with physical controllers, the other modes (Cup Battles, Training) and the iPhone are not tested.
- The 9 geometry shaders: matches draw with one through the host's geometry compute (`geometry compute: vertex 783f36c63e0e90a5_b, geometry 17b2e32668a6d6f1_bg3`); how they look is not checked.
- The prewarm list (`pso_manifest.tsv`) is empty: not recorded yet.
