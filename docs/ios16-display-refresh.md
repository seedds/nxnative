# iOS 16 automatic high refresh

The Z-A iOS production package requests up to 120 Hz automatically, with no additional
settings screen or interaction. `DM_DISPLAY_HZ=120` requests callback cadence
through UIKit's `CADisplayLink`, available on this iOS 16 build. The preferred
rate is clamped to the window screen's supported maximum, up to 120 Hz. A screen
which supports only 60 Hz requests 60. Low Power Mode, thermal conditions and
system policy may still provide a lower callback cadence. The supported maximum
and requested/preferred rate are logged independently from measured results.
An unset `DM_DISPLAY_HZ` retains the existing system policy; an explicit 60 value
remains available for diagnostic comparisons through the environment file.

This frontend link never acquires a drawable, submits Metal work, generates
frames or advances the guest clock. The existing classic presentation thread
continues to own `nextDrawable` and `presentAfterMinimumDuration:`. The combined
Z-A iOS package separately uses `DM_PRESENT_HZ=120`, `DM_PRESENT_INTERVAL=game` and
`DM_ZA_FPS_MODE=dynamic60`. The explicit present-Hz request is clamped against
the active screen on the main thread before the guest starts and before NVN
caches the setting. The ordinary 60 Hz default is not raised implicitly.

Despite its historical name, `dynamic60` measures positive whole-frame active
time, including durations below 16.667 ms. It forces graphics code 0 (native
present interval 1), while publishing measured clock durations rather than a
fixed 60 FPS step. A 120 Hz present denominator then sets an 8.333 ms scheduling
floor on a supported screen. The iOS 16 classic path passes that period directly
to the drawable and its host acquire pacer; `mtl_fps_target()`'s 60 result is not
a scheduling cap on this path. Rendering remains limited by real CPU/GPU work.

The higher present denominator must not be paired with unchanged native game
timing or the fixed `static60` mode: those combinations do not establish correct
game speed. The dynamic mode retains its exact-build/table ownership gates,
positive ordered duration entries, original disabled-channel handling, explicit
16.667 ms seed and 50 ms long-stall limit. It remains a measured-clock experiment,
with secondary-provider and gameplay coverage described in
[the timing notes](ios16-za-dynamic-timing.md).

The iOS 16 backend cannot use its `CAMetalDisplayLink` branch, which first exists
on iOS 17; its classic `link_sync` disables `DM_PRESENT_LINK`. Setting
`DM_PRESENT_LINK=2` therefore does not enable a 120 Hz path in this build.

Every two foreground seconds, `display cadence` logs measure intervals between
actual `CADisplayLink.timestamp` values, the average target period and callback
period range. The same window independently counts real drawable presentations
through the existing `mtl_present_count()` counter. For example, 120 callbacks
per second with 30 drawable presentations is reported as 120 Hz and 30 FPS.
The existing visible FPS counter continues to show actual drawable FPS. Callback
cadence is not a claim about newly rendered game frames or panel scan frequency.

The link pauses and its measurement window resets when the scene loses focus or
enters the background. It reconfigures and starts a fresh window on activation;
background time is never averaged into the foreground callback rate. A changed
window-screen maximum also resets the measurement.

The portable policy/probe tests cover 60-only hardware, both requested rates,
invalid configuration, independent 120 Hz/30 FPS and 60 Hz/60 FPS windows, pause,
counter reset and invalid timestamps. Run
`python3 dm/ios/tests/test_display_refresh.py`.

The existing timing regression suite now also covers steady 60/120-rate active
time, 120-rate jitter, and the actual table-consuming clock fixture at a 120-rate
cadence. Every measured frame must conserve accumulated active time within 2 ms,
including the original one-frame provider latency. The 120-rate fixture consumed
time at ratio 0.999761111 and a latency-adjusted ratio 1.000041429 over 30 seconds;
no limited time was recorded. Run
`make -C dm/host/tests GAME=0100F43008C44000 za-timing`.

An earlier M2 field sample had 17.02–25.47 ms median frame windows, approximately
39–59 FPS, which do not meet a 120 FPS renderer's 8.33 ms budget.
Actual callback cadence, drawable presentation intervals and game-clock speed
require separate device evidence; a 120 Hz preference establishes none of them
by itself.
