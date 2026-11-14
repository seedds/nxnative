# Historical minimum-duration presentation control

This is a saved, opt-in diagnostic patch from the OOT investigation, not a default renderer change or a demonstrated pacing improvement. It preserves the source as it was investigated rather than silently losing it during upstream integration.

The patch adds `DM_PRESENT_MIN_CONTROL=1`: in selected 180-frame blocks after frame 3600, asynchronous presentation requests 98% of its normal positive minimum duration. Frame simulation, image work, FIFO and the requested displayed target remain unchanged. Target-timed and immediate presentations retain their old path. Extra trace labels identify control blocks. The patch also expands the optional asynchronous GPU trace's stdio buffer from 1 to 4 MiB.

No performance improvement or whole-game image-equivalence claim is attached to this saved control. It is not compiled into the integration branch. Forward-port and inspect the patch before applying it to a newer renderer; measure actual positive display callbacks, not calls to `present`. Keep the clock guard, scene, output size and power conditions matched. Do not use synchronous screenshots inside a measured window. See [the dated findings](../../../../../docs/oot-20261114-findings.md) for the observed screenshot stall and unresolved native-60 deficit.
