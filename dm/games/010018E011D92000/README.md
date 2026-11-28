# Pokémon Shining Pearl (`010018E011D92000`)

Update 1.3.0 (`v393216`) over the base game. Unity 2019.4.27f1 IL2CPP, Nintendo SDK 11.4.3, NVN 55.15, Wwise and FMOD. The SDK and `NintendoSDK_movie` module builds match [Pokémon Brilliant Diamond](../0100000011D90000/README.md).

## What it needs

- The NPDM declares 16 MiB of system resource. In the boot check, `nninitStartup` passed the Switch application pool less memory already in use to `StandardAllocator` through virtual address memory. With the [shared SDK address-region rules](../../../docs/reference/memory.md#the-sdks-address-region-node-area), the allocator reserved 3.14 GiB starting 0.06 GiB into the alias region. Live regions reached 3.30 GiB into the 4.375 GiB alias region in the first 15 seconds in `nnMain`.
- Unity calls `nvnDeviceSetDepthMode` with near = 0 on startup. `depth_mode zero` gives prebuilt Metal pipelines the game's depth convention.
- The shader split found 14,441 stage shaders (7,247 vertex, 7,173 pixel, 20 compute and one geometry). `main` contains 11 GLSLC outputs. The [built-in Unity compute shaders](../../../docs/reference/shaders.md#unity-computeshader-objects) supply 16 skinning kernels and three blend-shape kernels.
- The profile has no game-specific code cuts.

## Frame rate

The profile requests 60 fps through Unity's shared frame limiter. The rewriter found `Application.targetFrameRate` and `QualitySettings.vSyncCount` from Unity's named internal calls at different addresses in the 1.0.0 and 1.3.0 binaries. The host uses those fields and its existing engine-aware present cap with the default `present_interval game`. `DM_FPS_TARGET=0` restores the game's original timing. The launcher's Frame rate setting also offers 30 fps. [Unity's integer target frame rate](../../../docs/reference/frame-rate.md#unitys-integer-target-frame-rate)

On an Apple M6 Mac, separate 1.3.0 runs loaded the same Twinleaf Town save. Over 600 standing frames in the bedroom, the stock setting averaged 29.95 fps (33.34 ms median), and the new default averaged 60.00 fps (16.66 ms median). The host's CPU power estimate rose from about 0.35 W to 1.06 W. The 60 fps run opened the in-game menu and wrote a manual save. One audio output logged 15 underruns during the first 60 fps startup (75 ms total). Neither output reported underruns afterward or during the repeat startup.

## Mac testing

Run `scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> --platform mac` and use the command it prints. Set `DM_METAL_HUD=1` for the performance HUD. Logs and saves live under `<NSP folder>/010018E011D92000_build/`.

On 2026-11-27, an Apple M6 Mac running macOS 27.2.1 with Xcode 27.2 (27B52) built every shader and reached the playable bedroom in Twinleaf Town from a fresh save. Language selection, Rowan's introduction, character and name selection, the in-game menu and a manual save passed. The room held about 30 fps at the game's original interval. With the iPhone-sized alias region, the mapped high-water mark was 3.25 GiB. In the room, resident memory settled around 2,470–2,495 MB with about 1,003 MB allocated by Metal. Both audio outputs reported zero underruns.

The manual save wrote a 979,108-byte `SaveData.bin`. A second launch from a copy of that save played the VP8 startup movies and loaded back into the same room.

The first draw waited about 22.5 seconds for pipelines in a fresh cache. Opening the menu once logged a 133 ms Metal residency commit and a brief hitch, then frame pacing recovered.

## iOS package

The standard packer produced `PokemonShiningPearl.ipa` (4.33 GiB) with the 1.3.0 RomFS and all 14,441 stage shaders. The archive CRCs and the launcher's 60 fps default were checked. The package is ready for sideloading.

## Open

- Gameplay beyond the first room and on-device iOS testing remain open.
- The Switch community flags physics at higher frame rates for 1.3.0. Movement and collision at 60 fps remain unverified on the Mac. [FPSLocker Warehouse](https://github.com/masagrator/FPSLocker-Warehouse/blob/v4/README.md?plain=1#L858)
