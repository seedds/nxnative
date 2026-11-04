# Xenoblade Chronicles 2, Nintendo Switch 2 Edition (`0400E95004038001`)

The Switch 2 program of [Xenoblade Chronicles 2](../0100E95004038000/README.md)'s Nintendo Switch 2 Edition: program 1 of the application `0100E95004038000`'s update 1114112 (program 0 is the Switch 1 game). SDK 21.4.4 built for the Switch 2 (`Ounce`), NVN2 API 231.39, Monolith's engine. Modules `main`, `subsdk0` and `subsdk3` (movie) and `sdk`; `subsdk1` (DLSS) and `subsdk2` (libc) supply nothing the host does not.

**Status: plays on the Mac (Mac only), through its first battle.** Headless, from a new save: the title (from frame 600), the one-time screen on the Switch 2 Edition's new content, the main menu, New Game, the difficulty choice, the opening (its credits over in-engine scenes, skipped partway) and the first battle against King Crustip with its tutorial, at 60 fps (frame time p50 16.6 ms, p95 17.1 to 17.4 ms) with no crash and no `[RED]` line. Audio has not been heard and no IPA has been built (the iPhone needs a cut, Open).

## What it needs

| Setting | Where | Why |
|---|---|---|
| its own profile, picked for program 1 of a dump | [`game.tsv`](game.tsv), `scripts/build_ipa_from_nsp.py` | the Switch 1 profile's modules, memory and shader tables do not fit: with them the boot crashed in `nninitStartup` |
| `mem_alias 0x2000000000`, `mem_reserve 0x2028000000` (Mac only) | `game.tsv` | `nninitStartup` and `nnMain` both run the allocator's setup (`main+0xbddb40` → `main+0xb78ed0`), and each takes a 60 GB address region with `nn::os::AllocateAddressRegion(&p, 0xf00000000)` (`main+0xb7bdc0`); the second ends at +121.07 GB. With less, the second fails, the allocator's region stays zero and its first block is null: `memset(NULL, 0, 0x20000)` from `main+0xb7a650` |
| `mem_map_limit on` | `game.tsv` | the setup maps 1 MB pages into the region until a map fails, to measure its memory (`main+0xb7be04`); without the limit the host let it map all 60 GB |
| `sys_resource 0x6000000`, `app_pool 0x240000000` | `game.tsv` | the NPDM declares 96 MB (virtual address memory on); the Switch 2's 9 GB pool, as the other Switch 2 games |
| `romfs_marker dummy.bin` | `game.tsv` | its own RomFS (below) |
| display version `3.0.1` in the `build` row | `game.tsv` | the game compares its display version with DLC 98's `dlcver.bin` (`3.0.1`) and shows the system's add-on content version error when they differ (`main+0x265d30` → `err::ShowUnacceptableAddOnContentForUpgradeVersionError`; the host runs no error applet and crashed in `applet::StartLibraryAppletImmediately`). With the placeholder `v1114112` it stopped there |

## Files

- A dump of the update with `program/<n>/` folders (no `base/`) and seven DLC (indices 1 to 5, 98 and 99). The NSP script packs program 1 by default ([`scripts/pack_dump.py`](../../../scripts/pack_dump.py)).
- Its own RomFS holds one file, `dummy.bin` (5 bytes, `dummy`). The game data is DLC 98 (`0100E95004039062`, its update 65536, `dlcver.bin` 3.0.1): a `bf2.arh` / `bf2.ard` pair of 67,476 entries, 14 GB, the whole game and Torna with NVN2 shaders. Program 0's `bf2` pair (41,478 entries) is the Switch 1 game's, with NVN shaders.
- The game registers `rom:/bf2.arh` with `bf2.ard`, then, for DLC 98, `aoc98:/bf2.arh` with `aoc98:/bf2.ard` under the same `rom:/` prefix (`main+0x735144`, `main+0x73521c` → `main+0xb9ec40` → `main+0xb9c6a0`). With its own RomFS, `rom:/bf2.arh` is not found and DLC 98's pair is the archive. Packed with program 0's RomFS instead, the game looked files up in program 0's index and read them from DLC 98's `.ard`: `font_binary2.txt` came back as other data, the parser at `main+0xba48a0` (magic `FNTC`, checked but not enforced) read past its 0x17a03 bytes, and a bus error followed (`main+0xba50f8`).
- It also probes `rom:/ira.arh` and `rom:/monolib/shader/ndsr.pck`, found in neither archive (not followed).

## Shaders

- `main`'s standalone SLCT has NVN2 control sections (0xd0 bytes, their own length at +0x30, the code's at +0x44): the HCPS reader in `shader_split.py` takes them now (`test_monolith.py` `test_nvn2_controls`).
- 22,561 shaders, split from its own RomFS and the seven DLC (DLC 98's archive holds the NVN2 ones). An earlier split from program 0's RomFS had 33,004, the Switch 1 game's among them.
- Its upscaler is agl's NDSR network in `/monolib/shader/ndsr.pck`, an XBC1 entry of `bf2.ard` (40 shaders, 3 compute: `9badd23a1111e0ce`, `f809de4b7aab2c96`, `fc2f8e5de188c665`). The split read it only from this game on ([Monolith's Switch 2 shaders](../../../docs/reference/shaders.md#monoliths-switch-2-shaders-nvn2-controls-in-hcps-and-the-ndsr-pack-in-an-archive-entry)); before, its nine keys were "not in the shader map".
- `main` holds six NVN2 control sections the split leaves out (`main+0xc34ec8` to `+0xc352d8`, 0xd0 apart; `raw_shaders` reads Switch 1 controls only). No draw at the title uses them (open).

## Rendering

- The frame (current, at the title): the scene renders at 1024x576, a composite grades it through a 16x16x16 LUT blended from four of the game's own (`shd_post.wishp`), DLSS takes it to 1536x864 (MetalFX's temporal scaler stands in), and the game's NDSR compute shader (`f809de4b7aab2c96`) scales that 2x into the window texture's top-left 3072x1728; the UI is drawn over it, and the window is cropped to 3072x1728 (`nvnWindowSetCrop`). The DLSS networks are 2560x1440, 2048x1152 and 1536x864, method 0, preset 5.
- Resolved, three causes of a black, mis-coloured title with striped text:
  1. The NDSR shaders were not in the map, so its dispatch was dropped (Shaders, above).
  2. DLSS never ran: the game evaluates only when its input lies between the smallest and largest size `GetOptimalSettings` reports, and the host reported one size, 768x432 ([NVN2's DLSS on MetalFX](../../../docs/reference/rendering.md#nvn2s-dlss-on-metalfx), Input range). The NDSR pass then upscaled a texture nothing wrote.
  3. Its file textures are made with flags 0 and are in the Tegra X1's order inside a GOB; the host read every texture of a Switch 2 game in the Switch 2's order ([The Switch 2's order inside a GOB](../../../docs/reference/nvn-objects.md#the-switch-2s-order-inside-a-gob)). The grading LUTs came out a weave, so the scene turned green and red, and the R8 font atlas (4264x4364) put every other pair of rows 16 texels aside.
- Screenshots: `DM_NVN_SHOT` saves the whole 3840x2160 window texture, so the picture sits in its top-left 80 %; with `DM_NVN_SHOT_OUT=1920x1080` it is scaled as the present shows it.
- No logo was seen at the title within 180 s (not checked against the console).

## Playing it on the Mac

- The work folder's `run.sh <name> <seconds> [VAR=value ...]` runs the packed game headless and silent with a fresh save; `ctl.sh` appends a line to a `DM_CTL` file (`ctl.sh A`, `ctl.sh "LUp~1.0"`, `ctl.sh snap NAME` for a screenshot it waits for). Started as `run.sh play 1800 DM_CTL=<work folder>/play/ctl.txt DM_NVN_SHOT_OUT=1920x1080`.
- The route: A at the title (from frame 900) opens the content screen once, A returns to the title, A opens the menu (New Game, Options, Information, and the Torna banner), A on New Game, A on Normal, A on the note about difficulty. The opening plays from about 15 s later; Plus pauses it and A skips it (`PAUSED`, A Skip / B Resume), to the first battle about 20 s on.
- Not tried yet: winning the battle (Rex had not drawn his weapon), a save, Torna, the Merc Assault mode.

## Audio

- Sound is decoded through SDK 21's hardware Opus decoder manager ([SDK 21's hardware Opus decoder manager](../../../docs/reference/audio.md#sdk-21s-hardware-opus-decoder-manager)) and played through an OutputStream ([SDK 21's OutputStream on a virtual output device](../../../docs/reference/audio.md#sdk-21s-outputstream-on-a-virtual-output-device)), both new in the host for this game. Not heard yet (open).

## Open

- Gameplay past the first battle: winning it, a save and a reload, the first open area, Torna.
- **The iPhone:** 121 GB of address regions do not fit; the two 60 GB regions need a cut, as Super Smash Bros. Ultimate's ([A 62 GB page allocator behind `nu::VirtualAllocHook`](../../../docs/reference/memory.md#a-62-gb-page-allocator-behind-nuvirtualallochook)), once it is known how much of them the game uses.

## Found on this game

- [Monolith's Switch 2 shaders: NVN2 controls in HCPS, and the NDSR pack in an archive entry](../../../docs/reference/shaders.md#monoliths-switch-2-shaders-nvn2-controls-in-hcps-and-the-ndsr-pack-in-an-archive-entry).
- [NVN2's DLSS on MetalFX](../../../docs/reference/rendering.md#nvn2s-dlss-on-metalfx): the input range `GetOptimalSettings` reports.
- [The Switch 2's order inside a GOB](../../../docs/reference/nvn-objects.md#the-switch-2s-order-inside-a-gob): textures without flag 0x4 keep the X1's order.

- [SDK 21's hardware Opus decoder manager](../../../docs/reference/audio.md#sdk-21s-hardware-opus-decoder-manager).
- [SDK 21's OutputStream on a virtual output device](../../../docs/reference/audio.md#sdk-21s-outputstream-on-a-virtual-output-device).
