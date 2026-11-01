# Pipeline archive reuse across shader chains

Implement the selected content-based archive cache for macOS and iOS. The generated pipeline configuration, referenced library bytes, GPU architecture, platform, minimum OS and Metal translator version determine the binary. Chain labels, library locations and profile stamps do not. Preserve reflection-chain validation and refresh runtime metadata on every successful invocation. Keep the CLI and one archive per architecture, version the cache format, and conservatively rebuild legacy keys once.

Alternatives considered: chain-based invalidation repeats compilation of identical inputs; per-pipeline archive shards need a larger runtime change. The user selected whole-archive content reuse first and authorized implementation and benchmarking.

Benchmark LegendsZA in an isolated checkout: warm the new cache, make a temporary chain-only source update and rebuild the Mac app. Compare legacy and new archive-stage runs against the same resulting libraries. Keep game data and timings in the game's build folder. Distinguish full-app measurements from stage comparisons. Test both platform paths; use real iOS libraries if available. Restore temporary chain edits. No iOS device actions. The user authorized committing the completed change and opening a PR on 2026-11-01.
