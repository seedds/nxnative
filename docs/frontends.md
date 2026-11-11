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

With `--progress json` it asks nothing and writes one JSON object per line on stdout. Every object has `ev` (the event) and `t` (Unix time, seconds):

| `ev` | Fields | When |
|---|---|---|
| `stage` | `k`, `n` (stage k of n), `name`, `step` | a stage starts; `n` can grow during the build (a provisional profile's heap check) |
| `step` | `k`, `name`, `step` | a new step within the stage; its count starts again |
| `progress` | `k`, `name`, `step`, `done`, `total`, `unit` (`''`, `'B'` for bytes, `'files'`, …), `est` (the total is an estimate), `note`, `frac` (0 to 0.99, or `null` when the work cannot be counted), `eta` (seconds or `null`), `elapsed` | the count changes; at most every 0.5 s while only the count moves |
| `stage_end` | `k`, `name`, `ok`, `summary`, `elapsed`; `warn` (`true`, only when the stage finished without what it makes, with `ok` true: a pipeline archive that failed, so the app is packed without one) | a stage ends |
| `shaders_stale` | `reason` (`'chain'`: libraries of another translation chain `DMSH_CHAIN`; `'inputs'`: the profile's shader inputs differ from the library's stamp `inputs.tsv`), `old` (the chain folders `b-<V>` or the stamp's digest), `new` (`b-<V>` now or the digest now), `count` (base libraries scheduled: one per current shader key and selected target, including cache restores) | once per shader step, before rebuilding after a chain or profile change; interrupted chain rebuilds still report it until the selected targets have stamped libraries. A step that reuses its libraries, or has none cached for its targets, sends none. A `say` with the same news follows |
| `game` | `title_id` (the base game's, as the app's `DMTitleID`), `name`, `version`, `bundle_id` (what the app will carry, after `--app-name` and `--bundle-id`), `out` (the IPA or Mac app the build will write; `null` for `--prepare-xcode`), `icon` (the image given with `--icon`, else the game's own icon as extracted, a 256×256 `icon.jpg` in the build folder, else the image given with `--fallback-icon`, or `null` without any; pass a stand-in with `--fallback-icon` rather than `--icon`, so a game that carries an icon keeps it) | once, when the app's name, version and bundle ID are settled, before the shader stages; not for `--platform mac`, which makes no app. Copy the icon before the build ends: an IPA build without `--keep-extracted` deletes the extraction it is in. A `say` with the same news follows |
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

**A Mac app** is started like any app, or by running its `Contents/MacOS/nxnative` with the chosen settings in its environment (`DM_FPS_TARGET=30`, …). The launcher there runs the host as a child, forwards SIGTERM, SIGINT and SIGHUP to it and exits with its exit code: 0 for a quit (⌘Q, the Home prompt, closing the window), 70 after the alert that macOS stopped running the game's GPU work ([A GPU whose access macOS revoked stops the game](reference/rendering.md#a-gpu-whose-access-macos-revoked-stops-the-game)), 134 for an abort. A variable already in the environment wins; then come `~/Library/Application Support/nxnative/<title ID>/env.txt` and the bundle's `dm/env.txt`, one `KEY=VALUE` per line ([A Mac app](development.md#a-mac-app)). The save is in `save/` in that Application Support folder; the log is `~/Library/Logs/nxnative/<title ID>.log`, the previous run's `<title ID>.prev.log`. The title ID is `DMTitleID` in the app's `Info.plist`.

A Mac app shows its own settings window before the game, drawn from its `launcher.json` and keeping its choices in the same store ([Saved choices](#saved-choices), [The Mac settings window](settings.md#the-mac-settings-window)). It shows a variable the launch environment or an `env.txt` sets as fixed. A front end that draws the settings itself starts the app with `DM_LAUNCHER=0` in its environment (or `--no-launcher` on its command line) to skip the window. The saved choices still apply, so a front end that writes them gets the same game as the window would.

A Mac game quits when the player holds a controller's Home button for 2 seconds and confirms with A ([On the Mac](playing.md#on-the-mac)). Its prompt is in English; a front end that shows the player another language passes the texts in `DM_HOME_QUIT_TEXT`, one per line in this order: the hold indicator ("Keep holding to quit"), the title ("Quit <game>?"), the note ("Unsaved progress will be lost."), the quit button and the cancel button. An empty or missing line keeps the English text. The same goes for the prompt that waits when a game asks for more players than have joined (Mac and iOS): `DM_CTRL_PROMPT_TEXT`, one per line, the title ("{min} players wanted"), the hint ("Press a button on each controller to join."), the count ("Players: {joined} of {min}"), the done button and the cancel button; `{min}`, `{max}` and `{joined}` are replaced by the numbers. Whether macOS lets the game have the Home button is the player's Home button setting for apps, which a front end reads itself on macOS 27 (`GCControllerHomeButtonSettingsManager`, while a controller is connected) to tell the player when the system keeps Home.

Settings a player chose in a front end and wants kept, also for a start from Finder or the Dock, go in the app's saved choices ([Saved choices](#saved-choices)), not in its environment.

**An IPA** is installed by the player (sideloading); its own settings screen starts the game. Lines in the app's `Documents/env.txt` win over that screen ([Settings in a file](settings.md#settings-in-a-file)). The save and `log.txt` are in the app's Documents folder.

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

### The cloud saves folder

The folder a Mac app's save is synced through ([Cloud saves](settings.md#cloud-saves)) is not a row of the settings list, since a folder is none of its kinds. It has a key of its own in the same store: `cloudSaves.folder`, a string with the folder's absolute path (`defaults write org.nxnative.<title ID> cloudSaves.folder "$HOME/Library/Mobile Documents/com~apple~CloudDocs/My Saves"`). Remove the key, or write an empty string, to turn cloud saves off. The app's settings window shows and changes the same key, so the player sees the folder a front end chose.

- **Read at every start** by `dm/mac/launcher.c`, with or without the window, and passed on to the host as `DM_SAVE_SYNC`. A change takes effect at the game's next start.
- **The order** is that of the saved choices: `DM_SAVE_SYNC` in the launch environment wins (a folder for this start only, or `0` to start without cloud saves), then an `env.txt`, then this key. `DM_SAVE_SYNC=icloud` is iCloud Drive › nxnative Saves.
- **One folder serves every game.** Each game keeps its own `<title ID>/save.dmsync` in it, so a front end can write the same path into each app's domain to give its whole library one cloud folder. A path that already ends in the game's title ID is taken as that game's own folder.
- **The iPhone app** keeps its folder as a security-scoped bookmark that only its own folder picker can make, so it is chosen there.
- **The player's choice.** When the cloud's save and this device's both changed since the last sync (or the device has never synced and both have a save), the host takes the newer one, and the other is kept in `<DM_SAVE>-backups`. A front end that asks the player instead sets `DM_SAVE_SYNC_CHOICE` at every start; then the host replaces neither until the player has chosen: it plays with this device's and sends nothing. The front end compares the two before the start (the header of `save.dmsync` has the cloud's hash, save time and device; `<DM_SAVE>.sync` has the hash last synced, in a `base` line; `dm/host/save_sync.c` has both formats) and passes what the player chose (a Mac app whose `Info.plist` has the integer `DMSaveSyncChoice=1` reads it): `ask` when there was nothing to choose (the two were the same, or only one changed), `cloud:<hash>` brings in the cloud's save with that hash, `local:<hash>` lets this device's replace it (`local:none` when the cloud had none), and the one not chosen is kept in `<DM_SAVE>-backups`. A choice holds only for the cloud save it names: if that save changed after the check, the choice is not used, and a save that changed on both sides is replaced on neither. `offline` says the cloud could not be read before the start: the host does not wait for it. With or without a front end, a send never replaces a cloud save that changed after the start compared it, or one the start could not read: the next start decides.

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
