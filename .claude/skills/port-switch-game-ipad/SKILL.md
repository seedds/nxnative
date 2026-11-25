---
name: port-switch-game-ipad
description: Build a Switch game you own (Super Mario Bros. Wonder as the worked example) as a native iPad app with nxnative and install it on your own iPad from Xcode. Use when someone asks to port, build, install or update a Switch game on their iPad or iPhone.
---

# Port a Switch game to your iPad

This is the procedure that built Super Mario Bros. Wonder for an iPad Pro, written so that one person can repeat it on their own Mac, with their own copy of the game, for their own iPad. Everything here applies to any game in the [games table](../../../dm/games/README.md#games); the example values are Wonder's. The general documentation is [README.md](../../../README.md) (How to play, Install, Switch 2 games) and [docs/development.md](../../../docs/development.md) (The packing script in detail, Build and run on a device with Xcode, Quick reinstalls); read those when something here does not fit the game at hand.

## Before starting

- **What you need**: a Mac with Apple silicon on macOS 26.6 or later, Xcode 27.2 with the iOS platform and the Metal toolchain installed, Homebrew, Python 3.12 or newer (arm64), an Apple ID signed into Xcode, an iPad with an M1 chip or later on iPadOS 26 or later, a USB cable, and about 2× the game's size plus 10 GB free on an APFS drive.
- **Your game files**: dumped from your own cartridge or console with your usual homebrew dumping tools (this skill does not cover dumping). Two routes:
  - **Switch 1 NSPs** (base game and its update, `.nsp`/`.xci`/`.nsz`/`.xcz`) plus your console's `prod.keys`. Wonder needs both the base game and the 1.2.1 update in the folder: its shader tables cover both versions. Put them in one folder, e.g. `~/Games/MarioWonder/`.
  - **A Switch 2 Edition NUP** (`.nup`, one container with the Switch 1 program, its update and the Switch 2 add-on): no keys needed. Wonder's Switch 2 program (`040015100B514001`) is still open, so pack the Switch 1 program with `--program 0`.
- **Keys and game files never go into the repository, the home folder or a chat.** Keep them in the game's folder; the build folder goes beside it or on an APFS volume of your choice. Never print key values.
- **Free vs paid Apple account**: a paid developer account signs the app for a year and can provision the Increased Memory Limit entitlement, which the bigger games need. A free account signs for 7 days (reinstall from Xcode afterwards; the save stays in the app's Documents while the bundle ID is unchanged) and cannot provision that entitlement: prepare with `DM_ENTITLEMENTS=` (empty) in the environment of the build command, and expect iOS to close a game whose memory exceeds the default limit. Wonder uses a 5 GB guest layout, so a paid account is the safe choice.

## 1. Tools (once)

```
sudo xcode-select -s /Applications/Xcode.app
xcodebuild -downloadComponent MetalToolchain
git clone https://github.com/seedds/nxnative.git && cd nxnative
brew install xcodegen zstd glslang libimobiledevice
python3 -m venv .venv
.venv/bin/python3 -m pip install lz4 "capstone>=5,<6" fonttools nsz zbic "backports.zstd; python_version < '3.14'"
export DM_PYTHON="$PWD/.venv/bin/python3"
```

`libimobiledevice` gives `afcclient`, which the RomFS push below needs for a large image. Run every later command from the repository root with `DM_PYTHON` set (put the `export` in your shell profile).

## 2. Find the iPad's UDID and enable Developer Mode

Connect the iPad by USB, unlock it, tap **Trust**, then:

```
xcrun devicectl list devices
```

Note its identifier (`00008103-…` for an M1 iPad). On the iPad, **Settings > Privacy & Security > Developer Mode** must be on (it appears after the first Xcode contact; the iPad restarts).

## 3. Build and prepare the Xcode project

One command does the extraction, the code rewrite, the shader translation and compilation for iOS, the pipeline archive for the iPad's GPU and the Xcode project. Choose a bundle ID you own (reverse-DNS with your team's prefix; keep it for every later build of the same game, another ID is another app with its own saves) and your team ID (Xcode > Settings > Apple Accounts > your team, the 10-character code).

Switch 1 NSPs:

```
"$DM_PYTHON" scripts/build_ipa_from_nsp.py ~/Games/MarioWonder -k ~/Games/keys/prod.keys \
  --platform ios --prepare-xcode --romfs separate \
  --xcode-workspace ~/Games/NxNative.xcworkspace --work-dir ~/Games/MarioWonder_build \
  --team-id ABCDE12345 --bundle-id com.example.nxnative.wonder \
  --gpu g13g --language es --progress lines
```

A Switch 2 Edition NUP, Switch 1 program:

```
"$DM_PYTHON" scripts/build_ipa_from_nsp.py "~/Games/Super Mario Bros Wonder NUP" --program 0 \
  --platform ios --prepare-xcode --romfs separate \
  --xcode-workspace ~/Games/NxNative.xcworkspace --work-dir ~/Games/MarioWonder_build \
  --team-id ABCDE12345 --bundle-id com.example.nxnative.wonder \
  --gpu g13g --language es --icon "~/Games/Super Mario Bros Wonder NUP/icon.png" --progress lines
```

- `--gpu g13g` is the M1 family (`applegpu_g13g`): the app carries the pipelines made for that GPU, so the first start does not compile them. An M2 is `g14g`, an M4 `g16g`, an A19 Pro iPhone `g18p`; the app's log names the GPU it runs on when it has no archive for it. Several are allowed, comma-separated; each adds build time and size.
- `--romfs separate` keeps the game files out of the app: the install is small and the files are pushed once (step 5). Without it the whole game goes into the app at each install.
- `--language es` picks the game language (a code the game supports; English by default). `--icon` only matters for a NUP or dump without control data.
- `--work-dir` on an APFS volume: a build on an exFAT drive is many times slower.
- The first build takes 30 to 60 minutes (the iOS shaders are most of it); a rebuild reuses everything unchanged. It ends by printing the `push_romfs.sh` command of step 5.
- Optional: `--env-file settings.txt` bakes host switches into the app (one `KEY=VALUE` per line). The build above used, on an M4: `DM_OPMODE=1` (docked mode), `DM_FPS_TARGET=60`, `DM_PRESENT_SNAP=1`, `DM_PSO_MTL4=1`, `DM_DYNAMIC_RESOLUTION=0`, `DM_RENDER_SCALE=1.0`. On an M1 leave the two resolution keys out and let the game scale. The player can still change these in the app's `Documents/env.txt` (Files app) and in the in-app settings.

## 4. Sign and install from Xcode

Either open the workspace and press Run (the documented route: `open ~/Games/NxNative.xcworkspace`, select the scheme **Super Mario Bros. Wonder (010015100B514000)**, the DmIOS target's **Signing & Capabilities** with **Automatically manage signing** and your team, the iPad as destination, Run), or do the same from Terminal:

```
xcodebuild -workspace ~/Games/NxNative.xcworkspace \
  -scheme "Super Mario Bros. Wonder (010015100B514000)" -configuration Release \
  -destination generic/platform=iOS -allowProvisioningUpdates -allowProvisioningDeviceRegistration \
  DEVELOPMENT_TEAM=ABCDE12345 DM_COPY_ROMFS=NO build
xcrun devicectl device install app --device <UDID> \
  ~/Games/MarioWonder_build/xcode-derived-data/Build/Products/Release-iphoneos/DmIOS.app
```

The first `xcodebuild` with a new Apple ID may ask for the account in Xcode's settings first. "Bundle identifier unavailable" means another account registered that ID: pick another and prepare again with the new `--bundle-id`. The iPad must be unlocked for the install and the launch.

## 5. Push the game files (once per game version)

```
scripts/push_romfs.sh <UDID> com.example.nxnative.wonder ~/Games/MarioWonder_build/xcode/Gen/resources.env
```

It streams the RomFS image (about 3.6 GB for Wonder) to the app's `Documents/romfs.img` over USB at about 30 MB/s and validates it. Repeat it only after a new game version or a fresh extraction, and after deleting the app (deleting the app deletes its Documents). The app refuses an image its build was not made for and says which push command to run.

## 6. Play, settings, saves

- Launch from the home screen, or `xcrun devicectl device process launch --terminate-existing --device <UDID> com.example.nxnative.wonder`.
- A Bluetooth controller is player 1 when connected; otherwise the touch gamepad. Controls, saves and settings: [docs/playing.md](../../../docs/playing.md).
- The app's `Documents` folder (Files app > On My iPad > the game) holds `env.txt` (host switches, e.g. `DM_PRESENT_UPSCALE=metalfx` and `DM_UPSCALER=metalfx` for MetalFX upscaling, `DM_FPS=1` for a frame counter), the save under `save/`, and `log.txt`. Anything wrong is a `[RED]` line in `log.txt`: search [docs/agents/symptoms.md](../../../docs/agents/symptoms.md) for its text.

## 7. Updating the app later

After a `git pull` (new host or shader chain), run the step 3 command again (it rebuilds only what changed and asks for a new preparation when the shader chain changed), then step 4. Keep the same bundle ID. The game files on the iPad stay valid unless the game version changed.

## Sharing a change

Only the repository's owner can push to `seedds/nxnative`, and the network at hand may block SSH to GitHub. Keep the HTTPS clone, sign `gh` in with your own GitHub account (`gh auth login -h github.com -p https -w`, then `gh auth setup-git`), fork the repository once (`gh repo fork seedds/nxnative --clone=false`, then `git remote add fork https://github.com/<you>/nxnative.git`), and for each change start a branch from `origin/main`, commit as [06-dev-loop.md](../../../docs/agents/06-dev-loop.md#8-commit-messages) says, push it to the fork and open the pull request against `seedds/nxnative:main` (`gh pr create --repo seedds/nxnative --head <you>:<branch>`). A branch that carries older unmerged work stays separate: cherry-pick the one change onto a fresh branch and regenerate `dm/gfx/nvn/dmsh_chain.h` there when the change touches the shader chain, since the chain hash depends on main's current tools. [PR #3348](https://github.com/seedds/nxnative/pull/3348) was made this way, and this skill arrives with it.

## Another game

Run step 3 on the other game's folder. A game with a profile in `dm/games/` builds as above (the games table says which were tested on an iPad or iPhone). A game without one gets a survey and a provisional profile, and its build may not boot: see [Port another game](../../../docs/development.md#port-another-game).
