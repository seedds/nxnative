# Xenoblade Chronicles 2 – Nintendo Switch 2 Edition

Program `0400E95004038001`, version 3.0.1 (`v0` in the merged NUP), SDK 21.4.4
for the Switch 2 (`Ounce`), NVN2 API 231.39, Monolith's engine and Wwise audio.
The loaded modules are `main`, `subsdk0` and `subsdk3` (NintendoSDK_movie),
and `sdk`. The other shipped modules are NintendoSDK_DLSS (`subsdk1`) and the
C library (`subsdk2`). The loaded modules and the shared host supply the imports.

## Current status

Mac opening tested from a fresh save on an Apple M6 Mac mini (16 GB, macOS
27.2.1, Xcode 27.2). The 3.0.1 title and menus rendered, all seven add-ons
mounted, stereo output initialized, and New Game on Normal reached the
King Crustip tutorial after playing and skipping the prologue. The 3840×2160
swapchain presented a 16.62 ms median frame while standing at the tutorial.
The full shader build produced all 22,561 Mac libraries on chain
`3f2b26d1773d` with no missing shaders reported on that route. The log recorded
one 142 ms residency commit during startup. The rest of the battle, later
areas, save/reload and add-on gameplay remain untested.

The Mac app built from the registered profile includes an `applegpu_g18g`
archive with 16,955 render-pipeline bases and 33 compute pipelines. The
packaged launcher reached the 3.0.1 title with a scratch save and found all
16 probed bases in that archive. On this Mac, the first launch took about
three minutes before the launcher log opened.

Build it from the NUP with `scripts/build_ipa_from_nsp.py <folder> --platform mac-app`.
On a fresh save, dismiss the Switch 2 Edition information screen, choose New
Game and Normal, then use Plus followed by A to skip the prologue.

## Address space

The NPDM declares 96 MB of system resource. The Mac's Switch 2 guest layout
provides a 128 GB alias region and a 9 GB application pool. Monolith's allocator
reserves two 60 GB virtual ranges through `main+0xb7bdc0`, reaching alias
+120.13 GB in the first 15 seconds of `nnMain`. The second range holds 6.10 GB
of mapped pages at the end of that check. The existing SDK-wide rewriter rule
matches the 21.4.4 address-region node area. The game needs no executable cuts
of its own on the Mac.

**iPhone and iPad remain open.** With `DM_ALIAS_SIZE=0x118000000` on the Mac,
the first 60 GB reservation fails and `nninitStartup` reaches `memset` with
a null destination (`main+0xb7a64c` → `sdk+0x798ce8`). The stock 128 GB Mac
layout reaches the frame loop. The compact iOS alias layout needs a separate
allocator investigation before an IPA can be validated.

## Files and shaders

- The supplied NUP has one Switch 2 program, with its full RomFS. The base image
  holds 28,446 files (25.63 GB), including 214 WebM movies. `bf2.ard` is the
  RomFS marker and its companion `bf2.arh` indexes 67,476 archive members.
- The container lists seven add-ons, five with content and two license-only.
  The packer preserves all seven entries. The tested English locale is `en-GB`,
  the English locale in the supplied control data.
- The shared Monolith reader finds 22,561 shaders from the base, add-ons and
  six stages in `main` (6,195 vertex, 16,330 pixel, three geometry and 33
  compute). The split records 16,955 vertex/pixel variation pairs and three
  vertex/geometry pairs. The shader map covers all 248 keys requested by the
  startup probe.

## Found on this game

- [The Mac's console-sized alias region](../../../docs/reference/memory.md#the-macs-alias-region-is-the-switchs-64-gb)
  holds both Monolith reservations.
- [Monolith's shader containers](../../../docs/reference/shaders.md#monolith-arh1-xbc1-and-hcpsslct-shader-containers)
  include the NDSR pack inside `bf2.ard`.
- [The Switch 2's order inside a GOB](../../../docs/reference/nvn-objects.md#the-switch-2s-order-inside-a-gob)
  applies to its font atlas and grading textures.
- The host uses [MetalFX for DLSS](../../../docs/reference/rendering.md#nvn2s-dlss-on-metalfx),
  and implements the [SDK 21 output stream](../../../docs/reference/audio.md#sdk-21s-outputstream-on-a-virtual-output-device)
  and [Opus decoder manager](../../../docs/reference/audio.md#sdk-21s-hardware-opus-decoder-manager).
