<!-- Keep a PR to one change. AGENTS.md and CONTRIBUTING.md say where findings go; link to them instead of repeating them here. -->

## What and why

<!-- The symptom or the need, the cause, and the change. Link the issue (Fixes #...) and the reference entry or game note it adds. -->

## Checked

<!-- Delete what does not apply. Say what was not checked too. -->
- Tests: <!-- e.g. make -C dm/host/tests GAME=<id> run; make -C dm/gfx/tests GAME=<id> run (Apple silicon, Metal); the dm/tools/tests that apply -->
- Games run, with title ID and version: <!-- what was played, for how long, and what was compared (log lines, frame times, screenshots) -->
- Devices and OS: <!-- e.g. M2 Max, macOS 26.1; iPhone 17 Pro Max, iOS 26.1 -->
- For a change to the shader chain, the host or a tool that every game uses: the games it was run on beyond the one it was made for, or why none
- Measurements before and after, where the change is about speed, memory or power

## Docs

- [ ] The finding is in its game note or its `docs/reference/<subsystem>.md` entry, and `python3 dm/tools/docs_symptoms.py` was run (the symptom index)
- [ ] A chain tool changed: `dmsh_chain.py --header` was run and `dm/gfx/nvn/dmsh_chain.h` is part of the change
- [ ] No game files, keys or data extracted from a game in the change or the attachments
