# Pokémon Shield (`01008DB008C2C000`)

Update 1.3.2 (`v458752`) over the base game, SDK 7.3.2. Pokémon Sword's twin ([0100ABF008968000](../0100ABF008968000/README.md)): the same engine, and the same `sdk`, `subsdk0` (movie) and `subsdk1` (GLSLC) builds; only `main` is a build of its own (`a1680262…`, Sword's is `a3b75bcd…`). Everything in Sword's note about the engine, its services, files, shaders and rendering holds here; this note gives only what differs or was checked again. The profile is Sword's with Shield's title, name, IPA name, build row and shader tables. Plays on the iPhone (IPA 12.4 GB, user, 2026-10-26) and on the Mac at 30 fps, as on the Switch.

## What it needs

The same keys as Sword's profile, for the same reasons ([Sword, What it needs](../0100ABF008968000/README.md#what-it-needs)): the 4 GB layout with a 3.13 GB heap (`main.npdm` declares no system resource either), `mem_used 0x4c00000`, `pad_layout full`. The node area cut and the alignment of large regions come with the `sdk` build (`fcf93a41…`, `dm/tools/sdk_patches.py`). No host code is keyed to either game's `main`.

No `patches.py`.

## Files and shaders

- The RomFS has the same layout as Sword's (GFPAK archives under `bin/archive`, Wwise banks, loose BNSH files); `bin/sound/NX64/wwise/bank/Stream.pck` is the `romfs_marker` again.
- Shaders: 41,516, the same as Sword's. The split's three tables have the same data rows byte for byte (the game's own shaders, the GFPAK and loose BNSH files, and YEBIS's `.tfxo` stream in `main`, at the same offset); only the map's header note differs. The prewarm list is Sword's (it names shaders by content key).
- Each shader's cache key covers the profile's `shader_*` keys, so Shield's build does not hit Sword's cache entries; with `DMSH_PREFILL` naming Sword's library roots, the Metal compile cache takes Sword's libraries and only the translation runs again (the Mac build: 21,974 filled from Sword's, 45,728 reused, 176 compiled; 15 minutes for the 41,516; the iOS build: 21,684 filled, 175 compiled). IPA 12.4 GB.

## Performance

- On the Mac: 30 fps as on the Switch (frame time p50 33.3 ms, p95 34–35 ms in the house and outside in Postwick), no red line in 10 minutes from a fresh save.

## Mac routes

- The work folder's `run/boot.sh`, `run/ctl.sh` and `run/step.sh` are Sword's, run headless as there; `run/mash.sh <prefix> <rounds>` presses A ten times a round and takes a screenshot after each.
- From a fresh save (`DM_KBD` answers the name; without it the keyboard counts as cancelled and the game goes back to the photo), A at the language (about frame 600), the photo and the name reach the opening; A presses lead through it to the living room in Postwick, and the stairs to the bedroom with the Bag.
- `run/save_ref` is a save in the bedroom with the Bag taken, the same place as Sword's: Sword's route out of the front door (`LDown~0.8 LLeft~2.5 LRight~0.5 LDown~2.5 LLeft~0.3 LDown~2`, `run/route_out.sh`) works unchanged.
- The route checked on the Mac: the first boot's language, photo and name choices, the opening (the Champion's broadcast, the stadium), Postwick (the house, Hop's visit, the bedroom and the Bag), a save, and out of the front door to Hop.
