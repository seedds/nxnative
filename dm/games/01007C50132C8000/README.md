# Ace Angler: Fishing Spirits (`01007C50132C8000`)

Switch update 1.2.3 (`v589824`), Nintendo SDK 13.3.5 and NVN API 55.15. The game uses Unity 2021.2.17f1 IL2CPP, CRIWARE archives and NintendoSDK_movie. The existing rewriter handles the three modules with SDK-wide rules.

The Apple M6 Mac on macOS 27.2.1 reached the Joy-Con notices, the title screen and the first-run license agreement from a fresh save at 1920 × 1080. The title at rest had a median frame time of 16.67 ms, with stereo audio through CoreAudio and no `[RED]` lines on that route.

The base game 1.0.0 (`v0`) was played further on the Mac, from a fresh save through character creation, the story tutorial, the hub, the aquarium and the Fish Gacha Machine, then Ace Angler+ with casts by button and one fish landed: about twenty minutes, driven through `DM_CTL`, with no crash and no `[RED]` line ([Base game 1.0.0](#base-game-100)). That run used the NSP script's provisional profile for `v0` (a 1 GiB `app_pool` and no `stdalloc_cap`), not this one.

## What it needs

| Setting | Why |
|---|---|
| `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000`, `sys_resource 0x1000000` | NPDM specifies 16 MiB of system resource. The game uses virtual address memory and makes no `SetHeapSize` call at startup. |
| `stdalloc_cap 0x80000000` | The uncapped SDK allocator reserved 3.12 GiB and mapped 0.43 GiB in the first 15 seconds. Unity's later reservations reached +4.51 GiB, beyond the iPhone's 4.375 GiB alias region. With the cap and the iPhone-sized layout, live regions reached +3.36 GiB after 35 seconds on the Mac. The reported Switch application pool is still 3,285 MiB. |
| `depth_mode zero` | The game sets near depth to zero. The pipeline prebuild and live renderer use the same vertex shader entry point. |

## Shaders

The update has 24,809 UnityFS bundles in 73 CRI CPK archives. The split finds 31,933 distinct stages, including 108 compute and two geometry shaders. The old split found 1,522. The split took 18 seconds with the fix and 12 seconds before it on the same Apple M6. All stages compiled for Mac and iOS with Xcode 27.2.

## Found on this game

- [Unity asset bundles inside CRI CPK archives](../../../docs/reference/shaders.md#unity-asset-bundles-inside-cri-cpk-archives).

## Mac controls

Press J (A) to confirm the Joy-Con safety notice and at the title prompt. K is B and Return is +. The Mac performance HUD is enabled with `DM_METAL_HUD=1`.

## Base game 1.0.0

Measured on an M6 Mac with the provisional `v0` profile (the layout above with `app_pool 0x40000000` and no `stdalloc_cap`, chosen by the NSP script's heap check), on 2026-11-16.

### Frame rate

- The game sets the present interval itself: 2 in the hub, the story scenes and the menus (30 fps), 1 in the fishing stages (60 fps).
- With a window and sound, the title and the hub hold 33.3 ms (p95 33.7 ms, p99 33.8 ms over 300-frame windows), GPU time under 2 ms.
- Windowless, the fishing stages run at p50 16.6 ms and p95 20 ms, GPU 1.3 ms, with single frames of 23 to 25 ms and none of them building a pipeline. The stages were not measured with a window, so whether those frames are the windowless pacing or the game's is open.
- A first run stalled once for 7 s at the title, while the pipeline prebuild wrote its archive (`compiles held back by the writes`); later runs, with the archive made, did not.

### Memory

- 3.40 GB resident and 1.30 GB of GPU memory at most over about 21 minutes, windowless, through the route below. One run says nothing about growth; a soak is open.

### Files

- The 1.0.0 RomFS has 345 files, 5.66 GB: 22 CRI CPK archives (3.29 GB) under `Data/StreamingAssets/aa/Cpk/`, which hold 21,205 Addressables bundles, 190 MP4 movies, and CRI audio (61 ACB, 38 AWB). Its split found 31,364 shaders, all built for the Mac.
- The movies play through the SDK's movie library (`subsdk0`), whose decoder the host answers; the tutorial cards and the stage previews are such movies.

### Route

- Windowless, add `DM_KBD=<name>`: the game asks the software keyboard for the character's name, and without a window the host answers a cancel, which the game takes as no name and does not let the character be saved.
- From a fresh save: A at the strap screen and the title. The game shows its EULA and its privacy policy, each with Agree on A. Two lines of dialogue, then the character menu: A on Create, Down twice and A for Name, Down and A for Save, A to confirm.
- The tutorial is dialogue on A (about 28 presses) into the hub; holding Up walks to the aquarium entrance (two holds of 4 s with an A between), 16 more presses inside, Up for 2.5 s to the Fish Gacha Machine and A. Choose the ticket gacha (Right, A), the rare ticket (Right twice, A), and press A through the result and about 60 more lines back in the hub until the Goals screen.
- Plus opens the Fishing Pad with Ace Angler + selected: A travels there, A through its two cards and its title, A on the first machine. Five how-to-play cards on A, and the stage starts.
- In a stage: Y inserts a medal (a Normal Rod takes 3), A takes the rod, A fixes the direction, and A again stops the Power Gauge, which sweeps by itself: the cast lands as far as the gauge stood. A miss gives the medals back. Pressing A repeatedly after a bite reels in; the first catch was a Red Cornetfish for 5 medals.
- The game saves after character creation and when a stage is left; a run stopped inside the tutorial starts it again.

## Open

- Gameplay on 1.2.3 with this profile past the first-run license agreement; the 1.0.0 route above has not been repeated on it.
- **Current (2026-11-16, on 1.0.0): motion casting does not answer to the host's `Shake`.** At the Power Gauge a scripted `Shake~0.5` leaves the gauge sweeping; A casts. The host's shake moves the controller side to side (x acceleration and yaw, [`hle_hid.c`](../../host/hle_hid.c)), and the game's own names (`FwNxPadSwingFishingRod`, `calcAccelMinY`, `ACCEL_JUDGE_VALUE`, `SwingDown` in its IL2CPP metadata) point at a downward swing judged on the y acceleration. Its swing code was not read, so the axis and the threshold are not known.
- Everything past the first catch: the other modes (the story's tours, the pusher game, the aquarium), multiplayer on one console, a boss fish, a long session. Online and local wireless play (NEX, Pia, `nn::ldn`) are out of scope.
- The face editor's last three rows showed empty grey circles where the rows above had pictures (1.0.0). Whether those are items still locked or icons that did not draw was not checked against the console.
- The 1.0.0 title screen shows 1.1.1 in its corner. Not looked into; it may be the game's own data version.
- Physical Joy-Con input and iPhone gameplay.
