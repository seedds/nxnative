# Ace Angler: Fishing Spirits (`01007C50132C8000`)

Base game 1.0.0 (v0), SDK 13.3.5. Unity 2021.2.17f1 IL2CPP with CRIWARE on NVN (the survey also reports an FMOD marker in `main`). Modules `main`, `subsdk0` (the SDK's movie library) and `sdk`. Plays on the Mac from a fresh save: the title, the agreements, character creation, the story tutorial through the hub, the aquarium and the Fish Gacha Machine, then Ace Angler+ with casts by button and one fish landed. That is a short test: one catch, about twenty minutes of play, driven through `DM_CTL`, and the route ran on the provisional profile, which has the same shaders; the registered profile ran a 75 s boot to the title from that save. No game-specific rewriter cut and no host change of its own; the one change it needed is general ([Found on this game](#found-on-this-game)). Motion casting, the game's main way to cast, is open. iPhone and iPad validation remains open.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `sys_resource 0x1000000` | [`game.tsv`](game.tsv) | the NPDM's 16 MB of system resource: virtual address memory |
| 5 GB guest layout: `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000`, `app_pool 0x40000000` | `game.tsv` | [Super Mario RPG](../0100BC0018138000/README.md)'s layout, the same Unity family; the NSP script's heap check chose the 1 GB pool: the game sets no heap, and with the hardware pool the alias region reached +4.25 of 4.38 GB |
| `depth_mode zero` | `game.tsv` | the game calls `nvnDeviceSetDepthMode` with near = 0 (log: `depth mode near=0`), so the pipeline bases made before it runs take the `main0_z` vertex entries |
| `shader_groups /Data/=data` | `game.tsv` | everything with shaders is under `Data/`: the player's serialized files and the CPK archives of bundles |

## Frame rate

- The game sets the present interval itself: 2 in the hub, the story scenes and the menus (30 fps), 1 in the fishing stages (60 fps).
- **Current, with a window and sound:** the title and the hub hold 33.3 ms (p95 33.7 ms, p99 33.8 ms over 300-frame windows), GPU time under 2 ms.
- **Current, windowless:** the fishing stages run at p50 16.6 ms and p95 20 ms, GPU 1.3 ms, with single frames of 23 to 25 ms and none of them building a pipeline. The stages were not measured with a window, so whether those frames are the windowless pacing or the game's is open.
- A first run stalled once for 7 s at the title, while the pipeline prebuild wrote its archive (`compiles held back by the writes`); later runs, with the archive made, did not.

## Memory

- 3.40 GB resident and 1.30 GB of GPU memory at most over about 21 minutes, windowless, through the route below. One run says nothing about growth; a soak is open.
- The heap check's region check saw alias +2.25 GB in the first 15 s in `nnMain`.

## Files and shaders

- The RomFS has 345 files, 5.66 GB: 22 CRI CPK archives (3.29 GB) under `Data/StreamingAssets/aa/Cpk/`, which hold the game's 21,205 Addressables bundles, 190 MP4 movies, and CRI audio (61 ACB, 38 AWB).
- 31,364 shaders (15,625 vertex, 15,629 pixel, 108 compute, 2 geometry): 31,353 from `Data/` and the player's 11 GLSLC outputs in `main`. All built for the Mac with no failure, in under four minutes.
- `pso_manifest.tsv` has 641 lines (376 shaders, 260 render pipelines, 5 compute): the `pso_user.tsv` of the runs along the route below, not a separate `DM_PSO_RECORD` pass with prewarm off.
- The movies play through the SDK's movie library (`subsdk0`), whose decoder the host answers; the tutorial cards and the stage previews are such movies.

## Mac routes

- Build with `scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> --platform mac` and use its printed command. Windowless, add `DM_KBD=<name>`: the game asks the software keyboard for the character's name, and without a window the host answers a cancel, which the game takes as no name and does not let the character be saved.
- From a fresh save: A at the strap screen and the title. The game shows its EULA and its privacy policy, each with Agree on A. Two lines of dialogue, then the character menu: A on Create, Down twice and A for Name, Down and A for Save, A to confirm.
- The tutorial is dialogue on A (about 28 presses) into the hub; holding Up walks to the aquarium entrance (two holds of 4 s with an A between), 16 more presses inside, Up for 2.5 s to the Fish Gacha Machine and A. Choose the ticket gacha (Right, A), the rare ticket (Right twice, A), and press A through the result and about 60 more lines back in the hub until the Goals screen.
- Plus opens the Fishing Pad with Ace Angler + selected: A travels there, A through its two cards and its title, A on the first machine. Five how-to-play cards on A, and the stage starts.
- In a stage: Y inserts a medal (a Normal Rod takes 3), A takes the rod, A fixes the direction, and A again stops the Power Gauge, which sweeps by itself: the cast lands as far as the gauge stood. A miss gives the medals back. Pressing A repeatedly after a bite reels in; the first catch was a Red Cornetfish for 5 medals.
- The game saves after character creation and when a stage is left; a run stopped inside the tutorial starts it again.

## Found on this game

- The first split found 1,458 shaders and the title asked for programs the map did not have: the Addressables bundles are inside CRI CPK archives, which the split did not read as Unity bundles. [UnityFS bundles in CRI CPK archives](../../../docs/reference/shaders.md#unityfs-bundles-in-cri-cpk-archives).

## Open

- **Current (2026-11-16): motion casting does not answer to the host's `Shake`.** At the Power Gauge a scripted `Shake~0.5` leaves the gauge sweeping; A casts. The host's shake moves the controller side to side (x acceleration and yaw, [`hle_hid.c`](../../host/hle_hid.c)), and the game's own names (`FwNxPadSwingFishingRod`, `calcAccelMinY`, `ACCEL_JUDGE_VALUE`, `SwingDown` in its IL2CPP metadata) point at a downward swing judged on the y acceleration. Its swing code was not read, so the axis and the threshold are not known, and a real controller's motion (a Joy-Con or a DualSense through GameController, the iPhone's own) was not tried. The game reports JoyDual to itself on the Mac keyboard (it supports Handheld and JoyDual, then all five styles).
- Everything past the first catch: the other modes (the story's tours, the pusher game, the aquarium), multiplayer on one console, a boss fish, a long session. Online and local wireless play (NEX, Pia, `nn::ldn`) are out of scope and were not opened.
- The face editor's last three rows showed empty grey circles where the rows above had pictures. Whether those are items still locked or icons that did not draw was not checked against the console.
- The title screen shows 1.1.1 in its corner on the 1.0.0 base game. Not looked into; it may be the game's own data version.
- Nobody has watched it in motion beyond single frames, or listened to it: sound is checked only in the log (one AudioOut with 6 channels, no underrun in 110 s). Input has gone only through `DM_CTL`, not a controller or the keyboard.
- iPhone and iPad runtime validation; no IPA has been built. Memory there is the first thing to watch.
