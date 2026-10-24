# nxnative

Play your own Nintendo Switch games natively on iPhone, iPad, and Mac. Bring your game (its NSP files) and your `prod.keys`; one script turns them into an IPA that you sideload, or a native `.app` bundle for your Mac.

It is not an emulator: the game's own code runs directly on the Apple chip, with Switch graphics rebuilt on Metal. Nothing from any game is in this repository, and the project is not affiliated with Nintendo or any game publisher.

## Acknowledgements

This project is built on other people's work:

- **[d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile)** (MIT) developed this native-execution approach, for Diablo II: Resurrected. nxnative started as a copy of its generic parts: the NSO rewriter, the host that stands in for Horizon, the NVN-on-Metal layer and the iOS app shell. Fixes go both ways.
- **Ryujinx** (MIT): its shader translator (`Ryujinx.Graphics.Shader`, `Ryujinx.Common` and `Spv.Generator` 1.3.2, in `dm/third_party/ryujinx` with a few fixes listed in [its README](dm/third_party/ryujinx/README.md)) turns the games' Maxwell shaders into SPIR-V. Its emulation of the kernel and system services is also a reference when the host's behaviour is in doubt.
- **[Atmosphère](https://github.com/Atmosphere-NX/Atmosphere)**: its kernel, mesosphère, is the reference for Horizon's synchronization semantics (address arbiter, condition variables).
- **yuzu**: Maxwell instruction encodings for the shader reference tool, and a cross-check for kernel behaviour.
- **[LibHac](https://github.com/Thealexbarney/LibHac)**: reads NSPs and layers updates over the base game in `dm/tools/nx_extract`. **hactool** did the first extractions.
- **[SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross)**: SPIR-V to Metal Shading Language. **[glslang](https://github.com/KhronosGroup/glslang)** compiles the GLSL of OpenGL games to SPIR-V for it.
- **deko3d** (zlib): its texture layout size algorithm is ported in `dm/gfx/nvn/nvn_tex.c`.
- **libvpx** (VP9 movies), **stb_vorbis** (movie sound), the **Vulkan headers** and **NVIDIA FLIP** ship in `dm/third_party` with their licenses ([list](dm/third_party/README.md)).
- **Capstone**, **lz4**, **fontTools** and **XcodeGen** are used by the build.

## Games

All of these are tested on an iPhone and playable:

Needs iOS / iPadOS 26 or later on an iPhone 15 Pro or newer (A17 Pro chip or later) or an iPad with an M1 chip or later: the games' textures are BC-compressed, and older iPhones can't read that format. Other games aren't supported yet. For them the script writes a survey and still tries its best to pack an IPA, guessing the game's settings; such an IPA may not start.

Under each game: the IPA's size, and "60 fps with 1.2.0" where the game runs at 60 fps (30 on the Switch) with that update.

| | | |
|:---:|:---:|:---:|
| ![13 Sentinels: Aegis Rim](docs/images/13-sentinels.jpg)<br>13 Sentinels: Aegis Rim<br><sub>~5 GB</sub> | ![Absolum](docs/images/absolum.jpg)<br>Absolum<br><sub>~2 GB</sub> | ![Ace Attorney Investigations Collection](docs/images/ace-attorney-investigations.jpg)<br>Ace Attorney Investigations Collection<br><sub>~7 GB</sub> |
| ![Advance Wars 1+2: Re-Boot Camp](docs/images/advance-wars-1-2-re-boot-camp.jpg)<br>Advance Wars 1+2: Re-Boot Camp<br><sub>~7.1 GB</sub> | ![AI: THE SOMNIUM FILES](docs/images/ai-the-somnium-files.jpg)<br>AI: THE SOMNIUM FILES<br><sub>~8.3 GB</sub> | ![AI: THE SOMNIUM FILES - nirvanA Initiative](docs/images/ai-the-somnium-files-nirvana-initiative.jpg)<br>AI: THE SOMNIUM FILES - nirvanA Initiative<br><sub>~11.6 GB</sub> |
| ![Animal Crossing: New Horizons](docs/images/animal-crossing-new-horizons.jpg)<br>Animal Crossing: New Horizons<br><sub>~10.5 GB</sub> | ![ANIMAL WELL](docs/images/animal-well.png)<br>ANIMAL WELL<br><sub>~54 MB</sub> | ![Balatro](docs/images/balatro.jpg)<br>Balatro<br><sub>~82 MB · played by touch</sub> |
| ![Bayonetta 3](docs/images/bayonetta-3.jpg)<br>Bayonetta 3<br><sub>~14.2 GB</sub> | ![Big Brain Academy: Brain vs. Brain](docs/images/big-brain-academy-brain-vs-brain.jpg)<br>Big Brain Academy: Brain vs. Brain<br><sub>~0.8 GB</sub> | ![Brain Memory Training Academy](docs/images/brain-memory-training-academy.jpg)<br>Brain Memory Training Academy<br><sub>~0.1 GB</sub> |
| ![Captain Toad: Treasure Tracker](docs/images/captain-toad-treasure-tracker.jpg)<br>Captain Toad: Treasure Tracker<br><sub>~2 GB</sub> | ![Catherine: Full Body](docs/images/catherine-full-body.jpg)<br>Catherine: Full Body<br><sub>~14 GB</sub> | ![Celeste](docs/images/celeste.jpg)<br>Celeste<br><sub>~0.9 GB</sub> |
| ![Cuphead](docs/images/cuphead.jpg)<br>Cuphead<br><sub>~5 GB</sub> | ![DARK SOULS: REMASTERED](docs/images/dark-souls-remastered-creation.jpg)<br>DARK SOULS: REMASTERED<br><sub>~4 GB · 60 fps with 1.0.3</sub> | ![Digimon Story Cyber Sleuth: Complete Edition](docs/images/digimon-story-cyber-sleuth.jpg)<br>Digimon Story Cyber Sleuth: Complete Edition<br><sub>~5 GB</sub> |
| ![DRAGON QUEST VII Reimagined](docs/images/dragon-quest-vii-reimagined.jpg)<br>DRAGON QUEST VII Reimagined<br><sub>~5.9 GB</sub> | ![DRAGON QUEST XI S: Echoes of an Elusive Age – Definitive Edition](docs/images/dragon-quest-xi-s.jpg)<br>DRAGON QUEST XI S: Echoes of an Elusive Age – Definitive Edition<br><sub>~15 GB · 60 fps with 1.0.4</sub> | ![FINAL FANTASY RESONANCE](docs/images/final-fantasy-resonance.jpg)<br>FINAL FANTASY RESONANCE<br><sub>~10 GB · 60 fps with 1.0.1</sub> |
| ![FINAL FANTASY TACTICS - The Ivalice Chronicles](docs/images/final-fantasy-tactics.jpg)<br>FINAL FANTASY TACTICS - The Ivalice Chronicles<br><sub>~11 GB</sub> | ![Fire Emblem Engage](docs/images/fire-emblem-engage.jpg)<br>Fire Emblem Engage<br><sub>~15 GB · 60 fps with 2.0.0</sub> | ![Fire Emblem: Three Houses](docs/images/fire-emblem-three-houses.jpg)<br>Fire Emblem: Three Houses<br><sub>~13 GB · 60 fps with 1.2.0</sub> |
| ![The Great Ace Attorney Chronicles](docs/images/the-great-ace-attorney-chronicles.jpg)<br>The Great Ace Attorney Chronicles<br><sub>~4 GB</sub> | ![Hades](docs/images/hades.jpg)<br>Hades<br><sub>~4 GB</sub> | ![Hades II](docs/images/hades-ii.jpg)<br>Hades II<br><sub>~4 GB</sub> |
| ![Hollow Knight: Silksong](docs/images/hollow-knight-silksong.jpg)<br>Hollow Knight: Silksong<br><sub>~6.4 GB</sub> | ![Hyrule Warriors: Age of Calamity](docs/images/hyrule-warriors-age-of-calamity.jpg)<br>Hyrule Warriors: Age of Calamity<br><sub>~15 GB · 60 fps with 1.3.0</sub> | ![The Legend of Zelda: Breath of the Wild](docs/images/zelda-breath-of-the-wild.jpg)<br>The Legend of Zelda: Breath of the Wild<br><sub>~16 GB · 60 fps with 1.9.0</sub> |
| ![The Legend of Zelda: Link's Awakening](docs/images/zelda-links-awakening.jpg)<br>The Legend of Zelda: Link's Awakening<br><sub>~6 GB</sub> | ![The Legend of Zelda: Tears of the Kingdom](docs/images/zelda-tears-of-the-kingdom.jpg)<br>The Legend of Zelda: Tears of the Kingdom<br><sub>~17 GB · 60 fps with 1.4.3</sub> | ![Luigi's Mansion 3](docs/images/luigis-mansion-3.jpg)<br>Luigi's Mansion 3<br><sub>~7.5 GB · 60 fps with 1.4.0</sub> |
| ![Mario Kart 8 Deluxe](docs/images/mario-kart-8-deluxe.jpg)<br>Mario Kart 8 Deluxe<br><sub>~11.9 GB · no Mii heads</sub> | ![Metroid Dread](docs/images/metroid-dread.jpg)<br>Metroid Dread<br><sub>~5 GB</sub> | ![Monster Hunter Rise](docs/images/monster-hunter-rise.jpg)<br>Monster Hunter Rise<br><sub>~22 GB · 60 fps with 16.0.2</sub> |
| ![Monster Train 2](docs/images/monster-train-2.jpg)<br>Monster Train 2<br><sub>~1.6 GB · 60 fps with 1.1.1</sub> | ![Octopath Traveler 0](docs/images/octopath-traveler-0.jpg)<br>Octopath Traveler 0<br><sub>~4 GB · 60 fps with 1.0.5</sub> | ![Paper Mario: The Thousand-Year Door](docs/images/paper-mario-the-thousand-year-door.jpg)<br>Paper Mario: The Thousand-Year Door<br><sub>~5 GB · 60 fps with 1.0.1</sub> |
| ![Patrick's Parabox](docs/images/patricks-parabox.jpg)<br>Patrick's Parabox<br><sub>~0.2 GB</sub> | ![Pikmin 4](docs/images/pikmin-4.jpg)<br>Pikmin 4<br><sub>~10.8 GB</sub> | ![Pokémon Legends: Z-A](docs/images/pokemon-legends-z-a.jpg)<br>Pokémon Legends: Z-A<br><sub>~6 GB</sub> |
| ![Pokémon Sword](docs/images/pokemon-sword.jpg)<br>Pokémon Sword<br><sub>~12.4 GB</sub> | ![Raging Loop](docs/images/raging-loop.jpg)<br>Raging Loop<br><sub>~0.9 GB</sub> | ![Rhythm Heaven Groove](docs/images/rhythm-heaven-groove.jpg)<br>Rhythm Heaven Groove<br><sub>~3 GB</sub> |
| ![ROMANCE OF THE THREE KINGDOMS 8 REMAKE](docs/images/romance-of-the-three-kingdoms-8-remake.jpg)<br>ROMANCE OF THE THREE KINGDOMS 8 REMAKE<br><sub>~16.8 GB · controller only</sub> | ![STEINS;GATE RE:BOOT](docs/images/steins-gate-reboot.jpg)<br>STEINS;GATE RE:BOOT<br><sub>~6 GB</sub> | ![Super Mario 3D World + Bowser's Fury](docs/images/super-mario-3d-world.jpg)<br>Super Mario 3D World + Bowser's Fury<br><sub>~3 GB</sub> |
| ![Super Mario Bros. Wonder](docs/images/super-mario-bros-wonder.jpg)<br>Super Mario Bros. Wonder<br><sub>~4 GB</sub> | ![Super Mario Galaxy](docs/images/super-mario-galaxy.jpg)<br>Super Mario Galaxy<br><sub>~3 GB</sub> | ![Super Mario Galaxy 2](docs/images/super-mario-galaxy-2.jpg)<br>Super Mario Galaxy 2<br><sub>~3 GB</sub> |
| ![Super Mario Odyssey](docs/images/super-mario-odyssey.jpg)<br>Super Mario Odyssey<br><sub>~6 GB</sub> | ![Super Smash Bros. Ultimate](docs/images/super-smash-bros-ultimate.jpg)<br>Super Smash Bros. Ultimate<br><sub>~17.5 GB · offline</sub> | ![Taiko Risshiden V DX](docs/images/taiko-risshiden-v-dx.jpg)<br>Taiko Risshiden V DX<br><sub>~3.3 GB · Japanese · played by touch</sub> |
| ![Unicorn Overlord](docs/images/unicorn-overlord.jpg)<br>Unicorn Overlord<br><sub>~4 GB</sub> |  |  |

## Pack an IPA

Prefer a graphical interface to the command line? **[NX2Apple](https://github.com/LNPANDY/NX2Apple)** is a community fork with a macOS app that runs this whole pipeline for you and packs the game files into a sharded RomFS archive for a slimmer IPA. It is a separate project that follows this one; the steps below are nxnative's own command-line way.

**1. Get nxnative and its tools** (once, on a Mac with [Xcode](https://apps.apple.com/app/xcode/id497799835) and [Homebrew](https://brew.sh)):

```
git clone https://github.com/seedds/nxnative.git && cd nxnative
brew install xcodegen zstd spirv-cross glslang
brew install --cask dotnet-sdk
python3 -m pip install lz4 capstone fonttools "backports.zstd; python_version < '3.14'"
```

**2. Put one game's NSPs in a folder**: the base game (an NSP, or an XCI cartridge dump), plus its update and DLC if you have them:

```
~/Games/MarioWonder/
  Super Mario Bros. Wonder [010015100B514000][v0].nsp
  Super Mario Bros. Wonder [010015100B514800][v327680].nsp
```

Any version works, with or without an update: the script packs the newest version in the folder. DLC is optional too: the DLC in the folder is packed, and without it the game sees none, as on a Switch without it. Each game was tested on one version (an update named in the table above is the one its 60 fps needs). With another version the script builds the game with that version's own shaders and says that the version was not tested; fixes made for the tested version's code, such as the 60 fps ones, don't apply to it.

**3. Run the script:**

```
python3 scripts/build_ipa_from_nsp.py ~/Games/MarioWonder -k ~/keys/prod.keys
```

The IPA appears in the same folder (here `~/Games/MarioWonder/SMBWMobile.ipa`). The first build takes 30–60 minutes, later ones about 10. You need free disk space of about twice the NSPs' size plus 10 GB. Options such as `-o` (output path) and `--app-name` are listed by `-h`.

To build a native macOS application bundle (`.app`) instead of an iOS IPA, pass `--platform mac`:
```
python3 scripts/build_ipa_from_nsp.py ~/Games/MarioWonder -k ~/keys/prod.keys --platform mac
```
This builds a macOS app (e.g. `Super Mario Bros. Wonder.app`) with the game's icon, menu bar, preferences (`⌘,`), and controller support (`open "Super Mario Bros. Wonder.app"`).

**4. Install** the IPA with your sideloading tool (AltStore, Sideloadly, LiveContainer, …). The app gets the game's name, icon and version (the update's version, for example 1.2.1 for Mario Wonder; the base game's when there is no update). Sign it with the **Increased Memory Limit** capability (`com.apple.developer.kernel.increased-memory-limit`) if your tool can: without it iOS gives the app about 3.4 GB and closes the bigger games (Hyrule Warriors: Age of Calamity and Hades II use more, Breath of the Wild and Cuphead come close); with it a recent iPhone gives about 6 GB.

With an Apple developer account you can instead build and install from Xcode: `--prepare-xcode` makes an Xcode project for the game rather than an IPA ([Build and run on a device with Xcode](docs/development.md#build-and-run-on-a-device-with-xcode)).

## Play

**Settings.** A screen before the game starts offers what the game supports: frame rate, render scale, docked or handheld mode, game language, the frame-rate counter and Apple's performance HUD. Tap **Play** to start; the app remembers your choices.

**Touch controls.** Tap **2D** / **3D** at the top to switch layouts; the app remembers your choice. Wonder, Engage, Dread, 13 Sentinels, Link's Awakening, Ace Attorney Investigations Collection, Unicorn Overlord, Rhythm Heaven Groove, Octopath Traveler 0, Pokémon Legends: Z-A, Cuphead, Absolum, Brain Memory Training Academy, The Great Ace Attorney Chronicles, STEINS;GATE RE:BOOT, Big Brain Academy: Brain vs. Brain, Patrick's Parabox, Celeste and Raging Loop start in 2D, the other games in 3D (STEINS;GATE RE:BOOT's quick save is R3, in the 3D layout). In 3D, the right stick (the camera in most games) is not drawn: swipe on the right third of the screen, starting anywhere off the buttons. In Super Mario Galaxy and Super Mario Galaxy 2, other touches outside the controls move the pointer; in Captain Toad: Treasure Tracker they touch the game's screen (touch blocks, stun enemies), and in Big Brain Academy: Brain vs. Brain too (its menus and activities).
A connected game controller hides the touch controls and keeps every button.

**Rumble.** Game vibration uses the iPhone's haptic engine or a compatible connected controller. Controllers with separate left/right haptic outputs keep those effects separate. The launcher's **Game rumble** and **Rumble strength** settings control it independently of **Touch feedback**. Phone rumble approximates the Switch's effects; physical feel and controller compatibility still need device testing.

**Shaking.** Shake the iPhone to shake the controller, in games that read its motion: Super Mario Odyssey's motion moves (such as the cap throw on a shake) need it, and Odyssey only takes them in its default docked mode. A light bump does not count; a sharp flick of the phone does.

- **Game language** is English by default. Pick another when packing with `--language` (`-l`), by code or by name: `-l ja`, `-l zh-Hans`, `-l French` (`LANGUAGE` in Spyder). It must be one the game has; the script prints the game's languages and stops on one it lacks. To change it without rebuilding, pick another on the settings screen.
- **Saves and logs** are in the Files app, under the app's name: `save/` and `log.txt`. When something goes wrong, send `log.txt` (the previous run is `log.prev.txt`).
- **Short stutters** the first time a new scene appears are normal: the app compiles graphics for it once and remembers them for the next launch.
- **Reinstalling:** some sideloading tools give a reinstalled app a new data folder. Copy `save/` and `pso_user.tsv` over from the old one to keep your progress.
- **Settings in a file:** lines like `DM_OPMODE=0` (handheld mode: lower resolution, higher frame rate) in `Documents/env.txt` win over the settings screen, which then shows them greyed out. `DM_FPS_TARGET=30` keeps Breath of the Wild, Tears of the Kingdom and Octopath Traveler 0 at their original 30 fps, which saves battery; `DM_PRESENT_INTERVAL=game` does the same for Monster Train 2. `DM_SHOW_FPS=1` shows the frame rate at the top left (`--show-fps` when packing turns it on from the start). `DM_LAUNCHER=0` skips the settings screen.

## Known issues

- No amiibo and no online features.
- Fire Emblem Engage: after switching away from the app and back, some sounds may stay silent until you restart it.
- Fire Emblem: Three Houses: small square patches on the ground where decals land.
- Super Mario Galaxy and Super Mario Galaxy 2: effects drawn as points or lines come out one pixel wide.

## For developers

How it works, the script's options, running games on the Mac, adding a game, and per-game notes: [docs/development.md](docs/development.md). In-depth bring-up and debugging notes: [docs/agents](docs/agents/README.md).

## License

MIT (see [LICENSE](LICENSE)). Third-party code keeps its own license; see [dm/third_party](dm/third_party/README.md).
