<!-- Title: name the game and its title ID when it is about one game ("Star Fox (0400E88025B1A000): …"), so searches and labels find it. Open it as a draft early. -->
<!-- Keep a PR to one change. AGENTS.md and CONTRIBUTING.md say where findings go; link to them instead of repeating them here. -->

## What and why

<!-- The symptom or the need, the cause, and the change. Link the issue (Fixes #...) and the reference entry or game note it adds. -->

## Checked

<!-- Delete what does not apply. Say what was not checked too. -->
- Tests: <!-- e.g. make -C dm/host/tests GAME=<id> run; make -C dm/gfx/tests GAME=<id> run (Apple silicon, Metal); the dm/tools/tests that apply -->
- Games run, with title ID and version: <!-- what was played, for how long, and what was compared (log lines, frame times, screenshots) -->
- Existing work: <!-- the issues, pull requests and branches searched for the same area or game, and how this relates to them (docs/charter.md, Before starting work) -->
- Devices and OS: <!-- e.g. M2 Max, macOS 26.1; iPhone 17 Pro Max, iOS 26.1. Say whether the lowest supported device of each platform the change reaches was among them (docs/charter.md, Devices) -->
- Reach and risk: <!-- narrow, shared or wide (docs/charter.md, The bar rises with reach and risk), and what that tier asks for. For a default changed for every game: the devices it was measured on (a test request issue for the lowest), the switch that restores the old behaviour, and the docs/decisions.md entry -->
- For a change to the shader chain, the host or a tool that every game uses: the `scripts/regress.py check --games` report on the games it most likely reaches (`regress.py impact` helps), or why none ([06-dev-loop.md](https://github.com/seedds/nxnative/blob/main/docs/agents/06-dev-loop.md))
- Measurements before and after, where the change is about speed, memory or power

## Front ends

<!-- Usually delete this. Only for what a front end has to act on and git does not show (docs/frontends.md, Following main): one line each, `Front-ends: break: …`, `Front-ends: interface: …` or `Front-ends: new: …`, kept in the commit message too. -->

## Docs

- [ ] The finding is in its game note or its `docs/reference/<subsystem>.md` entry, and `python3 dm/tools/docs_symptoms.py` was run (the symptom index)
- [ ] A chain tool changed: `dmsh_chain.py --header` was run and `dm/gfx/nvn/dmsh_chain.h` is part of the change
- [ ] No game files, keys or data extracted from a game in the change or the attachments
