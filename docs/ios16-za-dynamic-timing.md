# Z-A whole-frame dynamic timing experiment

`DM_ZA_FPS_MODE=dynamic60` remains opt-in for the general host, while the Z-A
iOS production profile selects it explicitly. It requests a 60 Hz graphics
interval while advancing the game clocks by measured whole-frame active time.
It does not make a renderer that takes 22 ms render at 60 FPS. Hardware results
must separately establish frame rate, real game-clock speed, and scene stability.
Use `DM_PRESENT_INTERVAL=game`; do not confuse display frequency with game speed.

## Why the earlier modes are insufficient

For Z-A 2.0.2, the original clock-selection work interval at `main+0x1b296f0`
can be about 6 ms even when the complete frame takes 28 ms. `static60` and the
existing `adaptive60` therefore select a 16.667 ms game step and run slowly in
that scene. Neither mode is changed by this experiment.

The independently implemented dynamic controller samples `guest_ns()` at
successive interval observations. This timebase excludes system background
suspension. The original controller still receives graphics code 0 in w20, while
w22 selects clock code 0 or 1 after the native max operation. Only that selected
entry of the shared duration table changes; the other entry is nominal.

## Exact data and mapping gates

The full original NSO build ID inspected offline is
`b1f12fd919eae86ab8a978317677e64bce443d1f000000000000000000000000`.
Runtime uses the loader's exact supported 16-byte build ID field
`b1f12fd919eae86ab8a978317677e64b`, both instruction-validated observation gates,
and exact original segment boundaries: text end `0x3163f70`, ro start
`0x3164000`, data start `0x3bc2000`.

The naturally aligned table at `main+0x3393e20` must initially contain five
64-bit nanosecond values:

| Code | Original duration |
| --- | ---: |
| 0 | 16,666,667 |
| 1 | 33,333,334 |
| 2 | 50,000,000 |
| 3 | 66,666,667 |
| 4 | 83,333,334 |

Its full 40-byte range must be inside the original ro segment and a currently
readable, writable, nonexecutable Mach VM leaf mapping. No permission change is
attempted. Deterministic-clock mode is rejected because it does not provide
whole-frame active wall time. A failed prerequisite retains game timing.

Original providers at `main+0x1b2a0e4`, `main+0x1b5407c`, and `main+0x29f4214`
read these table entries before passing nanoseconds to their existing clock
setters. The base clock's actual delta is observed at `main+0x1b292dc` before
the original load and conversion to engine seconds. Shared-table ratio/quality
callers also exist, so table ordering and positive values are retained.

Only aligned entries 0 and 1 are atomically updated. Slots 2 through 4 are checked
but never written. Pair publication also preserves ordering between its two
stores. Every publication verifies the previous owned values; an unexpected
writer returns the mode to game. Returning to game/static60/adaptive60 restores
only entries that still equal this controller's owned values. A foreign value
is reported and left intact. Guest executable instructions, SDK code, code
caves, scratch BSS, and clock-provider pause flags are never changed.

## Timing, jitter, and pause policy

The accepted active frame range is `(0, 50]` ms. A quarter-weight integer EMA
smooths timing, but signed phase debt bounds the accumulated difference between
measured and issued time to 2 ms. Unlike a free-running EMA or integer-FPS
quantization, a persistent change in frame time cannot accumulate clock drift.
The emitted step can differ from one raw frame by at most 4 ms within this range.
Clock tier 1 is preferred below 46 FPS, subject to table-order bounds. There is no
integer FPS rounding. The first callback has no preceding full frame and issues
one explicitly counted nominal 16.667 ms seed.

An active frame above 50 ms is limited to preserve ordering before the unchanged
native 50 ms tier. Its discarded active time is explicitly logged and included
in `limited`; it is not repaid as later acceleration. This policy intentionally
does not claim correct speed below 20 FPS or across real multi-second stalls.
Clock discontinuities get a separately counted seed. All such events are
visible, and a nonzero `limited` requires examination before a speed claim.

A system suspension creates a large wall/active-time difference and resets only
the EMA. It preserves phase debt and uses the actual positive active delta on
resume, with no background catch-up. A zero active delta publishes nothing and
leaves native handling in place. Original disabled channels continue to receive
zero through their original setter. The native adaptive long-work guard above
one second executes after our observation point; reaching it explicitly returns
to game mode instead of silently changing its code-4 semantics.

Reissuing `dynamic60` changes the diagnostic epoch without reseeding the frame
controller or changing its table. The observer's two-sample warmup does not
affect engine time. A real mode change can have one preceding-mode frame in
flight; original values are restored immediately and no zero table duration is
introduced. Switching back into dynamic60 starts a new explicitly counted seed.

## Validation and device acceptance

Run `make -C dm/host/tests GAME=0100F43008C44000 za-timing`.
Pure tests cover steady 37 FPS, positive/negative periodic jitter, asymmetric
jitter, boundary values, long stalls, a five-minute suspension, and resume with
or without a callback during the frozen period. Conservation checks hold after
every measured frame.

The integration test uses actual Mach mappings and a model of the original
table lookup plus disabled-channel setter. It counts the deltas actually
consumed by that clock, independently of diagnostic counters. It resets the
diagnostic epoch every 23 frames over 3,600 frames. The time genuinely consumed
by the modeled provider remains within one pending frame plus 2 ms of active
time; after closing that explicit one-frame latency, error is within 2 ms
(latency-adjusted ratio 0.999979613 over 98.1 s). Additional cases
reject wrong build/layout/table, unaligned data, deterministic clocks, read-only
and executable mappings, exercise ownership conflicts and the native guard,
and confirm restoration when switching to existing modes. The preexisting
opt-in/register-preservation/live/concurrent-mode tests remain unchanged.

For a device A/B, opt in at startup with `DM_ZA_TIMING=1`. Live changes can then
use `DM_ZA_FPS_MODE=dynamic60` and `game` through the existing control path.
Record at least three 300-frame windows of actual `ratio_active`, selected
codes, issued and measured totals, seeds, and limited time. The issued total is
not a substitute for the actual observed engine clock. Long-window phase error
includes a normal one-frame provider latency, so a few initial samples are not
a speed result. Confirm menus, cutscenes, disabled channels, suspension/resume,
movement, camera, and combat before claiming compatibility or stable 60 FPS.

## Provenance

The factual table, channel semantics, and original instruction addresses were
verified from this exact locally supplied original executable. The reference
author's RC3 observes full-frame system ticks and changes the same two duration
slots, but its code cave and SDK machine code were not copied or distributed.
The downloaded pack had no explicit reusable source license. This host policy,
integer phase-bound algorithm, validation, and tests are independently written.

Reference author: [Fl4sh9174's original repository](https://github.com/Fl4sh9174/Switch-Emulator-Ultrawide-FPS-Mods).

## Initial device measurement (2026-10-25)

On M2 / iPadOS16.1 in the restored alley scene, 900 observed base-clock frames
advanced 19.758014 seconds over 19.765974 active seconds (ratio0.999597). The
controller reported no limited active time during this sample. This establishes
the observed base clock, not every secondary provider or animation clock. Actual
frame time varied: 300-frame windows had medians from17.02 to25.47ms, so this
was not stable 60 FPS and that diagnostic session was returned to `game` mode.
The production iOS profile later selected `dynamic60` to avoid a fixed 30 FPS
limit while retaining the measured near-unity game-clock ratio. This remains a
request for up to 60 FPS, not a claim that every scene renders at 60 FPS.

The inspected native interval loop supplies one selected code to every enabled
channel of its two-channel provider; disabled channels get zero. The executable
integration fixture currently models one consuming channel. Independent callers
of the other two provider families have not all been proven to select that same
code, so full secondary-clock/gameplay coverage remains required.
