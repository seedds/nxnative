# Castlevania: Belmont's Curse (`0100BA4026E94000`)

Update 1.1.0 (v65536) over the base game, SDK 19.3.16. Unity 2022.3.62f2 IL2CPP with FMOD (one AudioOut) on NVN. Modules `main`, `subsdk0` (the SDK's movie library, for the opening movie) and `sdk`. Plays on the Mac: the first-run screens, the title, a new game, the opening movie, the dock dialogue, then Paris Streets and the sewer behind it: walking, jumping, attacks, an enemy killed, damage, healing, a death and respawn, and a save loaded after a restart. No game-specific rewriter cut and no host change of its own. iPhone and iPad validation remains open.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `sys_resource 0x1000000` | [`game.tsv`](game.tsv) | the NPDM's 16 MB of system resource: virtual address memory |
| 5 GB guest layout: `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000`, `app_pool 0x40000000` | `game.tsv` | [Cult of the Lamb](../01002E7016C46000/README.md#memory)'s layout, the same Unity generation; see [Memory](#memory) |
| `depth_mode zero` | `game.tsv` | the game calls `nvnDeviceSetDepthMode` with near = 0 (log: `depth mode near=0`), so the pipeline bases made before it runs take the `main0_z` vertex entries |
| `shader_groups /Data/=data` | `game.tsv` | everything with shaders is under `Data/`: the player's serialized files and the `StreamingAssets` bundles |
| No `pad_layout` (the 2D default) | `game.tsv` | B jumps, Y attacks, A dashes in play and confirms in menus, a held L heals |

## Frame rate

- 60 fps, as the game asks: the window starts with present interval 2 and the game sets 1 (`main+0x1f6be14`) before the title.
- With a window, on the title and in play: p99 frame interval 16.83 to 16.94 ms, with single stretches higher around a scene load (p99 up to 189 ms in the window that held a load).
- Windowless, the fixed 60 Hz timer shows p99 20 to 31 ms over the same scenes, so compare frame times only between runs of the same kind.
- A window that loses focus logs `oe focus state → 2`, and frames then pace at 30 fps and `DM_CTL` screenshots stop, as on other games.

## Memory

- `nninitStartup` hands application pool − used memory to `StandardAllocator::Initialize(nullptr, size)` as one address region (0.93 GB at alias +0.06 GB, returning to `main+0xca8c0c`) and never calls `SetMemoryHeapSize`.
- The hardware pool would not leave Unity room: the heap check reached alias +4.25 GB of 4.38 GB within two seconds, before the game's own regions. It was not played with that pool; the NSP script's heap check chose 1 GB and every run below used it.
- The node area takes the SDK-wide function-matched cut (`sdk+0x1423fc` / `+0x142468`, [implementation 5](../../../dm/tools/sdk_patches.py)). The SDK-wide alignment rule reports no match, correctly: this `sdk` aligns regions to 64 KB or 2 MB only (`sdk+0x142544`), as [Cult of the Lamb](../01002E7016C46000/README.md#memory)'s does.
- **Current:** alias region high-water mark +3.50 to +3.51 GB of 4.38 GB with at most 1.76 GB mapped, the same at the end of every run that reached play. Resident about 2.5 GB (peak 2.62 GB) and at most 0.78 GB of GPU memory over a 15-minute windowless run through the first areas, a death and a respawn; 2.95 GB resident at most in a windowed run from a fresh save. Later areas are not measured: with about 0.87 GB of the alias region left, they are the first place to look if a later area fails.

## Files and shaders

- The NSP's file names carry `[ERROR]` on the base game and both DLC, but `nx_extract` lists no damaged file and the build does not stop. The merged RomFS has 3,101 files, 3.35 GB: 3,051 UnityFS bundles and 13 FMOD banks. There are no loose movie files: the MP4s are inside the `narration-videos-1080p` (769 MB) and `narration-videos-720p` (375 MB) bundles, and docked the movie module decodes 1920x1080.
- Both DLC NSPs are licences with no content; the host reports indices 1 and 2 as owned.
- `Plugins/cimgui.nro` is registered by `.nrr/dynamic.nrr`, so it is collected and rewritten, but no run loaded it.
- 19,162 shaders (9,519 vertex, 9,523 pixel, 119 compute, one geometry): 19,151 from `Data/` and the player's 11 GLSLC outputs in `main`. All built for the Mac with no failure.
- The prebuild makes 9,507 pipeline bases from 9,522 shader pairs on the Mac: none fail, 15 are left to the draws. A run on them compiled no base during play (`0 compiled, 165 from the archive`).
- `pso_manifest.tsv` has 524 lines, recorded along the route below and through a save load.

## Mac routes

- Build with `scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> --platform mac` and use its printed command.
- From a fresh save, the first-run language screen is up by about 40 s. Its **Continue takes a long hold of A**: `A~3` was always enough, `A~1.5` only sometimes, and short presses never. Then A on the confirm button screen, a held A on the brightness screen, A on Start, A on New Game.
- The opening movie takes about 3.5 minutes, and neither a held A nor Plus skipped it. About 30 presses of A go through the dock dialogue that follows. **Stop pressing A when the HUD appears:** A dashes, and Rose starts at the left edge of the pier facing left, so each press drops her in the water, which costs about 6% of her health and fades to black with only the HUD showing before she respawns on the pier. That is the game, not a rendering fault: frames right after a press show the dash, the fall and the red respawn burst, and A on solid ground costs nothing.
- L heals only when held (`L~2.5` worked, a 0.3 s tap did nothing).
- In control of Rose on the dock: hold right for about 14 s to reach the door into the sewer, then about 10 s more (a tarot dialogue plays the first time) to a ledge, where B jumps up. Y attacks. The first zombie is just past a heavy door on that ledge.
- A later start skips the first-run screens: A on Start, A on Resume returns to the dock. A run stopped during the opening resumed with the opening movie again.

## Found on this game

- Existing [Function-matched cuts](../../../docs/reference/rewriter.md#function-matched-cuts-for-unregistered-builds) work on this `sdk` build without a row of its own.

## Open

- iPhone and iPad runtime validation; no IPA has been built.
- Sound is checked only in the log (one AudioOut, no underruns): nobody has listened to it. Input has gone only through `DM_CTL`, not a controller or the keyboard.
- Everything past the sewer's first room, including bosses and the area changes after it, and memory there.
- The base game alone was not played or registered; packing it takes a derived profile.
