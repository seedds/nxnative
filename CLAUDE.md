@AGENTS.md

# Claude Code

AGENTS.md is the project's rule set. This file adds only what Claude needs on top of it.

## Language

Reply to the user in their language. Code, comments, docs, log messages, commit messages and PR text stay in English.

## Code search

- Search code with the codebase-memory-mcp graph, for anything. Use `search_graph` to find functions, classes and routes, `trace_path` for call chains and data flow, `get_code_snippet` for a symbol's exact source, `query_graph` for complex patterns, `get_architecture` for structure, and `search_code` for text search across code.
- Text, configs, docs, TSV files and logs use Grep, Glob and Read.
- Always Read a file before editing it.
- Check that the project is indexed with `list_projects`. If it is not, run `index_repository` first. The graph does not update on its own, so re-index after large changes.

## Boundaries and abstractions

- Before changing a component, name the boundary it sits behind: host, a gfx backend (`dm/gfx/nvn`, `dm/gfx/vk`, `dm/gfx/gl`), tools, or a game profile. Check the matching section of `docs/reference` first.
- Change one backend or one profile at a time. A change inside a backend must not require a change in another. If the need is shared, put it in the common interface.
- Do not add an abstraction until a second caller needs it.
- Migration readiness is shown by `scripts/regress.py check` and the tests, not by assertion. Run them on the games a change reaches and report the result.
- At the end of a change, say which boundary it touched and which it left alone.

## Delegation

Delegating to subagents is the default. Each bounded task that can be described in full goes to a fresh subagent on Opus, with its effort set from the table below. Work that needs this conversation's context stays here, and so does a single-file edit.

- Launch subagents with `model: "opus"`. Never use `fork`, which keeps this session's model.
- Set `effort` from the table. Judge each task on three criteria: scope (files and games affected), reversibility, and whether the cause is known. Use the highest level that any criterion calls for.

| Effort | When | Examples |
|---|---|---|
| `low` | Mechanical, no decisions, checkable with a command | Renames, doc links, regenerating the symptom index, running tests and summarizing |
| `medium` | Bounded change in one module that has tests. Default. | Fixing a function with a test, adjusting a profile |
| `high` | Unknown cause, or low-level code | Crashes, disassembly, a wrong value, per-build addresses |
| `xhigh` | Shared components or several games | Changes in `dm/gfx/nvn`, host or the chain; `regress.py impact` |
| `max` | A design decision that is costly to reverse. Rare. | Separating a component so it can be migrated |

- Every brief is a complete specification, because the subagent sees nothing else. It states:
  - The goal, and the absolute paths of the files to read and to change.
  - That code search goes through the codebase-memory-mcp graph, as in the Code search section.
  - The exact commands to run, and the build ID or `DM_GAME` when it applies.
  - The AGENTS.md constraints that apply to the task.
  - An acceptance check that can be run.
  - The report format: changed files with `file:line`, commands run with their output, open questions. The report cites evidence and does not only assert.
- Subagents do not commit, do not print key values, and do not write outside the work folder.
- Two subagents never write the same file. Risky writes run in an isolated worktree.
- Before relaying a subagent's result, reread its diff and repeat the check it names.
- For a follow-up, continue the same subagent with `SendMessage` instead of starting a new one.
