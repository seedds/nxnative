# Borderlands: Game of the Year Edition (`010064800F66A000`)

1.0.2 (v131072). SDK 9.3.0. Gearbox's Unreal Engine 3 (package version 595) on NVN, with FMOD for the sound; modules `main` and `sdk`. Plays on the Mac at 30 fps, as on the Switch: the title screen, the main menu, character select, the Arid Badlands in first person with the HUD, a save loaded from disk, a save written from the pause menu, and save-and-quit back to the main menu. An unsigned IPA has not been built; iOS is untested.

Four things stood between the game and a picture, and three of them are the same finding seen from different ends: the 922 cooked packages are zero-padded past their last chunk, one vertex shader aborted the whole shader compile, the two-part vertex path read its constants from the wrong side of part B, and the account service reported a user before the application had opened one — which emptied the character list, named a save device that cannot exist, and left the save-and-quit panel up for ever.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `mem_used 0x5e00000` | [`game.tsv`](game.tsv) | the provisional 64 MB made `nninitStartup` ask for a heap larger than the app pool; see [Memory](#memory) |
| `depth_mode zero` | [`game.tsv`](game.tsv) | the game selects near-zero NVN depth, so the offline pipeline archive must be built with `main0_z` vertex entries |
| A package's chunk chain ends at a zero tail | `shader_split.py` `ue3_body` | all 922 cooked packages are padded to 32 KB and failed with `no compressed chunk at 0x…`; see [Files and shaders](#files-and-shaders) |
| Part B's location from control +0x748, and c1 from +0x700 | `dmsh_build.py` `vertex_ab` | one vertex shader stopped the whole compile, and 176 two-part vertex shaders got an all-zero c1 (the missing reticle); see [Files and shaders](#files-and-shaders) |
| `ListOpenUsers` reports only users with an open handle | `hle_account.c` | the game reads its player's user id once, 20 ms after it maps a gamepad; see [Saves](#saves) |
| A create on a save mount makes the directories on the way | `hle_fs.c` `save_dirs` | the game writes `save0:/save255:/Save0001.sav` and never creates `save255:` |

No frame-rate key: the game asks for present interval 2 on a 60 Hz tick and runs at 30 fps, as on the Switch.

## Files and shaders

- RomFS: 1,173 files, 11.5 GB, of which 923 are cooked `.xxx` packages (Oodle Mermaid chunks of 0x20000 bytes, each package padded to 32 KB) and the rest texture caches (`.tfc`), FMOD banks (`.afc`) and Bink movies (`.bk2`). The engine's global shader cache, `WillowGame/CookedSwitch/GlobalShaderCache-PC-NVN-SM5.bin`, is a `BMSG` of 1,060,867 bytes.
- **28,479 shaders (27,227 pixel, 1,252 vertex)** from **922 packages**, every one built for the Mac with no failure: [Unreal Engine 3 shader caches (Oodle packages)](../../../docs/reference/shaders.md#unreal-engine-3-shader-caches-oodle-packages). Before the zero-tail rule all 922 packages raised `[RED] … no compressed chunk at 0x1da4 (the chunks do not run to the end of the package)` and only the global cache's shaders were found.
- **Resolved: 251 of the 1,252 vertex shaders are two-part (VertexA + VertexB), and the translator read their constants from the wrong side of part B.** 176 of them carry non-zero data between part A and part B, which is where NVN's c1 offset (control +0x700) points in this package version; `vertex_ab` took the region after part B instead, which is zero in all 251 records. The translated shader reads `c1[0].x` as the min-axis guard it normalises a billboard axis against (`max(|axis|², c1[0].x)`), so a zero there made every clip position of the scope reticle's draw NaN and nothing rasterized: [VertexA and VertexB parts merged into one vertex shader](../../../docs/reference/shaders.md#vertexa-and-vertexb-parts-merged-into-one-vertex-shader). Part B's own segment is the trailing region in Catherine's form of the same record, which a census of every two-part shader here confirms: 169 records have `+0x700` == the end of part A, 56 add a 0x40–0xc0 block, and 26 are Catherine's header form.
- `c1_guard`: `c1_full.tsv: not proven bounded: 18 shaders` (not chased; it is a uniform-read bound annotation and no run reports a missing uniform). `cb_extent.tsv`: 47,229 uniform slots over 27,715 shaders, storage slots never written 0.
- The pipeline archive is built from this profile's bases at `depth_mode zero`: the packaged run logs `pipeline bases so far: 0 compiled, 68 from the archive, 4 reused` and no mismatch line.

## Saves

- **Resolved, and it was one cause for three symptoms.** The character list was empty, a save from the pause menu committed and left the game on its "Saving content. Please don't turn off your console." panel for ever, and save-and-quit never came back. All three are the player's user id being −1: `ListOpenUsers` answered one user at boot, before the application had opened any, so the game's own `AddUserToMap` (at 14.6 s) came after the one read that mattered (`ULocalPlayer::UserId`, 20 ms after the first gamepad was mapped), and −1 became both the save device it builds (`save-1:`, i.e. `save255:` written and `asset:/save-1:/` listed) and the user byte it compares a save against (`0xffffffff` against the save's `0xff`, so `NotifySaveComplete` takes its skip branch and never clears the panel): [A user listed before the application opened one](../../../docs/reference/services.md#a-user-listed-before-the-application-opened-one).
- **Resolved: a save into a directory the game never created.** With the user fixed the game writes `save0:/save255:/Save0001.sav` (and `.sav.tmp`) and calls no `CreateDirectory` for `save255:`; every save answered `fs path not found`, the write went through a handle that was never a descriptor (`fs pwrite failed errno=9`), the error applet came up (`Result 0x2a2`) and an empty save committed: `save commit #N … (0 save writes so far)`. A create on a writable transactional save mount now makes the directories on the way: [A create on a save mount makes the directories on the way](../../../docs/reference/services.md#a-create-on-a-save-mount-makes-the-directories-on-the-way).
- **Current:** `fs open [50.29s] save0:/ProfileData.bin mode=1 → fd 11`, the save is listed and read (`save0:/Save0002.sav` 2,917 B, `save0:/Save0001.sav` 4,501 B), a pause-menu save commits twice (`save commit #1 … (1 save writes so far)`, `#2 … (2 …)`), and save-and-quit reaches the MAIN MENU in 6–7 s — four times, three of them on the build the app carries and one from the app bundle's own payload. The `.sav.tmp` the game also writes is 8 bytes and the load path reads both.
- A save written by a build with the −1 user sits under `save255:/`; the host does not migrate it, and no rule here moves it. The player's own save folder was only ever read.

## Memory

- 4 GB block (`mem_reserve 0x100000000`), `mem_alias 0x10000000`, `app_pool 0xCD500000`, `sys_resource 0x0` (the NPDM declares no system resource, so `IsVirtualAddressMemoryEnabled` is false).
- **Resolved: the first boot crashed at `sdk+0xbf430` writing to `0x2000`.** The provisional profile declared 64 MB of used memory, so `nninitStartup` asked for a heap of `app pool − 64 MB` (`0xc9400000`); with `modules 0x5e00000` that is more than `app pool − modules`, the host refused it with `ResultLimitReached` and the game — which checks neither result — handed `StandardAllocator::Initialize` an unwritten out-parameter. Reporting the real footprint (`0x5e00000`, the number the host's own `[mem] SetHeapSize … + modules … > app pool` line names) makes the request `0xc7700000` and it fits.

## Rendering

- 371 draws in a busy frame, 23,627 translated instructions per draw (the portability run, frame 600).
- NVN submit+present p50 9.5 ms of a 33.4 ms frame; GPU p50 5.4 / p95 6.8 ms on the packaged payload. `[RED]` lines: 0.

## Frame rate

- **Current:** 30 fps, the game's own present interval (2 on a 60 Hz tick), on an Apple silicon Mac, headless: frame time p50 33.4 / p95 39.3 ms, GPU p50 5.4 ms on the app bundle's payload about a minute in; a save-and-quit run measured p50 33.4 / p95 71.2 ms over the window that held the save.
- No `fps_target` row: above 30 the engine's own limiter paces it, and nothing here was measured above 30.

## Mac routes

- Build with `scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> --platform mac`. The shader split needs the Mac host for Oodle (`DM_OODLE_SERVE`), which the NSP script starts itself.
- From an empty save, headless with `DM_CTL` presses: A at the title, A on START GAME, A on Play Game, A on SOLDIER, then A/A/X/A through the ECHO dialog (LEGAL and CONGRATULATIONS appear once the user is valid), `LDown` to walk. Pause menu: `Plus`, `Down`×3, A, `Up`, A — hold each press for 0.05 s; a 0.2–0.3 s hold is read as two presses (auto-repeat) and lands on View Lobby.
- A save can only be made once the intro is over, for a loaded character too: the confirm dialog grows its `All progress will be saved.` line only then, and that line — not the absence of `CANNOT SAVE` — is the thing to read.

## Open

- No IPA has been built; iOS is untested on a device.
- The console's own scoped view is not captured. The game ships `0.01` in that shader's c1 and the translated shader reads it, so the console draws this reticle, but a capture from hardware would close the loop.
- The other 175 two-part vertex shaders whose c1 changed (first float 1.0 in 5, 0.001 in 4, 36.0 in 3, 0.01 in 162) are the same class of data correction. Nothing looked wrong in the frames checked — hip, HUD, load, menu, save, quit — but only the reticle's draw was looked at closely.
- `[RED]` guard for a two-part record whose part B carries constants of its own: no such record exists here (0 of 251), so the guard has never fired on real data.
- An in-play checkpoint save (a New-U station) was not driven; only the pause → Exit save was, which is the same `SaveDataToDisk` / `NotifySaveComplete` pair.
- Aiming down sights misses about half the time in scripted runs: `ret1`–`ret3` never engaged, `ret4`–`ret6` did with the same key list. The press edge lands while the intro or tutorial still has input, so re-run rather than theorise.
- 18 shaders are "not proven bounded" in `c1_guard`.

## Found on this game

- [A user listed before the application opened one](../../../docs/reference/services.md#a-user-listed-before-the-application-opened-one): the empty character list, the stuck saving panel and save-and-quit, one cause.
- [A create on a save mount makes the directories on the way](../../../docs/reference/services.md#a-create-on-a-save-mount-makes-the-directories-on-the-way): `save0:/save255:/Save0001.sav` with no `CreateDirectory`.
- [VertexA and VertexB parts merged into one vertex shader](../../../docs/reference/shaders.md#vertexa-and-vertexb-parts-merged-into-one-vertex-shader): the second two-part form, where control +0x700 is the c1 offset and not part B's header.
- [Unreal Engine 3 shader caches (Oodle packages)](../../../docs/reference/shaders.md#unreal-engine-3-shader-caches-oodle-packages): a package zero-padded past its last chunk.
- An application-initiated exit now works: `svcExitProcess` (SVC 0x07) and `oe::ExitApplication` run the same wrap-up as the window's Quit instead of returning success to an instruction the guest never reaches (`hle_svc.c`, `hle_oe.c`, `hle_sdk16.c`). This title takes neither path — it does not import `ExitApplication` and never executes SVC 0x07 — so this is a host gap closed while chasing the saving panel, not what fixed it.
