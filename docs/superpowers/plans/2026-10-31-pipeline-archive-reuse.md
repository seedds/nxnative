# Pipeline archive reuse implementation plan

> **For agentic workers:** Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Reuse identical offline pipeline archives across chain updates on macOS and iOS, and benchmark LegendsZA.

**Architecture:** Hash the complete generated configuration with paths normalized to stable library labels, referenced library bytes, compiler identity and target settings. Keep chain validation and runtime metadata separate from binary identity.

**Tech Stack:** Python, unittest, metal-tt, existing NSP builder.

**Spec:** ../specs/2026-10-31-pipeline-archive-reuse.md

## Global constraints

No shader/runtime semantics changes or iOS device actions. Game files and measurements stay in the game's folder. Platform binaries stay separate; legacy keys rebuild once. Commit and PR publication follow the user's explicit 2026-11-01 request.

## Review focus

Chain-only relocation must reuse while refreshing stamps. Changed library bytes, entries and descriptors must rebuild. Compiler, architecture, platform and OS remain inputs. Device-only pair/profile metadata changes refresh metadata without rebuilding identical bases. Missing archives, failures and invalid reflection cannot be accepted as hits.

### Task 1: Cache identity

**Files:** dm/gfx/tools/pso_offline.py; dm/tools/tests/test_pso_offline.py.

**Interfaces:** archive_key(configuration, libraries, arch, platform, osmin, toolchain) returns a digest; main keeps its CLI.

- [x] Add behavioral tests for both platforms and the review-focus cases, using existing synthetic libraries and compiler stand-in.
- [x] Run python3 -m unittest dm.tools.tests.test_pso_offline -q; observe chain-only reuse failures.
- [x] Implement content identity and preserve validation and metadata writes.
- [x] Run focused tests, relevant Python suites and lint; fix new failures.

### Task 2: Measurement and documentation

**Files:** docs/reference/shaders.md; dm/games/0100F43008C44000/README.md; generated symptoms; benchmark artifacts outside the repo.

**Interfaces:** Use Task 1's CLI and the NSP builder, save commands/revisions/toolchain/timings/hashes.

- [x] Save legacy builder; warm cache. Make temporary chain-only edit; rebuild LegendsZA and time stages.
- [x] Compare legacy and new archive builder on identical resulting inputs; test real iOS reuse if libraries exist.
- [x] Restore benchmark-only edits. Run regress.py check for the only discovered local Mac build, LegendsZA, recording route limitations.
- [x] Document mechanism and measurements, regenerate symptoms and test docs.
- [x] Review final diff and report measured savings and limits, leaving changes uncommitted.

## Progress

User explicitly requested implementation; proceeding inline without another approval. Baseline 8dfa9c6d8b0e20f5a9563d7034773eda5755ca49. No shared interfaces beyond the existing archive CLI. No commit steps because AGENTS.md forbids unsolicited commits.

Task 1 complete: red tests demonstrated four failures (including both platform chain moves); 26 focused tests pass. Native full suites: 112 files, 1,020 tests, 13 skipped, no failures. The first Intel-Python suite hit architecture/dependency errors; the complete native rerun resolved all of them. Lint: 392 baseline findings, zero new. Native pipeline bundle: 52 checks, zero failures. Independent review: no actionable findings. Real iOS archive: cold 4.445 s, chain move 0.234 s, same inode/mtime and refreshed stamp.

Regression: LegendsZA is the only discoverable local Mac build. Full check PASS, 28,603/28,603 shaders, zero changed MSL or new interface failures; no smoke route available, no gameplay claim. Warm-up used the initial Intel Python and overlapped checks, so its elapsed time is excluded from the controlled comparison. Both timed comparison commands use native Apple Silicon Python; no other task builds remain active.

Benchmark method: a temporary chain comment changed a7dbbce711c3 to 5b1637c7f097. Runner and logs live in the game build/performance-research/pipeline-archive-reuse folder. The runner restored the source/header in finally. Legacy cache was primed with the verified warm binary and exact old input key, then a real legacy invocation confirmed reuse before the chain change.

Full chain-update app build succeeded: 785.618 s (13m06s); extraction 4m59s, shader preparation 5m10s (28,603 restored), archive 16s reused, packaging 2m02s. Archive SHA-256 and mtime unchanged. Before/after referenced-library SHA-256 both 4b344b6430c790929f2e0166f865708d00d116d894ce53ab5b49232a61bb1bb6; content key stays 80a513fcff5e514852882c0214fc4f6cb308b2feb801868c03845c6a4ba5c4c1, legacy key changes a9c44b4d0ced3c09 to c43822d88f388eb5. This is a controlled comment-only chain transition, not a direct total-time comparison with the earlier accumulated-update build. LegendsZA has no pso_mtl4 profile setting, so its default iOS build does not request an offline archive; the shared iOS path was verified with a real synthetic archive.

Task 2 complete: archive A/B 462.779 s versus 10.098 s, saving 452.681 s (97.8%); full app 785.618 s. Source/header restored to a7dbbce711c3. All measured compiler inputs and primary archive bytes/mtime stayed identical. Measurements and method are in the game note; raw data remains beside the game. No commits, pushes, device installs or profile flags changed.

Publication, 2026-11-01: the user requested a commit and PR with benchmark and platform-validation details. Keep the historical benchmark revision explicit, and transplant only this change onto upstream main to exclude the unrelated controller commits in the original checkout.
