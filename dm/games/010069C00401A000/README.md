# RIVE: Ultimate Edition (`010069C00401A000`)

Base game with Update 1.22 (`v131072`). No game-specific patches, configuration or pipeline modifications are required; RIVE runs with the default pipeline.

## Tested on Mac

- **Device:** Apple M1 MacBook Air, 16 GB unified memory
- **OS:** macOS 27.0
- **Resolution:** 2880 × 1800
- **Game mode:** Hard
- **macOS Game Mode:** Off
- **Observed frame rate:** 60 FPS
- **Displayed frame interval:** 16.67 ms
- **Metal memory:** 922.09 MB
- **App memory:** 2.49 GB

A separate sustained-play session lasted approximately four hours. Gameplay remained consistently at 60 FPS, with occasional dips to 59 FPS and no noticeable stuttering. The MacBook Air remained barely warm throughout, even while resting on a lap.

## Frame-time capture

A separate capture recorded 11,923 frames over approximately 3 minutes 19 seconds:

- Median frame time: 16.7 ms
- 99th-percentile frame time: 18.1 ms
- Slowest recorded frame: 153 ms
- Recorded hitches: 8
- Dropped frames: 121

The recorded spikes coincided with character-death events during the Hard-mode session. They were not noticeable during play. They are retained in the measurements rather than excluded; the observed correlation does not establish their underlying technical cause.

## Profile notes

The profile uses the default pipeline. No game-specific shader-chain changes or patches are required. Game-derived per-title shader records and generated local-only tables are intentionally not committed, so this profile does not ship a precomputed per-game shader table.
