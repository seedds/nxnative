# Tunic (`0100DA801624E000`)

Update 1.0.3 (v196608) over the base game, SDK 14.3.0. Unity 2020.3.36f1 IL2CPP with FMOD, on NVN; modules `main`, `subsdk0` (the movie library) and `sdk`. Plays on the Mac: boot, title, menus and gameplay with sound at 30 fps, the game's own rate. The app is 4.8 GB; the heap check sized its 1 GB pool.

## What it needs

| Setting | Where | Why |
|---|---|---|
| 4 GB layout: `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000`, `app_pool 0x40000000` | [`game.tsv`](game.tsv) | the NPDM declares 16 MB of system resource (virtual address memory), so the game uses the alias region; with no heap set in the first 5 s it reached +4.26 GB of the 4.38 GB alias region, and the 1 GB pool leaves StandardAllocator room for its own regions |
| `sys_resource 0x1000000` | [`game.tsv`](game.tsv) | the NPDM's 16 MB system resource |
| `depth_mode zero` | [`game.tsv`](game.tsv) | the game calls `nvnDeviceSetDepthMode` with near = 0, so the prebuilt pipeline bases take the `main0_z` vertex entries |
| the frame rate | none | the game presents at 30 fps and its logic follows that interval; see [Frame rate](#frame-rate) |

## Frame rate

- **30 fps, the game's own rate.** The game presents at interval 2 and its logic counts frames. `DM_PRESENT_INTERVAL=display` presents at 60 but runs the game twice as fast; there is no frame-step patch for this build, so the default is the supported rate.
- **Handheld mode** is the lower-load mode: the game asks for a lower performance configuration. It used to crash in the SDK's `nn::apm` call; the host now answers the one-argument `oe::SetPerformanceConfiguration` overload ([Handheld mode asks apm for the performance configuration](../../../docs/reference/services.md#handheld-mode-asks-apm-for-the-performance-configuration)). The picture is the game's own handheld resolution.
- On the Mac, about **2.0–2.3 GB resident**, 30 fps, with occasional dropped presents (0–10 in 300 frames in the recorded session); GPU work is small (about 4–5 ms a frame in the opening area).

## Rendering

- Unity 2020.3.36f1 IL2CPP on NVN; 1920x1080 RGBA16F forward scene. No red lines in the Mac logs.
- The game uses bindless textures (`TBDR optimizations off` is expected) and a large 204 MB pipeline archive was built for `applegpu_g13g`.
- CPU-written textures are taken by the page-protection path; no `tex_flush_upload` or `tex_dirty_upload` key was needed in the tested route.

## Files and shaders

- RomFS: 2.75 GB, 10,553 files; the largest file is `Data/resources.assets.resS`.
- Shaders: **35,753** — 17,845 vertex, 17,849 pixel, 3 geometry and 56 compute. The Unity Shader objects and GLSLC outputs are read from the UnityFS bundles and `data.unity3d` by `shader_split.py`; the profile's tables are in this folder.
- The prewarm seed is recorded from the Mac session: 1,565 rows. The first run's prewarm prepared 799 unique lines (389 shaders, 410 render pipelines) in 1.79 s; the seed makes a cold app start do that work before the first frame.
- `shader_facts_manual.tsv` is not needed for the tested route; `shader_map.tsv.conflicts.tsv` is empty.

## Mac routes

- Open the app, then the title; continue from the existing save.
- Handheld mode is the same route with **Operation Mode → Handheld** in the launcher, or `DM_OPMODE=0` in the support `env.txt`.

## Found on this game

- **Handheld mode's one-argument performance configuration:** [Handheld mode asks apm for the performance configuration](../../../docs/reference/services.md#handheld-mode-asks-apm-for-the-performance-configuration). The game calls `nn::oe::SetPerformanceConfiguration(int)` when it switches to handheld; only the two-argument overload was hooked, so the SDK reached `nn::apm::SetPerformanceConfiguration` with a null apm manager and crashed at `sdk+0x1c4fac`. The missing hook is in `hle_oe.c`.
- **Provisional profile promoted after the heap check:** the 1 GB pool and `depth_mode zero` were set by `build_ipa_from_nsp.py`; the profile's shader tables and prewarm seed came from the same Mac build.

## Open

- The provisional profile was validated on the opening and the user's current save, not the whole game. Later areas, every menu, and all handheld/console transitions are not measured.
- No frame-step patch: the game stays at its native 30 fps. A 60 fps patch would need its frame step found; `DM_PRESENT_INTERVAL=display` is **not** a substitute (it doubles the game speed).
- The prewarm seed is route-local (title, menus, first gameplay session); it does not cover the whole game.
- iPhone: not built and not tested.
