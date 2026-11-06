# Nintendo Switch 2 Welcome Tour (`040057C02159A000`)

A Switch 2 game: update v262144 over the base game, SDK 20.5.29 built for the Switch 2 (`Ounce`), NVN2. NintendoWare's Bezel Engine, modules `main`, `subsdk0`, `subsdk1` (movie), `subsdk3` and `sdk`. **In bring-up; plays on the Mac** (M1 Pro, macOS 27.2): past its intro video to the tour, its menus and exhibits, at about 60 fps (frame time p50 16.7 ms, p95 20 ms over 300 frames, 9 minutes into a session). No IPA has been built.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `mem_alias 0x400000000`, `mem_reserve 0x550000000`, `mem_stack 0x10000000`, `app_pool 0x240000000` | [`game.tsv`](game.tsv) | the Switch 2's 9 GB, as Splatoon Raiders and Star Fox: with `profile_guess.py`'s 5 GB block (a 4.4 GB alias region and a 1 GB pool) Bezel's allocator found no room at boot ([Bezel Engine's two memory setups](../../../docs/reference/memory.md#bezel-engines-two-memory-setups)) |
| `sys_resource 0x1000000` | `game.tsv` | the NPDM declares 16 MB, so virtual address memory is on |
| `modules` without `subsdk2`, `subsdk4` | `game.tsv` | C / libc modules main takes nothing from that the host does not supply |

## Files

- Built from a dump folder (`base/`, `updates/262144/`). The display version `v262144` is a placeholder: no control data gave a better one.
- RomFS: 722 files, 1.49 GB, most of it Bezel Engine archives (`Archive/*.bea`).
- Shaders: 6,758 in [`shader_map.tsv`](shader_map.tsv). On chain `223f10d34f97`, 23 fail to translate (current): Ampere instructions `0x3ad` and `0x80d` are not implemented, and one `AST` with an offset register and an address. In 8 more shaders the translator deleted 424 bindless accesses (their results read 0). 6,102 pipeline pairs prebuild.

## Controllers

The exhibits use the Joy-Con 2's features: the mouse sensor, the Joy-Con 2 npad styles and HD rumble. With the Switch 2 controller pull requests of [issue #1267](https://github.com/seedds/nxnative/issues/1267) (not on `main` yet), a left and a right Joy-Con 2 over Bluetooth LE moved the cursor on a desk, clicked with R, played the mouse exhibits and the bead and rubber-ball rumble exhibits. Without them the game runs, but the cursor does not move: the host answers that there is no sensor ([The newest Joy-Con mouse sensor state and a Nintendo application's scene id](../../../docs/reference/services.md#the-newest-joy-con-mouse-sensor-state-and-a-nintendo-applications-scene-id)).

## Open

- Ray tracing: `nvnQueueBuilderSetRayTracingMemorySize`, `nvnProgramBuilderSetRayTracingModules`, `nvnProgramBuilderSetProgramRayTracingFlags`, `nvnProgramBuilderSetMaxTraceRecursionDepth`, `nvnProgramBuilderSetMaxCallableRecursionDepth`, `nvnProgramGetShaderGroupHandle` and `nvnDeviceCalculateAccelerationStructureMemoryRequirements` are unimplemented (`[RED]`, once each at boot), as is `nvnDeviceRegisterFastClearStencil`. What the game draws with them is not known.
- `[RED] nn::codec H.264: a packet refused with 12 pictures waiting for a released buffer`, a few times in the videos.
- `[RED] stage 0 cb7 has no uniform buffer bound`, once.
- An IPA: the 16 GB alias region is over iOS's limit.

## Found on this game

- [`vi::InitializeWithVrrLfcEnabled`](../../../docs/reference/services.md#viinitializewithvrrlfcenabled): the first crash at boot.
- [The newest Joy-Con mouse sensor state and a Nintendo application's scene id](../../../docs/reference/services.md#the-newest-joy-con-mouse-sensor-state-and-a-nintendo-applications-scene-id): a null read each frame, and as each scene module starts.
- [The application's GPU time span](../../../docs/reference/services.md#the-applications-gpu-time-span): a null read after the first frame.
- [Bezel Engine's two memory setups](../../../docs/reference/memory.md#bezel-engines-two-memory-setups): the virtual-memory path, with the Switch 2 layout.
