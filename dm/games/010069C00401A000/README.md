# RIVE: Ultimate Edition (`010069C00401A000`)

Base game with Update 1.22 (`v131072`). No game-specific patches, configuration or pipeline modifications are required; RIVE runs with the default pipeline.

## Tested on Mac

- **Device:** Apple M1 MacBook Air, 16 GB unified memory
- **OS:** macOS 27.0
- **Resolution:** 2880 × 1800
- **Game mode:** Hard
- **macOS Game Mode:** Off
- **Display composition:** Composited

A gameplay screenshot captured during a character-death event showed 59.01 FPS, 9.19 ms GPU time, and a 16.95 ms frame interval. Metal memory was 823.80 MB and app memory was 2.33 GB. The frame-time graph shows spikes during the death/explosion sequence. These brief events were not noticeable during play. This is a momentary capture, not a claim that every frame ran at exactly 59.01 FPS.

A separate sustained-play session lasted approximately four hours. Gameplay remained consistently around 60 FPS, with occasional dips to 59 FPS and no noticeable stuttering. The MacBook Air remained barely warm throughout, even while resting on a lap.

## Frame-time capture

### Hard-mode gameplay — 2026-11-26 07:43:41

- Frames recorded: 121,093
- Reported frame rate: 60 FPS
- Median frame time: 16.7 ms
- 99th-percentile frame time: 18.0 ms
- Slowest frame: 158 ms
- Recorded hitches: 45
- Dropped frames: 7

At the reported 60 FPS, 121,093 frames represent approximately 33 minutes 38 seconds of captured gameplay.

## Profile notes

The profile uses the default pipeline. No game-specific shader-chain changes or patches are required. Game-derived per-title shader records and generated local-only tables are intentionally not committed, so this profile does not ship a precomputed per-game shader table.
