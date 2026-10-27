# Deploying to your own device

With an Apple developer account you never have to touch Xcode's interface: the whole path — prepare, sign, build, install, launch, read the log back — runs from the command line. That is what this page describes, together with every failure seen on the way and what its message actually means. For the unsigned-IPA route and sideloading tools, see the [README](../README.md#pack-an-ipa); for the Xcode project itself, [Build and run on a device with Xcode](development.md#build-and-run-on-a-device-with-xcode).

## Before you start

```bash
xcode-select -p                      # must print /Applications/Xcode.app/Contents/Developer
xcrun devicectl list devices         # your device, Reality "physical", State "connected"
```

`xcode-select -p` printing `/Library/Developer/CommandLineTools` means `xcodebuild` and `devicectl` do not exist as far as the shell is concerned, whatever Xcode is installed; `sudo xcode-select -s /Applications/Xcode.app` fixes it once.

Two things about where the game folder lives, both of which cost a whole build when they are wrong:

- **No colon anywhere in its path.** A colon breaks every .NET tool of the build in a way that looks like a broken .NET installation: [A colon in the game folder's path stops every .NET tool](reference/packing.md#a-colon-in-the-game-folders-path-stops-every-net-tool).
- **Not inside Desktop or Documents** if iCloud Drive syncs them. The build cannot be signed there, and the sync uploads your game dumps and `prod.keys`: [A folder synced by iCloud Drive cannot be code signed](reference/packing.md#a-folder-synced-by-icloud-drive-cannot-be-code-signed). `xattr -l ~/Desktop` naming `com.apple.file-provider-domain-id` means the folder is synced.

On the device: Settings → Privacy & Security → **Developer Mode** on, then restart it. Leave it unlocked and plugged in for the whole install, and set auto-lock to Never if the transfer is long.

## The four commands

Collect three values first:

```bash
defaults read com.apple.dt.Xcode IDEProvisioningTeamByIdentifier | grep teamID   # your team, after adding your Apple ID in Xcode > Settings > Accounts
xcrun devicectl list devices                                                      # the device UDID
xcodebuild -workspace "<game folder>/../NxNative.xcworkspace" -list               # the scheme name, after the first prepare
```

Then:

```bash
python3 scripts/build_ipa_from_nsp.py "<game folder>" -k "<prod.keys>" --prepare-xcode --team-id <TEAM> --bundle-id <an identifier you own>
xcodebuild -workspace "<parent>/NxNative.xcworkspace" -scheme "<scheme>" -configuration Release -destination "id=<UDID>" -allowProvisioningUpdates -allowProvisioningDeviceRegistration build
xcrun devicectl device install app --device <UDID> "<game folder>/<title ID>_build/xcode-derived-data/Build/Products/Release-iphoneos/DmIOS.app"
xcrun devicectl device process launch --device <UDID> <your bundle id>
```

`--team-id` and `--bundle-id` are what keep Xcode's Signing & Capabilities pane out of the way: the generated project already carries your team, and the first build creates the certificate and the provisioning profile for you. Both flags are kept when you prepare the game again.

Reading the log back without the Files app:

```bash
xcrun devicectl device copy from --device <UDID> --domain-type appDataContainer --domain-identifier <your bundle id> --source Documents/log.txt --destination ./log.txt
```

## What the error messages really mean

`xcodebuild` reports most device problems as the same timeout, with the real reason buried in the block below it:

```
xcodebuild: error: Timed out waiting for all destinations matching the provided destination specifier to become available
        Destinations compatible with the "…" scheme:
                { platform:iOS, … error:Developer Mode disabled … }
```

Read that `error:` — it is written without a space after the colon, so a `grep "error: "` misses it. Its three usual values:

| What it says | What to do |
|---|---|
| `Developer Mode disabled` | enable it on the device and restart the device |
| `needs to be unlocked to enable development services` | unlock it, and keep it unlocked |
| the device is absent from the list | `xcrun devicectl list devices` shows `connected (no DDI)`: the developer disk image is not mounted yet. Any developer operation triggers the mount — `xcrun devicectl device info apps --device <UDID>` is enough, after which the state becomes `connected` |

The two signing failures have nothing to do with the device:

- `Device "…" isn't registered in your developer account` — `-allowProvisioningUpdates` creates profiles but does not register devices. Add `-allowProvisioningDeviceRegistration`.
- `The app identifier "org.nxnative.…" cannot be registered to your development team because it is not available` — the profile's default identifier belongs to someone else's account. Pass your own with `--bundle-id`, and keep it afterwards: another identifier is a separate app with separate saves.

And one that is not about signing at all although `codesign` reports it: `resource fork, Finder information, or similar detritus not allowed` means the build sits in an iCloud-synced folder (above).

## Before you diagnose anything

Update your checkout. A clone a few weeks behind will reproduce bugs that are already fixed upstream, and a patch written against the stale copy conflicts on the next pull.

```bash
git fetch origin && git status -sb
```

Preparing a game again after an update is cheap: extraction and the compiled shaders are reused as long as the chain version in `dm/gfx/nvn/dmsh_chain.h` and the profile's own tables did not change, so only what actually changed runs. Compare them against `origin/main` before deciding that an update will cost you a full shader build.

## Driving it with Claude

Claude Code can run this whole path, and the command-line route above exists partly because of what it cannot do.

It can prepare the game, read and explain the build logs, drive `xcodebuild` and `devicectl`, pull `log.txt` off the device and tell you what the `[RED]` lines mean, and fix the repository when the failure is in it.

It cannot type into Xcode: an IDE is granted click-only access, so menus, checkboxes and buttons are reachable but no field is. It will not enter your Apple ID or any password, and it cannot touch the device — Developer Mode, unlocking and trusting the Mac are yours. A pop-up menu such as the signing Team or the run destination is also out of reach while it works in the background, which is why `--team-id`, `--bundle-id` and `-destination id=…` replace them.

What makes it effective: give it the log rather than a description of the symptom. "It fails" costs a round of guessing; the log line names the cause. Tell it which device you are targeting and which account, and let it verify the state itself — it will read `devicectl`, the keychain and Xcode's preferences rather than ask you to.
