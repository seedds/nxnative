# Batman: Arkham City (`01003F00163CE000`)

Update 1.0.2 (`v131072`) over a 13 MB base stub, SDK 14.3.0. Rocksteady's Unreal Engine 3 (package version 864,
licensee 0x80e3) on NVN, with Wwise for the sound, CRI Sofdec `.usm` movies and Scaleform for the menus. Modules
`main` (`8983c5a34b178e5cbdb188902308ad50`), `sdk` (`2aef2cffb104ee7c9e212fd2f4c731e9`, the same SDK build as
Batman: Arkham Knight) and `rtld` (`8e405c8a464734d8f71c2c1be2dac518`). 12.34 GB RomFS, 2,835 files.

Status: brought up on the Mac from boot through the logos, the menus, the Catwoman prologue cutscene and into
gameplay, at 30, 60 and 120 fps, with saves. The Mac app and the IPA were both built from this tree and their
payloads checked; the IPA has not been run on a device.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `fps_target 60`, `fps_max 120`, `present_interval display`, `present_paced on` | [`game.tsv`](game.tsv) | the engine has **no frame limit of its own** (632–746 fps uncapped), so the rate is present pacing; `present_paced` lets the host offer 120 with no limiter to write (see [Frame rate](#frame-rate)) |
| `depth_mode zero` | `game.tsv` | the game calls `nvnDeviceSetDepthMode` with near = 0; the host logged "depth mode: near=0, vertex shaders use the main0_z entry" at boot |
| `shader_groups /shdump/=capture` | `game.tsv` | every shader in the map is a run-time dump of this build, not a file the split can read (see [Shaders](#shaders-a-run-time-capture-not-a-split)) |
| 4 GB layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000`; `sys_resource 0x0` | `game.tsv` | `main` sets a heap of the pool − 64 MB (3328 MB); the NPDM declares no system resource, so `IsVirtualAddressMemoryEnabled` is false, as on Horizon. The first boot ran to frame 900 at this block with the host heap unmoved |

No `patches.py` cut: the rewriter needs nothing for this build.

## Shaders: a run-time capture, not a split

**The port creates its NVN programs at load time, so no game file carries a finished one.** Measured on this build:
`GlobalShaderCache-Switch.bin` (BMSG, 558,785 B) holds **0** control sections (`0x98761234`) and **0** code areas
(`0x12345678`) — Arkham Asylum's holds 179 of each — and the RomFS carries `PersistentCookerShaderData.bin`
(169,875 B), the cooked shader data a port builds its programs from. A scan of the whole 12.3 GB RomFS finds no
`CTL_MAGIC`/`GFX_MAGIC` either. The key the host hashes *is* the control section, so a key for this title exists only
while the game runs.

The route is therefore a capture: run the Mac host with `DM_NVN_SHDUMP=<dir>` and it writes `<key>.ctl` (the control
section) and `<key>.code` (the code area) for every program it is handed; `shader_split.py` reads the folder back as a
source (`dumped_shaders`), refuses any pair whose bytes do not hash to its own file name, and files the rows under
`/shdump/<key>#RAW@0x0`. The map's header then says `# kind: capture` instead of `# kind: evidence`.

This commit carries **no shader data**: the profile, the note and the code that reads a capture. A build from it stops
until the pairs exist — `DM_NVN_SHDUMP` at run time, or `dm/games/01003F00163CE000/shdump/`.

## Frame rate

- **The engine has no limiter.** Uncapped (`DM_PRESENT_HZ=0 DM_FPS_TARGET=0`) this build runs **632–746 fps**, so the
  Switch's present interval 2 (30 fps) is the only thing holding it back on the console. The profile therefore paces at
  the present tick: `present_interval display` for one frame per display tick, and `present_paced` so that a 120 target
  needs no engine-limit write (`fps_high` accepts it).
- **Measured, each from its own headless run log:** 30 fps (the game's own interval 2), 60.0 fps, and 120.0 fps with
  `present tick 120 Hz`. The packed bundle itself ran 5,400 frames at 120.0 fps over 60 s with 0 `[RED]`.
- **Not measured:** that game logic is speed-invariant at 120 against 60 over the same inputs. The engine steps by the
  time that passed and runs uncapped at 700 fps, so it is expected to be fine — reasoning, not a measurement.

## Rendering

- The scene draws with the capture-derived library; every route played (boot, logos, menus, Catwoman prologue into
  gameplay) ended with **0 missing shaders and 0 `[RED]`** lines.
- Scenes the capture route never visited can ask for keys the map does not hold. That is the honesty limit of this
  title: extend it by playing further with `DM_NVN_SHDUMP` set, then rebuild.

## Sound and movies

Wwise on an `AudioOut`, CRI Sofdec `.usm` movies decoded in `main`; both play on the Mac. `zip_store` keeps
`.usm`/`.wad` stored rather than compressed in the app.

## Still open

- **The IPA has not been run on a device** (no device attached to the Mac that built it): built and payload-verified
  only.
- **The Mac app has not been launched windowed** in this build; it was accepted headless through its own payload.
- **Shader coverage is capture-derived.** The first route reached 362 keys; the capture in this branch's second commit
  holds 1,300. Anything past that needs more playtime, not a different reader.

## Found on this game

General changes first needed here; each applies to every game:

- **Run-time shader dumps as a split source** (`shader_split.py` `dumped_shaders`, the `shdump/` folder, the map's
  `kind: capture`), and the dump folder joining the split's reuse key (`build_ipa_from_nsp.py`) so a new capture
  invalidates a cached split instead of being ignored.
- **A dump keeps the program parts past the key bytes** (`hle_nvn.c` `nvn_ctl_keep`, `nvn_obj.h`, `nvn_mtl.m`): this
  game's VTG vertex programs put VertexB at +0x890 and further parts at +0xa30 / +0xb50, past the 0x880 bytes every
  section names at +0x20. The key does not hash them; the translator needs them.
- **VertexA/VertexB where a constant pool sits between the parts** (`dmsh_build.py` `vertex_ab`): part B is identified by
  its own SPH (type 1) and bounds instead of by `control + 0x700 + 0x30`.
- **`present_paced`** (`game.py`, `hle_nvn.c` `fps_high` and `fstep_tick`): a game whose engine steps by real time and
  has no limiter of its own needs presentation alone, at 120 as well as 60, and its holds stay at the cap's own vsyncs.
- **`fps_max 30`** (`game.py`, `settings.tsv`): a title whose own pacing holds 30 and no more takes the launcher's
  **Frame rate** choice away instead of offering rates it cannot reach.
- **Launcher rows may join several conditions with `&`** (`settings.py`): the **Frame rate** row is emitted per
  `DM_GAME_FPS_MAX`, so 30 and 120 can each have their own condition.
- **120 fps on the iPhone** (`settings.tsv`): the IPA sets `CADisableMinimumFrameDurationOnPhone`, so a ProMotion phone
  really runs the 120 choice.
