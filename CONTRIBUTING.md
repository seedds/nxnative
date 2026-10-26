# Contributing

Issues and pull requests are welcome. This page says how a change gets in; what the code does and where things are is in [AGENTS.md](AGENTS.md) and [docs/development.md](docs/development.md).

## Reporting a problem

Open an issue with the bug report form. It asks for what a problem is traced with: the nxnative commit, the shader chain version, the game's title ID and version, the device and OS, how the game was built and run, environment overrides, and the whole log of the run. [README.md](README.md#play) says where the iPhone app's log is; a Mac app writes `~/Library/Logs/nxnative/<title ID>.log`.

## Never game files or keys

Never commit or attach game files, keys, title keys or anything extracted from a game: RomFS, code modules, shaders, dumps. Game files and their extractions stay in the game's NSP folder ([AGENTS.md](AGENTS.md)). Logs, screenshots, title IDs, build IDs and offsets are fine.

## Before you start

- Read [AGENTS.md](AGENTS.md): it lists where to start for each kind of task, and the rules for profiles, per-build knowledge and the shader translation chain.
- A crash, a hang or a wrong picture: search the [symptom index](docs/agents/symptoms.md) for the text in the log first.
- Another game: run `scripts/build_ipa_from_nsp.py` on its NSP folder and follow [Port another game](docs/development.md#port-another-game).
- Prefer a fix that holds for every game to a game-specific rule, and fix a bug at its cause ([AGENTS.md](AGENTS.md)).

## A pull request

- **One change per PR, off `main`.** Rebase on `main` rather than merging it in.
- **Commits.** Each commit message says what changed and why, in plain English sentences, as the history does: the symptom, the cause, the fix, and what was measured or checked.
- **Where findings go.** A fact about one game goes in its note (`dm/games/<id>/README.md`). A general mechanism gets an entry in `docs/reference/<subsystem>.md` (Symptom, Cause, Fix, Found on), followed by `python3 dm/tools/docs_symptoms.py`. A new game takes its profile, its note and its row in the games table ([dm/games/README.md](dm/games/README.md)).
- **The shader chain.** After changing one of its tools, run `python3 dm/gfx/tools/dmsh_chain.py --header` and commit `dm/gfx/nvn/dmsh_chain.h` with the change. Every game's shaders are then rebuilt, so say which games you checked.
- **Tests.** CI runs the host tests, the CPU-only graphics tests, the tool tests and the builds ([Continuous integration](docs/development.md#continuous-integration)). GitHub's runners have no Metal 4, so run the GPU tests on an Apple-silicon Mac: `make -C dm/gfx/tests GAME=<id> run`.
- **The PR text.** Fill in the template: what and why, what was checked (tests, games, devices), and which docs changed.

## How changes get in

The maintainer reviews every PR. A PR is often taken in as a squashed or reworked commit on `main` that credits its author (`Co-authored-by`), and the PR is then closed rather than merged; that is expected. A PR that conflicts with `main` is rebased by its author.
