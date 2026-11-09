# Reussir: Code de la route (`0100EA300DE6E000`)

Update 1.0.1 (`v65536`), Unity 2018.3.9f1 IL2CPP with FMOD and NintendoSDK_movie, SDK 7.3.2, NVN. No new host or rewriter fix was needed. Its SDK build is shared with Raging Loop and already has the address-region cuts in `sdk_patches.py`.

## Validation

- **Current, 2026-11-02:** built the registered profile on chain `e047be1b6829`, on an Apple M1 Pro (MacBookPro18,3), macOS 27.2 (26B5091g), Xcode 27.2 (27B52), with the pinned Metal compiler 32023.922. All 614 shaders compiled for both Mac and iOS; Mac pipeline prebuild succeeded. The unsigned IPA is about 1.3 GB and passed ZIP integrity, display-version, and profile/library stamp checks.
- **iPhone, 2026-11-02:** the owner tested the updated 1.0.1 IPA built from `047f474c` on TF4-iPhone (iPhone 16 Pro, iOS 27.2, build 24B5089g) and reported that the game works correctly, including local multiplayer. This is owner-reported device validation; no iOS log, test duration or detailed mode coverage was supplied.
- Played the intro, menus and a complete ten-question training set on the Mac: answer selection, correct/incorrect feedback, score changes and the 8/10 result screen. The save contains all ten answers and the score; restarting with that save restored the training progress and showed the next level unlocked. The run lasted about six minutes, with no crash or shader-load errors; logged resident memory peaked at 879 MB and Metal allocations at 545 MB. This is a functional check, not a before/after performance comparison.
- Sound uses the real AudioUnit output and produces nonzero PCM. The owner confirmed it sounded normal after restoring the system volume. One 216 ms device-callback interruption triggered the host's existing AudioUnit restart; later reports showed zero underruns. The interruption did not recur during the completed set.
- Python 3.14.7 tool-suite discovery passed 902 tests with 16 optional skips. Those skipped checks then passed separately: seven compiler-backed image-atomic checks (13 tests including portable checks) with the committed translator, and nine ARM64 timing checks with Unicorn. Documentation/profile tests pass, and `docs_symptoms.py` regenerated the symptom index without changes.
- An earlier run overlapped the iOS build's refresh of the Mac libraries, so it logged missing shaders. That run was discarded; the completed set used stable libraries after both builds finished. No compiler override was used. No `validated.tsv` is claimed: these are direct builds and gameplay checks, not a recorded `regress.py` run.

## Extended checks (2026-11-02)

- The complete native graphics suite passed (`make -C dm/gfx/tests GAME=0100EA300DE6E000 run`), including its separately invoked pipeline-archive test. Graphics Python discovery passed 200 tests with the compiler, Metal, GPU and native fixtures enabled; the one discovery skip is that separately invoked archive test. The geometry fixture exercised 40 PrimitiveID GPU cases. All 13 image-atomic tests passed with Metal/GPU checks enabled, and all 42 iOS Python tests passed.
- The earlier host and ASan/UBSan runs completed with one AV1 assertion failure on this M1 Pro. After updating to `f6bf0d9b`, the upstream dav1d software decoder resolved it: `t_codec_video` passed under ASan/UBSan, including actual AV1 frame decoding. No sanitizer findings were reported. The focused retest also passed 147 HID checks, Apple controller mapping and 19 raw-HID checks; the entire suite was not rerun after the update. Host `tsan`, `memory-tsan`, and `tsan-threads` passed. Graphics `thermal-tsan`, `perf-order-tsan`, and `gl-imports` passed. Texture tests and their ASan run passed (5,344 CPU checks).
- Further gameplay covered all ten sign-building rounds (2/10), the result and answer-review screens; Survival's question video, wrong-answer feedback and result; lesson text/images and page/chapter navigation; progression tabs and history; changing/restoring the voice slider; and changing the avatar. History lists the completed training, sign-building and Survival sessions. The Survival leaderboard connection reported failure and online play is disabled in the menu.
- A separate synthetic save unlocked the mock exam for validation: all 40 questions completed, the result was 5/40, and answer review worked. The same fixture completed the twenty-question timed training level 130 (0/20), including countdown, timeout, feedback and result handling. These checks do not validate the normal unlock paths; the original progression-test save was not modified.
- The extended headless gameplay overlapped synthetic audio-device tests and logged two recovered AudioUnit interruptions. It had no crash or shader-load errors; this overlap is not used as an audio-stability measurement.

- The `f6bf0d9b` Mac rebuild passed with all 614 shader libraries and pipeline prebuild; the intro and menus rendered correctly. Two audio callback interruptions (214 and 236 ms) recovered through AudioUnit restarts. This focused retest did not rebuild the IPA or test an iPhone.

## What it needs

- `depth_mode zero`: the Mac run logs `depth mode near=0`; offline pipeline bases need the corresponding vertex entry.
- The shared SDK address-region cuts, the 5 GB guest layout and the hardware application pool in [game.tsv](game.tsv). The NPDM declares 16 MB of system resource, and the boot check sets no heap.
- French (`--language fr`), the only advertised language; controller navigation with the 2D layout and default docked mode. Touch did not activate the tested menu control in handheld mode.

## Files and shaders

The current reader decodes 614 shaders: 300 vertex, 299 pixel and 15 compute. The original bring-up's 599 shaders omitted the compute kernels; the registered tables now include them. The reader also supplies 291 legacy Unity pipeline pairs in `pso_pairs.tsv`. Assets and shaders use the existing Unity serialized-file and bundle readers; the four shaders embedded in `main` use the GLSLC reader.

Handheld mode renders into a 1280x720 crop of a 1920x1080 swapchain. Raw diagnostic screenshots include the unused area unless `DM_NVN_SHOT_OUT` is set before boot.

## Source container

The supplied combined XCI lists the update before the base. The original extractor picked the first Program NCA and could not read that patch alone (LibHac reported an alignment error). The secure partition's content metadata was used to put the base and update contents into separate temporary NSP containers, named with title IDs and versions, before running the ordinary packing script. This profile does not add combined-XCI import support.

## Open

- **macOS local multiplayer** still did not pass in the `f6bf0d9b` retest: two physical Pro Controllers were registered separately by the rebuilt windowed host, and the controller applet reported two connected, but neither A nor B on controller 2 joined a second player. The cause is not established; the owner subsequently confirmed local multiplayer works on iOS.
- This is sampled mode coverage, not an exhaustive pass through every question, lesson, training level or normal unlock path.
