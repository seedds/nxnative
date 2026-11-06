# Contributing

Issues and pull requests are welcome. This page says how a change gets in; what the code does and where things are is in [AGENTS.md](AGENTS.md) and [docs/development.md](docs/development.md).

## Reporting a problem

**First, update to the latest `main`** (`git fetch origin` and rebuild from `origin/main`) and try again: the problem may already be fixed, and a report against an old commit is often traced to something that changed since. Then search the open and closed issues for the game and the symptom.

Open an issue with the bug report form. It asks for what a problem is traced with: the nxnative commit, the shader chain version, the game's title ID and version, the device and OS, how the game was built and run, environment overrides, and the whole log of the run. [Playing](docs/playing.md) says where the iPhone app's log is; a Mac app writes `~/Library/Logs/nxnative/<title ID>.log`.

**Tag it through its title.** Labels are added automatically from an issue's title (and a pull request's changed files): put the game's name and title ID in it, and the area in plain words (`Star Fox (0400E88025B1A000): black screen on Mac after the title`, `NVN: …`, `iOS: …`). Check the labels it gets, and fix the title if they are wrong; that is how others working on the same game or area find it.

`python3 scripts/bug_report.py <title ID | path to the .app | NSP folder>` collects most of this into one zip to attach: `report.md` with the game, commit, chain version, device, settings and the list of files, plus the Mac app's logs, the NSP script's logs under `<title ID>_build/logs` and recent crash reports. Add `--ios-log log.txt` for an iPhone log. It reads only text logs and settings, never game files or keys; it writes your home folder as `~` and drops lines that look like key material. Look at the zip before you attach it.

## Never game files or keys

Never commit or attach game files, keys, title keys or anything extracted from a game: RomFS, code modules, shaders, dumps. Game files and their extractions stay in the game's NSP folder ([AGENTS.md](AGENTS.md)). Logs, screenshots, title IDs, build IDs and offsets are fine.

## Before you start

- **Read the [charter](docs/charter.md)**, at least its five-line summary: what the project is for, the Macs a measurement should name, and what counts as a regression.
- **Look for existing work first, every time**: open and merged pull requests and issues by the game's label (`game: <title ID>`) and area label, by the game's name and title ID in titles, and the active branches ([how, with or without `gh`](docs/agents/06-dev-loop.md#0-before-you-start-existing-work-then-a-branch)). Many people work on the same games, mostly through agents that don't see each other: build on what is there, or ask its author.
- **Make your work findable**: open a draft pull request early, with the game's name and title ID in its title (`Star Fox (0400E88025B1A000): …`), and link the issue it belongs to.
- Read [AGENTS.md](AGENTS.md): it lists where to start for each kind of task, and the rules for profiles, per-build knowledge and the shader translation chain.
- A crash, a hang or a wrong picture: search the [symptom index](docs/agents/symptoms.md) for the text in the log first.
- Another game: run `scripts/build_ipa_from_nsp.py` on its NSP folder and follow [Port another game](docs/development.md#port-another-game).
- Prefer a fix that holds for every game to a game-specific rule, and fix a bug at its cause ([AGENTS.md](AGENTS.md)).

## A pull request

- **One change per PR, on its own branch off `main`** (in your fork if you have no write access), never on your local `main` ([the cycle](docs/agents/06-dev-loop.md#0-before-you-start-existing-work-then-a-branch)). Rebase on `main` rather than merging it in. A general fix found while bringing up a game is its own PR, opened as soon as it works.
- **Its tier.** Say whether it is narrow, shared or wide, and show what that tier asks for ([charter](docs/charter.md#pull-requests), [which tier](docs/agents/06-dev-loop.md#which-tier)).
- **Commits.** Each commit message says what changed and why, in plain English sentences, as the history does: the symptom, the cause, the fix, and what was measured or checked.
- **Front ends.** A change a front end has to act on takes a `Front-ends:` line in the commit message and the PR text, unless git already shows it ([Following main](docs/frontends.md#following-main)). A change that makes the host read saves or other kept data differently also carries the old form forward.
- **Where findings go.** A fact about one game goes in its note (`dm/games/<id>/README.md`). A general mechanism gets an entry in `docs/reference/<subsystem>.md` (Symptom, Cause, Fix, Found on), followed by `python3 dm/tools/docs_symptoms.py`. A new game takes its profile, its note and its row in the games table ([dm/games/README.md](dm/games/README.md)).
- **The README gallery.** After Mac gameplay testing, add its screenshot to `docs/images/` and its row to [gallery.tsv](docs/images/gallery.tsv), then run `python3 dm/tools/docs_gallery.py`. The table preserves your chosen order; notes hold the IPA size and noteworthy features or limitations. Leave the note empty until the IPA size is measured. Concurrent additions merge in the TSV; regenerate README to resolve a conflict inside its gallery. Conflicting rows for one game need a manual resolution before regeneration.
- **The shader chain.** After changing one of its tools, run `python3 dm/gfx/tools/dmsh_chain.py --header` and commit `dm/gfx/nvn/dmsh_chain.h` with the change. Every game's shaders are then rebuilt, so say which games you checked.
- **Tests.** CI runs the host tests, the CPU-only graphics tests, the tool tests and the builds ([Continuous integration](docs/development.md#continuous-integration)). GitHub's runners have no Metal 4, so run the GPU tests on an Apple-silicon Mac: `make -C dm/gfx/tests GAME=<id> run`.
- **The PR text.** Fill in the template: what and why, what was checked (tests, games, devices), and which docs changed.

## Large changes and other platforms

- **Agree on the scope first.** Open an issue before writing a change that adds a platform or device class, adds a second implementation of an existing path (for example a rendering path for GPUs without Metal 4), or runs to more than about 1,000 lines. A large PR built on an older `main` can rarely be reviewed or merged, however well it was tested.
- **One concern per PR.** A new front end, a platform baseline and one game's fixes are separate PRs, each on current `main`.
- **Supported devices** are the ones [README.md](README.md) names: iOS / iPadOS 26 or later on an iPhone with an A17 Pro chip or later or an iPad with an M1 chip or later, and Macs with Apple silicon. Supporting older devices or Intel Macs is the maintainer's decision, so ask in an issue first: it would add code paths that every later change has to keep working and that few contributors can test.

## Graphical front ends

An app that packs games, lists them or starts them (a build window, a game library) is a separate project that builds on nxnative, not a part of this repository. [NX2Apple](https://github.com/LNPANDY/NX2Apple) is one, and [README.md](README.md) points players to it. Such an app needs no changes here to work: the NSP script reports its stages as JSON lines (`--progress json`), a built Mac app or IPA carries the list of its game's settings (`dm/launcher.json`, from `dm/games/settings.tsv`), and the app takes those settings as environment switches or as saved choices in its preferences, which every start reads ([Front ends](docs/frontends.md)). When a front end needs more from nxnative (a new event, a field in the settings list), open an issue or a small PR for that piece.

The settings screen inside the iPhone and iPad app and the settings window inside a Mac app are part of nxnative ([Settings](docs/settings.md)): they draw the same list and keep the same saved choices as any front end.

## How changes get in

The maintainer reviews every PR. A PR is often taken in as a squashed or reworked commit on `main` that credits its author (`Co-authored-by`), and the PR is then closed rather than merged; that is expected. The maintainer may rebase or rework a PR that conflicts with `main` while taking it in.
