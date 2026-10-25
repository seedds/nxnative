# iPadOS 16 compatibility checkpoint

This branch carries the cumulative traditional Metal backend port and iPadOS 16.1 build changes. It is based on upstream commit `ebe75c9d5d34486f714678f850f0e74bbab398ac`. It needs the user's own game inputs; none are part of this branch.

## Rendering and memory

The traditional Metal backend replaces newer Metal interfaces, explicitly declares resources reached through argument buffers, indexes buffer addresses, and decodes unsupported BC formats. Embedded guest code stays in the executable's original signed text mapping while its writable data hole is populated separately. `dm/ios/tools/set_text_maxprot.py` updates the unsigned executable's maximum text protection before external signing.

GPU-written indirect records are sanitized on the GPU, in batches, without a mid-frame CPU wait. A draw's base instance is copied into its vertex driver constants at c0+0x644. Each command occurrence owns its constants; later draws cannot rewrite an earlier draw's SSBO table, and CPU initialization preserves the GPU-owned word even when encoding continues after a split commit. Returning to a direct draw invalidates the indirect vertex binding.

Guest pool aliases are not fully represented by Metal resource object identity. Render, compute and blit replay passes therefore use an explicit queue-local fence. NVN Barrier ends an active pass and invalidates argument state. Without real barriers, the device hit GPU Hang in a single-texture-sample 7x4 postprocess pass, then rejected later submissions.

## Input and timing

Keyboard movement uses WASD; arrow keys turn the camera. J/K/U/I map to A/B/X/Y. Virtual face-button positions are A bottom, B right, X left and Y top, with hit testing following the same logical positions. The touch overlay hides while a physical controller, keyboard or mouse is connected and returns after disconnection. No visible right stick was added.

Mouse and trackpad camera input accepts GCMouse relative movement. UIKit hover and trackpad scroll gestures provide fallbacks, with recent raw input taking precedence over duplicate hover events. A single timer releases camera input after the last movement, including during touch tracking.

For Pokemon Legends: Z-A, use `DM_PRESENT_INTERVAL=game`. The game sets interval 2; the former `display` override ignored it and is not a correct-speed 60 fps patch. This checkpoint follows the game's timing instead.

## Validation and limits

On 2026-10-25, local device package v48 passed the previous hang point, loaded the existing station save, and continued beyond 8,000 frames without GPU Hang, GPU Address Fault or SubmissionsIgnored. A controlled 0.8-second camera rotation changed the view from the platform to the benches and wall, with rendering continuing afterwards. Canopy, arches, lamps and benches were visible. Raw trackpad movement and automatic keyboard/mouse overlay hiding were confirmed in the device log.

This is a limited real-device checkpoint, not a claim that every scene or other game is correct. Longer play, all scene assets, controller variants and regressions in other games still need testing. The broad backend port should be reviewed separately from the smaller input and synchronization fixes before upstream integration.

Checks passed: complete unsigned arm64 iOS Release build targeting iOS 16.1; targeted cross-compilation of rendering and input units; 10 present-pacer checks; 12 shader-chain consistency tests using Python 3.15. Upstream's build script syntax requires Python 3.12 or newer.
