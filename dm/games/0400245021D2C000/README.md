# Yoshi and the Mysterious Book (`0400245021D2C000`)

A Switch 2 game: update v131072 (Ver. 1.0.2 on its title screen), SDK 20.5.15 built for the Switch 2 (`Ounce`). Unreal Engine 5.4.4 (`SDK MW+EpicGames+UnrealEngine-5_4_4` in `main`; project `PJ045`, platform `Sage`) with its deferred renderer on NVN2. Modules `main`, `subsdk2` (NintendoSDK_movie) and `sdk`; `sdk` and the movie module are the builds Hyrule Warriors: Age of Imprisonment runs. **Plays on the Mac** at 60 fps from a new save: the opening movie, Mr. E's dialogue, Chapter 1's page, the first habitat, walking, jumping and the flower friend, ending an exploration, a save, and a reload of that save. No IPA has been built.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `mem_alias 0x118000000`, `mem_reserve 0x140000000` | [`game.tsv`](game.tsv) | Engage's 5 GB block. Unreal's engine region takes 4.06 GB of it (the rewriter's engine-wide gate) and the allocator is the host's (the engine-wide cut). See [Memory](#memory) |
| Node area cut 1 GB → 62 MB (`sdk+0x14df18` / `+0x14dfa4`) | [`dm/tools/sdk_patches.py`](../../../dm/tools/sdk_patches.py) SDK-wide rule, no build ID | without it the engine region did not fit and `nninitStartup` called `exit(1)` |
| `mem_stack 0x10000000`, `app_pool 0x240000000` | `game.tsv` | as Donkey Kong Bananza, the Switch 2's 9 GB |
| `sys_resource 0x1400000` | `game.tsv` | the NPDM declares 20 MB, so virtual address memory is on |
| `depth_mode zero` | `game.tsv` | the game sets near = 0, so pipeline bases made offline use `main0_z` |
| `pad_layout full` | `game.tsv` | a 3D field and both triggers in use |

`subsdk0` (NVIDIA's DLSS library, 41 MB) and `subsdk1` (CUDA) are not loaded: `main` takes nothing from them that the host does not supply. There is no `tex_dyn_slots`: of the 1,519 shaders of the playtests, only the 3 under [Shaders](#shaders) have a read whose handle the translator could not trace.

## Files

- One [NUP container](../../../docs/development.md#the-packing-script-in-detail) of 41 GB: the base game and update v131072, whose RomFS (20 GB, 147 files) is whole. No NACP; the name comes from the profile.
- RomFS: one IoStore container, `PJ045/Content/Paks/PJ045-Sage.utoc` / `.ucas` (19 GB, TOC version 6, Oodle, 84,412 chunks), `PJ045-Sage.pak` (102 MB, 4,723 files: configs and the global shader cache), and loose movies under `PJ045/Content/Movies` (`.mp4` for the opening, the title and the ending, `.webm` for the staff roll).

## Shaders

- 133,154 in [`shader_map.tsv`](shader_map.tsv) (45,614 vertex, 60,840 pixel, 21,654 compute, 5,041 mesh, 5 geometry), split in 76 s: [Unreal Engine 5.4 shaders for the Switch 2](../../../docs/reference/shaders.md#unreal-engine-54-shaders-for-the-switch-2).
- The runs here used a partial Mac library of the keys the playtests asked for, listed in [`shader_seen.tsv`](shader_seen.tsv) (1,519, of which 395 come from [PR 1261](https://github.com/seedds/nxnative/pull/1261)'s runs through part of the second level) ([Building selected Mac shaders](../../../docs/development.md#building-selected-mac-shaders)), which takes about a minute to build. A full build is not estimated yet; Pokopia's similar map came to about 6 hours on an M4 Pro. All of the listed shaders translate. With no stamp the host logs a red line at each start and every c1 takes its own 64 KiB.
- Open: 3 pixel shaders (`6f91c450371a880c`, `c132ee1a17f37080`, `cbd80cb96ad38a61`) lose 5 texture reads to `Failed to find handle source`: their results are 0. Two of them contain 0x364. The cause is not traced, and whether it shows in a picture is unknown.
- Open: SURED.MAX.U64 (33,550 uses in the map, Nanite's visibility buffer) is refused at translation, so those shaders are left out. The first chapter does not ask for them. `SageEngine.ini` sets `r.Nanite.*`, so another level might.

## Frame rate

- Current: 60 fps, the game's own: `rhi.syncinterval=1` in `DefaultDeviceProfiles.ini`, so no `fps_target`. In the first habitat on an M6 Mac with 16 GB, frame time p50 16.7 ms, p95 17.1 ms, CPU about 47% of one core and 1.5 W, with an occasional long frame of 65–79 ms. Long frames were not traced.

## Memory

- Current: `nninitStartup` reserves a 62 GB region, and the rewriter's engine-wide gate gives it 4.06 GB. Unreal's FMallocBinned3 is replaced by the host's allocator through the engine-wide cut (`main+0x11aa0`).
- The alias region is mapped up to +3.51 GB, +3.51 GB and +3.75 GB after one, two and three entries into the first habitat, of 4.38 GB; about 3 GB is mapped at a time. 5.6 GB resident, about 3 GB of Metal allocations.
- Open: later chapters are not measured, and the room left is about 0.6 GB. If they run out, a Mac can take Splatoon Raiders' 16 GB alias region; an iPhone cannot.

## Saves

- The save is one file, `Slot00`. The game creates it empty when New Game is chosen, and writes it (308 KB) when an exploration ends. So until then a reboot shows New Game again. The title then lists the slot, and Start Game continues at Chapter 1's page with the discovery recorded.

## Driving it on the Mac

Run headless, `DM_CTL` at a control file. The work folder has `boot.sh <secs> <log>`, `ctl.sh` (control lines, `snap NAME`, `wait S`), `to_level.sh`, `keys.sh <logs>` (adds the keys the logs ask for and builds them), `loop.sh` and `run/save_ref` (Chapter 1's page, after one exploration).

From an empty save: at 70 s `A` (title), 8 s later `A` (New Game), 10 s later `Plus` then `Plus~2` skips the movie, 15 s later 34 `A` presses 3 s apart through the dialogue. The magnifier is on the left stick: from the centre, `LLeft~0.4` and `LDown~0.25` put it on the flower, then `A`, about 9 more presses into the first habitat. `LRight` walks, `A` jumps. In a habitat: `Plus`, `LDown` four times, `A` (End Exploration), then `A` a few times to the page.

From `save_ref`: at 72 s `A`, 8 s later `A`, 14 s later the book is open at Chapter 1.

## Open

- Current, on chain 9bb5d8678204: the first habitat's picture and frame times are as on 3dd02e581c0c, the pause menu's blurred background included. The black background seen there earlier did not come back on this route.
- Reported by [PR 1261](https://github.com/seedds/nxnative/pull/1261), not run here: the first level and part of the second work; "Who's there?" has the game's picture behind it on a Switch 2, never black; on the second level the red lines `program has no vertex shader`, `dispatched program has no compute shader`, `image read as float` (format 13) and a geometry shader `502dcf1ce20751aa_b` with no vertex shader.
- The second level on the 5 GB block (see [Memory](#memory)), later chapters, the picture against a Switch 2, listening to the sound (audio opens and starts with no red line), the `.webm` staff roll, Nanite.
- An IPA: the 5 GB block suits iOS, but no iOS shader build or iPhone run has been done.

## Found on this game

- [Unreal Engine 5.4 shaders for the Switch 2](../../../docs/reference/shaders.md#unreal-engine-54-shaders-for-the-switch-2): `shaders: 0` from the split.
- [The SDK's address-region node area](../../../docs/reference/memory.md#the-sdks-address-region-node-area): `nninitStartup` exits. The rule for this `sdk` also matches those of Mario Kart World and Donkey Kong Bananza.
- [More than 16 vertex attributes](../../../docs/reference/rendering.md#more-than-16-vertex-attributes): every bind refused.
- [Images and buffer textures with their handle in a uniform buffer](../../../docs/reference/shaders.md#images-and-buffer-textures-with-their-handle-in-a-uniform-buffer): compute passes skipped.
- [A guarded load whose destination is its address](../../../docs/reference/shaders.md#a-guarded-load-whose-destination-is-its-address): the scene's instance data read at index 0.
- [Texture and sampler words in either order](../../../docs/reference/shaders.md#texture-and-sampler-words-in-either-order): the G-buffer bound as dummies, a black picture.
- [Buffer textures behind handles in uniform buffers, and rows of finalized descriptors](../../../docs/reference/rendering.md#buffer-textures-behind-handles-in-uniform-buffers-and-rows-of-finalized-descriptors): flat-coloured objects, and a new Metal texture per bind.
- [A deferred write under a false guard](../../../docs/reference/shaders.md#a-deferred-write-under-a-false-guard): the white-out from bloom.
- [Ampere forms with a handle in a register, and SURED's other operations](../../../docs/reference/shaders.md#ampere-forms-with-a-handle-in-a-register-and-sureds-other-operations).
- [TXD through a uniform register kept no handle](../../../docs/reference/shaders.md#txd-through-a-uniform-register-kept-no-handle): reads deleted in 14,079 shaders the playtests have not reached.
- [Layer entry points with the point size passed by reference](../../../docs/reference/shaders.md#layer-entry-points-with-the-point-size-passed-by-reference): 38 vertex shaders without them.
