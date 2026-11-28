# Xenoblade Chronicles X: Definitive Edition – Nintendo Switch 2 Edition

Program `0400453019AA8001`, version 2.0.0 (`v0` in the supplied NUP), SDK
20.5.14 for Switch 2 (Ounce), NVN2 API 231.30, Monolith's engine and Wwise
audio. The loaded modules are `main` and `sdk`. The host supplies the imports
from the shipped NintendoSDK_movie module (`subsdk0`).

## Current status

On an Apple M6 Mac mini (16 GiB, macOS 27.2.1, Xcode 27.2), a fresh game
rendered the title, opening movie, avatar creation, name entry and the first
field in Starfall Basin with Elma. The 3840×2160 swapchain had a 16.63 ms
median frame over the last 300 frames of that run. The host reported 3,294 MB
resident and 2,363 MB allocated by Metal at the end. The host reported both
add-on licenses, opened stereo output and logged two slow residency commits
(205.5 ms and 116.9 ms). No shaders were missing. Combat, later areas,
save/reload and iOS devices remain untested.

The 17.2 GiB Mac app passed signature verification. Its first launch reached
the 2.0.0 title in about 2 minutes 20 seconds and used the bundled pipeline
archive.

## Running on the Mac

Build it from the NUP with `scripts/build_ipa_from_nsp.py <folder> --platform mac-app`.
Choose New Game, create an avatar and follow the opening prompts. `Plus`
then `A` skips a scene after the confirmation appears.

## Address space

Monolith reserves 60 GiB from `main+0xae54c4` before `nnMain`. In the first 15
seconds of `nnMain`, the reservation had 3.11 GiB mapped and had opened pages
up to 8.01 GiB within it. A separate 64 MiB region brings total reservation
reach to alias +60.13 GiB. The Mac's 128 GiB Switch 2 alias region holds both.
The NPDM declares 96 MiB of system resource and the SDK identifies the 9 GiB
Switch 2 application pool. The existing SDK rewriter matches the address
region node-area implementation. There are no game executable cuts.

The version's 60 GiB request exceeds the current 4.375 GiB iPhone and iPad
layout. A Mac run forced to that layout stopped in `nninitStartup` before
`nnMain`, at `memset` (`sdk+0x6d6448`, called from `main+0xae38d0`) with a
null destination. Device bring-up is open.

## Files and shaders

- The NUP has one program with its full RomFS and two license-only add-ons.
  The RomFS contains `sts.ard` and `stso.ard` with ARH2 indexes, two sets of
  46 WebM movies and five Wwise sound packs.
- The shared Monolith reader found 24,815 shader keys, 19,058 vertex/pixel
  variation pairs and four vertex/geometry pairs. The map covers all 749
  keys requested during the first 15 seconds of boot. The split reported six
  control sections in `main` for which it found no code area.
- All 24,815 Mac shader libraries built on chain `343aa69bfb7a`. The Mac host
  prebuilt all 19,058 variation bases without a failure. The Mac app bundles
  an `applegpu_g18g` archive with 19,058 render bases and 36 compute pipelines.

## Found on this game

- [Indexed RomFS mounts](../../../docs/reference/services.md#mounting-the-current-programs-romfs-by-index)
  use the running program's own image for `romo:` (index 1).
- [The Mac's console-sized alias region](../../../docs/reference/memory.md#the-macs-alias-region-is-the-switchs-64-gb)
  holds Monolith's startup reservation.
- [Monolith's shader containers](../../../docs/reference/shaders.md#monolith-arh1-xbc1-and-hcpsslct-shader-containers)
  cover both archive indexes.
