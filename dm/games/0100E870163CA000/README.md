# Batman: Arkham Asylum (`0100E870163CA000`)

Update 1.0.1 (v65536) over the base game (6.84 GB; the update NSP is 2.56 GB and needs it). SDK 14.3.0, the same `sdk` build as [Batman: Arkham Knight](../0100ACD0163D0000/README.md) (`2aef2cff…`). Rocksteady's Unreal Engine 3 (package version 0x243, licensee 0x15) on NVN, with FMOD for the sound, Bink for the movies (`.bik`, decoded by the game on the CPU), Oodle for the packages and Scaleform for the menus. Modules `main` and `sdk`. Playable on the Mac: about 40 minutes from a new game, through the intake, the first fights and Detective Mode. It runs at 30 fps as on the Switch, and at 60 with the configurations' frame cap raised ([Frame rate](#frame-rate)). A double-clickable Mac app is packed; iOS is untested.

## What it needs

| Setting | Where | Why |
|---|---|---|
| The compact shader records | `dm/gfx/tools/shader_split.py` `ue3_compact` | the shader split found no shaders: see [Files and shaders](#files-and-shaders) |
| `depth_mode zero` | [`game.tsv`](game.tsv) | the game selects near-zero NVN depth at boot |
| `present_interval display` | `game.tsv` | one frame per display tick, so the configurations' frame cap decides the rate: see [Frame rate](#frame-rate) |
| Memory: `mem_reserve 0x100000000`, `sys_resource 0` | `game.tsv` | the survey's guess: the NPDM declares no system resource, and main sets a heap |

No code of the game is cut: the `sdk` takes the five cuts of the SDK-wide rules (`dm/tools/sdk_patches.py`), as Arkham Knight's.

## Files and shaders

- RomFS: 2,822 files, 6.72 GB: 2,674 cooked packages (`BmGame/CookedSwitch/*.xxx`, chunks of Oodle), the texture cache (`Textures.tfc`, 805 MB), the global shader cache (`GlobalShaderCache-Switch.bin`), the language configurations (`Coalesced_<language>.bin`) and 42 Bink movies (`BmGame/Movies`, 2.8 GB).
- **Resolved: the shader split found no shaders.** Unlike Arkham Knight's records (a code area and a whole 0x880-byte control section), this game stores each shader as a compact record: u32 code region length, u32 metadata length (under 0x880), u32 0, the code region (the code from its SPH, then its constants, then a second code) and the metadata (at most 0x40 bytes read). The loader (`main+0xcc6194`) takes a metadata length of 0x880 or more as a whole control section, and otherwise builds one from a fixed template (`main+0x164b5c0`, `main+0x1649a10`, `main+0x1649fd0`) and the metadata's lengths and GLSLC fields, and a code area with the magic, the code at +0x30, the constants at `(code + 0x12f) & ~0xff` and the second code 0x30 into the block after them. `ue3_compact_build` does the same; its control sections and code areas were identical, byte for byte, to the 39 that the game gave `nvnProgramSetShaders` at boot and that the global cache holds (`DM_NVN_SHDUMP`). The split finds 177 records in the global cache and 31,263 shaders in all (26,950 pixel, 4,313 vertex).
- Two packages hold data with a record's shape that is no shader: `Max_B6_CH12.xxx` (no graphics stage in its SPH) and `Overworld_A1_Tunnel.xxx` (a code length GLSLC never writes, so its control section would not match its code area). `ue3_compact` skips such records without a red line.
- The split's one red line, `main control section @0x164b5c0: no code area`, is the loader's template: it is expected.

## Movies

- **Open on `main`: the movies play at 16 fps** (the opening `101_BATMOBILE_INTRO.BIK`, 1280x720 at 29.97), frames of about 62 ms with the GPU at 0.4 ms and the CPU nearly idle. Bink's sound driver (on `nn::audio`'s renderer, opened from `main+0x94eac`) polls from the game's rendering thread, several times a movie frame (`main+0x950a0`): `RequestUpdateAudioRenderer`, then `GetReleasedWaveBuffer`. Each of those updates found no frame rendered since the previous one and rendered a catch-up frame; with the catch-up queue full (four frames) the update waited for the render thread to play one, about 5 ms, so the rendering thread spent most of each frame waiting (`catch-up renders so far` grew by about 1,800 a 2,000 renders). On the Switch an update never waits for a frame. A separate change to `dm/host/hle_audren.c` (an update with the catch-up queue full renders nothing and does not wait) makes the movie run at 30.0 fps; it is proposed on its own, since it reaches every game.

## Frame rate

- The game asks for present interval 2 (`WindowInitialize … interval 2`), and the engine's own frame smoothing caps it at 30 as well: `Coalesced_<language>.bin` sets `bSmoothFrameRate=TRUE`, `MinSmoothedFrameRate=24`, `MaxSmoothedFrameRate=30`, `MaxDeltaTime=0.0416666`. With `present_interval display` and the configurations as they are, it runs at 30.
- **60 fps:** `MaxSmoothedFrameRate` 30 → 60 in the ten `Coalesced_<language>.bin` (the value's two characters right after the key, so nothing else in the file moves), copied out of the player's own RomFS into a folder named by `DM_ROMFS_OVERLAY` (`BmGame/CookedSwitch/` inside it; for a Mac app, in `env.txt` through the NSP script's `--env-file`). The repository holds no such files. The menus and the intake hold 59.6 fps on an M4 (frame time p50 16.8 / p95 17.2 ms, GPU p50 7.6 ms). Unreal Engine 3 steps by the time that passed: walking forward for 3 s from the same spot ended at the same place at 30 and at 60 fps. The movies keep their own 30.

## Mac routes

- Build with `scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> --platform mac` (the `--platform mac-app` pack adds the app); its last line runs the game.
- From the first save onwards, with `DM_KEYS`: `35:A*12/4` (the language, the licence, the notices), `85:Down,86:Down,87:A` (the game options' Continue), `90:A`; the opening movie starts about 90 s in, `105:A,106:A` skips it, and the intake follows.

## Testing

- 2026-11-21, Mac (M4, macOS 26.6, Xcode 26.2's Metal compiler with `--allow-other-metal`): boot, the language screen, the licence, the autosave notice, the game options, the opening movie (and its skip) and the intake with the Joker, at 60 fps with the overlay, no red line and no shader missing from the map.
- 2026-11-22, the same Mac, the packed app at 60 fps: about 40 minutes of play from a new game, with the first fights and Detective Mode, and no fault. The log's last 26 minutes (82,200 frames): 243 of 274 windows of 300 frames at 60 fps (frame time p50 about 16.9 ms, GPU p50 7.1 ms, at worst 8.1), the other 31 the movies at 30. About 100 frames over 50 ms, the longest (150 to 270 ms, six of them) when a new area's pipelines were compiled for the first time (`[longframe] … new pipelines 26 to 61`); 1,756 pipelines were written to the archive at exit. Memory settled at about 2.8 GB resident and 1.6 GB of Metal allocations.
- Not tested: the later areas and their movies, the Xcode 27.2 Metal compiler, iOS.

## Open

- The movies at 16 fps on `main` ([Movies](#movies)).
- The first visit to an area stalls for up to about 0.3 s while its pipelines compile; there is no prewarm list (`pso_manifest.tsv` is empty).
- The heap and pool sizes are the survey's guesses.
- iOS: no IPA built.
