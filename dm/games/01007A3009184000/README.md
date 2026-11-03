# Princess Peach: Showtime! (`01007A3009184000`)

The base game, 1.0.0 (v0), SDK 15.3.2. Good-Feel's game on Unreal Engine 4.27 (project `PJ037`, one `PJ037-Switch.pak`) with Nintendo's NVN RHI and Wwise. Modules `main`, `subsdk0` (NintendoSDK_movie) and `sdk`.

Status: plays on the Mac. From a new game, on chain `be66496d6205`, a headless run went through the opening movie, the title, the lobby, the ticket event, the opening cutscene and the whole first swordfighter play: ribbon fights, the forest's three spotlight plants, the first Sparkle Gem, the castle, the transformation and sword fights, then the lobby with 1 Sparkle Gem and a written save. On chain `16dec73a79e7` that save loads and the ninja play's first set renders. No crash, and the only red lines are the [size queries](#found-on-this-game). Not tested on the iPhone: no IPA has been built.

## What it needs

No rewriter cuts and no `patches.py`.

| Setting | Where | Why |
|---|---|---|
| `mem_reserve 0xF0000000`, `mem_alias 0xC8000000`, `mem_stack 0x10000000` | [`game.tsv`](game.tsv) | the layout of the other Unreal games with virtual address memory, in place of the provisional 64 GB alias region: the engine's region gets 2.81 GB, and the first play ran on it |
| `fps_target 60` | [`game.tsv`](game.tsv) | the game asks for present interval 2 (30 fps); see [Frame rate](#frame-rate) |
| `depth_mode zero` | [`game.tsv`](game.tsv) | the game calls `nvnDeviceSetDepthMode` with near = 0; the NSP script's address region check found it |

## Files and shaders

The pak is a version 11 pak with an encrypted index; `uepak.py` finds its key in `main` (103,924 files, Oodle, Zlib and uncompressed). Beside it, 8 `.mp4` movies in the RomFS. The shaders are in two `SF_GLSL_SWITCH` shader code archives in the pak, LZ4-compressed: 56,594 distinct shaders (10,068 vertex, 45,424 pixel, 1,021 compute, 81 geometry), all of which build for the Mac. A first Mac build compiles them in about 2 h 45 min on an M6, and the pipeline prebuild of 10,068 pairs takes 10 minutes more.

## Frame rate

At `fps_target 60` the lobby, the cutscenes and the first play hold 60 fps on an M6, with dips to about 57 when enemies appear (`[longframe]` lines of 40–140 ms with 5–9 ms of GPU time, so on the CPU; the cause is open) and one 300-frame window at 24 fps across the transformation, whose pipelines the prewarm list did not hold yet in that run. The game's speed follows real time: from the same save, a 3 s walk in the lobby ends in about the same place at 30 and at 60 fps, and the opening's dialogue advances with the same presses. `DM_FPS_TARGET=30` restores the game's own rate.

## Memory

Over 25 minutes on the small layout, the host's resident memory peaks at 6.1 GB and Metal's allocated size at 3.1 GB, during the transformation; 3.6 GB in the lobby, about 5 GB in the play. That is high for an iPhone, and is to be measured there.

## Mac routes

Headless with `DM_CTL` and screenshots ([05-debugging](../../../docs/agents/05-debugging.md)). `A` skips the opening movie; `A` passes the title. On a dialogue line that repeats while Peach stands by its speaker, `B` closes it.

New game: the lobby opens with Toad's ticket line, and the stairs and doors stay closed. Three conversations open the way, each started with `A` next to its speaker (the scripts `AE_00_NPC_00`, `_01` and `_02` in the pak name `ORIGIN_EVENT_TALK_00` to `_02`): the usher in the red vest at the door with the sword sign ("We'll be ushering you…"), the Theet at the cowgirl poster on the right, and the Theet at the foot of the stairs. Toad then waits at the south end of the central bowl with one arm up: `A` next to Toad gives the ticket and starts the opening cutscene, a long one with an `A` for each line once its text starts.

In the play: `B` spins the ribbon, stick + `B` throws it, `A` jumps. The forest's exit needs the three plants spun open, the two on the side platforms from the back row. The first gem is in the doorway they open.

With a save past the first play, the lobby is taken over and the ninja door, the next one right of the sword door, opens with `A` on its carpet.

## Found on this game

- [Wwise loses the span header in a virtual block from Unreal's heap](../../../docs/reference/memory.md#wwise-loses-the-span-header-in-a-virtual-block-from-unreals-heap): 10 of 16 boots crashed in `AK::MemoryMgr::Init` before the fix ([#996](https://github.com/seedds/nxnative/issues/996)).
- [A size query on a 3D texture gets a 2D slot](../../../docs/reference/shaders.md#a-size-query-on-a-3d-texture-gets-a-2d-slot): open. Up to 19 red lines per run, `declares slot 128 as Metal type 2, texture ID … is type 7`, formats 0x9a and 0x9c, all from a shader's size query on a volume it also samples.

## Open

- The iPhone: no IPA has been built or tested. See [Memory](#memory).
- The sound was not heard. The runs were muted; a `DM_AUDIO_DUMP` of 160 s through the opening holds the game's mix, with signal in every 10 s window (peak 29,489).
- A window was not tested: every run was headless (`DM_WINDOW=0`).
- Only the first play was finished. Of the others, only the ninja play's first set was seen; their costumes and stages are untested.
- The long CPU frames in [Frame rate](#frame-rate).
