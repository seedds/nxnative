# Godzilla: Destroy All Monsters Melee Remastered (`0100A00026D70000`)

Update 1.0.2 (title `0100A00026D70800`) over base 1.0.1, SDK 21.4.2. Atari, **Unreal Engine 5.8.1** (`SDK MW+EpicGames+UnrealEngine-5_8_1` in `main`), NVN. Modules `main`, `subsdk0`, `sdk`.

Status: brought up on the Mac. Boot, the Atari logo, the EULA, the intro, the title, a new Adventure (Classic), the character select and a full round of the first battle (Godzilla vs Anguirus, San Francisco) all render with no red line, and every model, menu and HUD draws. Two things are open: the frame rate (the profile asks for 60 but the rewriter cannot name UE 5.8's
console variables yet, see [60 fps is blocked on the rewriter](#60-fps-is-blocked-on-the-rewriter)) and a hitch during the boot video (see [Open](#open)).

## What it needs

| Setting | Where | Why |
|---|---|---|
| `fps_target 60` | [`game.tsv`](game.tsv), `hle_fpslock.c` | the game sets `rhi.SyncInterval` 2 (30 fps) and re-applies it at level loads; the host writes 1 back. **Does not take effect on UE 5.8 yet** — see [60 fps is blocked on the rewriter](#60-fps-is-blocked-on-the-rewriter) |
| `depth_mode zero` | [`game.tsv`](game.tsv) | the game calls `nvnDeviceSetDepthMode` with near = 0; without the key `pso_offline.py` builds the Mac pipeline archive for the GL convention and the host rejects it (see [The pipeline archive](#the-pipeline-archive-was-built-for-the-wrong-depth-mode)) |
| `tex_dyn_slots on` | [`game.tsv`](game.tsv) | 27 vertex shaders pick their texture slot at run time; see [Shaders](#shaders) |
| Three `CUTS` of `main` at `0xb7f20`, `0xb8530`, `0x1ffa2c` | [`patches.py`](patches.py), build `b28a1cc08f675a437de4d399e239bbaa` | the shipped image replaces three function prologues; a fresh rewrite that runs them crashes at `main+0xbc584` in `nninitStartup`'s DT_INIT. See [Boot](#boot) |
| `mem_reserve 0x1028000000`, `mem_alias 0x1000000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000`, `sys_resource 0x1400000` | [`game.tsv`](game.tsv) | Unreal's `nninitStartup` reserves 62 GB and its engine region grows with play; see [Memory](#memory) |
| `pso_manifest.tsv`: 1,154 rows recorded on the Mac | [`pso_manifest.tsv`](pso_manifest.tsv) | the shaders and render pipelines the game creates on the submit thread, built at the next start instead of on the frame that first draws them |
| `DM_KBD_PLAYER=own` | run-time switch (`Documents/env.txt`, the app bundle's `dm/env.txt`) | one keyboard plus two controllers is three players (see [Local multiplayer](#local-multiplayer)) |
| `DM_DBG_AU_FRAMES=840` | run-time switch, same files | 17.5 ms of device buffer; the 5 ms default underruns and crackles (see [Sound](#sound)) |
| `DM_PRESENT_LINK=0` | run-time switch, same files | the classic present path; the display link adds a fixed 49.9 ms lead the sound does not take |

No `mem_stack`/`app_pool`/`sys_resource` survey overrides beyond the generated ones, and no DLC: the tested copy is base 1.0.1 plus update 1.0.2.

## Boot

- **Resolved: the game hung in `FRenderCommandFence::Wait` and the engine's own watchdog aborted it after 120 s.** With observation gates on the fence, the task-graph queue and the render command's execution, the run showed the render command reaching the render thread and running, the queue empty and every thread asleep, while the fence's completion byte stayed 0. `RenderingThread.cpp:1106` (`GameThread timed out waiting for %ls after %.02f secs`) is Unreal's watchdog, and the thread it names is hard-coded (`// TODO: Walk the wait chain`) — so it is not evidence against the render thread. See [Frame rate](#frame-rate) for the cause.
- **Resolved: a fresh rewrite crashed at `main+0xbc584`.** The shipped image has three function prologues replaced (forward through `this->vtable[3]` / `[5]`, and a stored pointer instead of two calls at `0x1ffa2c`); a rewrite that runs the originals reaches a null vtable read inside `nninitStartup`'s DT_INIT. Copied byte for byte into [`patches.py`](patches.py), a clean build from the NSPs comes out **identical to the shipped `main.patched.bin` (0 differing words)** and boots to the same place.
- **Note:** the three cuts are the shipped image's own bytes, not a deviation — the switch to 1.0.2 is what makes a fresh rewrite differ from the release here.

## Frame rate

- **Current: 30 fps, the game's own.** The profile declares `fps_target 60`, which is the host's mechanism for an Unreal game (`hle_fpslock.c` `ue_rows` writes the
  engine's `rhi.SyncInterval` and `t.MaxFPS` through the offsets the rewriter finds by name), but on UE 5.8 those offsets are not found yet: see
  [60 fps is blocked on the rewriter](#60-fps-is-blocked-on-the-rewriter) below. With no offsets the host writes nothing, keeps the game's own present interval and
  logs no red line, so the profile is safe to ship as it is — it reaches 60 fps the moment the rewriter can name the variables.
- **Resolved: the boot hang was the swapchain sync depth.** `r.GTSyncType` is **2** in this build, which makes `FFrameSync::Sync` pick `ESyncDepth::Swapchain`
  (`RenderingThread.cpp:2526`): the fence's `UE::Tasks::FTaskEvent` then waits on `RHITriggerTaskEventOnFlip`, which only `FRHIFrameFlipTrackingRunnable` reaches
  through the platform RHI's `RHIWaitForFlip()`. On a host that reports no flips the event never completes and the GameThread sits in `GameThreadWaitForTask` until
  Unreal's watchdog aborts it after 120 s. Current `hle_vi.c` implements `ListFrameHistory`, so the flips are reported and the game boots from a clean tree.
- At 60 fps the margin is real: a frame costs **5.4 ms of GPU and 1.6 ms of CPU** against the 16.67 ms a 60 fps frame has, and the measured frame time is
  **p50 16.67 ms with 0 frames dropped of 300** (`p99 16.67`). At the shipped 30 fps the budget is 33.33 ms and the game measures **33.4–33.8 ms**, so it misses
  by a hair and drops a frame now and then (10–19 of 300, p99 66.67 ms) — that is the stutter reported before the frame rate was raised.
- **Resolved: the display link's fixed 50 ms lead.** `@link_lead_ms` measures **49.9 ms** in a window: the system calls the display link back about three frames
  before the target display time, and audio does not take that delay, so the sound is heard first. `DM_PRESENT_LINK=0` (the classic present path) has no such lead.
  Measured over three 90 s runs each: the display link dropped 21 frames and underran audio once, the classic path dropped 0 and underran 0.
- `DM_FPS_TARGET=30` keeps the game's own 30 and saves power.

### 60 fps is blocked on the rewriter

`dm/tools/ue_cvar.py` finds an Unreal console variable by the name it is registered under and looks for that name loaded **into `x1`** — which is where
`TAutoConsoleVariable`'s constructor took it up to UE 5.6. UE 5.8's constructor is `(this, Flags, Name, DefaultValue, Help)`, so the name is the third argument and
lands in **`x2`**. Run against this build's `main`, every name comes back `not registered`:

```
ue_cvar.py <main>  ->  t.MaxFPS: not registered; rhi.SyncInterval: not registered;
                       nvn.SyncInterval: not registered; r.Streaming.PoolSize: not registered;
                       Project.FrameRateLimit: not registered
```

Checked on the same `main` with this repository's tool and with upstream `490859e`: both find none of the five. Nothing about the game is special here — any
UE 5.7 / 5.8 title loses every console variable the same way, so this is a fix for the rewriter (let `registered_at` accept the register the caller saw the name
loaded into), not a key this profile can carry.

`present_interval display` — the profile key that makes the host present one frame per display tick whatever the game asks — was measured as a workaround and does
**not** work here: it lifts the present interval to 16.7 ms but the game still steps **30 times a second** (900 frames in 30 s, the same as with the game's own
interval), i.e. every frame is shown twice. That matches the note in `docs/reference/frame-rate.md`: for an Unreal game the interval is not what holds the frame
rate back. It is therefore not in this profile.

## Memory

- Unreal Engine 5.8's `nninitStartup` reserves a 62 GB address region for the engine and its `FMallocBinned3` takes 37 GB of it, as on the other Unreal games; the rewriter's engine-wide rules cut the allocator's constructor call and size the region to what the alias region has room for (`dm/tools/ue_malloc.py`, `ue_region.py`), and the host runs Unreal's heap.
- **Checked: UE 5.8 did not change the allocator's vtable.** The base vtable at `main+0x74e3f50` has **32 slots**, the same layout as `UE5_6`: 6 `Malloc`, 7 `TryMalloc` (a tail call to 6), 8 `Realloc`, 9 `TryRealloc` (a tail call to 8), 10 `Free` (`(ptr − base) >> 29` picks the pool), **11 and 12 the same address** (`MallocZeroed` / `TryMallocZeroed`, slot 6 then `memset`), 13 `QuantizeSize(size, alignment)` (its pool limit is `0x3800`), 14 `GetAllocationSize` (`>> 33`), 15 the TLS-cache switch, 23 `DumpAllocatorStats`, 20–22 and 27–29 `ret`, 24/25 `return true`, 30/31 `return 0`, 26 `GetDescriptiveName` returning the UTF-16 string **`Binned3`** at `main+0x6e4e9b4`, and slot 32 = 0. So the profile needs no allocator layout of its own.
- The region is given 62 GB (`mem_reserve 0x1028000000`) with a 64 GB alias region, from the survey: the game's regions reach alias +63.00 GB in its first 15 s in `nnMain`. What it takes later in play is not measured.

## Shaders

- **6,878 unique shaders** (1,017 vertex, 4,206 pixel, 1,655 compute) in the RomFS's two shader code archives, `GZPro/Content/Paks/…` → `ue_shaders/ShaderArchive-IoStore-771a015d3264b974.ushaderbytecode` (4,981 records) and `ShaderArchive-IoStore-7f40e074194f7bcc.ushaderbytecode` (1,984 records), decoded from the IoStore chunks by the NSP script through the game's own Oodle in the Mac host (`uepak.py`).
- Every record is a **raw NVN shader**, not an `LSLG` record: the code area (with its `0x12345678` graphics / `0x12345679` compute magic) first, then the control section (`0x98761234`). The engine combines vertex and pixel shaders per draw, so they are paired as single-stage programs (`separable_pairs`): 1,017 vertex shaders share a cover of 3 pixel shaders.
- **Resolved: the shader map's keys did not match the host's — every one of them.** `shader_split.py`'s raw-NVN branch hashed the code area *after* the 4-byte NVN magic (`raw_code[4:]`), while the host's `keys_hash` hashes `ncode` bytes from the magic *including* it, and `ncode` (control +0x704) already counts those 4 bytes. The keys were all shifted by 4 bytes: **0 of 267** shaders dumped with `DM_NVN_SHDUMP` matched, and a boot logged 60 `[RED] shader … is not in the shader map` lines with models and menu text drawing black and overlapping. Taking `raw_code[:ncode]` matches **267 of 267**.
- **Current: 27 vertex shaders pick their texture slot at run time** (they are `NVN_RB_TEXD` / `NVN_SAMPD` arrays in the reflection). Without `tex_dyn_slots` the host refuses the reflection with `a texture handle the translator could not trace to one slot`, then `failed to load`, and the draw is skipped; with it the replay writes slot indices into that shader's c2 tables. Same shape as FINAL FANTASY RESONANCE and OCTOPATH TRAVELER 0.

## Sound

- The game opens three `AudioOut`s: `#0` (opened and closed at boot, `main+0x65d2b2c`), `#1` (the sound effects and music, `main+0x65d2f4c`, `StartAudioOut` at `main+0x65d34f4`) and `#2` (the host's `AudioRenderer`, 32 kHz, upsampled to 48 kHz in the host).
- **Resolved: crackle — the Mac output buffer was too small.** The game queues one 5 ms block per callback and refills it only when the callback takes it, so at the host's
  5 ms default (`hle_audio.c` `au_buffer`, 240 device frames) every callback arrived with **1.0 blocks queued** and nothing spare: 286 of every 2000 callbacks found
  the queue empty and padded **858 ms of zeros**. `DM_DBG_AU_FRAMES` is the host switch for it, and **840 frames (17.5 ms)** queues 2.0 blocks and underran
  **0 times in 2000**. 480 frames (10 ms) is enough at the game's own 30 fps but not at 60 (161–1000 underruns of 2000); 960 (20 ms) underran again.
- **Open: a hiss on the menu's selection sound — the upsampler's filter is too short.** The `AudioRenderer` runs at 32 kHz and the host upsamples 3:2 with a windowed sinc; at 16 taps and a 0.9 cutoff its response was only **−8 dB at 16 kHz** (the source's Nyquist) and −4 dB at 18 kHz, so images leaked above 15 kHz: measured on a menu selection, **7.4 % of its energy sat above 15 kHz**, where a 32 kHz source has nothing. 48 taps put it at −49 dB at 16 kHz and −30 dB at 16.5 kHz, and the same measurement gives 2–4.7 %. The tap count is `hle_audren.c`'s `RS_T` (16) with no switch
  behind it, so this one cannot be set from a profile or an env file; it is left as a host change rather than worked around here.

## Local multiplayer

One keyboard plus two controllers is three players, and that needs `DM_KBD_PLAYER=own`: by default the front end merges the keyboard into player 1 together with the
first controller (`front_mac.m`, `g_kbd_own`), so one keyboard plus two controllers fills players 1 and 2 only and the game leaves the third monster to the computer,
which reads as "the controllers stop working in the battle".

| Setting | Slots | npads the game reads |
|---|---|---|
| default | keyboard + controller 1 → player 1, controller 2 → player 2 | **No1, No2** (2 players) |
| `DM_KBD_PLAYER=own` | keyboard → player 1, controller 1 → player 2, controller 2 → player 3 | **No1, No2, No3** (3 players) |

The game asks for four (`SetSupportedNpadIdType` with `0xf`, No1..No4), so three is within what it accepts. Checked by counting the reads through a temporary
instrument in `hle_hid.c`'s `fill` (not part of this change): with the switch the game reads npad 0, 1 and 2 about 60 times a second each, where the default
presents two; the game's calls to `DisconnectNpad`, `SetNpadJoyAssignmentMode*` and `SwapNpadAssignment` are zero, so nothing in the host drops a player. The
switch also stays inert when unset (0 lines, no red line).

It is an existing switch of the host, so the profile does not need a key of its own: set it in `Documents/env.txt` (the iPhone) or the Mac app's `dm/env.txt`.

## Files

- `RomFS.img` carries one IoStore container pair, `GZPro/Content/Paks/GZPro-Switch.utoc` / `.ucas` (the profile's `romfs_marker`), a `global.utoc` pair (926 bytes) and the two shader code archives (`ue_shaders/`, 44 MB + 19 MB, decoded on the Mac by the game's own Oodle).
- The profile's `zip_store` keeps `.bk2` raw: the game's movies are Bink video.

## The pipeline archive was built for the wrong depth mode

A boot logged, verbatim, the failure Octopath Traveler 0's profile documents as resolved:

```
pipeline archive bundled with the app: made for chain 17e56636959c; depth near=-w;
  pairs … this run needs chain 17e56636959c; depth near=0; …
  the device builds the bases it does not hold
```

The game sets near = 0 through `nvnDeviceSetDepthMode`, the profile had no `depth_mode` key, so `pso_offline.py` built the Mac app's archive for the GL convention and the host rejected it — every base was built on the device instead. With `depth_mode zero` the archive is accepted (`holds the bases of every shader pair (16 of 16 probed bases found)`). Note the stamp lives in `Resources/pso/prebuilt.tsv` (`depth near=0`), not in the `.mtl4a`; replacing only the archive leaves it rejected.

## Open

- **60 fps does not take effect from a clean tree** until the rewriter can name UE 5.8's console variables — see [60 fps is blocked on the rewriter](#60-fps-is-blocked-on-the-rewriter). The profile's `fps_target 60` is the right key and needs no change once that lands. `present_interval display` was measured as a substitute and does not work; it is not in this profile.
- **A hitch during the boot video, and about once a second afterwards.** The log's `[longframe]` reports 200–750 ms frames whose `whole frame` is entirely outside render work: GPU 4–7 ms, `submit+present` 1–2 ms, 0 texture uploads, 0 new pipelines, and the process' own CPU over that frame is 20–30 ms. macOS' own `Animation Hitches` trace over the same run reports a longest frame of **28 ms**, and the host's per-frame rows mark the long frames `@presented_ms = -1` (discarded) — the guest is blocked waiting, and the frame is not shown.
- There is one measured correlation: **the fewer audio callbacks, the fewer hitches.** 840 device frames (57 callbacks a second) gave 43 frames over 150 ms in 40 s, 2048 frames (23/s) gave 22, 8192 frames (6/s) gave 13. The mechanism to check is the one the source shows: a released block ultimately reaches `hle_signal_system_event()`, which takes the global guest lock `G` — the same lock every guest SVC takes — so each audio callback is a chance to delay a guest thread. The corollary is that a large buffer fixes the hitch but underruns the audio, so the two cannot both be had this way; 17.5 ms is the shipped compromise (0 underruns, 43 hitches a minute).
- Excluded so far, each with a measurement: file IO (the ROMFS reads at 7.5 GB/s and the one 80 MB read does not line up with the hitches), lost wakeups (`DM_SYNC_POKE`), the real-time thread policy (`DM_RT=0`, `DM_RT_COMP`), window occlusion and App Nap (`DM_WINDOW_TOP=1`, unfocused, full screen), MetalFX snapshots (`DM_PRESENT_SNAP=0`), pipeline compilation (the recorded prewarm list changes nothing) and system load (the load average fell from 7.5 to 4.2 with the hitch count unchanged).
- **A hiss on short sounds** stays open: `hle_audren.c`'s `RS_T` (16 taps) has no switch, so a profile or an env file cannot raise it. See [Sound](#sound).
- **Not tested on an iPhone.** The audio session's I/O buffer duration and the display link behave differently there, and none of the Mac measurements above carry over unmeasured.
- The base-only 1.0.1 dump has its own build ID. Everything here is bound to 1.0.2's, so 1.0.1 needs a build row and a check of its own rather than this profile's tables.

## How this was found

The route is the one [docs/agents](../../../docs/agents) describes. The short version, for the next Unreal port:

1. **The NSP script's provisional profile boots**, then hangs. Do not chase the hang before the profile's own gaps are fixed: this one had `depth_mode` and `tex_dyn_slots` missing, which showed up as a rejected pipeline archive and as shaders that will not load.
2. **Read the engine's own error strings out of `main`** — they carry the source file and line (`RenderingThread.cpp` 1106 here). With the matching UE source in hand, `FRenderCommandFence`'s type and the watchdog's hard-coded thread name stop being guesswork.
3. **Console variables are UTF-16** and UE 5.8 moved the name to the constructor's third argument (`(this, Flags, Name, DefaultValue, Help)`), so `ue_cvar.py`'s "the name is in x1" found none of them. Without that fix nothing in `hle_fpslock.c` works, and `r.GTSyncType`, `rhi.SyncInterval` and `t.MaxFPS` are all unreachable.
4. **`DM_NVN_SHDUMP` turns "the picture is wrong" into a number**: dump the shaders the host cannot map, then hash them in both candidate ways until the counts agree.
5. **`DM_NVN_TSV` / `DM_SLOWFRAME` attribute a slow frame** (work, upload, present, GPU, and the process' CPU); `@presented_ms = -1` is what says "this frame never reached the screen", which is the difference between a slow renderer and a blocked guest.
