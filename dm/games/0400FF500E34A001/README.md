# Xenoblade Chronicles: Definitive Edition – Nintendo Switch 2 Edition (`0400FF500E34A001`)

Program ID `0400FF500E34A001`, display version 2.0.0 (`v0`), SDK 21.4.4 built for the Switch 2 (`Ounce`),
Monolith's engine, NVN2 with DLSS. Modules: `main`, `subsdk0` and `subsdk3` (two builds of the SDK's
movie module), `sdk`; `subsdk1` (`NintendoSDK_DLSS`) and `subsdk2` (the C library) are shipped but
`main` imports nothing from them the host does not supply. The `sdk` and `subsdk0` are
[Star Fox](../0400E88025B1A000/README.md)'s builds. The Switch 1 game is
[its own profile](../0100FF500E34A000/README.md).

## Current status

Mac gameplay tested (2026-11-17, Apple M6, macOS 27.0.1, a Mac app from
`scripts/build_ipa_from_nsp.py --platform mac-app`): title screen and menu, New Game, the
prologue's real-time cutscenes and the Sword Valley battle as Dunban through its tutorials,
targeting, Monado arts and the first Mechon M63 defeated, with sound. About 12 minutes from
launch with no `[RED]` line and no crash. The title screen and the battle run at 60 fps
(frame time p50 16.62 ms); the cutscenes at about 29 fps (p50 35 ms), which looks like the
game's own cap (not checked). Memory was flat through the battle: 3.4 GB resident, 2.9 GB on
the GPU. Further story progression, saving and reloading, and the iPhone remain open; no IPA
has been built.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `romfs_marker dummy.bin` | [`game.tsv`](game.tsv) | the Switch 2 program's own RomFS holds one 5-byte file, `dummy.bin`. The game reads its data from the Switch 1 program's RomFS (index 0) and the upgrade add-on (`0100FF500E34B001`), which the NSP script packs beside it ([A Switch 2 Edition NUP](../../../docs/reference/packing.md#a-switch-2-edition-nup-stops-at-2-base-packages-or-is-not-a-base-title-id)). The provisional profile guessed `sound/jp.pck`, a file of the Switch 1 program's RomFS |
| `mem_reserve 0x2028000000`, `mem_alias 0x2000000000`, `mem_stack 0x10000000`, `app_pool 0x240000000` | `game.tsv` | the NPDM declares 96 MB of system resource (`sys_resource 0x6000000`), so virtual address memory is on, and `main` never sets a heap. `nninitStartup` reserves a 60 GB address region (returning to `main+0x969a64`), as [Xenoblade Chronicles X](../0100453019AA8000/README.md)'s does; the 128 GB alias region holds it. The pool is the Switch 2's 9 GB |
| `mem_map_limit on` | `game.tsv` | Horizon's physical-pool limit: the engine probes its capacity by mapping until a map fails. With it, the alias region was mapped up to +8.25 GB and stopped below the 9 GB pool |
| `pad_layout full` | `game.tsv` | a 3D game, as the Switch 1 profile |

There is no game-specific patch and no rewriter cut. The SDK takes the existing
function-matched `AddressRegionManager` cut (`rewrite_sdk.log`).

## Files

- A NUP container of the Switch 2 Edition 2.0.0: the Switch 2 program (this profile, 0.07 GB),
  the Switch 1 program with the game data and the upgrade add-on (20 GB).
- The merged RomFS: 18,530 files, 13.61 GB, Monolith's loose HCPS / XBC1 / DRSM containers
  as on Switch 1 ([Monolith containers](../../../docs/reference/shaders.md#monolith-arh1-xbc1-and-hcpsslct-shader-containers)),
  and 18 `DMPA` model packages (`/chr/obj/oj*.wimdo`) with plain HCPS programs, the same
  files in the Switch 1 program and the add-on.

## Shaders

16,971 in [`shader_map.tsv`](shader_map.tsv): 11,417 pixel, 5,518 vertex, 33 compute, 3 geometry;
by source 8,933 map, 4,332 character, 2,826 `monolib`, 874 effect and 6 from `main`. All of them
translate; a Mac build takes 1 min 33 s on the M6. The split leaves out 6 control sections in
`main` (`@0xa0e928` to `@0xa0ed38`, 0xd0 apart) with no code area it can find; none was asked
for through the first battle.

Before `DMPA` was read the split found 16,506, and New Game logged 14 `is not in the shader map`
keys and `program has no vertex shader`. `DM_NVN_SHDUMP` showed all 14 code areas uncompressed in
the add-on's `oj300001.wimdo` and `oj300021.wimdo`. Resolved by the general fix in the split.

## Driving it on the Mac

The Mac app's first launch after each pack spends about 70 s in macOS's launch check before
the launcher writes anything; the log is `~/Library/Logs/nxnative/0400FF500E34A001.log`. Headless:
`<app>/Contents/MacOS/nxnative --no-launcher` with `DM_CTL=<file>`. The title screen is up
about 30 s after the host starts. Then `A` (the Switch 2 Edition's "News Update"), `A` (to the
menu), `A` (New Game). The prologue's cutscenes run about 6 minutes to "Battle Introduction 1"
with no input. In battle, `R` targets, the D-pad picks an art and `A` uses it (not `B`), and `A`
closes each tutorial page, of which several come up in the first fight.

A `shot` during the cutscenes was written several seconds late: the host's present runs far
less often than the game's frames there (an `on-screen` line counted 269 of 300 frames
dropped, 268 discarded), while the battle showed none dropped. Not looked into.

## Found on this game

- [Monolith containers](../../../docs/reference/shaders.md#monolith-arh1-xbc1-and-hcpsslct-shader-containers):
  `DMPA` model packages hold the only copy of 14 shaders the prologue draws with.

## Open

- iPhone: the 60 GB address region does not fit the iPhone's alias region. Xenoblade
  Chronicles X shrinks the same reservation with build-bound cuts; this build has none.
- A `pso_manifest.tsv` has not been recorded.
- Story past the first battle, saving and reloading.
- The 6 control sections in `main` the split leaves out.
- The cutscenes' discarded presents (above).
- The Switch 1 profile's `shader_map.tsv` was made before `DMPA` was read: the same 18 files
  hold shaders it does not list.
