# Reussir: Code de la route (`0100EA300DE6E000`)

- **Status:** update 1.0.1 (`v65536`) brought up on the Mac through the intro movie, character selection, menus, training tutorial and first question. Four minutes without a red runtime line or crash; resident memory settled near 694 MB at the question, Metal allocations near 408 MB. All 599 shaders build for Mac and iOS. The unsigned IPA is about 1.4 GB; iPhone validation, audible sound, full question sets and multiplayer remain untested.
- **Engine:** Unity 2018.3.9f1 IL2CPP with FMOD, NVN and NintendoSDK_movie on SDK 7.3.2. Its SDK build is shared with Raging Loop and already has the address-region cuts in `sdk_patches.py`; no new host or rewriter fix was needed. The 5 GB guest layout keeps the hardware application pool; the boot check sets no heap. Shaders come from Unity serialized assets and bundles, using the existing Unity 2018.3 reader.
- **Input:** controller navigation, with the 2D pad layout and default docked mode; a restart kept the selected character and skipped the completed tutorials. French is the only advertised language; pack with `--language fr`. Handheld renders into a 1280x720 crop of a 1920x1080 swapchain; raw diagnostic screenshots include the unused area unless `DM_NVN_SHOT_OUT` is set before boot.
- **Source container:** the supplied combined XCI lists the update before the base. The existing extractor picks the first Program NCA and cannot read that patch alone (LibHac reports an alignment error). For this bring-up, the secure partition's content metadata was used to put the base and update contents in separate temporary NSP containers, named with title IDs and versions, before running the ordinary packing script. Direct combined-XCI import remains unsupported.
## Validation scope

These measurements and the unsigned IPA came from the original checkout at `ebe75c9`, with pre-existing local changes. The profile is ported here without those unrelated host or tool changes. Gameplay and shader compilation on this newer checkout have not yet been repeated. No `validated.tsv` is claimed for its chain.

## What it needs

- `depth_mode zero`: the Mac run logged `depth mode near=0`; this newer checkout uses the profile value for offline pipelines.
- The shared SDK address-region cuts, the 5 GB guest layout, and the hardware application pool in [game.tsv](game.tsv).
- French (`--language fr`) and controller navigation with the 2D layout.

## Open

- Rebuild and run on the current translation chain before requesting merge.
- Test on an iPhone, check audible sound, complete question sets and multiplayer.
- Combined-XCI import was not fixed by this profile; the original bring-up used separate base and update NSP containers.
