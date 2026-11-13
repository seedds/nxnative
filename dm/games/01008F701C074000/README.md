# Sonic Superstars (`01008F701C074000`)

Update 1.1.8 (v327680) over the base game, SDK 16.2.0, NVN. Modules `main`, `subsdk0` (NintendoSDK_movie, unused in play: the USM movies go through CRI Mana's own decoder) and `sdk`, the build Sea of Stars and Super Mario Bros. Wonder 1.0.0 / 1.0.1 ship ([Sea of Stars](../01008C0016544000/README.md)). SEGA and Arzest's 2023 game on Unity 2021.3.25f1 IL2CPP, with CRIWARE (ADX2 sound, USM movies) and FMOD. The standard host and rewriter run this build with no game-specific cut or host change. The profile is the NSP script's provisional one with its names, its category and `stdalloc_cap` ([What it needs](#what-it-needs)). Only the update is registered.

On the Mac (2026-11-07, M4 Pro): the logos and the intro movie, the title, save creation, the DLC notices (all nine add-ons are found), Story Mode's character choice and its opening movie, then Bridge Island Act 1 with Sonic: running, jumping, rings, enemies and the stage's scene changes, with sound. It runs at the game's 60 fps (frame time p50 16.7 ms) with no `[RED]` line, no missing shader and about 2.3 GB resident (0.5 GB of Metal allocations). The save loads again in a later run. The IPA is not built.

## What it needs

| Setting | Where | Why |
|---|---|---|
| 5 GB layout: `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | [`game.tsv`](game.tsv) | the provisional layout for a game with virtual address memory |
| `sys_resource 0x1000000` | `game.tsv` | the NPDM's 16 MB of system resource: virtual address memory |
| `stdalloc_cap 0x60000000` (1.5 GB) | `game.tsv` | StandardAllocator's 3.12 GB left Unity's own regions too little of the iPhone's alias region; see [Memory](#memory) |
| `depth_mode zero` | `game.tsv` | the game sets near = 0 depth (the heap check saw it) |

No code of the game is cut. The game asks for present interval 1, so it needs no frame-rate key. `pad_layout` keeps the default 2D layout: the game is played on a 2D plane.

## Memory

- **Resolved: a write at 0xffffe0 (`main+0x18a2a30`) in the load of Bridge Island Act 1 with the iPhone's alias region** (`DM_ALIAS_SIZE=0x118000000` on the Mac), x19 Unity's `ALLOC_DEFAULT_MAIN` allocator: a 16 MB block − 0x20 written past a null region, the high-water mark at +4.25 GB of 4.375 GB. The same mechanism as Sea of Stars' ([Size-class alignment doubles Unity's regions](../../../docs/reference/memory.md#size-class-alignment-doubles-unitys-regions), whose SDK-wide alignment rule this `sdk` already gets): `nninitStartup` calls `StandardAllocator::Initialize(nullptr, 3.12 GB)` (returning to `main+0x1cb71d0`, application pool − used), which takes alias +0.06 to +3.19 GB and maps only 0.38 GB of it by the first stage, while Unity's own 201 regions (`main+0x4169c`) hold 1.34 GB live and, on the Mac's 64 GB alias region, reach +5.41 GB. The NSP script's region check stops 15 s into `nnMain`, at +4.07 GB, so it did not set `stdalloc_cap`.
- **Fix:** `stdalloc_cap 0x60000000` gives StandardAllocator 1.5 GB through the [host's allocator gate](../../../docs/reference/memory.md#standardallocators-over-12-to-62-gb-of-address-space), four times what it maps. With the iPhone's alias region the same route then plays Bridge Island Act 1 for over a minute: 202 live regions, 2.84 GB of address space with 1.67 GB mapped, ending at +3.78 GB. On the Mac the cap applies too (the request is larger than the cap), so the Mac runs the iPhone's heap layout.
- **Current:** about 2.3 GB resident and 0.5 GB of Metal allocations on the Mac in Bridge Island.

## Files and shaders

- RomFS: 278 files, 4.25 GB, under `Data/`: 177 UnityFS bundles (3.40 GB), 50 CRIWARE `.acb` / `.awb` / `.acf` files and 12 CRI USM movies (0.21 GB). Nine DLC (character skins and parts) are packed from their NSPs and reported by `aoc`.
- 44,240 shaders: 22,088 vertex, 22,092 pixel, 59 compute and one geometry shader. 44,229 come from `Data/`, 11 are the player's GLSLC outputs in `main`. All built for the Mac with no failure, and no run met a shader missing from the map.
- The game draws 1280x720 into 1920x1080 window textures and crops the window to it (`nvnWindowSetCrop`), so a `DM_CTL` `shot` (the whole texture) has the picture in its top-left corner; the window shows the crop.
- **Open:** the prewarm list (`pso_manifest.tsv`) is empty.

## Frame rate

- 60 fps from the logos into play (`power` lines 60.0 fps; CPU about 0.2–0.4 W headless).
- **Open:** after about 1,500 frames, every headless run (the menus too, with or without the cap) has its frame times alternate around 16.7 ms: p50 16.3–16.7 ms, p95 22–25 ms, while the rate stays 60.0 fps. Not looked into.

## On the Mac

- After the NSP script's `--platform mac` build, its printed command runs the game; with `DM_FOCUS_KEEP=1 DM_WINDOW_TOP=1` a host started from a background shell keeps presenting.
- From a fresh save: A then Plus at about 20 s skip the intro movie to the title; A (Start Game), A (Data 1), A through the four DLC and tip notices, A (Story Mode), A A (Sonic, Confirm). Story Mode's opening movie skips with Plus held for 1.5 s (a press does not skip it); the stage follows its Tornado intro.
- Movement is the left stick (`LRight` / `LLeft` in `DM_KEYS` / `DM_CTL`), jump A.

## Found on this game

- The alias region check's 15 s did not reach the regions Unity takes in play; `stdalloc_cap` set by hand ([Memory](#memory); [StandardAllocators over 12 to 62 GB of address space](../../../docs/reference/memory.md#standardallocators-over-12-to-62-gb-of-address-space)).

## Open

- iPhone: no IPA built yet; the iPhone layout is checked only on the Mac with `DM_ALIAS_SIZE`.
- The frame-time alternation ([Frame rate](#frame-rate)).
- The empty prewarm list ([Files and shaders](#files-and-shaders)).
- Only Bridge Island Act 1 was played: later zones, bosses, the Emerald Powers, multiplayer, Battle Mode and the other languages are untested.
