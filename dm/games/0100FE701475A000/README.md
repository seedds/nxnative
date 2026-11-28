# Teenage Mutant Ninja Turtles: Shredder's Revenge (`0100FE701475A000`)

Update 1.0.9 (v589824, title screen 1.0.0.342 - 8), SDK 13.3.2. Tribute Games' ParisEngine uses MonoGame on NVN, AudioRenderer for sound and NintendoSDK_movie for the opening MP4. The Mac route reaches local Story, Leonardo's character selection and Episode 1, Jaw-Breaking News! The game writes `GameSave.sav` as the new Story starts and loads it on a subsequent boot.

Standing still in Episode 1 on Apple M6 with macOS 27.2.1 and Xcode 27.2, frame time was 16.67 ms at p50 and 16.67–16.69 ms at p95. Memory settled near 3.0 GiB resident with 1.35 GiB of Metal allocations. AudioRenderer opened a real stereo output. The log recorded underruns during startup and none in the last 30 seconds.

The unsigned 828 MiB iPhone IPA built successfully and passed ZIP integrity verification.

## What it needs

- The `main`, `subsdk0` (NintendoSDK_movie), `subsdk1` and `sdk` modules. The last two are byte-identical in this ExeFS and are loaded in the surveyed order. The profile leaves out `subsdk2` (GLSLC), whose compiler is unused.
- The NPDM declares zero system resource. The game requests a 3.14 GB heap in the profile's 3.25 GB heap region.
- The two supplied DLC packages carry licenses. The packer extracts both add-on indices and includes them with the game.

## Shaders

88 NVN shaders (35 pixel, 53 vertex), all compiled with the shared translation chain for Mac and iPhone. Six MGFX v10 effects in `main` yield 64 shaders. Thirteen raw-DEFLATE `.zxnb` effects in `Content/Shaders` yield the other 24. The Mac pipeline prebuild uses the 53 generated shader pairs. [MGFX effects in MonoGame XNB files](../../../docs/reference/shaders.md#mgfx-effects-in-monogame-xnb-files)

## Mac route

Build the base NSP plus update with the NSP script's `--platform mac` and run its printed command. Set `DM_METAL_HUD=1` for the Metal HUD. From a fresh save, press A through the title, Story, New Game and difficulty, then hold Plus to skip How to Play. Press A through the character and palette selections and again when Leonardo shows OK. Hold Plus to skip the Story cutscene. Episode 1 starts at Channel 6 Studios.

## Found on this game

- **EOS initialization.** The generic unresolved-import stub reported success and the game dereferenced a null platform. The host now supplies `EOS_NotImplemented` for the unresolved import. [Online SDKs in NROs are never loaded](../../../docs/reference/services.md#online-sdks-in-nros-are-never-loaded)
- **EOS lobby calls in local character selection.** The game still called two lobby helpers after initialization failed. The calls at `main+0xa33e54` and `main+0xa33eac` pass through `main+0x75ae00` to query the lobby interface and dereference the null wrapper at `main+0x6d1b4c`. A function-matched patch omits those calls and leaves the local player state updates in the same routine. [Rewriter patch](patches.py)

## Open

- The intro plays with the existing `DrawTexture with blending on` diagnostic. The tested movie frames are visible. [NVN DrawTexture](../../../docs/reference/rendering.md#nvncommandbufferdrawtexture)
- Online play depends on the unavailable EOS SDK. Later episodes, local co-op, DLC modes, the base build, and iPhone gameplay are untested.
