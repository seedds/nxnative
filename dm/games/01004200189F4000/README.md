# Factorio (`01004200189F4000`)

Nintendo Switch update 2.0.76 (`v2031616`, game build 84451), Nintendo SDK 21.4.2. Wube Software's engine uses SDL and desktop OpenGL through EGL. The profile loads `main` and `sdk`. The host answers the EGL imports from `subsdk0` (`NintendoSDK_OpenGL`). The `.vert` and `.frag` files in the RomFS provide 97 shader stages, translated offline for the Mac and iOS.

## Mac route

Build with `scripts/build_ipa_from_nsp.py --platform mac` and use its run command with `DM_METAL_HUD=1`. Dismiss the update changelog with B if it appears. Use the D-pad to focus **Single player**, then **New game**, and press A on each. With the first tutorial selected, press X to start it. Tested on an Apple M6 with macOS 27.2.1 and Xcode 27.2.

The corrected build reached the first tutorial with controller input and audio enabled. Standing still at 1920×1080, it presented 900 frames in 15.06 seconds (59.8 fps). The reported footprint stayed near 2.55 GB and Metal allocations near 1.11 GB. The host logged no `[RED]` lines on this route. The unsigned IPA built with all 97 shader stages and passed the ZIP CRC check.

The NPDM declares 0 MB of system resource. The profile uses the usual 4 GB guest layout with a 256 MB alias region and a 256 MB stack region. The graphics backend uses the game's own atlas compression path and 1080p docked mode.

## Found on this game

- [OpenGL integer vertex inputs and timestamp queries](../../../docs/reference/rendering.md#gl-integer-vertex-inputs-and-timestamp-queries).
- [OpenGL framebuffer draw and read buffers](../../../docs/reference/rendering.md#gl-framebuffer-draw-and-read-buffers).
- [OpenGL sprite-atlas copies and readback](../../../docs/reference/rendering.md#gl-sprite-atlas-copies-and-readback).
- [OpenGL texel buffer views](../../../docs/reference/rendering.md#gl-texel-buffer-views).

## Open

- iPhone and iPad gameplay, multiplayer, and save reload remain unverified.
- SDL logs that the Switch video driver lacks a custom selection cursor. The first tutorial can be played with controller input.
