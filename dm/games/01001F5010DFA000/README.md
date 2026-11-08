# Pokémon Legends: Arceus (`01001F5010DFA000`)

Update 1.1.1 (`v262144`) over the base game, SDK 11.4.0. Game Freak's engine between Pokémon Sword's ([0100ABF008968000](../0100ABF008968000/README.md)) and the Trinity engine of Pokémon Scarlet and Legends: Z-A, on NVN, with Havok physics, Wwise sound and Pia / NPLN networking. Modules `main`, `subsdk0` (the movie module, which the profile does not load: `main` takes nothing from it the host does not supply), `subsdk1` (GLSLC) and `sdk`; the `sdk` build (`b39add63…`) is Kirby and the Forgotten Land's ([01004D300C5AE000](../01004D300C5AE000/README.md)). Started from the provisional profile the NSP script wrote from its survey (`dm/games/profile_guess.py`). Plays on the Mac at 30 fps, as on the Switch, from the first boot to free movement on the Obsidian Fieldlands beach (a short check, below). An unsigned IPA is built (4.2 GB); iOS is untested.

## What it needs

| Setting | Where | Why |
|---|---|---|
| 5 GB block: `mem_reserve 0x140000000`, `mem_alias 0x118000000` (4.38 GB), `sys_resource 0x1000000` | [`game.tsv`](game.tsv) | the NPDM declares 16 MB of system resource (virtual address memory); `nninitStartup` sets a 0.02 GB heap and initializes malloc's StandardAllocator over 32 GB of address space (`main+0x2779d8c`), which the host gives 4.06 GB ([StandardAllocators over 12 to 62 GB of address space](../../../docs/reference/memory.md#standardallocators-over-12-to-62-gb-of-address-space)) |
| Node area cut and 2 MB alignment of large regions (SDK 20's) | `dm/tools/sdk_patches.py` SDK-wide rules, `sdk` build `b39add63…` (Kirby's) | without the alignment the 4.06 GB region goes to the next 1 GB boundary past the node area and does not fit ([1 GB alignment of regions of 1 GB and more](../../../docs/reference/memory.md#1-gb-alignment-of-regions-of-1-gb-and-more)) |
| `dual_want.tsv` | profile | two post-effect pixel shaders are blended with their output 1 as the second source ([A written secondary blend source](../../../docs/reference/rendering.md#a-written-secondary-blend-source-nvn-dual-source-blending)) |
| `depth_mode zero` | `game.tsv` | the game sets the near=0 depth mode, so the pipeline bases made before it runs take the `main0_z` vertex entries |
| `pad_layout full` | `game.tsv` | the 3D gamepad layout: the camera is on the right stick |
| The capture demo's empty result | [`patches.py`](patches.py) `MATCH_CUTS` | a workaround for a null read in the game's own code, whose cause is open: [Crash in the capture demo](#crash-in-the-capture-demo) |

No host code keyed to this game.

## Bring-up

- **Resolved: a `Break` in `nninitStartup`** (`Break#1 reason=0` at `sdk+0x39fb54`, from `StandardAllocator::Initialize` at `sdk+0x2f98e4` ← `main+0x2779d8c`). The host's cap lowered the allocator's 32 GB to 4.06 GB, but this `sdk` build aligns a region of 1 GB or more to 1 GB, so the region would have started at alias +1 GB and ended past the 4.38 GB alias region. The SDK-wide node-area patch matched this build (implementation 3), the alignment patch had no implementation for SDK 11.4: its placement helper (`sdk+0x34e1d8`) is the SDK 12.3.9 one with 8 more bytes. It is now the alignment patch's second implementation, and both cuts are registered for this build: a build a profile was brought up on takes only registered cuts, so with this profile in `dm/games` the SDK-wide rules no longer reached it (the `Break` came back once). The region lands at alias +0.06 GB.
- **Resolved: an abort on Metal's compiler queue** a few seconds into the language screen (`[AGX] Failed assertion "error == MTLCompilerErrorNoError"` in `findOrCreateComputeFastClearPipeline`, `MTLCompilerService` crash reports in `AGCLLVMStatelessFragmentObject::buildBlendFactor`), with or without the pipeline prebuild. The pipeline was `ha_post_effects.bnsh` variation 4 (`f4f36a03b0c6691d`): an RGBA16Float target blended with destination factor `SRC1_ALPHA`, the shader writing outputs 0 and 1. A temporary log of each blended attachment in `pso_desc` named it as the last pipeline before the abort. `DM_NVN_DUAL_WANT` then listed a second one, `ff45da02e225f961`, in the first minute.
- `[RED] no render target could get an MTLTexture` once at boot ([Passes with no render target](../../../docs/reference/rendering.md#passes-with-no-render-target)); no other red line on the route below.
- The NSP script's rewrite step was reused after the alignment rule was added: it keyed only the patch files that name one of the modules' builds ([The packing script in detail](../../../docs/development.md#the-packing-script-in-detail)).

## Crash in the capture demo

Open: the cause; the crash is worked around ([GitHub issue #712](https://github.com/seedds/nxnative/issues/712)).

- **Symptom:** `CRASH sig=11 addr=0x118 pc=main+0x16e34fc lr=main+0x16e34ac` on `JobThread2`, x8 = 0, after 37 to 65 minutes of normal play on the Mac, with no `[RED]` line before it. Three reporters; the first lost all three sessions of one day to it. The backtrace is always `main+0x19d244c main+0x1cd25e0 main+0x1cceda8 main+0xace34c main+0x1bd9e9c`, and the last frame takes about 1 ms of GPU time.
- **What the code does:** the capture demo task (`main+0x1ccf1a4`; `bin/demo/graphic/system_resource/capture/ob0201_00/anm/capture_animcfg.tracn`, `ThrowableObject`, `my_poke_ball`) builds a ball object and calls `main+0x19d2368`, which calls `main+0x16e33ec` on a 0x1d0-byte object held at `+0x130` of a 0x2e8-byte owner. That function locks the weak reference at `this+0x88` (`main+0x16e9e98`) and reads `[result + 0x118]` with no check. Two things give a null result: the reference itself is null, or its object is dead. The crash log cannot tell them apart.
- **What was measured:** the object's constructor zeroes `+0x88` (`main+0x18954c4`, frame 4595 on the first load) and the field setup's state 0x18 fills it (`main+0x1877a4c` through `main+0x16dde90`, frame 4648): 53 frames with a null reference in that run. That state runs on whichever thread takes the job: the main thread in that run, `JobThread1` (frame 5548) in another. The part of the task that was read has no check for this. At the crash the main thread and `JobThread1` are parked in the job wait, so nothing else is running on that object at that instant. No host service is on the path.
- **Not established:** which null path the crashes take; whether the owner is rebuilt for each scene (one reporter's single hit was followed by a new scene's textures). The likely reading is an order the game does not enforce between the task's own loading and the field setup, which the host's timing exposes. A fresh save does not reach the path: 9 minutes of the route below never called `main+0x19d2368`.
- **Workaround:** `patches.py` returns the function's empty result (`{0, 0}` through x20) on both null paths, before it takes any reference. All 12 direct reads of the result (the ball's `+0x810`, `main+0x19cbcd0` to `main+0x19d1fac`) null-check it; three other places copy it as a reference (`main+0x19cc230`, `main+0x19d2074`, `main+0x19d257c`), whose users were not followed. The room for the check is two of the function's five identical blocks, so the abort stub stays. It is function-matched on 1.1.1 ([function-matched cuts](../../../docs/reference/rewriter.md#function-matched-cuts-for-unregistered-builds)); other builds are unchecked and get a red line if the function differs.
- **Checked:** the rewriter matches the rule once and writes the five cuts, and the game then boots on the Mac to the field setup with no crash (200 s, on the disk's older shader libraries). The patched function run on the module's own bytes in a small interpreter: both null paths return `{0, 0}` with the stack and callee-saved registers restored, and never reach `main+0x16e34fc`; a live object goes on to `main+0x16e3420` as before. The issue's earlier form of the cuts, with the same effect, ran about 7 hours on the Mac with no crash, and the skip was seen once (id 1, frame 142020, followed by 55 minutes of play). These cuts have not met the path in play, and any visible effect on that one ball is unknown.
- **To settle the cause:** `DM_PROBE=main:16e341c,main:16e3828` on a save with Poké Balls, with `DM_PROBE=main:16dde90` to see the order.

## Performance and memory

- On the Mac (M6): 30 fps as on the Switch (`power` lines: 30.0 fps, CPU 0.64 W, about 20% of a core), with long frames of 100–260 ms where new pipelines are first built (that run's archive was made for the near=−w depth mode, before `depth_mode zero`; the Mac build from this profile makes the near=0 bases, not measured on the route since). Resident memory 2.7 GB and 1.46 GB of Metal allocations on the beach after 7.5 minutes.

## Files and shaders

- RomFS 6.0 GB, 19,101 files: effects (`bin/effect`, 2.5 GB), 678 GFPAK archives under `bin/archive` (1.6 GB, every entry Oodle Kraken: [Oodle entries of GFPAK archives](../../../docs/reference/shaders.md#oodle-entries-of-gfpak-archives)), Wwise banks (`bin/sound/NX64/wwise/bank/Default.pck` is the `romfs_marker`), message tables, compiled scripts (`bin/haxe`, `.blua`), and Trinity-style model files (`.trmdl`, `.trmsh`, `.trmtr`). The game probes `arc/data.trpfd` and `arc/data.trfmd` (Trinity's pack index), which it does not ship.
- Shaders: 12,851, by where each first appears: 11,352 in the effects' particle files (`.ptcl`), 1,386 in the loose BNSH files of `system_resource/gfx2/shader` (materials, terrain, water, grass, lighting, shadows, post effects), 73 in the GFPAKs, 40 in `main`. 6,156 pixel shaders, 5,474 of them write exactly colours 0 and 1.

## Mac routes

- The work folder's `run/boot.sh <log> [secs] VAR=value…` runs `build/plahost` (a renamed copy of the host) on the NSP script's build, RomFS image and shader libraries; `run/ctl.sh` and `run/step.sh <shot> [wait] <presses…>` are Sword's (presses through `DM_CTL`, screenshots of the next frame). Run headless.
- From a fresh save (`DM_KBD=<name>` answers the name): A at the language screen (about 70 s after start with the archive opening and the first shaders), A presses through Arceus's questions and the name, the fall through the rift, waking on the beach, Professor Laventon and the three Pokémon, then the player walks (`LUp`, `LRight`).
- The route checked on the Mac: language, name, the opening, the beach with Laventon, Cyndaquil, Rowlet and Oshawott, and a walk through the grass toward the first marker. The game had not saved yet, so there is no reference save.

## Found on this game

General changes first needed here; each applies to every game:

- **Function-matched cuts take branches inside the matched function, and read the trap word `0xe7ffdefe`** (`nso_match.py`): [Function-matched cuts](../../../docs/reference/rewriter.md#function-matched-cuts-for-unregistered-builds).
- **The node area and 32 MB region alignment cuts of `sdk` `b39add63…`, and the SDK 11.4.0 implementation of the SDK-wide alignment patch** (`sdk_patches.py` `MATCH_CUTS`; the build's row was removed on 2026-10-29, as the rules give the same cuts): [1 GB alignment of regions of 1 GB and more](../../../docs/reference/memory.md#1-gb-alignment-of-regions-of-1-gb-and-more).
- **Dual-source blending for NVN shaders** (`dmsh_build.py` `fs_dual`, `main0_d`, the profile's `dual_want.tsv`, `DM_NVN_DUAL_WANT`; `pso_desc` `pso_reads_src1`): [A written secondary blend source (NVN dual-source blending)](../../../docs/reference/rendering.md#a-written-secondary-blend-source-nvn-dual-source-blending).
- **The NSP script's rewrite key always covers `sdk_patches.py`** (`patch_files`): [The packing script in detail](../../../docs/development.md#the-packing-script-in-detail).
