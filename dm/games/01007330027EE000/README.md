# ULTRA STREET FIGHTER II: The Final Challengers (`01007330027EE000`)

Version 1.1.1 (v131072), Capcom's MT Framework on NVN, SDK 1.3.1. `main` loads beside `subsdk0` (movie), `subsdk1` (libcurl), `subsdk2` (NVN), `subsdk3` (`nn::sf`) and `sdk`.

## What it needs

| Setting | Why |
|---|---|
| 4 GB reservation with a 3,328 MB heap region | The NPDM declares zero system resource. The game's heap is bounded by the application pool less 64 MB. |
| `depth_mode zero` | The game sets `nvnDeviceSetDepthMode` to near = 0. The offline pipeline archive uses the `main0_z` vertex entries. |

The profile uses the host's existing SDK 1 paths for graphics, audio and movies. The rewriter applies no game cuts.

## Files and shaders

- RomFS has 922 files, including 564 ARC v7 archives, Opus audio and `nativeNXx64/app/movie/advertise.mp4` (H.264 and AAC).
- Six headed SPKs hold 337 shader records and 246 unique shaders (54 vertex, 192 pixel). All 246 compiled for Metal on the Mac. Each vertex shader is paired with the two-pixel-shader cover for the stage inputs. See [SPK shader package in an MT Framework ARC](../../../docs/reference/shaders.md#spk-shader-package-in-an-mt-framework-arc).

## On the Mac

- Build with the NSP script and `--platform mac` to get the run command, or use `--platform mac-app` for a Finder app. On a fresh save, press A at the save prompt. After the movie, press Plus at the title, then A to choose Arcade, confirm its settings, select a fighter and confirm their color.
- On an Apple M6 with macOS 27.2.1 (2026-11-23), the registered profile reached Ryu vs. Dee Jay in Arcade. The first round ran at 59–60 fps with frame time p95 about 17.4 ms. The host's footprint held around 1.5 GiB through the end of the run, and audio output reported zero underruns. The 150-second run ended at the test alarm.
- The Finder app passed signature verification, booted and loaded its bundled Metal pipeline archive.
- The game requests an I3DL2 reverb effect. The SDK 1 audio service currently passes the dry signal through that effect. The optional `nvnDeviceWaitForError` lookup also logs a warning at startup.
- The opening movie decoded and reached the title. At the end of one run, a packet sent after a decoder flush logged that its AVC parameter sets were missing. The game continued into its attract demo.

Training, local and online multiplayer, and Way of the Hado remain untested.

## On iPhone and iPad

All 246 shaders built for iOS and the unsigned IPA packed. Device testing remains open.

## Found on this game

- Two rewriter jump tables, [bit-test bounded index](../../../docs/reference/rewriter.md#bit-test-bounded-masked-jump-table-index) and [distant bounds check](../../../docs/reference/rewriter.md#bounds-check-further-back-than-16-instructions).
- Short ARC v7 directory entries and [headed SPK records](../../../docs/reference/shaders.md#spk-shader-package-in-an-mt-framework-arc).
- [SDK 1 movie buffer getters](../../../docs/reference/movies.md#sdk-1-movie-buffer-getters) and [SDK 1 `audren:u`](../../../docs/reference/audio.md#sdk-1-the-renderer-is-the-audrenu-service).
- PikaMan80 reported the original bring-up failures and supplied a patch. [Issue #960](https://github.com/seedds/nxnative/issues/960)
