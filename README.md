# Ocarina of Time low-memory kit for nxnative / NX2Apple

Makes **The Legend of Zelda: Ocarina of Time** (title ID `0400486024DEC000`, shader libraries `cb2e91c138e3`) run on a Mac with 8 GB of memory without `GPU out of memory` crashes, at a steady 20 fps, while keeping the UI and map sharp.

Tested on a MacBook Neo (A18 Pro, 8 GB, macOS 27.3), built with NX2Apple (host `dmhost`, NVN to Metal). It is a set of host changes plus two small tools. You bring your own legally dumped game; nothing here contains game data.

> **Status:** the Mac results below were measured by the project owner from the game's log and the Metal HUD. The iPhone builder has **not been tested on a device**. Treat it as a starting point and read its log.

## What it does

| Change | Switch | Effect |
|---|---|---|
| RAM saver | `DM_GUEST_FILE=1` | the game's own memory pools live in sparse disk files, out of the process footprint. App memory dropped from about 9 GB to about 2.6-3.0 GB, and the run got much smoother |
| Texture stand-ins | `DM_STAND_KB`, `DM_STAND_KB_MID`, `DM_STAND_KB_SMALL` | block-compressed textures are replaced by small stand-ins built from coarse mips, in three size tiers (big 512 KB and up, medium 128 KB and up, small) |
| UI / map bypass | `DM_TEXTURES_KEEP=ui` | textures that look like UI art (one mip level, non-power-of-two side) keep full detail: HUD, map, text. **Always on in both tools** |
| Texture drop | `DM_TEXTURES_OFF=big` | the biggest textures are not loaded at full size |
| Shadow map | `DM_SHADOW_DIV=1..48` | divides the 12288x12288 shadow map (and its scissor and viewport). 16 looked good, 48 is the lowest tried |
| Texture sweep | `DM_ALIAS_SWEEP=600` | every 300 frames, cached textures that nothing refers to and nobody used in 600 submissions are released. Gives back the memory a cutscene leaves behind (about 15 s after it ends) |
| 20 fps | `DM_FPS_TARGET=20` | holds 3 vsyncs per frame. The game steps in whole 1/60 s ticks, so 24 fps is not possible without slowing the game down |
| Optional | `DM_SKIP_WATER`, `DM_SKIP_EFFECTS`, `DM_SKIP_GRASS` | skip those shader families. Grass skipping also removes leaves and saves little: not recommended |
| Staging | `DM_UP_BLOCK_MB=8` | smaller upload blocks, about 140 MB less GPU memory, loads slower |

## Tested settings

These ran a full session including the opening cutscene without a crash:

```
DM_GUEST_FILE=1
DM_FPS_TARGET=20
DM_STAND_KB=6          # big textures
DM_STAND_KB_MID=4      # medium textures
DM_STAND_KB_SMALL=2    # small textures
DM_TEXTURES_OFF=big
DM_TEXTURES_KEEP=ui
DM_SHADOW_DIV=48
DM_ALIAS_SWEEP=600
DM_SKIP_WATER=1
DM_SKIP_EFFECTS=1
DM_UP_BLOCK_MB=8
```

Measured with that set (Kokiri Forest, 2816x1762 window): **19.5-20 fps**, **App 2.6-3.0 GB** in the Metal HUD (Activity Monitor: 2.3 GB), Metal working set about 4.8-5.15 GB against the recommended 5.46 GB. Before the RAM saver the process used about 9 GB.

A balanced starting point if the picture is too blocky: big 8, medium 4, small 2, shadow divider 16. Raising the texture numbers worked too (the owner went above the set shown); raise them one step at a time and watch the log's `GPU allocated`. If the log shows `GPU out of memory`, step back.

Detail levels: higher number = bigger stand-in = more memory. 1 is tiny and blocky, 32 is close to normal. Pick the level, not the byte count.

## Mac: patch an existing app

1. Keep `nx_ocarina_patcher.py`, `ocarina-host-156de5a1.patch` and `ocarina-fps20-156de5a1.patch` in one folder. Needs macOS, Python 3 with Tk, and (to build the host) Xcode with the Metal toolchain and git.
2. `python3 nx_ocarina_patcher.py` opens the window. Pick the game `.app`.
3. Left: resolution and frame rate. Right: the game app and the host. Bottom left: the memory options. Bottom right: buttons and the log.
4. Tick **RAM saver**, **Texture sweep**, choose the texture tiers, the shadow divider and 20 fps. The "also keep sharp" box is optional: **`ui` is always on, you do not need to type it.** Add sizes such as `256x256` only if some other art looks blocky.
5. Press **Get the checkout** (a folder with no spaces in its path), then tick **Build and install the patched host** and **Apply**. The app is signed ad-hoc again.
6. Turn the Metal HUD on (`DM_METAL_HUD=1`) the first time to see App memory and fps.

**Restore originals** puts the `.orig` copies back. Command-line version: `python3 nx_ocarina_patcher.py --help`.

If the app's own settings window had saved choices, press **Reset** there so the new defaults apply.

## iPhone / iPad: build, sign and install over USB-C

`nx_ocarina_ios_builder.py` wraps nxnative's own `scripts/build_ipa_from_nsp.py`: it patches the checkout, prepares the Xcode project, builds it signed with your Apple team (the project asks for the Increased Memory Limit entitlement), installs over USB with `devicectl`, writes `Documents/env.txt` and pushes the game files.

You need: a Mac with Xcode (iOS platform installed), Python 3.12+, the nxnative or NX2Apple checkout, your game's NSP folder and `prod.keys`, your Apple team ID (a free Personal Team has one), and an iPhone with A17 Pro or later on iOS 26+, connected by cable, unlocked, trusted, Developer Mode on. For fast game-file copies: `brew install libimobiledevice`.

`python3 nx_ocarina_ios_builder.py` opens the window; fill in the checkout, NSP folder, keys, team ID, pick the phone and press **Build, sign and install**. The first build takes 30-60 minutes. The tool saves your entries.

Caveats: untested on a device; the Mac patcher's resolution edit is not part of it; whether a free account keeps Increased Memory Limit depends on Apple's side, and the log will tell you.

## Reading the log

`[nvn-mtl] memory up to frame N: resident ..., GPU allocated ...` every few hundred frames. `idle shared texture sweep at frame N: X released, about Y MB` shows the sweep working. `[mem] guest file: ...` shows the RAM saver. Send these lines with a bug report.

## Known issues

- The log line for the UI keep prints a literal `%u KB` (cosmetic).
- `DM_TRIM_MEM` returned 0 MB in tests: leave it off.
- Cannot force low-LOD models: that choice is in the game's code, not the host.
- The shared buffers (about 2.2 GB) and the render-target heaps (about 1 GB) cannot be shrunk from the host, so GPU memory has a floor of about 5 GB.
- Written for the 10 nxnative commits from `80fc2ecb` to `156de5a1` (shader libraries `cb2e91c138e3`). The 20 fps patch does not apply to `80fc2ecb`.

## Layout

```
ocarina-lowmem/
  nx_ocarina_patcher.py          Mac app patcher (GUI + CLI)
  nx_ocarina_ios_builder.py      iPhone builder (GUI + CLI)
  ocarina-host-156de5a1.patch    host changes (dm/gfx/nvn/nvn_mtl.m)
  ocarina-fps20-156de5a1.patch   20 fps target (hle_nvn.c, loader.c)
```

The same changes are already committed on branch `ocarina-low-memory-156de5a1`; the tools detect that and skip applying the patches.


---

# Upstream nxnative README

# nxnative

Play your own Nintendo Switch and Switch 2 games natively on Apple silicon Macs, iPhone and iPad. One script packs your game into a Mac app or an unsigned IPA that you sideload.

It is not an emulator: the game's own code runs directly on the Apple chip, with Switch graphics rebuilt on Metal. Nothing from any game is in this repository, and the project is not affiliated with Nintendo or any game publisher.

[Games](#games) · [Community front ends](#community-front-ends) · [Requirements](#requirements) · [How to play](#how-to-play) · [Discussions](#discussions) · [For developers](#for-developers)

## Games

The full [tested games table](dm/games/README.md#games) includes original Switch games, tested builds, engines and graphics APIs. Each game's note records its tested platforms, progress and known issues.

These Switch 2 games have passed Mac gameplay testing. Build them from a [`.nup` file or a dump folder](#switch-2-games):

- [Donkey Kong Bananza](dm/games/040074A01BF12000/README.md)
- [Fire Emblem Fortune's Weave](dm/games/04007EF01F8AE000/README.md) — up to 120 fps
- [Hyrule Warriors: Age of Imprisonment](dm/games/04003F40232DE000/README.md)
- [Kirby Air Riders](dm/games/040077201BE50000/README.md)
- [Mario Kart World](dm/games/0400C3F00006E000/README.md)
- [Mario Tennis Fever](dm/games/0400D4201C6EA000/README.md)
- [Pokémon Pokopia](dm/games/0400A3A01EE4C000/README.md)
- [Splatoon Raiders](dm/games/0400380020056000/README.md)
- [Star Fox](dm/games/0400E88025B1A000/README.md)
- [The Legend of Zelda: Ocarina of Time](dm/games/0400486024DEC000/README.md)
- [Yoshi and the Mysterious Book](dm/games/0400245021D2C000/README.md)

## Community front ends

Community projects build on nxnative with tools to turn your games into apps for your iPhone, iPad or Mac without the command line, and libraries to organize and launch them. They are separate projects: questions and bugs about them go to their own pages.

- [NX2Apple](https://github.com/LNPANDY/NX2Apple) — Mac builder with a shared iPhone/iPad game library.
- [NX Play](https://github.com/varyform/nx-native-wrapper) — Mac library with device installs and save sync.
- [NxNative Launcher](https://github.com/rennan101/nxnative_Launcher) — Mac launcher with guided builds and controller mapping.
- [LumaNX](https://github.com/Xc1Ym/LumaNX) — Mac game builder with IPA export.
- [GameCase](https://github.com/Tiaoge-tou/GameCase) — iOS game library based on LiveContainer.

## Requirements

- **Mac:** Apple silicon, macOS 26.6 or later. Building games requires Xcode 27.2.
- **iPhone:** A17 Pro chip or later, with iOS 26 or later.
- **iPad:** M1 chip or later, with iPadOS 26 or later.

The games' textures are BC-compressed, which older iPhones cannot read. Switch 2 builds are tested on the Mac; check each game's note for device-specific validation.

For a game without a profile, the script writes a survey and tries a provisional build with guessed settings. That build may not start; see [Port another game](docs/development.md#port-another-game).

## How to play

Switch 2 games are built from one `.nup` file or a folder of their files, not from NSPs, and are tested on the Mac only: see [Switch 2 games](#switch-2-games).

### Build from the command line

You need a Mac with macOS 26.6 or later, [Xcode](https://developer.apple.com/download/all/?q=Xcode%2027.2) 27.2, [Homebrew](https://brew.sh) and Python 3.12 or newer (an arm64 one, not an Intel Homebrew's). An iPhone build also needs Xcode's iOS platform (Xcode → Settings → Components).

The shaders are built with Xcode 27.2's Metal compiler. Select it before step 1: `sudo xcode-select -s /Applications/Xcode.app`. With several Xcode versions installed, name this one `Xcode-27.2.app` and select it with `sudo xcode-select -s /Applications/Xcode-27.2.app`.

**1. Install the tools** (only once), in Terminal:

```
xcodebuild -downloadComponent MetalToolchain
git clone https://github.com/seedds/nxnative.git && cd nxnative
brew install xcodegen zstd glslang
python3 -m pip install lz4 "capstone>=5,<6" fonttools nsz zbic "backports.zstd; python_version < '3.14'"
```

**2. Put your game in a folder**: the game, plus its update and DLC if you have them (`.nsp`, `.xci`, `.nsz` or `.xcz`):

```
~/Games/MarioWonder/
  Super Mario Bros. Wonder [010015100B514000][v0].nsp
  Super Mario Bros. Wonder [010015100B514800][v327680].nsp
```

**3. Build**, with your game folder and your `prod.keys`:

```
python3 scripts/build_ipa_from_nsp.py ~/Games/MarioWonder -k ~/keys/prod.keys
```

The IPA appears in the game's folder. The first build takes 30–60 minutes and needs free disk space of about twice the game's size plus 10 GB; building again is much faster.

To play on the Mac instead, add `--platform mac-app`: you get a Mac app of the game to open from Finder. All options: [The packing script in detail](docs/development.md#the-packing-script-in-detail).

### Install

Install the IPA on your iPhone or iPad with a sideloading tool such as AltStore, Sideloadly or LiveContainer. If your tool offers **Increased Memory Limit**, turn it on: the bigger games need it.

Game too big to install? Build again with `--romfs separate`, and run the command the script prints once to copy the game files to your phone ([Quick reinstalls](docs/development.md#quick-reinstalls)). With AltServer, also add `--shader-files`: it cannot install a file of 2 GiB or more.

Controls, saves and settings: [Playing](docs/playing.md).

### Switch 2 games

A Switch 2 game is built from one [NUP](https://github.com/a55uka/nup) container (a `.nup` file) or a folder of its files, not from NSPs. Neither needs `prod.keys`. A game this project has no profile for is surveyed and gets a provisional profile, as NSPs do ([Port another game](docs/development.md#port-another-game)), which may not boot. You get a Mac app; iPhone and iPad are untested.

**From a `.nup` file**, which holds the base game, its updates and DLC in one file. Give the file, or a folder that holds it:

```
python3 scripts/build_ipa_from_nsp.py ~/Games/DonkeyKongBananza.nup --platform mac-app
```

[Hyrule Warriors: Age of Imprisonment](dm/games/04003F40232DE000/README.md) is played this way: its 47 GB NUP unpacks in under five minutes. A NUP that carries the game's control data (its NACP) and icon gives the app the game's name and icon; otherwise the name comes from the game's profile and the app has no icon unless you give it one with `--icon <image>` (`ICON` in Spyder; a JPEG or PNG of any size).

A NUP without the base game also works, when its base package is empty or it has none and its updates are marked `standalone_update`: the update is taken as the whole game, and the script says so. If the game then stops on missing files, that update holds only what changed: pack a NUP with the base game. The newest update is selected by default; `--package v<N>` selects another version in the container. Such a container has no `v0` (base game) to select.

**From a folder of files**. The script expects this layout (`updates` and `dlc` are optional):

```
~/Games/DonkeyKongBananza/
  base/exefs/                     main, main.npdm, sdk, rtld
  base/romfs/
  updates/458752/exefs/           the update's version, as a decimal number
  updates/458752/romfs/           the patched RomFS, not a patch
  dlc/040074a01bf13001/0/romfs/   the DLC's title ID
```

```
python3 scripts/build_ipa_from_nsp.py ~/Games/DonkeyKongBananza --platform mac-app
```

An update's folder alone will do (no `base/`), and so will `exefs/` and `romfs/` directly in the folder, as `nup extract` writes them, which count as `base/`. The app has no icon, because the folder holds none; `--icon <image>` (`ICON` in Spyder) gives it one. To turn such a folder into a NUP: `nup pack ~/Games/DonkeyKongBananza game.nup`.

## Discussions

Questions, how a game runs on your Mac or iPhone, and ideas go in [Discussions](https://github.com/seedds/nxnative/discussions); [Issues](https://github.com/seedds/nxnative/issues) are for bugs, with the log. Ocarina of Time's performance (frame rate, resolution, which Macs hold 60 fps) is followed in [Ocarina of Time: state on main](https://github.com/seedds/nxnative/discussions/1789).

## For developers

[How it works](docs/development.md) · [Game notes](dm/games/README.md#games) · [Bring-up and debugging](docs/agents/README.md) · [Front ends](docs/frontends.md) · [Contributing](CONTRIBUTING.md)

## Acknowledgements

This project is built on other people's work ([licenses](dm/third_party/README.md)):

- **[d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile)** (MIT) developed this native-execution approach, for Diablo II: Resurrected. nxnative started as a copy of its generic parts: the NSO rewriter, the host that stands in for Horizon, the NVN-on-Metal layer and the iOS app shell. Fixes go both ways.
- **Ryujinx** (MIT): its shader translator (`Ryujinx.Graphics.Shader`, `Ryujinx.Common` and `Spv.Generator` 1.3.2, in `dm/third_party/ryujinx` with a few fixes listed in [its README](dm/third_party/ryujinx/README.md)) turns the games' shaders into SPIR-V. Its emulation of the kernel and system services is also a reference when the host's behaviour is in doubt.
- **[Atmosphère](https://github.com/Atmosphere-NX/Atmosphere)**: its kernel, mesosphère, is the reference for Horizon's synchronization semantics (address arbiter, condition variables).
- **[LibHac](https://github.com/Thealexbarney/LibHac)**: reads NSPs and layers updates over the base game in `dm/tools/nx_extract`.
- **[SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross)**: SPIR-V to Metal Shading Language.
- **[glslang](https://github.com/KhronosGroup/glslang)** compiles the GLSL of OpenGL games to SPIR-V for it.
- **[MacNativeGames](https://github.com/ly05010419/MacNativeGames)** (MIT): the GPU widening of 8-bit indices and in-pass clears in `dm/gfx/nvn/nvn_mtl.m`, and the visibility-counter lowering in the shader translator.

## License

MIT ([LICENSE](LICENSE)). Third-party code keeps its own license ([dm/third_party](dm/third_party/README.md)).
