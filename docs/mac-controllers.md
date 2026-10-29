# Mac controllers

## Controllers and mapping

Connect a supported controller and press a button, then open
**Controllers → Button Mapping…**.
Select the controller and assign each physical button to a Switch button,
Shake, or Unassigned. **Nintendo Positions** is the default (right face button
is A); **Xbox Labels** makes the bottom face button A. Changes take effect
immediately. The first 16 button choices apply to **every controller for this
game**, using the same saved map as an external launcher. Rear buttons, pad
clicks, Guide/Share, trackpad modes and motion choices are saved per model in
the same app preferences domain. Extra buttons are usable only when the
controller/driver exposes them. A rear button mapped to A sends Switch A;
it is not remapped a second time by the standard button map.

The panel shows when standard buttons are fixed by a launch override or
`env.txt`; remove that override and restart to edit them. With
`DM_SAVED_CHOICES=0`, the panel starts with defaults and edits last only for
that session. Opening the panel reads the latest saved choices again.

Older builds saved mappings in `controllers.plist` under `DM_ROOT`, or
`~/Library/Application Support/org.nxnative.controllers/` without `DM_ROOT`.
If one exists, **Import previous mapping for this model** imports the selected
model's buttons, trackpads and motion choices. Its standard buttons become
the game's shared map for all controllers. Import is explicit because two
old model maps can disagree, or a launcher may already have saved choices.
The old file is kept unchanged; new edits use the shared preferences.

Shake drives that controller's six-axis sensor and follows its player assignment.
In one-player games it merges into player 1 with the controller's other input,
including when `DM_KBD_PLAYER=own` reserves the first slot for the keyboard.
A brief press completes one shake cycle; holding repeats it.

The existing eight-player allocation is retained: controllers take separate
slots. Press a button or move a stick to join with a GameController device;
reconnecting controllers reclaim a free slot of their model, and a
disconnect releases all held input. The keyboard shares player 1 by default.
`DM_KBD_PLAYER=own` reserves player 1 for the keyboard; `DM_PAD_MERGE=1` combines
controllers into player 1. Two identical models may exchange slots on reconnect.

## Steam Controller (2026)

The Mac host includes a direct IOKit HID adapter for Valve's 2026 **Triton**
controller. It takes exclusive ownership of validated Triton HID interfaces for
trackpads, motion and rumble, before registering native input. Matching native
profile names is unnecessary: identical controllers and separate Puck
interfaces keep their raw features. Empty Puck interfaces and cached native
profiles never claim a player slot. Other controllers use Apple's GameController.

The Triton adapter supports:

- USB-C, Bluetooth, and paired Puck controller interfaces (Valve USB IDs
  `28de:1302`, `1303`, `1304`, `1305`).
- Both sticks and stick clicks, face buttons, D-pad, shoulders, triggers,
  Menu/View, Guide/Quick Access, four rear buttons and both trackpad clicks.
- Both raw sticks use a 10% radial deadzone so resting drift does not suppress
  gyro aiming. Remaining travel is rescaled to full range. Set
  `DM_STEAM_DEADZONE` to a fraction from 0 to below 0.9 to adjust it (0 disables
  it); invalid values use 0.10. Trackpad coordinates are unaffected.
- Rear buttons and trackpad clicks can be assigned in the same mapping panel.
  Their default is Unassigned.
- Left trackpad defaults to **D-pad**, right trackpad to **Switch touchscreen**.
  In the mapping panel either can select Off, D-pad, Left stick, Right stick or
  Switch touchscreen. Touching the pad presses a finger at its absolute screen
  position; moving drags, lifting releases. Mouse and pad fingers can coexist.
  A game must read the Switch touchscreen for this mode to have an effect;
  some games do so only in handheld mode (`DM_OPMODE=0`).
- Gyro and accelerometer feed the game's six-axis input. **Motion controls**
  enables it; **Recenter gyro** resets the integrated orientation. Orientation
  uses raw gyro integration without drift correction, so recenter when needed.
  Missing samples expire after 250 ms; focus loss and disconnect clear motion.
- Left/right rumble is adapted from the game's vibration amplitudes, with a
  40 ms refresh for the firmware watchdog. It stops when the game window loses focus (including the mapping panel or
  minimization), on disconnect and normal exit. `DM_RUMBLE=0` disables it; `DM_RUMBLE_GAIN` adjusts strength
  (default 1, range 0–3). This approximates vibration; it does not reproduce
  Switch HD Rumble waveforms or add separate trackpad texture effects.
  `DM_RUMBLE_MIN` (default 0, range 0–0.9) silences controller intensities at or
  below the threshold and rescales the remaining range, after applying gain.

The adapter runs without Steam and temporarily disables desktop mouse/keyboard
emulation while connected to the game; the controller's watchdog restores it
after exit. The app owns those controller interfaces until it closes; other
apps cannot receive their input during that time. To use Steam's input route
instead, set `DM_STEAM_HID=0` before starting the game. If exclusive access fails,
native controls remain available, but raw rumble, gyro and trackpads cannot be
guaranteed. Quit Steam, then follow [Input Monitoring and rebuilt apps](#input-monitoring-and-rebuilt-apps)
to check the permission and reconnect. Errors appear in the app log.

Connecting a Steam Controller preserves other controllers' player slots, held
buttons/sticks and rumble. Native admission scans for raw interfaces before a
player joins, so delayed notifications cannot create duplicate players. In merge
mode the current rumble owner keeps its route until it disconnects.

Composite native and raw interfaces are opened together. A failed open releases
only that controller's group and keeps it on native fallback until the whole
group disconnects. Puck ports remain independent. If native input joins before
its raw sibling is available, that connection also stays native until reconnect.

The adapter uses USB device/interface ancestry or macOS's physical-device ID,
never profile names. If macOS omits that identity, it keeps native fallback and
blocks conflicting new raw claims. If an unidentified native interface appears
while a compatible raw route is already open, it defers new native admissions
until that ambiguity clears, preserving existing players. Restart with
`DM_STEAM_HID=0` to use only native input on such a driver. The log identifies
missing identity and exclusive-open failures.

For pairing, firmware updates and Puck setup, follow
[Valve's 2026 controller guide](https://help.steampowered.com/en/faqs/view/33E8-5EDF-24E6-4CFB).
The original 2015 Steam Controller is a different protocol and is not handled
by this adapter.

Validation uses real GameController snapshots, synthetic USB/Bluetooth/Puck
reports, sensor units/timestamp wraps, guest six-axis history, rumble output and
stop/refresh checks, trackpad touches, identical native profile names, stale
native events, exclusive HID ownership/failure/removal, player-slot checks and
saved-map round trips (including older button-only maps). Those
tests do not establish physical-device compatibility. A [PR #221 hardware
report](https://github.com/seedds/nxnative/pull/221#issuecomment-6428699708) confirms
USB-C buttons, sticks and rumble in TotK 1.4.3, and gyro aiming with the proposed
10% deadzone. This revision still needs hardware retesting; trackpads, rear
buttons, mapping persistence and other connection modes were not covered by
that report.

Native GameController motion and passive Switch Pro Controller motion use the
upstream motion system and follow the same player slots as mapped buttons.
Steam Controller history participates in that system's source selection:
usable native motion takes precedence, and iPhone builds retain their phone
motion selection. `DM_MOTION=0` disables all physical motion, including Steam
Controller motion. Focus loss and disconnect clear the affected source.
Native, Pro Controller and phone motion keep their live orientation during
mapped Shake, adding the shake acceleration. Direct Steam motion retains its
whole synthetic Shake cycle, then resumes its recorded physical samples.

## Shared launcher controls

The [controls coordination comment on PR #221](https://github.com/seedds/nxnative/pull/221#issuecomment-6441650311)
separates controller hardware support from the shared mapping model proposed in
[issue #388](https://github.com/seedds/nxnative/issues/388). The Triton report
reader supplies physical buttons, sticks, trackpads and motion; the mapping
layer turns those inputs into Switch controls. A launcher UI does not need to
implement or change the hardware protocol.

The launcher and host share the [saved-choices store](frontends.md#saved-choices)
and `input_map.h` parser from [PR #386](https://github.com/seedds/nxnative/pull/386).
`launcher.DM_KEYMAP` changes keyboard functions; `launcher.DM_PAD_MAP` changes
standard controller buttons. The controller panel writes the latter key,
removing it when Nintendo Positions restores the default. It does not change
the keyboard map. `Shake` is an additional controller target so existing
per-player Shake bindings remain expressible (`A=Shake`); stick directions
are keyboard functions, not controller button sources or targets.

The domain is the app's `CFBundleIdentifier`, including a custom bundle ID;
a standalone host uses `org.nxnative.<compiled title ID>` with the ID in
lowercase, matching the packer's default bundle ID. Launch environment
and `env.txt` values take precedence and remain temporary. Saved values must
pass the shared parser as a whole. `DM_SAVED_CHOICES=0` ignores saved controls
and prevents writes; `DM_LAUNCHER=0` skips the launch window but still applies
them. See [controller-specific choices](frontends.md#controller-specific-choices)
for the extra-input data in that same domain. Transport discovery, report
decoding, rumble and six-axis sampling remain independent of these choices.

The hardware report also leaves gyro scale calibration unconfirmed. It notes
that TotK resets its aim reference at an exactly identity orientation, including
a recenter; moving the controller resumed aiming in that test. This branch
keeps a truthful identity pose instead of fabricating a rotation. The tested
fix for aiming suppressed by resting sticks is the radial deadzone.

## Input Monitoring and rebuilt apps

macOS can require **System Settings → Privacy & Security → Input Monitoring**
permission for the Steam Controller's direct HID interfaces. Bluetooth pairing
does not grant this permission. Native buttons may still work when direct HID
access is denied, while rumble, gyro or trackpads are unavailable. Apple explains
the permission in [Control access to input monitoring on Mac](https://support.apple.com/guide/mac-help/control-access-to-input-monitoring-on-mac-mchl4cedafb6/mac).

1. Quit the game completely and quit Steam, so another process is not holding
   the controller.
2. Open **System Settings → Privacy & Security → Input Monitoring** and enable
   the game app you actually launch. If it is missing, use **+** to select that
   `.app` from its current location. For a standalone `dmhost` launched from a
   terminal, check the terminal application that launched it, such as Terminal
   or iTerm, instead; macOS can attribute the access to that application.
3. Accept **Quit & Reopen** if offered, or fully quit and relaunch the affected
   app yourself. Reconnect the controller and bring the game window into focus.
   Press a button, then open **Controllers → Button Mapping…** to confirm that
   the controller is listed and check its assignments. Close the panel and test
   input and rumble in the game; input and rumble are deliberately suspended
   while the mapping panel has focus.

If the controller worked before rebuilding or replacing the app, an enabled
entry may still refer to the previous build. The Mac packer signs ad-hoc by
default, and that signature identifies a particular version of the code;
permission may need to be granted again after a rebuild. Keeping the same app
name or bundle ID alone does not guarantee that macOS recognizes the new code
as the previously authorized app. See Apple's
[code identity explanation](https://developer.apple.com/documentation/technotes/tn3127-inside-code-signing-requirements#Designated-requirement).

With the rebuilt app closed, turn its Input Monitoring switch off and on, then
relaunch. If access is still denied, select its old entry and remove it with
**−**, use **+** to add the rebuilt `.app` from the exact path you now launch,
and enable it again. This matters when keeping old and new builds for comparison:
check the selected app's location rather than relying on identical display names.

If the entry remains stale, reset only this app's Input Monitoring decision from
Terminal. Replace the example path with the rebuilt app's absolute path:

```sh
app="/absolute/path/to/My Game.app"
bundle_id=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$app/Contents/Info.plist") &&
    test -n "$bundle_id" &&
    /usr/bin/tccutil reset ListenEvent "$bundle_id"
```

The reset removes the decision; it does not grant permission. Reopen the app,
enable it in Input Monitoring, then quit and reopen once more if requested.
The command is scoped to the bundle ID read from that app; copies with the same
ID share that scope. It does not clear other permissions or other apps' entries.
For a Terminal-launched host, reset/re-enable the terminal application's entry
instead. Apple documents `ListenEvent` and the per-app reset in
[Resetting access to protected resources](https://developer.apple.com/documentation/xcode/resetting-access-to-protected-resources-in-macos).

If the controller still does not appear, check the [Mac app log](development.md#a-mac-app)
at `~/Library/Logs/nxnative/<title ID>.log` (or the terminal output for a standalone
host). `Steam Controller HID access unavailable` and
`cannot exclusively open Steam Controller` point to HID access or another app
holding the device; neither message alone proves a permission problem. Also
check that `DM_STEAM_HID=0` is not disabling the direct adapter. If Bluetooth
input fails in Steam and other games too, troubleshoot the Mac's connection
first; reconnecting or restarting macOS resolved that separate issue during
testing. For repeated development builds, the packer's existing `--mac-sign`
option can use a stable signing identity; see [Mac app signing](development.md#a-mac-app).
