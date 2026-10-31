# Portal (`01007BB017812000`)

Update 1.0.3 (`v196608`) over the base game, SDK 13.3.0. Valve's Source engine as NVIDIA Lightspeed Studios ported it to the Switch: `main` is a 0.5 MB launcher with SDL, and the engine is 28 NRO modules (`nro/*.nro` in the RomFS, registered by `.nrr/launcher_main.nrr`) loaded with `nn::ro`. It renders with NVN, its D3D9 layer linked into `materialsystem.nro`. Modules `main`, `subsdk0` (GLSLC), `subsdk2` (NintendoSDK_movie) and `sdk`; `subsdk1` is NVIDIA's OpenGL driver and is never loaded.

**Current:** plays on the Mac at 60 fps: the opening movie, the title menu (its 3D background map), New Game, test chamber 00 with its portals (the view through them included), the pause menu and a saved game, with sound; no red line. About 1.5 GB footprint in the chamber. The unsigned IPA is built (2,823,721,518 bytes, about 2.6 GB); not yet tested on an iPhone.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `depth_mode zero` | `game.tsv` | the game selects near-zero NVN depth; offline pipeline bases must use the matching vertex entry |
| `modules main subsdk0 subsdk2 sdk` | [`game.tsv`](game.tsv) | `main` imports from no subsdk; the NROs do: `video_nx.nro` plays the movies through the movie module and both it and `materialsystem.nro` call GLSLC ([Modules only the NROs import from](../../../docs/reference/services.md#modules-only-the-nros-import-from)) |
| 4 GB layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000`, `sys_resource 0x0` | `game.tsv` | no system resource, so no virtual address memory; the game sets a heap in the 4 GB layout. The rewritten modules fit the default 64 MB of used memory |
| `pad_layout full` | `game.tsv` | first-person: move with the left stick, look with the right |
| `shader_groups /game.zip/=game` | `game.tsv` | the shipped programs come from `game.zip`; the run-time captures go to `other` |

## Rendering

- **Resolved: offline depth metadata.** Update 1.0.3 selects near-zero NVN depth in `regress/runs/20261028-172622-candidate-full-1/log.txt` (through frame 5,400), with no later GL switch. A short isolated boot reproduced the call with matching registered module IDs; evidence is in `01007BB017812000-work/depth-mode-audit/boot-20261031/` beside the NSPs. The profile now declares `depth_mode zero` for the [shared archive mechanism](../../../docs/reference/shaders.md#offline-pipeline-archive-and-profile-depth-disagree). This confirms the input; rebuilt archive acceptance and performance were not retested.
- **Resolved: the NSP script took it for an OpenGL game.** `main` imports 20 EGL functions and names `glShaderSource` (SDL's EGL backend and GL renderer), but draws nothing; `materialsystem.nro` and `video_nx.nro` import `nvnBootstrapLoader`. The first capture boot stopped with `[nvn-mtl] FATAL cannot read the shader map` from an NVN submit in `nro8` ([NVN in NROs under an SDL main that names GL](../../../docs/reference/rendering.md#nvn-in-nros-under-an-sdl-main-that-names-gl)).
- About 140 draws a frame at the title and in chamber 00; GPU p50 about 6 ms on an M4 Pro, NVN submit and present under 3 ms.

## Shaders

- The D3D9 layer links each Source shader combination (`hl2/shaders/fxc/*.vcs`, D3D9 bytecode) into one program and keeps the GLSLC output of every program in the game's precache list (`portal/glbaseshaders.cfg`, 219 entries) in `nxcontent/nvsb/<hash>.nvsb`: a 0x60-byte `NVSB` header (version 0x65, the file's length at +8), then one GLSLC output holding a vertex and a pixel shader. The game compiles the rest with `glslcCompile`: within 30 s of boot, 10 outputs (the movie's and the engine's own).
- **Resolved:** the split found none of the 219: they are deflated members of `game.zip` ([GLSLC outputs in a zip archive, and those NROs compile at run time](../../../docs/reference/shaders.md#glslc-outputs-in-a-zip-archive-and-those-nros-compile-at-run-time)). The map has 255 keys, 165 pixel and 90 vertex, 242 from the `.nvsb` files and 13 from the capture, with 229 vertex and pixel pairs.
- The game writes the programs it has linked to its save, `portal/glshaders/glshaders.cfg` (`-glshadersSaveFS` in its boot parameters): 219 entries after chamber 00, the precache list. More entries there mean programs compiled at run time on that route, which a fresh capture boot does not reach; on the iPhone each would give `[RED] shader … is not in the shader map`.

## Files

- The whole game is `game.zip` (2.7 GB, 41,416 members, Deflate or stored) beside the NROs; the Source file system reads it itself. The profile's `romfs_marker` is `game.zip`.
- The boot parameters are `nxcontent/rom_boot_params.txt` in the zip (`-game portal -glshadersSaveFS -nodev -nosteam +mat_queue_mode 2 +mat_picmip 0`); host arguments after `--` (a `+map` among them) change nothing.
- Only the merged update is registered. The base game's build has no profile row.

## Movies

- The opening movie (1280x720 H.264 with AAC) plays through the host's movie decoder from `video_nx.nro`, which shows the frames with NVN and plays the sound through its own `AudioOut`.

## Mac routes

- Run the command the NSP script prints for `--platform mac` (`DM_NVN_SHMAP` names this profile's `shader_map.tsv`), with `DM_CTL=<file>` to drive it ([05-debugging.md](../../../docs/agents/05-debugging.md)).
- From a fresh save: the title menu at about 25 s; `A` (New Game), `A` (Chapter 1) loads test chamber 00 in about 15 s. The portal opens in the vault after GLaDOS's first lines (about 30 s); the right stick turns, the left stick walks through it. `Plus` opens the pause menu; `Down`, `A`, `A` saves (`portal/save/*.sav` in the save).

## Found on this game

- **Offline depth metadata:** [Offline pipeline archive and profile depth disagree](../../../docs/reference/shaders.md#offline-pipeline-archive-and-profile-depth-disagree).
- [Modules only the NROs import from](../../../docs/reference/services.md#modules-only-the-nros-import-from): the provisional profile adds the subsdks the NROs need.
- [NVN in NROs under an SDL main that names GL](../../../docs/reference/rendering.md#nvn-in-nros-under-an-sdl-main-that-names-gl): the graphics API follows the NROs' imports.
- [GLSLC outputs in a zip archive, and those NROs compile at run time](../../../docs/reference/shaders.md#glslc-outputs-in-a-zip-archive-and-those-nros-compile-at-run-time): the split reads zip members; the run-time capture also runs for NROs that call `glslcCompile`.
- [`ro::LookupModuleSymbol` for a name the module does not define](../../../docs/reference/services.md#rolookupmodulesymbol-for-a-name-the-module-does-not-define): the engine's `BuiltDebug` probe is answered as on Horizon, without a red line.

## Open

- iPhone test of the IPA.
- Later chambers (energy balls, turrets, water, the escape) are untested; watch `glshaders.cfg` and the log for programs outside the shipped 219.
