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

## Frame-time captures

Two subsequent captures provide the current frame-time references.

### Replay capture — 2026-11-25 20:25:07

- Frames recorded: 16,844
- Reported frame rate: 60 FPS
- Median frame time: 16.7 ms
- 99th-percentile frame time: 18.1 ms
- Slowest frame: 134 ms
- Recorded hitches: 5
- Dropped frames: 13

At the reported 60 FPS, 16,844 frames represent approximately 4 minutes 41 seconds of captured frames.

### Earlier Hard-mode capture

- Frames recorded: 11,923
- Approximate capture duration: 3 minutes 19 seconds
- Median frame time: 16.7 ms
- 99th-percentile frame time: 18.1 ms
- Slowest frame: 153 ms
- Recorded hitches: 8
- Dropped frames: 121

In this earlier Hard-mode session, the recorded spikes coincided with character-death events and were not noticeable during play. This is an observed correlation, not proof of the underlying technical cause.

## Profile notes

The profile uses the default pipeline. No game-specific shader-chain changes or patches are required. Game-derived per-title shader records and generated local-only tables are intentionally not committed, so this profile does not ship a precomputed per-game shader table.
