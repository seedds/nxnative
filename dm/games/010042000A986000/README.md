# DRAGON QUEST BUILDERS 2 (`010042000A986000`)

Update 1.7.3d (v393216) over the base game, SDK 5.4.0. Square Enix / Koei Tecmo's Omega Force, on Koei Tecmo's KTGL engine with NVN. Modules `main`, `subsdk0` (GLSLC, NVN's runtime shader compiler), `subsdk1` (the movie library, NintendoSDK_movie) and `sdk`.

Status: played on the Mac from a fresh save: the logos, the title, character creation with the game's own name entry, the settings screen, the DLC messages (the host reports 16 add-on contents as owned), the opening scene, then in control through the prison ship's hold, up the stairs and to the first conversation on deck. No red lines; 60 fps with sound. Not reached: an island, breaking or placing blocks, a battle, a real save. Not run on an iPhone.

## What it needs

| Setting | Where | Why |
|---|---|---|
| No game-specific code | | no rewriter cut, no build gate; the two fixes it needed are general ([Found on this game](#found-on-this-game)) |
| `pad_layout full` | [`game.tsv`](game.tsv) | the camera is on the right stick |
| 4 GB layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | `game.tsv` | the NPDM declares no system resource, so the game uses `SetHeapSize`. The guessed partition held: heap 3.02 GB, footprint 2.5 GB on the ship |
| GLSLC capture (`/glslc_capture/=runtime` in `shader_groups`) | `game.tsv`, NSP script | 6 built-in shaders exist only as compiles of the game's own GLSLC at boot ([Built-in shaders compiled with the game's own GLSLC](../../../docs/reference/shaders.md#built-in-shaders-compiled-with-the-games-own-glslc)) |

## Frame rate

- **Current:** the game opens its window with interval 1 and presents 60 frames a second in the 3D scenes (frame 31,800 at about 9 minutes; p99 frame interval about 18 ms, about 600 draws a frame on the ship). The host adds nothing: `present_interval` is the default `game`. Not compared with a Switch.
- **Open:** a worker thread of the game polls with `YieldThread`, about 1.5 million yields per 300 frames (`main+0x8985cc`, `main+0x9aa4f4` on its stack). The host's yield back-off holds it at about 14,000 wakeups a second, CPU 50% of one core, 0.8 W of CPU energy on the Mac. The cost on an iPhone is not measured.

## Files and shaders

- RomFS: `LINKDATA.BIN` (1.1 GB, 59,193 entries, chunked zlib) with its index `LINKDATA.IDX`, sound banks in `binary/sound/*.nx`, two MP4 movies in `movie/`.
- 2,107 shaders: 2,068 in LINKDATA entries, 33 GLSLC outputs in `main`, 6 captured at boot. The full Mac build takes 1 minute and 574 MB.

## Mac routes

From an empty save, in about 6 minutes (`DM_CTL` lines; a plain `A` works, `A~0.2` is safer):

1. The title's Press A comes at about 17 s; A, A for Start New Game.
2. A through the text to the gender choice and the customise screen. There, A confirms the colour and returns to the tab row, and Plus finishes. Name entry: A types the selected letter, Plus finishes. A, Plus, A, A through the settings screen, then Yes.
3. A through the "You receive" messages and the opening scene, about 60 s of presses.
4. In the hold the cell door is straight ahead, slightly right of the first spot: A opens it (`Open` appears close by). Walk through and up the stairs with `LUp` to the deck.

The hold and the deck have no save point yet: `run/save_deck` in the work folder holds only `SYDAT.BIN`, the system data, so each run starts over.

## Found on this game

General host changes first needed here; each applies to every game:

- [Creating an ssl context needs no network](../../../docs/reference/services.md#creating-an-ssl-context-needs-no-network) (`hle_net.c`): startup failed without it.
- `LINKDATA.IDX` beside `LINKDATA.BIN`: [SPKG packages in KTGL LINKDATA](../../../docs/reference/shaders.md#spkg-packages-in-ktgl-linkdata) (`shader_split.py`).

## Open

- Past the deck: the island, building, battles, a real save and load, and the two movies (`movie/movie0.mp4`, `movie1.mp4`), which 45 s at the title did not start.
- iPhone: not built or run. Its 3.02 GB heap and 2.5 GB footprint may need a look on a phone with less memory.
- The polling workers in [Frame rate](#frame-rate).
- Base game v0 has not been run; only 1.7.3d is registered.
