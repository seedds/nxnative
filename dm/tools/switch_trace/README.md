# switch_trace: console ground truth

A code-injection module for a game running on a Switch or a Switch 2. It records what the game asks NVN to do, and how long the console's GPU takes, in the terms the host uses. The tools beside it then line that up with a host run of the same scene.

**Not yet run on a console, and not built with a Switch toolchain.** On the Mac, `test/run.sh` builds the module against a mock NVN driver and checks every mode (see [Testing](#testing)). The joiners have been run on a real Ocarina of Time host log.

## What it records

A capture is a short burst of frames (120 by default), started by a trigger. Each mode is cheap enough to leave on, and they can be combined (`mode=timing,seq`).

| Mode | What | Cost while capturing | Files |
|---|---|---|---|
| `timing` | GPU time per interval. Intervals are inferred as the host infers encoders: a new one at each `SetRenderTargets`, one per run of dispatches or of mesh / task draws (`split=run`), or one per dispatch (`split=each`). Each interval lists the shader keys of the programs its draws used | one GPU timestamp per boundary; a few ns of CPU per draw | `capture<N>.tsv` |
| `seq` | The ordered command stream, per command buffer, in submission order: targets, binds with their keys, draw runs, dispatches, mesh draws, barriers, event waits and signals, syncs, counter reports, render enable and conditional render, clears and copies. Also the queue's submits, waits, fences and presents | no GPU work; one 24-byte record per call (draw runs collapse) | `seq<N>.tsv` |
| `dump` | Render target copies (colour 0, optionally depth, and buffer ranges you name) taken after every interval that used a named shader key, or after every render interval | one copy per dump | `dump<N>.tsv`, `dump<N>_<n>.bin` |

**Idle cost:** with no capture running, each wrapped NVN call costs one relaxed load of a flag. Recording starts 2 frames before the first logged frame. The logged frames never wait on the GPU and never touch the SD card. After the burst, the module fences each queue without waiting, polls the fences at later presents, and writes the files once they have signalled. Expect one hitch a few frames after a capture, while the files are written.

**Shader keys** are the host's (`keys_hash` in [hle_nvn.c](../../gfx/nvn/hle_nvn.c)): the 16-hex keys of a game's `shader_map.tsv` and of the host's `[pass]` lines.
- **NVN2 (Switch 2):** SHA-256 of the control section's first 0x48 bytes, with +0x30..+0x33 as zero, then the code.
- **NVN (Switch 1):** SHA-256 of the first 0x880 bytes (or the section's own length), with the profile's `shader_key_skip` range as zero (config `key_skip=`), then the code.
- **Programs come from** `nvnProgramSetShaders`, `nvnProgramSetShadersExt` (`ext_stride=`, 0x18 by default) and NVN2's program builder.
- **When keys are hashed:** NVN2 stages are hashed after the capture, and only those it used, because Switch 2 games make thousands of programs. NVN stages are hashed when the program is made.

## Prerequisites

- **A console that loads code-injection modules into a game.** This means custom firmware that layers files over a game's ExeFS (Atmosphère-style `contents/<title id>/exefs/`).
  - **Switch 1 homebrew is mature** (exlaunch, skyline), so validate this tool on a Switch 1 game first.
  - Whether a Switch 2 can run such modules is for you to check.
- **A toolchain that builds such modules:** devkitPro's devkitA64, plus a framework that builds an injected `subsdk` NSO and hooks a function. `src/platform.h` has an adapter for exlaunch.
- **Headers declaring the `nn::fs`, `nn::hid` and `nn::os` functions** that `platform.h` calls. The module links against the game's own `sdk`, which exports them. Ocarina of Time's (Switch 2) does, as does `nvnBootstrapLoader`.
- **Your own game and console.** Nothing here reads keys or game files.

## Build

1. Put `src/` into a module project and compile `nvn_trace.cpp` as one C++17 translation unit, with `-DST_EXLAUNCH`. Add these as needed:
   - `-DST_TITLE_ID=\"<title id>\"` gives each game its own folder, `sd:/switch-trace/<title id>/`;
   - `-DST_NO_HID` leaves out the button combo;
   - `-DST_LARK` also reads the Switch 2 controller states, and needs headers that declare them.
2. Call `switch_trace_main()` from the module's entry point (`exl_main`), before the game sets up NVN.
3. Check the names `platform.h` marks against your framework and headers.
4. The output is an NSO in a free `subsdk` slot (`subsdk1`..`subsdk9`; the game's ExeFS shows which slots it already uses).

The module hooks only `nvnBootstrapLoader`. The game's NVN functions reach it through `nvnDeviceGetProcAddress`, and the module hands back its own wrappers for the ones it records.

## Install and trigger

1. Copy the NSO to `atmosphere/contents/<title id>/exefs/subsdk<N>`, or wherever your firmware layers ExeFS.
2. Create the output folder: `sd:/switch-trace/` (or `sd:/switch-trace/<title id>/`).
3. Start the game. `trace.log` in that folder should begin with `switch_trace: loaded in ...` and the settings. If it doesn't, the module did not load.
4. Start a capture with any of these triggers:
   - **The combo:** press both sticks in together, then let go. The capture starts `delay` frames after the release, so the press itself is over.
   - **`start_frame=`:** a frame number, counted in presents since boot.
   - **The go file:** an empty file named `go` in the folder, which the module checks once a second and deletes.
5. `trace.log` shows `capture <N> armed`, then `capture <N> written`.

### Configuration

`config.txt` in the output folder, one `key=value` per line; `#` starts a comment.

| Key | Default | Meaning |
|---|---|---|
| `mode` | `timing` | any of `timing`, `seq`, `dump`, comma-separated |
| `frames` | 120 | frames per capture |
| `delay` | 30 | frames from the trigger to the first logged frame |
| `combo` | `LS+RS` | buttons held together, then released (`A B X Y LS RS L R ZL ZR Plus Minus Left Up Right Down`, or `off`) |
| `start_frame` | none | log from this frame |
| `split` | `run` | `each`: a timing interval per dispatch and per mesh draw. This also keeps dispatches from overlapping: use it for per-dispatch cost, and `run` for pass totals |
| `key_skip` | none | NVN games: `OFF:LEN`, the profile's `shader_key_skip` |
| `ext_stride` | `0x18` | bytes per stage in `nvnProgramSetShadersExt` |
| `gpu_ts_hz` | none | the timestamp rate, when `nvnDeviceGetTimestampInNanoseconds` is missing (raw ticks otherwise) |
| `dump_keys` | none | up to 16 shader keys: dump after each interval that used one |
| `dump_all` | 0 | 1: dump after every render interval that drew something |
| `dump_size` | any | only targets of this size, e.g. `1920x1080` |
| `dump_depth` | 0 | 1: the depth target too |
| `dump_buffer` | none | `<gpu address hex>:<bytes>`, a buffer range copied with each dump (up to 4) |
| `dump_max`, `dump_mb` | 64, 256 | the most dumps per capture, and the memory they may use |

## Outputs

Every file ends with `#` lines:
- the slots and tables used;
- the game's own `ReportCounter` calls a frame, by counter type;
- where its own timestamps go (an engine's dynamic-resolution timer, for instance);
- how long writing took.

**`capture<N>.tsv`:** one row per interval.
- **Columns:** `frame idx us kind queue w h colors cfmt dfmt draws dispatches groups indirect keys waits signals barriers start_us flags`.
- **`kind`:** R, C or M; W and F rows are queue waits and fences; `?` marks a command buffer recorded before the capture (`unstamped`).
- **`groups`:** workgroups (`XxYxZ` for one dispatch, the total for a run), or tasks for mesh draws.
- **`flags`:** `dup` marks a command buffer submitted twice; only its last run's stamps survive.

**`seq<N>.tsv`:** one row per command.
- **Columns:** `frame idx queue cb n op args keys`.
- `cb` is the submission's index in the frame, and `n` the command's index in the command buffer.
- Each recording starts with the bindings it inherits.
- Events and syncs are numbered in order of appearance.

**`dump<N>.tsv`:** one row per dump.
- **Columns:** `n frame kind w h nvn_format bpp bytes buffer file`.
- The `.bin` files hold tightly packed rows, top row first.

## On the Mac

Profile the same scene on the host:
- `DM_NVN_GPUPROF=every:<n>` (or a frame range) for the timing join;
- `DM_NVN_PASSDUMP=<F>` for the frame's keys and order.

Then join the files.

| Script | What |
|---|---|
| `join.py <captureN.tsv> --log <game.log> --frame F [--passes LO-HI] [--game <id>]` | GPU time by shader key and by size class on each side, as times and as shares of the frame, and the keys that are cheap on the console but expensive on the host. `--game` labels the keys from the profile's `shader_map.tsv`; `--host-tsv` keeps the host table |
| `seqdiff.py <seqN.tsv> --log <game.log> --frame F [--passes LO-HI]` | Both orders as token streams (passes by size class, dispatches by key, barriers, signals, fences, render enable / conditional render), diffed. The host's splits, merges and hoisted dispatches show up as differences |
| `dump2png.py <folder> [--capture N]` | `dump<N>_<n>.bin` → PNG for the formats render targets use (8-bit, 16- and 32-bit float, R11G11B10F, RGB10A2, depth) |

Compare shares and order, not milliseconds. The console's clocks, the game's dynamic resolution and our fixed resolution all differ.

## Caveats

- **Timestamps perturb small intervals.** Each is a bottom-of-pipe write that can keep neighbouring work from overlapping, and an interval includes overlap with the next. Treat intervals under ~20 µs as noise.
- **Binds are tracked per NVN stage bit.** An ordinary draw reads the vertex to fragment stages; a mesh draw reads task, mesh and fragment. A program bound for one stage contributes all its stages' keys.
- **Keys show `?`** when the game copies a program by value, when the code sits in a pool the CPU cannot map, or when more than 32,768 stages are recorded.
- **Dumps:**
  - Only colour target 0, and only formats with a known size per pixel; block-compressed formats are skipped.
  - Multisampled and layered targets are not handled.
  - Each dump adds a barrier and a copy to the frame.
- **The module's own objects are oversized** (NVN2's struct sizes are in no public header): pools, syncs and builders of 0x400 bytes.
- **Memory:**
  - 3 MiB at start-up, for the stage records;
  - per mode, at the first capture: timing 2 MiB of counters plus 9 MiB of tables, seq 6 MiB, dump `dump_mb`.

## Testing

`test/run.sh` builds `nvn_trace.cpp` with the Mac's clang++ against `test/mock_nvn.cpp`, a mock NVN driver whose GPU runs each submission two frames late.
- **The mock game** records on two threads and makes its programs through every path the module covers, including an NVN (Switch 1) control section.
- **`test/check.py`** checks every interval's time, counts and keys (`split=run` and `each`), the whole sequence, and the dumped bytes. It also checks that no sync wait other than a zero-timeout poll was made.
- **The keys** are recomputed with `hashlib` from the mock's sections.

[test_switch_trace.py](../tests/test_switch_trace.py) runs the mock test and covers the joiners on synthetic host logs.

This tool builds on Pepper's OoT switch-trace (the key rule, the first `join.py`) and its rework for asynchronous bursts and OoT's NVN2 program paths, both of 2026-11-09.
