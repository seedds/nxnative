# Playing

How to build and install a game: [README](../README.md#how-to-play).

- **Settings:** a screen before the game starts sets frame rate, resolution, docked or handheld mode, language, rumble and more. Tap **Play**; your choices are remembered. What each one does: [Settings](settings.md).
- **Saves and logs** are in the Files app, under the game's name: `save/` and `log.txt` (the previous run is `log.prev.txt`). When something goes wrong, send `log.txt`.
- **Reinstalling:** some sideloading tools give a reinstalled app a new data folder. Copy `save/` and `pso_user.tsv` over from the old one to keep your progress.

## Touch controls

The gear at the top centre opens the in-game menu: **Controller layout** switches between **2D** and **3D** (and **Tap**, every touch to the game, in handheld mode), and the settings that take effect at once are there too, such as **Picture**, **Console mode** and **Game rumble** ([In-game menu](settings.md#in-game-menu)). The app remembers your choices. Wonder, Engage, Dread, 13 Sentinels, Link's Awakening, Ace Attorney Investigations Collection, Unicorn Overlord, Rhythm Heaven Groove, Octopath Traveler 0, Pokémon Legends: Z-A, Cuphead, Absolum, Blasphemous 2, Brain Memory Training Academy, The Great Ace Attorney Chronicles, STEINS;GATE RE:BOOT, Big Brain Academy: Brain vs. Brain, Patrick's Parabox, Celeste, Raging Loop and Mario vs. Donkey Kong start in 2D, the other games in 3D (STEINS;GATE RE:BOOT's quick save is R3, in the 3D layout).

In 3D, the right stick (the camera in most games) is not drawn: swipe on the right third of the screen, starting anywhere off the buttons. In Super Mario Galaxy and Super Mario Galaxy 2, other touches outside the controls move the pointer; in Captain Toad: Treasure Tracker they touch the game's screen (touch blocks, stun enemies), and in Big Brain Academy: Brain vs. Brain too (its menus and activities).

A connected game controller hides the touch controls, all but the gear, and keeps every button.

In portrait (an iPad, an iPhone Duo's inner screen) the picture sits above the controls; so it does on a Duo half folded, the picture on the upper half. An iPad game also runs in a window of any size (Stage Manager).

## Rumble

Game vibration uses the iPhone's haptic engine or a compatible connected controller. Controllers with separate left/right haptic outputs keep those effects separate. The settings screen's **Game rumble** and **Rumble strength** control it independently of **Touch feedback**. Phone rumble approximates the Switch's effects; physical feel and controller compatibility still need device testing.

## Motion (gyro)

A controller that supplies gyro data through Apple's controller support (DualSense, DualShock 4) drives the game's motion controls, on the Mac and the iPhone: aiming and tilt puzzles in games that read the controller's motion, such as Breath of the Wild with its Motion Controls option on. A controller without one (Backbone One, Razer Kishi and other controllers the phone mounts in) uses the iPhone's own gyro, as a Switch in handheld. Motion follows the controller in docked mode, the default.

`DM_PHONE_MOTION=0` in `Documents/env.txt` stops the phone's gyro, `DM_MOTION=0` all motion. If motion misbehaves, pack with `--no-motion` (`MOTION = False` in Spyder) for an app without it; `DM_MOTION=1` in `Documents/env.txt` turns it back on.

Joy-Con 2 and Pro Controller 2 gyro come over Bluetooth LE ([Switch 2 controllers](#switch-2-controllers-mac)). Switch Pro Controller gyro on the Mac still needs the HID support being developed in [PR #296](https://github.com/seedds/nxnative/pull/296).

**Shaking.** Shake the iPhone to shake the controller, in games that read its motion: Super Mario Odyssey's motion moves (such as the cap throw on a shake) need it, and Odyssey only takes them in its default docked mode. A light bump does not count; a sharp flick of the phone does.

## Game language

English by default. Pick another when packing with `--language` (`-l`), by code or by name: `-l ja`, `-l zh-Hans`, `-l French` (`LANGUAGE` in Spyder). It must be one the game has; the script prints the game's languages and stops on one it lacks. To change it without rebuilding, pick another on the settings screen.

## Settings in a file

Lines like `DM_OPMODE=0` (handheld mode: lower resolution, higher frame rate) in `Documents/env.txt` win over the settings screen, which then shows them greyed out ([the full list](settings.md#settings-in-a-file)). `DM_FPS_TARGET=30` keeps Breath of the Wild, Tears of the Kingdom, Octopath Traveler 0 and Octopath Traveler II at their original 30 fps, which saves battery; `DM_PRESENT_INTERVAL=game` does the same for Monster Train 2. `DM_SHOW_FPS=1` shows the frame rate at the top left (`--show-fps` when packing turns it on from the start). `DM_LAUNCHER=0` skips the settings screen.

## On the Mac

A Mac app shows the game's settings in a window before the game starts ([The Mac settings window](settings.md#the-mac-settings-window)). Its `env.txt` is `~/Library/Application Support/nxnative/<title ID>/env.txt`, and its log is `~/Library/Logs/nxnative/<title ID>.log`. [NX2Apple](https://github.com/LNPANDY/NX2Apple)'s library can also start it with the settings you pick there.

A controller macOS pairs but Apple's controller support does not list, such as an 8BitDo in its own mode (Ultimate 2C Wireless over Bluetooth), is read as a raw HID gamepad, with full analog sticks: it takes a player as any other controller does, and the log says `controller '<name>' (HID) connected`. A pad Apple's support says it will list gets two seconds to appear there first, so it is never read twice (an Xbox, PlayStation or Nintendo controller is always left to Apple's support); if it never appears, it is read as HID after that wait, and the log says so. Its buttons follow 8BitDo's numbering by default; another maker's pad may number them differently, and the log then says so. `DM_HID_MAP=A=1,B=2,X=4,Y=5` in `env.txt` moves them (here: the printed letters instead of the positions), `DM_HID_LOG=1` logs the number of each button pressed, and `DM_HID_PADS=0` turns this off.

Two identical controllers that Apple's controller support lists as one, such as two Pro Controllers over USB, are each a player of their own: after 3 seconds the log says `GameController lists 1 of 2`, and each takes a player, by USB port. They have no vibration, player lights or Home button to quit. `DM_PAD_IDENTICAL=0` in `env.txt` turns this off.

With [Press a button to join](settings.md#press-a-button-to-join) on in the settings (`DM_PAD_PRESS_TO_JOIN=1` in `env.txt`), on the Mac and on iOS, a controller becomes a player only when one of its buttons is pressed, as the lowest player free: the players choose their order and how many play, over Bluetooth or USB, identical pads included, and a controller connected while the game runs joins at its first press. The log says `press a button to play`, then `a button pressed: player N`. The press that joins also reaches the game. When a game asks for more players than it sees, a prompt over the game says how many are wanted and joined and waits for the others: a controller that presses a button (or connects, with the setting off) joins, A finishes once enough are in, and B or Escape closes it, as on a Switch.

To quit with a controller, hold its Home button (the logo button on an Xbox or PlayStation controller) for 2 seconds, then press A to quit or B to go back to the game. Quitting this way is the same as ⌘Q: progress the game has not saved is lost. If holding Home opens the Games app instead, macOS keeps the Home button for itself: its controller settings for the Home button decide whether apps may use it. `DM_HOME_QUIT=0` in `env.txt` turns the hold off.

### Switch 2 controllers (Mac)

Joy-Con 2 and the Pro Controller 2 do not appear in macOS's Bluetooth settings: they speak Nintendo's own Bluetooth LE protocol. Turn on **Switch 2 controllers** in the game's settings window ([Switch 2 controllers](settings.md#switch-2-controllers)), then:

1. **Pair a controller once, in the game.** While the game runs, hold the controller's small Sync button until its player lights sweep. The game pairs it and it plays; the log says `paired with this Mac`. A controller paired this way forgets the console it was paired with: pair it there again to use it with one.
2. **From then on, press any button on it** while a game with the setting on is running: it wakes and connects, no Sync needed. A controller woken for a Switch 2 nearby is left alone.

**Pairs and singles.** A left and a right Joy-Con 2 that connect play together as one pair. To make one of a pair a player of its own, held sideways, hold its **SL and SR** (the small rail buttons) for three seconds; to pair two single Joy-Cons again, hold **L** on the left one and **R** on the right one together for three seconds. The player lights show the new player numbers. A game that asks for more players than there are controllers splits a pair itself, as on a Switch (Super Mario Bros. Wonder's two-player screen). The game sees Joy-Cons as Joy-Cons: a pair is a dual controller with a sensor in each Joy-Con, a single one a sideways Joy-Con with its own sensor and rail buttons; a game that knows no Joy-Cons reads them as a Pro Controller. The gyro works, and so does rumble: each Joy-Con of a pair plays its side, a Joy-Con alone plays both, and **Game rumble**, **Rumble strength** and **Rumble cutoff** apply. Hold **Home** for two seconds for the quit prompt, as on other controllers. Each game app asks for Bluetooth access once. An app packed again asks again: macOS ties the answer to the app's signature, and the NSP script signs each build anew, so the same bundle ID does not carry it over. Not yet: the Joy-Con 2's mouse sensor. The Pro Controller 2 is untested.

The mouse pointer hides when you press a key or a controller button, or move a stick, while it is over the game picture. Moving the mouse shows it again, as does switching to another window or app.

## Shaders on first sight

Short stutters the first time a new scene appears are normal: the app compiles graphics for it once and remembers them for the next launch. What it remembered, and what the game's list names, is built before the game starts, behind "Preparing shaders" and a progress bar when that takes more than half a second.
