# Road 96 (`010031B0145B4000`)

Update 1.05 (v327680) on the base game (v0), SDK 11.4.3. Unity IL2CPP on NVN (the survey finds the strings of Unity 2019.3.0f5 and 2020.3.12f1); FMOD sound through `AudioOut`; VP8 movies through `NintendoSDK_movie` (`subsdk0`). Modules `main`, `subsdk0`, `sdk`. The shader tables come from the UnityFS bundles under `Data/` and Unity's 11 GLSLC outputs in `main`.

On the Mac: the boot, the opening movie, the title, the menus and the first maps, with sound: about 100 minutes in one session (170,000 frames) with no crash. Not tested: the iPhone, and the rest of the game.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `app_pool 0xCD500000` (3285 MB, the Switch's pool) | [`game.tsv`](game.tsv) | the heap check's 1 GB pool ran the game's allocator dry, see [Memory](#memory) |
| 7 GB block with a 6.375 GB alias region: `mem_reserve 0x1C0000000`, `mem_alias 0x198000000`, `mem_stack 0x10000000` | `game.tsv` | at startup the game takes pool − used as one 3.14 GB address region; Engage's 4.375 GB alias region was full before the title |
| `sys_resource 0x1000000` | `game.tsv` | the NPDM declares 16 MB of system resource (virtual address memory) |
| `depth_mode zero` | `game.tsv` | the game sets `nvnDeviceSetDepthMode` near = 0; without the key the bundled pipeline archive was built for near = −w and skipped, see [Frame rate](#frame-rate) |

## Frame rate

- 30 fps by the game's own limit (present interval 1): median frame time 33.3 ms, 95th percentile 34–35 ms in most 300-frame windows, almost no frames dropped on screen.
- **Resolved: hitches of 0.5–1.8 s at each new scene, all first-use pipeline builds** (`new pipelines 49 + shader libraries 80, 1168 ms` in one frame). The game sets `nvnDeviceSetDepthMode` near = 0 and the profile did not say so, so the pipeline archive bundled with the app (253 MB, made offline) was built for near = −w and the host skipped it (`the game's depth mode is not the one built for: the profile key depth_mode`). With `depth_mode zero` the archive is used: the first big scene went from 531 ms with 33 new pipelines to 78 ms with 6.
- **Open: `pso_manifest.tsv` is empty** (`pipeline prewarm list …: 0 lines`), so nothing is prewarmed on a first start, and a few new scenes still build pipelines on first use (13–53 pipelines, 0.4–1.5 s, a few times an hour). Record a list on the Mac over the scenes a new player reaches (`DM_PSO_MANIFEST=off DM_PSO_RECORD=<file>`, [Prewarm lists](../../../docs/development.md#prewarm-lists)).
- **Open: stretches at 15 fps (frame time 65–75 ms) of one to two minutes, about every 5–10 minutes.** Two kinds were seen:
  - *Heavy map, GPU near its budget.* In one map the GPU takes 26–28 ms a frame with a 95th percentile of 28–29 ms, which holds 30 fps; when a stretch of frames goes past 33 ms (95th percentile 31–43 ms) the game moves to a longer present interval, and it returns only after the spikes stop. At 15 fps the median GPU time is lower (18–24 ms) while the spikes remain. 91% of CPU time was on performance cores and changing windows did not help. Not tried: present snapshots (`DM_PRESENT_SNAP=1`, the launcher's Frame buffering), which the repo uses when the GPU idles between frames and macOS holds its clock down ([Present snapshots](../../../docs/reference/frame-rate.md#present-snapshots-the-games-swapchain-texture-free-at-once)).
  - *Process on efficiency cores.* Windows with 12–50% of CPU time on performance cores, at 15–20 fps, with the real-time threads still at priority 97 and the game in front; leaving the window and returning ended it. The cause is not found: the game's GPU time was 10–13 ms, nothing was building, and system idle time stayed at 0 s because the controller's motion stream keeps it alive.
  - Not the cause: memory (about 3.4 GB footprint at the end, no failed maps), thermal state as macOS reports it (nominal), power mode, movies, or the idle gaps between inputs.
- **Open: 5 shaders the shader map does not hold** (`[RED] shader … is not in the shader map`, once each, near an in-game movie), and a compute dispatch with no compute shader. Not yet traced: `DM_NVN_SHDUMP=<directory>` saves their control sections.

## Memory

- The NPDM declares virtual address memory; `main` sets no heap. At startup the game maps pool − used as one address region (3.14 GB at alias +0.75 GB), and Unity maps its allocators beside it.
- **Resolved: crash with a 1 GB pool** (the heap check's value): about 10 s into the title the allocator ran out at 1.5 GB mapped. Unity's block allocator (`main+0x280978`) returned NULL, and `main+0x225ef4` cleared the result without checking: `CRASH sig=11 addr=0 pc=sdk+0x53b41c` (`memset(NULL, 0, 0x368)`). The pool of 3285 MB is the survey's value and the Switch's.
- **Resolved: crash with a 3285 MB pool in the 4.375 GB alias region.** The region was full at startup (mapped up to +4.25 GB of 4.38 GB), and about 30 s in the next address reservation returned 0: `CRASH sig=11 addr=0x7fffe0 pc=main+0x924c18`, Unity's virtual block allocator (`Allocate` through the wrappers at `main+0x928c54`) storing through the handle it got. A 6.375 GB alias region fixed it; the high-water mark in the run was +4.76 GB.
- The heap check's 1 GB pool is a starting point, not a measurement, as on the other Unity games with virtual address memory ([dm/games/README.md, Adding a game](../README.md#adding-a-game), [Advance Wars 1+2](../0100300012F2A000/README.md#memory)). The alias region of 6.375 GB is not tuned: the smallest one that holds is not measured.

## Shaders

- The tables are `shader_split.py`'s own output for the 1.05 files (`shader_map.tsv`, `shader_facts.tsv`, `shader_pairs.tsv`, `pso_pairs.tsv`): hashes, stages and in-game asset names, no shader code. `shader_map.tsv` has about 21,500 rows. A full `dmsh_build.py` run is needed after any chain change.
- A host built from a checkout whose chain version differs from the app's shader libraries (`msl/b-<version>`) loads no shader and draws nothing, with the sound playing: the log shows `[RED] failed to load shader … .metallib` for every draw. Build the app and the host from the same checkout.

## Reading a crash on the Mac

- A Mac app's log is `~/Library/Logs/nxnative/<title ID>.log` (the previous launch's is `<title ID>.prev.log`), and its settings are in `~/Library/Application Support/nxnative/<title ID>/`. A crash prints `[dm] CRASH …` with the registers and a backtrace; `pc` and `lr` are module offsets ([05-debugging.md](../../../docs/agents/05-debugging.md#reading-a-crash-report)).
- Both crashes above were found by converting the NSOs with `dm/tools/nso2elf.py` and reading the instructions at the two addresses with `objdump -d`; the allocator's virtual functions were found with `DM_PROBE` (`x8` at the `blr` names the target).

## Found on this game

- The heap check's 1 GB pool and the 5 GB block are too small for a Unity game that maps its whole pool at startup: [Memory](#memory), [dm/games/README.md, Adding a game](../README.md#adding-a-game).
