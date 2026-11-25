# RIVE: Ultimate Edition (`010069C00401A000`)

Base game with Update 1.22 (`v131072`). No game-specific patches, configuration or pipeline modifications are required; RIVE runs with the default pipeline.

## Tested on Mac

- **Device:** Apple M1 MacBook Air, 16 GB unified memory
- **OS:** macOS 27.0
- **Resolution:** 2880 × 1800
- **Game mode:** Hard
- **macOS Game Mode:** Off
- **Display composition:** Composited

A separate gameplay screenshot captured during a character-death event showed 59.01 FPS, 9.19 ms GPU time, and a 16.95 ms frame interval. Metal memory was 823.80 MB and app memory was 2.33 GB. The frame-time graph shows spikes during the death/explosion sequence. These brief events were not noticeable during play. This is a momentary capture, not a claim that every frame ran at exactly 59.01 FPS.

A separate sustained-play session lasted approximately four hours. Gameplay remained consistently around 60 FPS, with occasional dips to 59 FPS and no noticeable stuttering. The MacBook Air remained barely warm throughout, even while resting on a lap.

## Completed first-mission capture

The first mission was played through to completion with frame-time recording enabled. The capture shown in the NX2Apple performance panel is timestamped **2026-11-25 19:43:39** and reports:

- Frames recorded: 23,006
- Reported frame rate: 60 FPS
- Median frame time: 16.7 ms
- 99th-percentile frame time: 18.2 ms
- Slowest frame: 144 ms
- Recorded hitches: 10
- Dropped frames: 3,697

At the reported 60 FPS, 23,006 frames represent approximately 6 minutes 23 seconds of captured frames. The graph shows brief frame-time spikes against an otherwise comparatively stable baseline. The dropped-frame count is included as reported by the tool and should not be conflated with the separate hitch count. This capture covers a completed first mission, rather than a short isolated gameplay moment.

## Earlier frame-time capture

A separate earlier capture recorded 11,923 frames over approximately 3 minutes 19 seconds:

- Median frame time: 16.7 ms
- 99th-percentile frame time: 18.1 ms
- Slowest recorded frame: 153 ms
- Recorded hitches: 8
- Dropped frames: 121

In that earlier Hard-mode session, the recorded spikes coincided with character-death events and were not noticeable during play. The spikes are retained in the measurements rather than excluded; this is an observed correlation, not proof of the underlying technical cause.

## Profile notes

The profile uses the default pipeline. No game-specific shader-chain changes or patches are required. Game-derived per-title shader records and generated local-only tables are intentionally not committed, so this profile does not ship a precomputed per-game shader table.
