# Community front ends

A front end is an app that packs games, keeps a library of them or starts them, and is a project of its own ([Front ends](frontends.md) is the interface it builds on). This page lists community front ends by that interface and by nothing else: it says which parts of the contract each one follows, and where its support is. It does not describe features or looks, compare front ends or recommend one.

## The criteria

Each is a yes or no that the front end's author answers for the current release:

1. **Reads `launcher.json`.** It reads the settings list of a built app (`schema` 1) and tolerates fields and sections it does not know ([The settings list](frontends.md#the-settings-list), Compatibility).
2. **Writes saved choices.** It keeps the player's choices as `launcher.<VAR>` in the app's preferences domain, not only as an environment it starts the game with ([Saved choices](frontends.md#saved-choices)).
3. **`DM_LAUNCHER=0`.** When it draws its own settings, it starts a Mac app with `DM_LAUNCHER=0` so the app's window does not show a second time ([Starting a game](frontends.md#starting-a-game)). "n/a" if it draws no settings.
4. **Keeps game files out.** Game files, keys and anything extracted from them stay out of its repository and its releases ([CONTRIBUTING.md](../CONTRIBUTING.md#never-game-files-or-keys)).
5. **Own support channel.** Problems with the front end are reported there, not in nxnative's issues. The column links the channel.

The neutral facts are the platform, the repository and the licence.

## The front ends

Sorted by name. A row is the author's statement; the checkmarks are theirs, and nxnative does not verify them.

| Front end | Platform | Licence | Reads `launcher.json` | Writes saved choices | `DM_LAUNCHER=0` | Game files out | Support |
|---|---|---|---|---|---|---|---|
| [NX Play](https://github.com/varyform/nx-native-wrapper) | macOS | MIT | no | no | n/a | yes | [its issues](https://github.com/varyform/nx-native-wrapper/issues) |
| [NX2Apple](https://github.com/LNPANDY/NX2Apple) | macOS | MIT | yes | yes, from [v0.5.0](https://github.com/LNPANDY/NX2Apple/releases/tag/v0.5.0) ([LNPANDY/NX2Apple#88](https://github.com/LNPANDY/NX2Apple/pull/88)) | yes | yes | [its issues](https://github.com/LNPANDY/NX2Apple/issues) |

Windows work is discussed in [#244](https://github.com/seedds/nxnative/issues/244): portability is a separate question from this page.

## Adding a row

The author of a front end opens a PR that adds one row, in alphabetical order, and confirms each criterion for the release it describes. Use `yes`, `no` or `n/a`, with a link where a claim needs one. A row can be `no` in any column; the page only says what is true. Keep a row current: a PR that changes it is the author's to open. Support for a front end goes to its own repository, so the row's last column must name one. A new front end that needs something from nxnative's side of the interface opens an issue here for that piece ([CONTRIBUTING.md](../CONTRIBUTING.md#graphical-front-ends)).
