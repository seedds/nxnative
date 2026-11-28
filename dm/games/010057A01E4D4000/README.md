# TSUKIHIME A piece of blue glass moon (`010057A01E4D4000`)

Update 1.0.2 (v131072), Nintendo SDK 17.5.4, NVN. The Mac host and packaged app reached the English prologue on an Apple M6 Mac mini (Mac18,5, macOS 27.2.1).

## What it needs

- [The profile](game.tsv) loads `main`, `subsdk0` (GLSLC), `subsdk1` (NintendoSDK_movie) and `sdk`. The NPDM declares zero system resource and uses the standard 4 GB memory layout.
- The full split finds 76 shaders, 59 in the loose BNSH files under `shader/` and 17 in `main`. The [prewarm list](pso_manifest.tsv) covers the English prologue. The existing native-texture [movie slot handling](../../../docs/reference/movies.md#a-native-texture-movie-shows-a-picture-16-frames-ahead-then-goes-back) and [linear texture writes](../../../docs/reference/nvn-objects.md#writetexels-into-a-linear-texture-dropped-the-write) serve its 1080p videos.

## Found on this game

- **Resolved:** [A large NXCX texture archive stalls the shader split](../../../docs/reference/shaders.md#a-large-nxcx-texture-archive-stalls-the-shader-split).

## Mac validation

Build with `scripts/build_ipa_from_nsp.py <dump folder> -k <prod.keys> --platform mac-app` and open the resulting app. A fresh save starts in the prologue. Use A to advance when the narrative pauses.

The prologue rendered at 1920×1080 with a 16.67 ms median frame time in steady sections. Over frames 300 to 6300 of the standing run, resident memory went from 1829 to 1827 MiB and Metal allocations went from 1910 to 1914 MiB. The AudioRenderer output through the stereo device had zero underruns in its first 4000 callbacks.

## Open

- Later story routes, other languages, the base build and on-device iOS gameplay await testing.
- The unsigned iOS IPA built and passed archive validation with `--romfs separate`. It requires a one-time transfer of the 22.8 GiB game image.
