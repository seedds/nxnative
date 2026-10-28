# Metroid Prime 4: Beyond (`010019A01E2F2000`)

Update 1.1.0 (v131072) over the base game, SDK 19.3, Retro Studios' engine (little-endian RFRM forms), NVN. Modules `main`, `subsdk0` (the SDK's movie library) and `sdk`. RomFS is 445 files: `MaterialArchive.arc`, a `.pak` per world and room, `Preload/DuplicateData.pak` (3.8 GB) and `Patch/Z_198_NXPatch.pak.patch`.

Brought up on the Mac (M4 Pro): boot, the logo and title movies, the Chinese menus, difficulty select, the opening cinematic, the first area on the planet and its first cave, combat, the pause menu and the map. Long headless runs with scripted input: 20 min at 2560x1440 and 120 fps (about 119 fps on average), 15 min at 3840x2160 and 60 fps (about 59 fps), no crash. Not built for iOS.

## What it needs

| Setting | Where | Why |
|---|---|---|
| 4 GB layout as the provisional profile guessed (`mem_reserve`, `mem_alias`, `mem_stack`, `app_pool`, `sys_resource 0x0`) | [`game.tsv`](game.tsv) | the game sets a heap of at most the pool − 64 MB |
| `tex_flush_upload on`, `tex_dirty_upload on` | `game.tsv` | streamed textures: the CPU writes higher mip levels into a texture's storage after its first use; without the dirty-page uploads those levels read ASTC's error colour (magenta cave walls and arm cannon) |
| `movie_tex_autorelease on` | `game.tsv` | RetroFMV never releases a native-texture movie output ([movies.md](../../../docs/reference/movies.md#players-that-never-release-a-native-texture-output)) |
| [`dual_want.tsv`](dual_want.tsv) | profile table | the pixel shaders drawn with dual-source blending, each given a dual-source entry point ([rendering.md](../../../docs/reference/rendering.md#a-written-secondary-blend-source-nvn-dual-source-blending)) |

## Bring-up, in order

- **Shaders:** little-endian RFRM forms with zstd ([shaders.md](../../../docs/reference/shaders.md#little-endian-rfrm-forms-with-zstd-metroid-prime-4-beyond)): 259,989 shaders, 129,972 pairs. `MaterialArchive.arc` also holds an `NVN2` chunk (the Nintendo Switch 2 Edition's programs), not read.
- **Boot:** `fs::MountRom(name, index, cache, size)` and `QueryMountRomCacheSize(size, index)` (SDK 19 overloads); paths with backslashes (`rom:\StartupUI.pak`); the game masks the low 3 bits off a `FileHandle`, so the host's handles are 16-byte aligned; `nvnTextureBuilderGetSwizzle`.
- **Movies:** `movie_tex_autorelease` (above).
- **First 3D scene:** dual-source blending crashed Apple's compiler service ([rendering.md](../../../docs/reference/rendering.md#a-written-secondary-blend-source-nvn-dual-source-blending)); the scene was black because the volumetric fog is injected by vertex shaders that store to a 3D image under rasterizer discard, which the host skipped ([rendering.md](../../../docs/reference/rendering.md#rasterizer-discard-draws-whose-vertex-shader-stores-to-images)).
- **Textures:** the two texture keys (above).

## Pipeline archive

An offline pipeline archive (`pso_offline.py`) of all 129,972 bases made metal-tt grow past 79 GB of memory; build with `--gpu none`. The app's own prebuild of the bases at first start keeps the game from starting for a long time, so `DM_PSO_PREBUILD=0` (draws build their pipelines as they meet them) is the better choice for now.

## Render size and frame rate

1080p / 1440p / 4K with the dynamic resolution off, and 60 / 120 fps, work through [run-time patches](../../../docs/reference/rewriter.md#run-time-patches-chosen-at-launch) made from existing community patches for this build (for example the Metroid Prime 4 package in [Fl4sh9174/Switch-Emulator-Ultrawide-FPS-Mods](https://github.com/Fl4sh9174/Switch-Emulator-Ultrawide-FPS-Mods)); their values are not in this repository. At 120 fps the game's speed matched 60 fps (the same input from the same save reached the same place).

## Found on this game

- [File handles with low bits set, and backslash paths](../../../docs/reference/services.md#file-handles-with-low-bits-set-and-backslash-paths) (services).
- [Little-endian RFRM forms with zstd](../../../docs/reference/shaders.md#little-endian-rfrm-forms-with-zstd-metroid-prime-4-beyond) (shaders).
- [Players that never release a native-texture output](../../../docs/reference/movies.md#players-that-never-release-a-native-texture-output) (movies).
- [Rasterizer-discard draws whose vertex shader stores to images](../../../docs/reference/rendering.md#rasterizer-discard-draws-whose-vertex-shader-stores-to-images) (rendering).
- [Run-time patches chosen at launch](../../../docs/reference/rewriter.md#run-time-patches-chosen-at-launch) (rewriter).
