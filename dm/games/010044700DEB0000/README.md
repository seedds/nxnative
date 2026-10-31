# Assassin's Creed: The Rebel Collection (`010044700DEB0000`)

Base game, 1.0.0 (`v0`), SDK 8 (`sdk` build `3ae34e68…`). Ubisoft's AnvilNext engine on NVN; its data sits in 28 "scimitar" `.forge` archives (11.2 GB of the RomFS' 120 files). Modules `main`, `sdk`. The collection is a shell around Assassin's Creed IV: Black Flag and Assassin's Creed Rogue: its launcher re-launches its own program as the game it starts.

On the Mac the launcher boots, becomes Assassin's Creed IV, and a save loads and plays. The shader map covers the 326 programs in `main` and no more: the `.forge` archives keep 45,853 shader-library records behind a packing no reader on hand decodes, so the rest of the game's shaders come from run-time dumps. No IPA was built, and iOS is untested: `oe::ExecuteProgram` replaces no process in an app, so the shell cannot become the game there.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000`, `sys_resource 0x0` | [`game.tsv`](game.tsv) | `main` sets a heap of at most the pool − 64 MB (a 3328 MB heap region); the NPDM declares no system resource, so `IsVirtualAddressMemoryEnabled` is false, as on Horizon |
| `mem_used 0x6e00000` | `game.tsv` | the rewritten modules take more than the default 64 MB, which `SetHeapSize` checks the request against |
| `romfs_marker DataNX_CaribbeanSea.forge`, `zip_store .bik:.bk2:.forge:.pck:.metallib` | `game.tsv` | the archive the `.forge` files are layered under; the movies and forge data are stored, not compressed |
| `shader_groups … /shdump/=main` | `game.tsv` | the programs a run dumped carry the split's `/shdump/` label and belong in `main`'s package, the only package with rows from the game files |

No `patches.py`, and no host code keyed to this game: the two calls under [Services](#services) are general.

## Bring-up

The five steps, none of which builds inside the repository. The fourth is this title's work, because `main` alone is not the game's shader set.

```bash
ID=010044700DEB0000
NSP="/Volumes/T7 Storage1/Assets/Software/Switch Roms/1. TESTING/AC 4+/"   # the folder holding the base NSP
W="$NSP/${ID}_build"

# 1. Survey: what the game is made of (no build yet).
python3 scripts/build_ipa_from_nsp.py "$NSP" --survey-only

# 2. Split from the game files and build: main's 326 programs, the Mac host, the shader library.
python3 scripts/build_ipa_from_nsp.py "$NSP" --platform mac

# 3. Capture the rest. Every program a draw asks for that the map lacks is written
#    as <key>.ctl (the 0x880-byte control section) + <key>.code (the code area, its
#    length at control +0x704).
DM_NVN_SHDUMP="$W/shdump" <the run command step 2 printed>

# 4. Fold the dumps in: the same split reads that directory as a source.
DM_NVN_SHDUMP="$W/shdump" python3 scripts/build_ipa_from_nsp.py "$NSP" --platform mac

# 5. A Mac app, if that is what you want: the same build, packed and signed.
python3 scripts/build_ipa_from_nsp.py "$NSP" --platform mac-app
```

- **The shader step.** A dump directory is a shader source: `DM_NVN_SHDUMP`, or `<version dir>/shdump` beside the RomFS, which step 4 then finds with no variable set. Each `<key>.ctl` + `<key>.code` pair is read once, its key recomputed as `sha256(control[:nctl] + code)[:16]`, and the file name must equal it — a pair that hashes to another key (a dump of a control section the game had already freed) or a `.ctl` with no `.code` beside it stops the split and names the file, rather than writing a row that matches no draw. The rows carry the fixed label `/shdump/<key>#RAW@0x0`, so the map and the package indices that follow its sort do not depend on the folder a porter used, and [`game.tsv`](game.tsv)'s `shader_groups … /shdump/=main` puts them in `main`'s package. Because the rows are the split's own output they survive every later split; the map's header names the captured programs, and the NSP script compares a profile with the split only where the rows are facts about the game files (`file_rows`), so a profile built from dumps is not a mismatch for anyone else. The mechanism and its symptoms are in [the shader reference](../../../docs/reference/shaders.md).
- **Driving the capture.** A session that sits on the collection launcher captures nothing: it is the game and its levels that mint shaders. From a save, `run/ctl.sh` in the working folder drives launcher → A → splash → Plus → main menu → A (story) → A (save select) → in game, which is where the captures came from. `DM_KEYS` and `DM_CTL` are the scripted forms ([05, Debugging recipes](../../../docs/agents/05-debugging.md)).
- **The map is the coverage metric.** `ls "$W/shdump"/*.ctl | wc -l` after a session. The owner's map at the time of writing held **840 keys: 326 from `main` and 514 from dumps**; each dump session appends to the package it belongs to and never renumbers the file rows. The profile ships the 326 the game files give: dumps are game bytes and are not committed.
- **Pairs the game files do not declare.** Two shadow-application passes draw fragments `421ae232a7b7df44` and `c275f18d9d3ca090` with the fullscreen pull vertex shader `fa4f282f3b5ed0bd`, which writes no `locn3`: Metal refuses both pipelines, they are the only consumers of the shadow atlas (texture serial 913), and with them gone no shadow is drawn anywhere while the lighting composite silently does not run. The two pairs live in [`pso_pairs_manual.tsv`](pso_pairs_manual.tsv), the profile table `shader_split.py` merges into `pso_pairs.tsv` on every split (it is otherwise regenerated wholesale, so a row added afterwards is gone at the next one), and `dmsh_build.py`'s `vs_pad` then gives that vertex shader a `dm_pad3` output.
- **The build.** `dmsh_build.py` compiles what the map holds; unchanged keys are cache hits, so a fold compiles only the new ones. The library stamp and `c1_full.tsv`/`cb_extent.tsv` follow the chain version, which is one for every game.

## Services

The launcher cannot start a campaign in-process; it uses the two calls a multi-program application uses, and the host answered both with nothing until this bring-up:

- `nn::vi::OpenDisplay(Display **, const char *)` with `"Default"`, right after `vi::ListDisplays`: the SDK's body goes through the vi service object the host never made and null-dereferences at `+0xf4`. The host has one display and returns it, as `OpenDefaultDisplay` does ([vi layer and display calls without vi service objects](../../../docs/reference/services.md#vi-layer-and-display-calls-without-vi-service-objects)).
- `nn::oe::ExecuteProgram(0, "/launcher:off /gamesoundlanguage:en …")`: on hardware this does not return — the program ends and the system starts program 0 with those parameters, which it reads once with `nn::oe::TryPopLaunchParameter`. The host re-executes itself with the same executable and argv, leaves the parameters in `$DM_TEMP/launch_param.bin`, and `TryPopLaunchParameter` hands them over once. Without it the shell carries on into code that expects the other program's state and dies at `main+0x13a08dc` (SIGILL) one frame after the press. Another program index keeps the old behaviour (logged, the game carries on): the host runs one program. ([Applet resource user ID, launch parameters and VR mode](../../../docs/reference/services.md#applet-resource-user-id-launch-parameters-and-vr-mode))

## Files and shaders

- RomFS: 120 files, 11.2 GB of them in 28 `.forge` archives. `main` holds 326 programs (250 pixel, 76 vertex), all BNSH, and that is the whole of what `shader_split.py` can read: its BNSH walk does not apply to the `.forge` shader-library records (the `grsc` block sits at `+0x23`, not at `bfsha + 0x60`) and their payloads are an LZ stream none of zlib, LZ4, LZO, LZMA, zstd or LZF decodes. A flat scan for control sections over the archives finds no pair either.
- The 326 keys are one package per stage (`vs_main.bin`, `ps_main.bin`): the profile's `shader_groups` keep the dumped rows in the same package, so a fold adds indices to the tables rather than a new library.
- Missing: scenes the captures have not visited log `[RED] shader … is not in the shader map` and drop the draw, not the frame. The map is coverage, not correctness: a key that is present can still translate badly.

## Status on the Mac

- Boots to the collection's launcher and, with the two calls above, re-launches into Assassin's Creed IV; a save at 01:16 was loaded and played (the port's owner plays it regularly). The shell's own menus and the game's intro, HUD, world and audio run.
- Not measured: frame rate on this title, memory, load times. The work was about reaching the game and its shaders, not about performance.
- Not checked: an iPhone / iPad build; anything past the scenes under [Mac routes](#mac-routes); a second campaign (Rogue) beyond the shared menu.

## Mac routes

- The working folder's `run/ctl.sh` drives the run above from a fresh save: A (launcher), A (splash), Plus, A (story mode), A (save select), in game at the first mission.
- `run/save` is a manual save at that point: loading it puts the game straight into the world.

## Found on this game

General changes first needed here; each applies to every game:

- **`nn::vi::OpenDisplay` by name** (`hle_vi.c`): [vi layer and display calls without vi service objects](../../../docs/reference/services.md#vi-layer-and-display-calls-without-vi-service-objects).
- **`oe::ExecuteProgram`, and the launch parameter it hands over** (`hle_oe.c`, `$DM_TEMP/launch_param.bin`): [Applet resource user ID, launch parameters and VR mode](../../../docs/reference/services.md#applet-resource-user-id-launch-parameters-and-vr-mode).
- **A run's dumped programs as a shader source** (`shader_split.py` `dumped_shaders`, `build_ipa_from_nsp.py` `file_rows`): [the shader reference](../../../docs/reference/shaders.md).
- **A profile's own pipeline pairs** (`shader_split.py` `manual_pairs`, `pso_pairs_manual.tsv`): written here for the two shadow-application pairs, and useful to any title whose files declare no pairing.

## Open

- **The `.forge` shader records.** 45,853 shader-library records, payloads packed with a stream of 2–4-byte control codes (no zlib, LZ4-block, lzo1x, lzma, zstd or LZF at any offset 0–80 decodes them). Whoever identifies that codec turns the map's tail into the whole set in one build.
- **Coverage per area.** Every new area mints shaders; the capture step above is the loop, and one dump directory per session keeps them sorted.
- **Vertical lit/unlit bands in the owner's screenshots.** Not reproduced headlessly from the same save (39 shots, none banded), and a pass dump closed the shadow-atlas question: the four cascade passes each use the viewport the game set, and slot 0 of the atlas is simply unused. Candidates left: a per-tile light list read with a wrong index, or a window-vs-render-size mismatch in the windowed app. `DM_NVN_PASSDUMP` prints the viewports a pass sets (`O` lines).
- **iOS.** Nothing was built or run there. `oe::ExecuteProgram` replaces no process in an app, so the shell cannot become the game on iOS as it does on the Mac; it logs that and carries on, which is where the iOS work would start.
- **The prewarm list** ([`pso_manifest.tsv`](pso_manifest.tsv)) is empty: no pipeline list was recorded, so the first frames of a scene compile pipelines as they are drawn.
