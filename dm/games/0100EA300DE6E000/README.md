# Reussir: Code de la route (`0100EA300DE6E000`)

Update 1.0.1 (`v65536`), Unity 2018.3.9f1 IL2CPP with FMOD and NintendoSDK_movie, SDK 7.3.2, NVN. No new host or rewriter fix was needed. Its SDK build is shared with Raging Loop and already has the address-region cuts in `sdk_patches.py`.

## Validation

- **Current, 2026-11-02:** built the registered profile on chain `e047be1b6829`, on an Apple M1 Pro (MacBookPro18,3), macOS 27.2 (26B5091g), Xcode 27.2 (27B52), with the pinned Metal compiler 32023.922. All 614 shaders compiled for both Mac and iOS; Mac pipeline prebuild succeeded. The unsigned IPA is about 1.3 GB and passed ZIP integrity, display-version, and profile/library stamp checks.
- Played the intro, menus and a complete ten-question training set on the Mac: answer selection, correct/incorrect feedback, score changes and the 8/10 result screen. The save contains all ten answers and the score; restarting with that save restored the training progress and showed the next level unlocked. The run lasted about six minutes, with no crash or shader-load errors; logged resident memory peaked at 879 MB and Metal allocations at 545 MB. This is a functional check, not a before/after performance comparison.
- Sound uses the real AudioUnit output and produces nonzero PCM. The owner confirmed it sounded normal after restoring the system volume. One 216 ms device-callback interruption triggered the host's existing AudioUnit restart; later reports showed zero underruns. The interruption did not recur during the completed set.
- Python 3.14.7 tool-suite discovery passed 902 tests with 16 optional skips. Those skipped checks then passed separately: seven compiler-backed image-atomic checks (13 tests including portable checks) with the committed translator, and nine ARM64 timing checks with Unicorn. Documentation/profile tests pass, and `docs_symptoms.py` regenerated the symptom index without changes.
- An earlier run overlapped the iOS build's refresh of the Mac libraries, so it logged missing shaders. That run was discarded; the completed set used stable libraries after both builds finished. No compiler override was used. No `validated.tsv` is claimed: these are direct builds and gameplay checks, not a recorded `regress.py` run.

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

- iPhone testing requires the owner to sideload the unsigned IPA; no device installation or signing was performed.
- Multiplayer, all other modes and question sets remain untested.
