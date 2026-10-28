@AGENTS.md

# Claude Code

AGENTS.md is the project's rule set. This file adds only what Claude needs on top of it: the environment it runs in, how it searches code, where the boundaries are, and how it delegates.

## Language

The user writes in Spanish: reply in Spanish. Code, comments, docs, log messages, script output, commit messages and PR text stay in English, as AGENTS.md requires.

## Environment

- The session runs on the user's Linux PC (x86-64), repository at `/home/jb/Documentos/nxnative`, branch `multiplataforma` (the Windows and Linux port; its handoff is `docs/design/migration-status.md`, read it before touching the port).
- Installed: `python3`, `cmake`, `ninja`, `make`, `docker`. Not installed: `clang`, `dotnet`, Xcode, Metal. Check with `command -v` before relying on a tool; do not install anything without asking.
- What runs here: the Python tool tests (`python3 -m unittest discover -s dm/tools/tests`; also `dm/ios/tests` and the `test_*.py` of `dm/gfx/tests`), the docs test (`python3 -m unittest dm/tools/tests/test_docs.py`), `python3 dm/tools/docs_symptoms.py`, `python3 scripts/regress.py impact main..HEAD` (git only), and the Linux host build through Docker once Phase 0 of the port is merged.
- The user cannot test on a Mac (a managed work Mac). A check that needs Windows or a Mac is prepared in the repository with a runner that writes its logs to the committed folder `logs/<platform>/` (`logs/README.md`); the user runs it and, on failure, pushes the logs to GitHub for the next session to read.
- What cannot run here and must be reported as unchecked, never as passed: `scripts/regress.py check`, the host Makefiles (`make -C dm/host`, `make -C dm/host/tests`), the Metal graphics tests (`make -C dm/gfx/tests ... run`), the shader chain builds (`dmsh_build.py`, `dmsh_chain.py`), `build_ipa.sh` and the NSP script's IPA step. They need an Apple-silicon Mac or macOS CI.
- Game files, keys and work folders stay outside the repository and outside the home folder (AGENTS.md). The repository root must never hold an NSP or XCI; if one is found there, say so and do not `git add` it.

## Code search

- The repository is indexed in codebase-memory-mcp as project `home-jb-Documentos-nxnative`. At the start of a session run `index_status` on it; if it is missing or stale (the graph does not update on its own), run `index_repository` with `repo_path` `/home/jb/Documentos/nxnative` and `mode` `full`. Re-index after a branch switch, a merge, or a change of more than a handful of files.
- Search code through the graph, for anything: `search_graph` to find functions, classes and routes; `trace_path` for call chains and data flow; `get_code_snippet` for a symbol's exact source; `query_graph` for complex patterns; `get_architecture` for structure; `search_code` for text search across code; `detect_changes` (`base_branch` `main`) to see what a branch touches and who calls it.
- Text, configs, docs, TSV files and logs use Grep, Glob and Read.
- Always Read a file before editing it.

## Boundaries and abstractions

- Before changing a component, name the boundary it sits behind and read that boundary's reference first:

  | Boundary | Where | Reference | Tests |
  |---|---|---|---|
  | Host (SVCs, SDK services) | `dm/host` | `docs/reference/services.md`, `threads.md`, `memory.md`, `audio.md`, `movies.md`, `frame-rate.md` | `make -C dm/host/tests GAME=<id> run` (Mac) |
  | Platform layer of the port | `dm/host/plat/` (Phase 0, branch `multiplataforma-fase0-wip`) | `docs/design/migration-status.md` | CTest in `dm/host/CMakeLists.txt` (Docker) |
  | Execution engine | native ARM64 on Apple (`dm/host/entry.S`, `dm/gfx/nvn/nvn_entry.S`, `nvn_thunks.S`, Mach state in `hle_thread.c`); AOT recompiler for x86-64 | `docs/design/aot-recompiler.md` | differential tests against dynarmic (Phase 2) |
  | NVN on Metal | `dm/gfx/nvn` | `docs/reference/rendering.md`, `nvn-objects.md`, `shaders.md` | `make -C dm/gfx/tests GAME=<id> run` (Mac) |
  | Vulkan and OpenGL layers | `dm/gfx/vk`, `dm/gfx/gl` | `docs/reference/rendering.md` (Vulkan layer and OpenGL layer sections) | same |
  | Vulkan backend of the port | not started | `docs/design/vulkan-backend.md` | Phase 4 |
  | Shader chain | `dm/gfx/tools/dmsh_*`, `dmsh_chain.h` | `docs/reference/shaders.md`, AGENTS.md chain procedure | `test_nsp_chain.py` |
  | Rewriter and tools | `dm/tools`, `scripts` | `docs/reference/rewriter.md`, `packing.md` | `dm/tools/tests` |
  | Game profile | `dm/games/<id>/` | that game's `README.md`, `dm/games/README.md` | the game's `smoke.tsv` route |

- Change one backend or one profile at a time. A change inside a backend must not require a change in another. If the need is shared, put it in the common interface.
- On the port: Apple code moved into `dm/host/plat/` is a verbatim move with unchanged behaviour; each other OS gets its own file; an unported Apple backend gets a declared null backend that logs a `[RED] ` line, not a silent stub.
- Do not add an abstraction until a second caller needs it.
- Migration readiness is shown by `scripts/regress.py check` and the tests, not by assertion. Run them on the games a change reaches and report the result; when they cannot run on this machine, say which were not run.
- A decision that adds a language or a large dependency (Boost, LLVM, SDL3, FFmpeg, CMake for first-party code) is presented to the user with criteria first; the user decides. The open ones are listed in `docs/design/migration-status.md`.
- At the end of a change, say which boundary it touched and which it left alone, and update `docs/design/migration-status.md` when a phase of the port moves.

## Delegation

Delegating to subagents is the default. Each bounded task that can be described in full goes to a fresh subagent on Claude Opus 5.5, with its effort set from the table below. Work that needs this conversation's context stays here, and so does a single-file edit.

### Models

| Model | ID | Use |
|---|---|---|
| Claude Fable 5.1 | `claude-fable-5-1` | This session only: planning, briefs, review of subagent diffs, talking to the user |
| Claude Opus 5.5 | `claude-opus-5-5` | Every subagent. The Agent tool names it `model: "opus"`; that value resolves to Opus 5.5 |
| Claude Sonnet 5.5, Claude Haiku 5.5 | `claude-sonnet-5-5`, `claude-haiku-5-5` | Not used in this project, not even for `low` effort |

- Launch with `subagent_type: "general-purpose"`, `model: "opus"` and an `effort`. Never use `fork` (it keeps this session's model) and never `model: "fable"`.
- Run at most two subagents at once: the plan's usage limit was hit with three in parallel, and an interrupted agent costs more than a slower one.
- Set `effort` from the table. Judge each task on three criteria: scope (files and games affected), reversibility, and whether the cause is known. Use the highest level that any criterion calls for.

| Effort | When | Examples |
|---|---|---|
| `low` | Mechanical, no decisions, checkable with a command | Renames, doc links, regenerating the symptom index, running tests and summarizing |
| `medium` | Bounded change in one module that has tests. Default. | Fixing a function with a test, adjusting a profile, a section of a design doc |
| `high` | Unknown cause, or low-level code | Crashes, disassembly, a wrong value, per-build addresses, the platform layer |
| `xhigh` | Shared components or several games | Changes in `dm/gfx/nvn`, host or the chain; `regress.py impact`; the recompiler |
| `max` | A design decision that is costly to reverse. Rare. | Separating a component so it can be migrated, a backend split |

### The brief

Every brief is a complete specification, because the subagent sees nothing else. It states:

- The goal in one paragraph, and the absolute paths of the files to read and of the files it may change. Nothing else is written.
- That code search goes through the codebase-memory-mcp graph, project `home-jb-Documentos-nxnative`, as in the Code search section.
- The exact commands to run, with `DM_GAME` or `GAME=<id>` and the build ID when they apply, and which of them cannot run on this machine.
- The AGENTS.md constraints that apply: root cause over workaround, per-build addresses gated by build ID, English only, no game files or keys in the repository, no key values printed.
- An acceptance check that can be run, with its expected output.
- The report format: changed files with `file:line`, every command run with its output, what was not verified and why, open questions. The report cites evidence and does not only assert.

### Rules

- Subagents do not commit, do not print key values, do not install software, and do not write outside the paths the brief names and the work folder.
- Two subagents never write the same file. Risky writes (host, chain, several files) run in an isolated worktree (`isolation: "worktree"`, under `.claude/worktrees/`, never committed).
- Before relaying a subagent's result, reread its diff (`git diff`, or `git -C <worktree> diff`) and repeat the check it names. A check the subagent could not run is reported to the user as not run.
- For a follow-up, continue the same subagent with `SendMessage` instead of starting a new one.
