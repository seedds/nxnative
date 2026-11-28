# DRAGON BALL FighterZ (`0100A250097F0000`)

Switch update 01.50 (v1703936), Unreal Engine 4.19.1, Nintendo SDK 4.5.0, NVN 53.313. The game loads `main`, `subsdk0` (Nintendo's movie module) and `sdk`.

## Mac validation

On Apple M6 with Xcode 27.2, the game reached the title screen, played the opening movie and entered the offline lobby. From the lobby, use ZL to warp to Practice, speak to the attendant and choose **Begin Battle Tutorial** → **Training Begins!**. The first fight drew Goku and Android 16, resumed from pause and accepted D-pad movement. The two runs used the 4 GiB iPhone guest address layout and logged no red lines, crashes or audio underruns. The title, lobby and tutorial ran close to the game's stock 60 fps, with median frame times around 16.8 ms. In the opening fight the host reported about 3.1 GB resident and 1.4 GB allocated by Metal. Other battle modes, online play and physical iPhone playback remain untested.

The packaged Mac app passed signature verification and reached the title using its bundled Metal archive on Apple M6. The host prewarmed 152 render pipelines and three compute pipelines. The unsigned iPhone IPA passed archive integrity and bundle metadata checks.

An earlier provisional build showed wrong or black textures on an iPhone with an A19 Pro. Those scenes still need a retest on update 01.50. [iPhone texture report #1432](https://github.com/seedds/nxnative/issues/1432)

## What it needs

- The 4 GiB guest layout gives the game a 3.25 GiB heap region. It opens about 3.14 GiB at startup. The NPDM declares no system resource.
- The game selects near-zero depth and an upper-left window origin. The profile uses the near-zero vertex entry point for offline pipeline builds.
- The full touch pad layout exposes the D-pad that the offline menus and battle tutorial use.
- The [prewarm seed](pso_manifest.tsv) records pipelines used from startup through the first battle tutorial.

## Files and shaders

The merged RomFS holds two version 4 Unreal paks with unencrypted indexes. Their 359,106 entries are stored or Zlib-compressed. The existing shader reader finds global shaders in `Engine/GlobalShaderCache-GLSL_SWITCH.bin` and material shaders inline in cooked packages. It found 12,545 unique shaders, and all translated on the Mac.

## Found on this game

- The [jump solver tracks a 16-bit masked index](../../../docs/reference/rewriter.md#a-guard-on-a-copy-masked-wider-than-12-bits). The shared rewriter handles this build without game-specific cuts.
- The [audio renderer owns its event](../../../docs/reference/audio.md#the-renderer-event-belongs-to-the-renderer-not-the-game). The SDK 4 renderer starts in the Mac host and its stereo 32 kHz output is resampled to 48 kHz.
