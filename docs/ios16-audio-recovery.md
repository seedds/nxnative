# iOS 16 audio-session diagnostics and recovery

The game keeps the SoloAmbient category and therefore respects Silent Mode. Its
non-mixing audio session is released when its scene resigns active, including a
visible but inactive Stage Manager card, and on background entry. AudioUnits are
stopped before deactivation with NotifyOthersOnDeactivation. Graphics behavior is
unchanged for a merely inactive card; another running app or VM is not closed.

Activation failure keeps real output suspended while the silent sink continues
buffer pacing. Main-queue activation retries back off through 1, 2, 4 and 8 seconds,
then continue at eight-second intervals only while active, foreground and not
interrupted. A long-held session therefore remains recoverable. Unchanged failure
messages are suppressed after the first few attempts; changed errors are reported.
Generation tokens cancel stale retries on lifecycle changes, interruptions,
successful activation and media-service resets. Repeated route notifications do
not reset the budget or recursively reconfigure the category. Foreground activity
starts a new recovery opportunity; a competing audio session is not forcibly ended.

After a media reset, missing AudioUnits retain their real-output intent. A started
output retries recreation with backoff up to eight seconds; stopped and suspended
outputs are not started by that retry. Creation happens on a helper stack. Backend
ownership plus a non-blocking pull guard keeps an obsolete callback from becoming
a second queue consumer when the device is stopped or reset.

Session events report category, mode, options, output volume, current output port
types/names, sample rate, I/O duration, latency, interruption and lifecycle state.
These detailed event snapshots are capped at 64 per run. The callback publishes
only a bounded PCM level summary after gain and diagnostic muting, sampling no more
than 4,096 PCM16 values. It performs no allocation, logging, locks or environment
lookup on the normal callback path. The watchdog reports peak, RMS and zero counts
after the first 100 callbacks and every 2,000 thereafter; raw PCM is not logged.
Nonzero PCM and advancing callbacks establish delivery to the AudioUnit, not
audibility at the speaker. Route selection and Silent Mode still need inspection.

For a reversible Silent Mode comparison, `DM_AUDIO_SESSION_CATEGORY=playback_mix`
selects Playback with MixWithOthers. Unlike the default SoloAmbient category,
Playback ignores Silent Mode; the mixing option permits other apps' audio to
continue. `DM_AUDIO_SESSION_CATEGORY=solo`, an unset value or an empty startup
value retains the original category. Other values are rejected. This diagnostic
does not choose an output port, adjust system volume or terminate another app.

The existing CTL interface can change the category without restarting the game:

```text
env DM_AUDIO_SESSION_CATEGORY=playback_mix
env DM_AUDIO_SESSION_CATEGORY=solo
```

Only the exact category key reaches the platform-neutral registered observer.
The iOS observer parses the value before CTL's line buffer is reused, then
captures the enum onto the main queue. It stops its AudioUnits and deactivates
its own session before configuring the chosen category and reactivating. Inactive,
background, interrupted and media-services-lost states defer the change until
eligible; a category request cancels the preceding configuration's retry epoch.
The default category and all interruption/lifecycle eligibility rules remain in
effect. No additional framework dependency is introduced in the generic HID host.

Explicit category commands produce route/PCM snapshots before and after the
transition, plus a guarded snapshot after 250 ms with the number of new callbacks.
These explicit diagnostic snapshots remain available after the normal 64-event
log budget is exhausted. A subsequent focus change, category change or recovery
epoch invalidates the delayed snapshot. Requested policy and actual category,
mode, options and route are logged separately so a failed configuration cannot be
mistaken for a successful comparison.

These fix concrete recovery defects but do not establish the cause of a reported
system-wide mute. The initial device log had advancing callbacks and fed queues
without an activation failure. A later foreground snapshot after the recovery
changes showed Speaker output, two channels, volume about 0.7 and nonzero
post-gain PCM (peak 3,834; RMS 0.0386; zero samples 0/2,048), while the user still
reported silence. The category comparison distinguishes Silent Mode behavior
from a fault further downstream; it does not establish the cause in advance.

Hardwareless regressions:

```sh
clang -O1 -Wall -o /tmp/t_audio_session_retry dm/host/tests/t_audio_session_retry.c
/tmp/t_audio_session_retry
clang -O1 -Wall -I dm/host -I build/compile/profiles/0100F43008C44000 \
  -o /tmp/t_audio_recovery dm/host/tests/t_audio_recovery.c -framework AudioToolbox
/tmp/t_audio_recovery
```

These tests cover parser defaults and rejection, live CTL callback delivery,
concurrent observer registration and retry cancellation on category/focus
transitions. AudioUnit lifecycle tests use injected calls; no hardware test claims
speaker audibility or AVAudioSession category behavior.

Apple references: [session activation](https://developer.apple.com/library/archive/documentation/Audio/Conceptual/AudioSessionProgrammingGuide/ConfiguringanAudioSession/ConfiguringanAudioSession.html),
[interruptions](https://developer.apple.com/library/archive/documentation/Audio/Conceptual/AudioSessionProgrammingGuide/HandlingAudioInterruptions/HandlingAudioInterruptions.html),
and [media-services reset recovery](https://developer.apple.com/library/archive/qa/qa1749/_index.html).
Category diagnostic references: [Playback](https://developer.apple.com/documentation/avfaudio/avaudiosession/category-swift.struct/playback)
and [MixWithOthers](https://developer.apple.com/documentation/avfaudio/avaudiosession/categoryoptions-swift.struct/mixwithothers).
Apple permits games without a media Play/Pause interface to resume after an
interruption without requiring ShouldResume; that behavior is retained.
