# Stray (`010075101EF84000`)

Switch update 1.6.0 (v131072), Unreal Engine 4.27.2, Nintendo SDK 17.5.6, NVN, and Bink movies. The game loads `main`, `subsdk0` (Nintendo's movie module) and `sdk`.

## Mac validation

On Apple M6 with Xcode 27.2, two fresh saves reached brightness calibration, save selection and the opening cutscene at the game's 30 fps. The second run verified the first playable scene and pause and resume. Both runs used the iPhone's 4.375 GiB alias layout and logged no crashes or red lines. The opening reached 2,505 MB resident and 1,198 MB allocated by Metal. Audio output ran without underruns on the logged device callbacks. Play beyond the opening scene and playback on a physical iPhone remain untested.

The same transition into the opening cutscene took a 2.57 s frame without the prewarm seed and a 0.31 s frame after loading all 154 rows of the seed on a second run with a fresh pipeline archive. Prewarm took 1.04 s on that run. Both runs kept a median frame time of about 33.4 ms during the opening.

The packaged Mac app opened with its bundled Metal pipeline archive on Apple M6. The unsigned iOS IPA passed archive integrity and bundle metadata checks.

## What it needs

- Near-zero NVN depth, as observed at boot. The profile selects the corresponding Metal vertex entry point for pipeline prebuilds.
- The full touch pad layout. `Hk_project/Config/DefaultInput.ini` maps the D-pad to menus and game actions and the right stick to camera movement.
- The shared Unreal rules locate FMallocBinned3 by its implementation and give its allocations a host malloc zone. The engine asks for a 62 GiB address reservation. On the iPhone layout the shared rule gives it 4.06 GiB, and the C library fits its startup heap in the remaining profile layout. The existing SDK rules match the node area and large-region alignment by function.
- The [prewarm seed](pso_manifest.tsv) covers 133 render and 21 compute pipelines observed from boot to the end of the opening cutscene.

## Frame rate

The default target is 60 fps. The Switch config enables Unreal's fixed simulation rate at 30 fps and sets `rhi.SyncInterval=2`. The rewriter finds the engine's fixed-rate field through Unreal's reflection data and the sync interval by its console variable name. The shared limiter writes 60 to the fixed rate and 1 to the interval. Choose 30 fps in the launcher or set `DM_FPS_TARGET=30` to use the stock rate.

On Apple M6 with the iPhone address layout, the stock opening measured 29.8–30.0 fps (33.3–33.5 ms median frame time). At 60 fps the same part measured 59.4–59.5 fps (16.8 ms median). The opening camera passed the same landmarks at the same pace after accounting for the later load in the stock run. The shared matcher finds both controls in the 1.0.0 and 1.6.0 executables. Gameplay checks used 1.6.0.

## Files and shaders

`Hk_project/Content/Paks/Hk_project-Switch.pak` is a version 11 pak with an unencrypted index, 57,176 entries, and stored, Oodle and Zlib data. The global cache is `Engine/GlobalShaderCache-SF_GLSL_SWITCH.bin`. Material shaders are inline in the cooked packages. The existing split reads them using the game's Oodle decoder and also scans `main`.

The split found 10,792 unique shaders (2,314 vertex, 7,799 fragment, 676 compute and 3 geometry). All translated.
