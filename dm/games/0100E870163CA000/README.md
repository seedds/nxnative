# Batman: Arkham Asylum (`0100E870163CA000`)

Base game 1.0.0 (v0), SDK 14.3.0. Rocksteady's Unreal Engine 3 (package version 579, licensee 21) on NVN, with FMOD for the game's sound (a six-channel AudioOut) and Bink for the movies (decoded in `main`, their sound through the audio renderer). Modules `main` and `sdk`. Plays at 60 fps (30 on the Switch) with sound, saves and controllers, on the Mac and on iOS.

Status: brought up on the Mac from boot through the language screen, the health warning, the title, the save select and the intro movie into gameplay in Intensive Treatment, saving and loading; the IPA was installed on the player's iOS device and plays with sound. No red line in a gameplay run.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `tex_flush_upload on`, `tex_dirty_upload on` | [`game.tsv`](game.tsv) | the engine writes its textures from the CPU after creating them and never flushes: without both the 3D scene is black; see [Rendering](#rendering) |
| `fps_target 60`, `present_interval display` | `game.tsv`, `hle_fpslock.c` | the host raises Unreal Engine 3's tick limit, and the window's present interval 2 is ignored; see [Frame rate](#frame-rate) |
| `fps_max 120` | `game.tsv` | the launchers' **Frame rate** offers 120 fps on the Mac, and **Uncapped**; both played on an M1 Max; see [Frame rate](#frame-rate) |
| `render_scale on` | `game.tsv`, `hle_fpslock.c` | the launchers' Render scale sets the engine's dynamic resolution |
| 4 GB layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000`; `sys_resource 0x0` | `game.tsv` | the NPDM declares no system resource and `main` sets a heap of the pool − 64 MB (3328 MB), as the NSP script guessed |
| `mem_used 0x4600000` | `game.tsv` | the rewritten modules are larger than the default 64 MB of used memory |

No `patches.py` and no `sdk_patches.py` cut: the rewriter needs nothing for either build. The profile ships no shader tables: the NSP script makes them from the game files in a profile derived from this one, beside the build (`derive_profile`), so every build runs the shader split again (20 s, the packages decoded by the game's own Oodle in the Mac host).

## Frame rate

- **Current:** 60 fps (30 on the Switch), frame time p50 16.7 / p95 17.2 ms in the menus and in gameplay on the Mac. The game steps by the time that really passed: 10.13 s of game time in 10.16 s of real time. How: [Unreal Engine 3's tick limit and a window built with interval 2](../../../docs/reference/frame-rate.md#unreal-engine-3s-tick-limit-and-a-window-built-with-interval-2). The Bink movies keep their own 30 fps.
- **120 fps and Uncapped** (`fps_max 120` in the profile, the Mac launchers' **Frame rate**): the same row writes the engine's `MaxSmoothedFrameRate`, 120 while frames fit and 60 when they do not, and Uncapped writes it to 1000 and free-runs presentation ([Uncapped](../../../docs/reference/frame-rate.md#uncapped-the-engines-limit-out-of-the-way-and-presentation-free-running)). The game steps by real time (`GUseFixedTimeStep` is 0), so its speed holds at 120. **Played on an M1 Max: 60, 120 and Uncapped all run smoothly.** Uncapped's run logs `frame rate uncapped`, `present tick 0 Hz`, the engine's limit at 1000 and the display link at 120 Hz, at 211–226 fps in the front end (frames 1200–3900); the frame times in gameplay at 120 and Uncapped were not recorded.
- The port's dynamic resolution budgets the GPU for 30 fps (85–100 % of the view, 33.3 ms); the host's rows set the budget for `fps_target` (16.2 ms at 60), and with `render_scale` `DM_RENDER_SCALE` fixes the minimum and the maximum alike. At 0.5 the scene draws at half the size and is scaled up (a shot of the first gameplay scene shows the blockier edges).
- The launchers' Console mode is the operation mode: the view is 1920 × 1080 docked and 1280 × 720 handheld (`SwitchApp::OnOperationModeChanged`, `main+0xe50b5c`).

## Rendering

- **Resolved: every lit surface of the first gameplay scene was black** (fog and a few lit objects over it; the menus and the movies drew). The base pass lights surfaces from vertex lightmaps and its uniforms were right; the engine's seven texture pools are filled by the CPU with no `nvnMemoryPoolFlushMappedRange`, so no texture was uploaded (`packaged textures: first uploads 0`). With `tex_flush_upload` and `tex_dirty_upload`, 845 textures are uploaded at their first use and the scene is lit and textured: [CPU writes with no flush, caught by page protection](../../../docs/reference/nvn-objects.md#cpu-writes-with-no-flush-caught-by-page-protection-tex_dirty_upload). `profile_guess.py` now sets both keys for any game with loose Unreal Engine 3 packages.

## Files and shaders

- No pak: the cooked packages are RomFS files of their own, `BmGame/CookedSwitch/*.xxx` (2,674), compressed in Oodle Leviathan chunks; `Textures.tfc` (805 MB, the `romfs_marker`) is the texture file cache; `BmGame/Movies` holds 75 Bink movies (3.0 GB). The RomFS is 7.3 GB.
- 41,809 shaders (11,654 vertex, 30,155 pixel) from the shader caches inside 574 of the packages and `GlobalShaderCache-Switch.bin`, read by `shader_split.py` (`ue3_shaders`) after the packages are decoded with the game's own `OodleLZ_Decompress` in the Mac host: [Unreal Engine 3 shader caches in Oodle-compressed packages](../../../docs/reference/shaders.md#unreal-engine-3-shader-caches-oodle-packages). Vertex and pixel shaders are paired as single-stage programs.
- 1,837 of the vertex shaders are written in two parts with a c1 block between them: [VertexA and VertexB parts merged into one vertex shader](../../../docs/reference/shaders.md#vertexa-and-vertexb-parts-merged-into-one-vertex-shader).
- The IPA is 8.3 GB with 114,373 entries, 111,517 of them shader library files (two or three per shader).

## Sound and movies

- Two outputs: FMOD opens a six-channel AudioOut (`main+0x6b350`), which the host mixes down to stereo; the host's audio renderer output carries the Bink movies' sound. On the Mac both play with no underrun; on iOS the app follows the device's silent mode, as every game does.
- **Resolved: the movies stalled 200–420 ms every eighth frame** (16 fps). Bink's sound thread updates the renderer every 2–13 ms while holding the lock its movie player needs, and every such update was a catch-up frame: [Updates within a frame of the last render](../../../docs/reference/audio.md#updates-within-a-frame-of-the-last-render). The opening movie now plays at 30 fps (p95 34–39 ms).
- `DM_AUDIO_DUMP` wrote both outputs into one file, which no reading could take apart; each output now has its own file: [Checking audio without listening](../../../docs/reference/audio.md#checking-audio-without-listening).

## Installing on iOS

- Sideloadly stopped with `PackageExtractionFailed (Could not extract archive)` from the device, with space to spare and an IPA whose every checksum, name and layout checked out. AltServer's Sideload .ipa, which unpacks and signs the app on the Mac and sends the device the app folder, installed it over a cable in about 15 minutes.

## Mac routes

- With a save from an earlier run, six presses of A through `DM_CTL` reach gameplay: the language screen (about 24 s after the start), the health warning, the title, the save slot, the notice and Continue Story, each a few seconds after the screen before it; Intensive Treatment is playable about 100 s after the start.

## Found on this game

General changes first needed here; each applies to every game:

- **Unreal Engine 3 shader caches read from Oodle-compressed packages** (`uepak.ue3_summary` / `ue3_image`, `shader_split.py` `ue3_shaders`, `profile_guess.py` `oodle_ue3`): [Unreal Engine 3 shader caches in Oodle-compressed packages](../../../docs/reference/shaders.md#unreal-engine-3-shader-caches-oodle-packages).
- **A c1 block between the VertexA and VertexB parts** (`dmsh_build.py` `vertex_ab`): [VertexA and VertexB parts merged into one vertex shader](../../../docs/reference/shaders.md#vertexa-and-vertexb-parts-merged-into-one-vertex-shader).
- **Both texture upload keys guessed for loose Unreal Engine 3 packages** (`profile_guess.py`): [CPU writes with no flush, caught by page protection](../../../docs/reference/nvn-objects.md#cpu-writes-with-no-flush-caught-by-page-protection-tex_dirty_upload).
- **No catch-up frame within a frame of the last render** (`hle_audren.c`): [Updates within a frame of the last render](../../../docs/reference/audio.md#updates-within-a-frame-of-the-last-render).
- **Engine rows for a tick limit, a dynamic resolution budget and its screen percentages** (`hle_fpslock.c`, the profile key `render_scale`): [Unreal Engine 3's tick limit and a window built with interval 2](../../../docs/reference/frame-rate.md#unreal-engine-3s-tick-limit-and-a-window-built-with-interval-2).
- **`DM_AUDIO_DUMP` per output** (`hle_audio.c`): [Checking audio without listening](../../../docs/reference/audio.md#checking-audio-without-listening).
- **A profile that ships no shader tables** gets them from the build, in a derived profile (`build_ipa_from_nsp.py`).

## Open

- Only the base game (1.0.0) was packed; no update is known.
- `pso_manifest.tsv` is not recorded (no prewarm list); shaders compile on first use.
- Not played far past Intensive Treatment, on the Mac or on iOS.
- Sideloadly's failure was not explained: whether the device rejects an archive with this many entries or something else.
