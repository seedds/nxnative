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

## Replayed frame-time capture

A subsequent replay was recorded at **2026-11-25 20:25:07**. The NX2Apple performance panel reports:

- Frames recorded: 16,844
- Reported frame rate: 60 FPS
- Median frame time: 16.7 ms
- 99th-percentile frame time: 18.1 ms
- Slowest frame: 134 ms
- Recorded hitches: 5
- Dropped frames: 13

At the reported 60 FPS, 16,844 frames represent approximately 4 minutes 41 seconds of captured frames. Compared with the earlier capture, the dropped-frame count is substantially lower (13 versus 3,697), while the median and 99th-percentile frame times remain consistent.

The NX2Apple panel states that frame-time recording writes a row per frame while the game is on and that nothing is recorded while it is off. Therefore, switching away from the game should not be assumed to explain the earlier dropped-frame count. The recorder's exact definition of "dropped" is not established by this screenshot; the value is reported as displayed.

## Earlier first-mission capture

The first mission was previously played through to completion with frame-time recording enabled. The capture was timestamped **2026-11-25 19:43:39** and reported:

- Frames recorded: 23,006
- Reported frame rate: 60 FPS
- Median frame time: 16.7 ms
- 99th-percentile frame time: 18.2 ms
- Slowest frame: 144 ms
- Recorded hitches: 10
- Dropped frames: 3,697

This earlier capture is retained for transparency, but the subsequent replay provides a cleaner measurement with only 13 reported dropped frames. The dropped-frame count is not conflated with the separate hitch count.

## Earlier gameplay frame-time capture

A separate earlier capture recorded 11,923 frames over approximately 3 minutes 19 seconds:

- Median frame time: 16.7 ms
- 99th-percentile frame time: 18.1 ms
- Slowest frame: 153 ms
- Recorded hitches: 8
- Dropped frames: 121

In that earlier Hard-mode session, the recorded spikes coincided with character-death events and were not noticeable during play. The spikes are retained in the measurements rather than excluded; this is an observed correlation, not proof of the underlying technical cause.

## Profile notes

The profile uses the default pipeline. No game-specific shader-chain changes or patches are required. Game-derived per-title shader records and generated local-only tables are intentionally not committed, so this profile does not ship a precomputed per-game shader table.
