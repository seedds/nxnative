# Front ends

A graphical app that packs games, keeps a library of them or starts them is a project of its own that builds on nxnative. [NX2Apple](https://github.com/LNPANDY/NX2Apple), [NX Play](https://github.com/varyform/nx-native-wrapper) and [NxNative Launcher](https://github.com/rennan101/nxnative_Launcher) are such apps. This page lists what such an app can rely on: how it runs a build and follows it, what a built app says about its game's settings, and how it starts a game with them. None of it needs a change to nxnative; when a front end needs more, the piece it needs is an issue or a small PR here.

- [Building: the NSP script](#building-the-nsp-script)
- [The settings list](#the-settings-list)
- [Starting a game](#starting-a-game)
- [An app that hosts the engine](#an-app-that-hosts-the-engine)
- [Following main](#following-main)
- [What a front end cannot see yet](#what-a-front-end-cannot-see-yet)

## Building: the NSP script

`scripts/build_ipa_from_nsp.py <NSP folder>` builds one game: an IPA by default, a Mac app with `--platform mac-app`, at the path `-o` names. Its options are in [The packing script in detail](development.md#the-packing-script-in-detail) and `--help`.

For the optional same-process Mac host, build `make -C dm/host GAME=<title ID> dmhost.dylib` and pass `--host-library dm/host/dmhost.dylib` alongside `--host dm/host/dmhost` to `dm/mac/pack_app.py` (plus its usual game-resource arguments). The library is optional; it must match the host executable's SHA-256, profile and bridge ABI. The Mac-app packing script supplies this pair; omitting the library retains child-process startup.

With `--progress json` it asks nothing and writes one JSON object per line on stdout. Every object has `ev` (the event) and `t` (Unix time, seconds):

| `ev` | Fields | When |
|---|---|---|
| `stage` | `k`, `n` (stage k of n), `name`, `step` | a stage starts; `n` can grow during the build (a provisional profile's heap check) |
| `step` | `k`, `name`, `step` | a new step within the stage; its count starts again |
| `progress` | `k`, `name`, `step`, `done`, `total`, `unit` (`''`, `'B'` for bytes, `'files'`, …), `est` (the total is an estimate), `note`, `frac` (0 to 0.99, or `null` when the work cannot be counted), `eta` (seconds or `null`), `elapsed` | the count changes; at most every 0.5 s while only the count moves |
| `stage_end` | `k`, `name`, `ok`, `summary`, `elapsed`; `warn` (`true`, only when the stage finished without what it makes, with `ok` true: a pipeline archive that failed, so the app is packed without one) | a stage ends |
| `shaders_stale` | `reason` (`'chain'`: libraries of another translation chain `DMSH_CHAIN`; `'inputs'`: the profile's shader inputs differ from the library's stamp `inputs.tsv`), `old` (the chain folders `b-<V>` or the stamp's digest), `new` (`b-<V>` now or the digest now), `count` (base libraries scheduled: one per current shader key and selected target, including cache restores) | once per shader step, before rebuilding after a chain or profile change; interrupted chain rebuilds still report it until the selected targets have stamped libraries. A step that reuses its libraries, or has none cached for its targets, sends none. A `say` with the same news follows |
| `say` | `msg` | a message; one starting with `[RED] ` is an error |

The exit code is 0 for a finished build, 1 for a build that stopped on an error (its last `say` says why, starting with `[RED] `; an unexpected error also prints its traceback on stderr), 2 for invalid command-line arguments, and 130 when interrupted. Argument parsing happens before JSON mode starts: an argument error writes its usage and error to stderr and emits no JSON events. A front end must check the process's exit status and capture stderr as well as the JSON stream. Each tool's full output goes to `<NSP folder>/<title ID>_build/logs/`, not to stdout. The build folder `<title ID>_build/` keeps what the next build can reuse, so a second build after a change takes a fraction of the first. A front end that keeps each game's build elsewhere passes `--work-dir <folder>`: that folder is the build folder, used as given (the script does not add `<title ID>_build`), so the front end names it per game; the logs are in its `logs/`, and the provisional profile of a game without one in `profile/`, so a front end that reuses the folder for a game keeps its profile (one from an earlier run in `<NSP folder>/<title ID>_build/profile` is copied there once, and stays). Without `-o`, the IPA or Mac app goes to the folder holding it, so give `-o` to put it somewhere else.

## The settings list

A built app carries the settings its game supports, as the iPhone app's settings screen shows them ([Settings](settings.md)): `dm/launcher.json` in the app inside an IPA, `Contents/Resources/dm/launcher.json` in a Mac app. The Mac app's list leaves out the settings only the iPhone app reads (the frame-rate counter, touch feedback). `dm/games/settings.py` writes it from `dm/games/settings.tsv`, whose header describes the rows. Breath of the Wild's Mac list, shortened:

```json
{"schema": 1, "game": "01007EF00011E000", "settings": [
 {"env": "DM_FPS_TARGET", "kind": "choice", "section": "Game", "label": "Frame rate", "note": "…", "default": "60",
  "options": [["30", "30 fps"], ["60", "60 fps"]], "level": "basic"},
 {"env": "DM_RENDER_SCALE", "kind": "range", "section": "Game", "label": "Render scale", "note": "…", "default": "1.0", "min": 0.5, "max": 1.0, "step": 0.05},
 {"env": "DM_FPS_HOLD", "kind": "toggle", "section": "Performance", "label": "Hold frames when late", "note": "…", "default": "on", "on": "on", "off": "off"}
]}
```

- `env`: the environment switch the setting sets. `kind`: `choice` (one of `options`, each `[value, label]`), `range` (a number from `min` to `max` in `step`s) or `toggle` (`on` or `off`, the values the switch takes).
- `section`: the heading the row goes under, today `Game`, `Controls` or `Performance`. Sections come in the order of their first rows, and rows in the order to show them. A new section may appear in schema 1: a reader draws a section it does not know like the others, under its name, as the iPhone app does.
- `default`: the value for this app's platform ([Tested defaults](settings.md#tested-defaults)), with profile defaults resolved for its game. A front end that keeps only the choices that differ from it lets a newer build's tested defaults reach the player, as the iPhone app does.
- `base` (optional, render scale only): `{"docked": [w, h], "handheld": [w, h]}`, the 3D views' size that the scale multiplies, so a front end can show 0.75 as 1200×675 docked. It comes from the game's profile (`render_base`), and only some games have it.
- `level` (optional): `basic` marks a setting to show first, for a front end that shows a short list and puts the rest behind a second step. Without it, the front end decides.
- `needs` (optional): `<VAR>=<value>` makes the row a sub-setting of the row that sets `<VAR>`, drawn only while that row has that value (Rumble strength only while rumble is on). A row naming a variable the list does not have stays drawn.
- An app without the file has no settings to offer: start it as it is.

**Compatibility.** [launcher.schema.json](launcher.schema.json) describes the file as a JSON Schema, so a front end can check it and generate its types from it. `schema` is 1, and schema 1 grows only by optional fields: a reader ignores fields it does not know, draws sections it does not know, and a field never changes its meaning. A new `kind` would be schema 2, since a reader cannot draw a kind it does not know. A list written before the field existed has no `schema` and is 1. In `settings.tsv`, the optional fields come from the last column, `meta` (`key=value` pairs separated by `;`); a new key is added in `settings.py`'s `META`, here and in the schema together, and `test_launcher_settings.py` fails when the three disagree.

## Starting a game

**A Mac app** is started like any app, or by running its `Contents/MacOS/nxnative` with the chosen settings in its environment (`DM_FPS_TARGET=30`, …). Without a host library, the launcher runs the host as a child, forwards SIGTERM, SIGINT and SIGHUP to it and exits with its exit code. With the paired `dmhost.dylib` and a player setup confirmed by Play, it runs the host in the same process; otherwise it retains child-process startup. With cloud saves enabled, the same-process host starts a separate, UI-free cleanup helper before entering the game. It watches process death with kqueue and flushes committed saves only after all host threads have stopped, including after a fatal crash or forced process termination. Child-process startup keeps the launcher as the post-death sender. Neither mode can send during a system shutdown that also stops the sender; unsent committed saves remain local for the next start. A variable already in the environment wins; then come `~/Library/Application Support/nxnative/<title ID>/env.txt` and the bundle's `dm/env.txt`, one `KEY=VALUE` per line ([A Mac app](development.md#a-mac-app)). The save is in `save/` in that Application Support folder; the log is `~/Library/Logs/nxnative/<title ID>.log`, the previous run's `<title ID>.prev.log`. The title ID is `DMTitleID` in the app's `Info.plist`.

A Mac app shows its own settings window before the game, drawn from its `launcher.json` and keeping its choices in the same store ([Saved choices](#saved-choices), [The Mac settings window](settings.md#the-mac-settings-window)). It shows a variable the launch environment or an `env.txt` sets as fixed. A front end that draws the settings itself starts the app with `DM_LAUNCHER=0` in its environment (or `--no-launcher` on its command line) to skip the window. The saved choices still apply, so a front end that writes them gets the same game as the window would.

A Mac game quits when the player holds a controller's Home button for 2 seconds and confirms with A ([On the Mac](playing.md#on-the-mac)). Its prompt is in English; a front end that shows the player another language passes the texts in `DM_HOME_QUIT_TEXT`, one per line in this order: the hold indicator ("Keep holding to quit"), the title ("Quit <game>?"), the note ("Unsaved progress will be lost."), the quit button and the cancel button. An empty or missing line keeps the English text. Whether macOS lets the game have the Home button is the player's Home button setting for apps, which a front end reads itself on macOS 27 (`GCControllerHomeButtonSettingsManager`, while a controller is connected) to tell the player when the system keeps Home.

Settings a player chose in a front end and wants kept, also for a start from Finder or the Dock, go in the app's saved choices ([Saved choices](#saved-choices)), not in its environment.

**An IPA** is installed by the player (sideloading); its own settings screen starts the game. Lines in the app's `Documents/env.txt` win over that screen ([Settings in a file](settings.md#settings-in-a-file)). The save and `log.txt` are in the app's Documents folder.

## Mac local-player setup (opt-in)

Enable **Assign players** under **Controls** in the existing Mac SwiftUI launch window.
A native AppKit `NSView` is embedded there before **Play**; no second modal or
**Local Players…** menu is offered. It is off by default and requires the paired
`dmhost.dylib` described above. Scope is tracked upstream in
[issue #878](https://github.com/seedds/nxnative/issues/878). For example:

```sh
DM_PLAYER_SETUP=1 /path/to/Game.app/Contents/MacOS/nxnative
```

The embedded view and the guest use the same host process and retained controller
objects; no device identities cross a process boundary. **Play** confirms the routes
and starts the guest. Without a confirmed window, `DM_PLAYER_SETUP=1` can instead
read [saved player assignments](#saved-player-assignments-on-mac), including on
`DM_LAUNCHER=0` / `--no-launcher` starts. Missing or malformed controller preferences,
`DM_SAVED_CHOICES=0`, or setup off retain legacy automatic routing and the existing
`DM_PAD_MERGE` / `DM_KBD_PLAYER` behavior.

- Only assigned connected sources appear in the overview. **Add input device** offers
  connected unassigned sources, including duplicate models and additional Joy-Con pairs. Press a controller button
  to highlight its named row (including Left Joy-Con / Right Joy-Con when separate).
  Keyboard presses highlight the keyboard row too. Numbers distinguish identical models
  within this session; they are never saved identities.
  Extended profiles and standard profiles exposing an analog Direction Pad, four face
  buttons and both shoulders are supported. Other profiles appear as unavailable and
  cannot supply input or count as players.
- The player count and live source summaries come first: **1 player**, followed by
  **Player 1: Pro Controller #1 + Keyboard**, means both inputs control the same player.
  Every active row has only its device name, a **Controls** player menu and **Remove**.
  The compact panel fits the launcher's minimum width without horizontal scrolling;
  each Joy-Con pair's Together / Separate selector sits beside its pair label.
  The player menu lists only active player IDs and **New player**, which allocates the
  lowest unused slot (up to the host's eight-player limit). There are no empty Player 2–8
  placeholders. Removing a source updates the choices but never renumbers other assignments;
  pending menu actions retain their actual player IDs, not mutable list positions.
  A game can support fewer players (Mario supports four); this does not change host routing.
  Add a source from the
  chooser or remove it back to the chooser; active player rows and summaries follow the routes,
  with no blank player summaries. Selecting an existing player explicitly combines inputs;
  **New player** gives that source a separate player. For example, the default Pro Controller
  and Keyboard both control Player 1 (one player). Choose Keyboard **New player** to get
  Player 2 (two players); choose Keyboard **Player 1** to combine them again (one player).
  Left and Right Joy-Con use the same menu: choose the same player for combined input,
  or **New player** for each half to play separately.
  Internally each player allows at most one primary source; target selection derives
  that role automatically, preserving an existing primary source on re-selection.
  Direct low-level conflicting assignments are still rejected without changing routes.
  Combined inputs merge buttons and use the more-deflected stick. The game's
  supported npad IDs still determine which players it can actually use; the host's
  existing always-present player-1/scripted-input compatibility remains unchanged.
- A logical **Nintendo Switch Joy-Con (L/R)** source offers **Together / Separate**
  for every connected pair, not just the first pair. Separate shows named **Left Joy-Con**
  and **Right Joy-Con** rows, each with a **Controls** target menu and **Remove**.
  This is app-local **split INPUT**, not physical unpairing. Hold L rotated counterclockwise
  and R clockwise. Each becomes that player's left stick: L maps `(x, y)` to `(-y, x)`,
  R to `(y, -x)`. L's original D-pad down/left/right/up becomes A/B/X/Y; R's original
  Nintendo X/A/Y/B becomes A/B/X/Y.
  Since SL/SR may be unavailable, each side's exposed shoulder/trigger becomes L/R;
  minus/plus becomes Plus, and stick click
  becomes left-stick click. Only exposed controls are available: no invented SL/SR,
  independent motion, or rumble for split sources. Whole sources retain normal controls;
  motion/rumble is assigned only when a player has one unambiguous whole controller.
- Connect/disconnect updates the embedded view live. Before **Play**, new supported
  devices never take another primary source's slot. Selected routes stay fixed until the
  user changes them; disconnecting other devices never renumbers them. During play a newly connected or reconnected
  device starts disabled; there is no in-game reassignment UI in this scope. Disconnect, reassignment,
  disabling and loss of window focus release routed inputs, including shared-player inputs.
  Guest input stays suspended during setup, even if focus changes.
  Connect, disconnect, assignment and confirmation log named routes with side, target
  and own/shared/off state; confirmation also logs the final per-player summaries.
  There is no per-frame route logging.

**Persistence and limits.** The router keeps strong controller references in this host
process. **Remember player setup** saves model preferences, not physical-device identities:
GameController has no documented stable device UUID, and `playerIndex` is not an identity.
`players.controllers.v1` in the host app's NSUserDefaults contains each supported gamepad's
vendor name, product category, player/role and optional split/right-side player/role.
A route restores automatically only when both descriptions are nonempty and exactly one
saved profile and one connected source match. Identical or incomplete models stay disabled:
choose **Identify controller to restore** on the desired saved row, then press a button on
that controller. Stick motion and releases do not confirm. Labels distinguish main and
combined inputs. A second matching model arriving during setup invalidates an automatic
match; that route then requires confirmation, even if the second device disconnects.
Explicitly confirmed/manual routes are not reassigned by later arrivals.

Missing/unconfirmed saved profiles are retained. **Forget** explicitly removes a pending
profile from the preview; it is not deleted from preferences until Play. The keyboard keeps
its numeric `players.keyboard.v1` (`-1` Not used, `0` Player 1 through `7` Player 8) and
`players.keyboard.mode.v1` (`0` own, `1` shared) keys; legacy keyboard-only choices keep
Remember checked, and slot-only choices retain shared semantics. Malformed values are
ignored. Only **Play** commits checked Remember choices; Play with Remember unchecked
clears these three preference keys. Quit, closing the window or turning setup off never
saves preview edits or Forget actions. `DM_SAVED_CHOICES=0` skips loading these preferences
and disables saving for that run. Preferences are per app. The embedded window keeps
the rules above; without a confirmed window, the opt-in saved routing below reads
the same model and role preferences. A front end keeps any cross-game priority list
in its own store and writes the resulting per-game choices before launch.

The opt-in picker currently lists GameController sources only: raw HID-only gamepads
cannot be assigned there and are not read during configured play. Disable player setup
to use the upstream raw HID fallback with automatic player slots. The picker never
fabricates a GameController identity for a raw HID device.

macOS may expose Joy-Cons individually or as an OS-combined L/R controller. Individual
Joy-Cons can expose the standard `GCGamepad` shape instead of `GCExtendedGamepad`.
The picker reads that shape through the supported `GCPhysicalInputProfile` dictionaries
and native element callbacks: its analog Direction Pad is movement, its standard face-button
positions map to Switch A/B/X/Y, shoulders to L/R and Menu to Plus. Apple's standard
profile already normalizes the sideways layout, so it is not rotated a second time.
Absent right-stick, trigger, motion and rumble capabilities are not invented. These
individual sources can be selected independently or routed to the same player; the
Together / Separate selector remains specific to an extended OS-combined pair.
**Arbitrary physical pair regrouping is unsupported**: GameController documents no Joy-Con
split/unpair API, and remote-controller coalescing Info.plist keys do not provide one.
Pair or unpair devices in the operating system yourself; the panel does not change Bluetooth
pairing or any device configuration. Interactive Joy-Con gameplay validation remains
pending. Routing tests are native and windowless with fake controller profiles; the
SwiftUI/AppKit integration test allocates a window but never shows or activates it.

## Saved choices

Each app keeps the player's choices in its own preferences domain, its bundle ID (`CFBundleIdentifier` in `Info.plist`, `org.nxnative.<title ID>` unless the app was packed with another): one string per setting, under the key `launcher.<env>` (`launcher.DM_FPS_TARGET` = `"30"`). The iPhone app's settings screen keeps them there, and a Mac app's launcher (`dm/mac/launcher.c`) reads them at every start, so a start from Finder or the Dock has them too. A front end writes the same keys (`CFPreferencesSetAppValue` with the app's bundle ID, or `defaults write org.nxnative.<title ID> launcher.DM_FPS_TARGET -string 30`), and a settings window inside the app would too: there is one set of choices, and every view of it agrees.

- **Only choices that differ from the tested default are kept.** A setting the player left at its default follows the defaults of a later build. Remove a key when the player picks the default again. For "Reset to tested defaults", remove the keys of the settings the player can change; the app's own screen does that when Play applies the defaults, and keeps the saved choice of a setting fixed for this start.
- **Checked when read.** The launcher uses a saved value only when this build still offers it: one of a `choice`'s options, a `toggle`'s `on` or `off`, or a number from a `range`'s `min` to `max`. It checks against the app's own list, which the packer copies into `Info.plist` as `DMSettings`. Any other value, a value that is not a string, or a key no setting has is ignored; it stays saved until the settings screen's Play replaces it with the player's choice. So a rebuild that stops offering a value falls back to its tested default.
- **The order**, the first that sets a variable wins:
  1. the launch environment (a front end's launch overrides, a terminal);
  2. the player's `env.txt` (`~/Library/Application Support/nxnative/<title ID>/env.txt`; on the iPhone, `Documents/env.txt`);
  3. the bundle's `dm/env.txt`;
  4. saved choices;
  5. the tested defaults (the host's own, from the game's profile).
- **Launch overrides are temporary.** A front end that starts the game with a variable in its environment overrides the saved choice for that start only. It writes the store only when the player means to keep the choice.
- **`DM_SAVED_CHOICES=0`** (in the environment or an `env.txt`) starts with the tested defaults and leaves the saved choices as they are. **`DM_LAUNCHER=0`** only skips the settings screen (the iPhone's, or a Mac app's window): the saved choices still apply.
- **A sandboxed front end** cannot write another app's preferences domain; this store is for front ends outside the App Sandbox, as NX2Apple is.

### Saved player assignments on Mac

**Capability.** A Mac app whose `Info.plist` has `DMPlayerPrefs` equal to
`players.controllers.v1` supports the windowless route described here. The packer
sets it only when the actual host executable defines `dm_mac_saved_player_prefs_v1`;
it inspects symbols without loading or running the host. Offering `DM_PLAYER_SETUP`
in `launcher.json` alone is not evidence of this capability: older builds offer
the window's UI without windowless restoration. For a capable host, the packer also
provides the saved activation rule in `DMSettings` when the optional settings list
does not contain it, so activation works without a `launcher.json`.

**Store and enable.** Write to the app's actual bundle ID, as for other saved choices.
`players.controllers.v1` is a property-list array of at most 32 dictionaries, not a
JSON string. Every dictionary contains all seven fields:

| Field | Property-list type | Value |
|---|---|---|
| `vendor` | string | GameController `vendorName`, empty when absent |
| `category` | string | GameController `productCategory`, empty when absent |
| `player` | integer | `-1` disabled, `0` Player 1 through `7` Player 8 |
| `role` | integer | `0` primary (own), `1` combined (share) |
| `split` | integer or boolean | `0` whole; `1` only for category `Nintendo Switch Joy-Con (L/R)` |
| `rightPlayer` | integer | Right half's target when split; otherwise `-1` |
| `rightRole` | integer | Right half's role when split; otherwise `0` |

Write numeric objects, not strings that look like numbers. `CFPreferencesSetAppValue`
accepts the structured property list; synchronize writes and wait for success before
launching the app. Keep `players.keyboard.v1` and `players.keyboard.mode.v1` unless
the user chooses to change them. Their absence keeps the shared Player 1 keyboard.
Enable routing with the string `"1"` at `launcher.DM_PLAYER_SETUP`, or the temporary
launch environment `DM_PLAYER_SETUP=1`. The usual environment and `env.txt` precedence
applies. `DM_SAVED_CHOICES=0` ignores all these preferences without deleting them.

Without a prepared window, the host calls `DmMacPlayers.startSavedRouting:` and
loads these choices from `NSUserDefaults.standardUserDefaults` in its own process.
A missing array or any malformed dictionary retains the automatic path. An empty
but valid array enables session routing with no initially configured models.
The windowless mode never saves preferences, including session-generated routes.
The window's Play/Remember behavior and its disabled late-arrival policy are unchanged.

**Session routing.** A complete vendor/category pair restores only when one layout
and one connected supported source match. Targets are zero-based and are never
compacted or renumbered by the host. Configured targets remain reserved even before
their model first connects and after it disconnects. A disconnected source releases
its buttons and axes; a unique reconnect restores the session's target and roles.

A model with no matching persisted profile first tries to restore a unique, complete
session route. Otherwise it takes the lowest slot never reserved in this session.
Its previous slots remain reserved even when incomplete or ambiguous identity prevents
restoration. Identical unknown models can therefore join together or separately;
neither arrival order renumbers an already assigned source. The keyboard reserves its
slot when primary, but its default shared role
allows a controller on Player 1. Exhausted reservations never evict a previous model:
the new source stays disabled. Unsupported GameController profiles and raw HID-only
devices do not gain a configured route.

A matching disabled, incomplete, ambiguous or conflicting persisted profile never
falls back to automatic allocation. Conflicting primary claims are blocked for the
whole session, including an entire split profile if either half conflicts. During
play, another identical model does not revoke an active route. If that model matches
a persisted profile, the newcomer stays disabled; once ambiguous, that profile cannot
restore without confirmation, which this mode does not offer. Unknown models instead
receive fresh slots when their temporary routes cannot restore safely. Repeated
reconnections with unclear identity can exhaust all eight reservations. Logs describe
routes and refused assignments, but remain diagnostic text.

A front end may calculate startup targets from its own priority list, omitting absent
models and sending a known ambiguous model with `player=-1`. The host does not read
that global list. A discovery snapshot can miss a late model; it then receives an
unused slot rather than displacing someone already playing. Finder uses the last
per-app choices written, not a fresh calculation of the front end's priority list.
Writes affect the next start, not an already running game. Turning setup off leaves
the stored layouts available to the window if the user enables it later.

A front end must also account for a previously saved primary keyboard: assigning a
controller as primary on that same slot is a conflict and disables that controller's
profile. Do not claim the priority was applied without resolving or explaining this
configuration. Configured routing ignores `DM_PAD_MERGE` and `DM_KBD_PLAYER`; raw
HID-only devices remain outside this mode, which a front end should explain before
enabling it for those devices.

The game's supported npad IDs still decide which routed players it reads. A game
supporting four players can ignore a newcomer routed to Player 5, even when a lower
slot is reserved but disconnected. One-player games retain their existing merging
of unsupported players into Player 1. Slot replacement and an in-game assignment UI
are outside this interface.

### The cloud saves folder

The folder a Mac app's save is synced through ([Cloud saves](settings.md#cloud-saves)) is not a row of the settings list, since a folder is none of its kinds. It has a key of its own in the same store: `cloudSaves.folder`, a string with the folder's absolute path (`defaults write org.nxnative.<title ID> cloudSaves.folder "$HOME/Library/Mobile Documents/com~apple~CloudDocs/My Saves"`). Remove the key, or write an empty string, to turn cloud saves off. The app's settings window shows and changes the same key, so the player sees the folder a front end chose.

- **Read at every start** by `dm/mac/launcher.c`, with or without the window, and passed on to the host as `DM_SAVE_SYNC`. A change takes effect at the game's next start.
- **The order** is that of the saved choices: `DM_SAVE_SYNC` in the launch environment wins (a folder for this start only, or `0` to start without cloud saves), then an `env.txt`, then this key. `DM_SAVE_SYNC=icloud` is iCloud Drive › nxnative Saves.
- **One folder serves every game.** Each game keeps its own `<title ID>/save.dmsync` in it, so a front end can write the same path into each app's domain to give its whole library one cloud folder. A path that already ends in the game's title ID is taken as that game's own folder.
- **The iPhone app** keeps its folder as a security-scoped bookmark that only its own folder picker can make, so it is chosen there.

## Cheats

Cheats are files, not settings: the host (`dm/host/hle_cheats.c`) reads Atmosphère-format cheat files for the running game's build and keeps the player's toggles beside them. A front end that wants to install cheats or pick which ones run, before the game starts, writes the same files; the Mac game window's Cheats menu and panel are one view of them. The engine itself and the format of a cheat line are described in [the cheats reference](reference/cheats.md); this section is the contract for the files.

- **The folder.** `DM_CHEATS=<folder>`, else `<DM_ROOT>/cheats`, which for a Mac app is `~/Library/Application Support/nxnative/<title ID>/cheats` (the launcher sets `DM_ROOT` to that folder; once a `dataFolder` moves the app's files, the cheats folder moves with it, as everything under `DM_ROOT` does). Two layouts are read: flat, `<folder>/<build ID>.txt`, and a Switch SD card's tree under it, `<folder>/atmosphere/contents/<title ID>/cheats/<build ID>.txt` (or without `atmosphere/`). The build ID is the first 16 hex digits of the main NSO's build ID, upper case, as cheat sites name their files; the game note of a profile lists its builds. With no file for the running build nothing changes in the game, and the folder is never created by the host.
- **The files beside `<build ID>.txt`**, all in the same folder, all plain text, written by the host and readable by anyone:
  - `<build ID>.enabled`: the cheats that are on, one name per line, in the file's own spelling (`Max HP`); a duplicate name's later entries are `Name #2`, `Name #3`, …. Masters (`{Name}` entries) are always on and are not listed; labels and one-pass cheats never are. The host rewrites the file on every toggle (a temporary file renamed over it, so it is never half-written) and reads it when the file is loaded. A front end that installs a file and wants some cheats on from the first start writes this file before the start.
  - `<build ID>.meta.tsv`: optional, written by `scripts/cheat_anchor.py` beside a file it compiled: one row per entry (columns by header name: `name`, `kind`, `section`, `type`, `action`, `ask`, `value_words`, `once`, `pair`, `resolved`, `note`, `address`), which gives the panel sections, notes, on / off pairs, typed values and run-once cheats. A hand-made or downloaded file has none, and needs none.
  - `<build ID>.values`: the typed values of `ask` cheats, `name<TAB>value` per line, written when the player sets one.
  - `.cheats-off-<title ID>`: the master switch. While this file exists no cheat of the game runs, toggles kept. `.cheats-running-<title ID>` is the host's own safe-mode flag (Mac only): it exists while cheats run and goes with a clean exit; found at the next start it means the last run crashed with cheats on, and the game starts with every cheat off until the player restores them. A front end leaves it alone.
- **What a front end may write while the game runs.** Nothing the host reads back on its own: the host reads `<build ID>.enabled` and the cheat file when it loads them, at the start and on the player's "Reload cheats" (⌘R in the Mac window, which re-reads the same file and the toggles with it), and never watches the folder. So a front end writes before the start, or tells the player to reload. The panel's refresh while it is open reads the engine's state, not the files. For one start, `DM_CHEATS_ON=<name>,<name>,…` in the launch environment turns the named cheats on without touching `.enabled` (case-insensitive, by name); `DM_CHEATS_ANY_BUILD=1` loads the folder's single file of another build when the running build has none (its addresses may not fit: for checking a carried file), and `DM_CHEATS_DEBUG=1` logs every write. The three are diagnostics, not settings: they are not in `launcher.json`.
- **The iPhone.** The engine is compiled into the iOS host too (without safe mode), with the same folder rule, but the iOS app sets neither `DM_CHEATS` nor `DM_ROOT`, so cheats are off there unless `Documents/env.txt` names a folder with `DM_CHEATS`; even then they write data only, never code (the app's code pages are signed). Giving the iOS app a default, `Documents/cheats`, is the natural next step once a front end wants to manage cheats on a device; it is not in this change.
- **Carrying a file to another build.** `scripts/cheat_anchor.py` imports a cheat file into a version-independent source and compiles it for any build of the game ([the rewriter reference](reference/rewriter.md#cheats-carried-across-game-updates)). A front end need not know about it: the compiled `<build ID>.txt` is an ordinary cheat file, and its `.meta.tsv` makes the panel richer.

## An app that hosts the engine

An app that links the host and the front end's game view into its own process (a launcher holding several games, say) has these hooks in `dm/host/front.h`. nxnative's own apps do not use them, so its tests and regression runs do not cover them. Each one is unset by default, and a hook must be set before the game starts.

- **`front_progress_hook`**: draws the prepare step's progress ("Preparing shaders", "Preparing pipelines") instead of the front end's screen. With a window, it gets each call's title, done and total on the main thread; the title is `NULL` when the step ends.
- **`dm_set_game_image_header`** (iOS): names the image that carries the game's rewritten modules, when the app links them into a framework it `dlopen`s from its signed bundle instead of into its executable. Call it with that image's mach header before `dm_host_preload_start`. Only the module sections are read from it.

## Following main

nxnative has no releases or tags: a front end pins a commit and moves it forward. Between the commit it has (`A`) and the one it moves to (`B`), two things say what changed for it.

**What git shows.** These need no note from anyone, and no commit adds one:

| What changed | The command |
|---|---|
| This page, the settings list's schema, the settings a game can offer | `git log -p A..B -- docs/frontends.md docs/launcher.schema.json dm/games/settings.tsv` |
| The NSP script's options | `git diff A..B -- scripts/build_ipa_from_nsp.py \| grep add_argument` |
| The environment switches, with each one's class (`switch`, `path`, `diagnostic`) | `git diff A..B -- docs/env_switches.tsv` |
| The shader chain: each game's shaders are rebuilt at its next build, which the `shaders_stale` event reports | `git log --oneline A..B -- dm/gfx/nvn/dmsh_chain.h` |

**What only the author can say.** A commit a front end has to act on, in a way the commands above do not show, says so in a line of its own in its message, one line per thing, and `git log A..B --grep='^Front-ends:'` finds them:

```
Front-ends: break: what stopped working, and what to do about it
Front-ends: interface: an existing variable, argument, event, field or path that now means something else
Front-ends: new: a kind of input or game, or a requirement, that a front end could offer or has to meet
```

Most commits have none, and commits from before this section have none either. A line is the author's knowledge, so its absence is no proof that nothing changed.

**Data a player already has moves by itself.** When a change makes the host read saves or other kept data differently, the host carries the old form forward, as with [saves named before backslash paths](reference/services.md#saves-named-before-backslash-paths): a front end has nothing to patch, and a player of nxnative's own apps gets the same fix.

## What a front end cannot see yet

While a game runs, the host says what happens only in its log, as text meant for people: a controller that connects or goes away, the preparing of shaders before the first frame, a red line. A front end that reads the log to show these would break when a line's wording changes. Events a program can read (one JSON object per line, as the NSP script's) would be the next piece of this interface; until then, treat log lines as diagnostics, not as an interface.
