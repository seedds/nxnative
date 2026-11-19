# Borderlands: Game of the Year Edition (`010064800F66A000`)

1.0.2 (v131072). SDK 9.3.0. Gearbox's Unreal Engine 3 (package version 595) on NVN, with FMOD for sound. Played through the first Fyrestone checkpoint on the Mac, then saved and returned to a lobby that read the character from disk. Gameplay targets 60 fps by default. iOS is untested.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `depth_mode zero` | [`game.tsv`](game.tsv) | the game selects near-zero NVN depth, so the offline pipeline archive must be built with `main0_z` vertex entries |
| `present_interval display`, `fps_target 60` | [`game.tsv`](game.tsv) | display pacing raises gameplay to 60 fps and keeps the player's 30 fps choice available |
| A package's chunk chain ends at a zero tail | `shader_split.py` `ue3_body` | all 922 cooked packages are padded to 32 KB and failed with `no compressed chunk at 0x…`; see [Files and shaders](#files-and-shaders) |
| Part B's location from control +0x748, and c1 from +0x700 | `dmsh_build.py` `vertex_ab` | the old placement check aborted on the first record with c1 before B; see [Files and shaders](#files-and-shaders) |
| `ListOpenUsers` reports only users with an open handle | `hle_account.c` | the game reads its player's user id once, 20 ms after it maps a gamepad; see [Saves](#saves) |

## Files and shaders

- RomFS: 1,173 files, 11.5 GB, of which 923 are cooked `.xxx` packages (Oodle Mermaid chunks of 0x20000 bytes, each package padded to 32 KB) and the rest texture caches (`.tfc`), FMOD banks (`.afc`) and Bink movies (`.bk2`). The engine's global shader cache, `WillowGame/CookedSwitch/GlobalShaderCache-PC-NVN-SM5.bin`, is a `BMSG` of 1,060,867 bytes.
- **28,479 shaders (27,227 pixel, 1,252 vertex)** from **922 compressed packages** and the global cache. A fresh Mac split matched the committed tables and built every shader on Apple M6, macOS 27.2.1, `metalfe-32023.922.4`. Before the zero-tail rule all 922 packages raised `[RED] … no compressed chunk at 0x1da4 (the chunks do not run to the end of the package)` and only the global cache's shaders were found. See [Unreal Engine 3 shader caches (Oodle packages)](../../../docs/reference/shaders.md#unreal-engine-3-shader-caches-oodle-packages).
- **Resolved:** 251 of the 1,252 vertex shaders have both VertexA and VertexB. In 75, control +0x700 points 0x30 bytes before B's SPH, as in Catherine, and the merged shader and c1 are byte-identical to main's output. The other 176 supply nonzero c1 data before B and have zero padding after it. The scope reticle's vertex shader reads `c1[0].x` as the minimum for `max(|axis|², c1[0].x)` when normalising its billboard axis. Reading zero from the tail made its clip positions NaN. See [VertexA and VertexB parts merged into one vertex shader](../../../docs/reference/shaders.md#vertexa-and-vertexb-parts-merged-into-one-vertex-shader).
- `c1_guard`: `c1_full.tsv: not proven bounded: 18 shaders` (not chased; it is a uniform-read bound annotation and no run reports a missing uniform). `cb_extent.tsv`: 47,229 uniform slots over 27,715 shaders, storage slots never written 0.
- The pipeline archive is built from this profile's bases at `depth_mode zero`: the packaged run logs `pipeline bases so far: 0 compiled, 68 from the archive, 4 reused` and no mismatch line.

## Saves

- In the bring-up, the old `ListOpenUsers` listed a user before the game opened a session. `AddGamepadToMap` calls its synchronous `TryOpenPreselectedUser` fallback only when the list is empty (`main+0x1452e28`). Without a user in the map, `SetUserIdFromControllerId` caches −1, which produced `asset:/save-1:/` in a character-list lookup and made `NotifySaveComplete` skip clearing the saving panel. Main now lists only live sessions. See [Local account selection, sessions and separate saves](../../../docs/reference/services.md#local-account-selection-sessions-and-separate-saves).
- On main's account and filesystem code with an empty save, `SwitchUserAndInputManager::AddUserToMap` ran at 2.023 s, before `ULocalPlayer::SetUserIdFromControllerId` wrote player index 0 at 2.044 s. `CreateFile("save0:/ProfileData.bin", …)` returned success and the first commit wrote the 186-byte profile at the root of the save mount.
- The 1.0.2 disassembly explains the bring-up's nested `save0:/save255:/Save0001.sav`. `UWillowSaveGameManager::Save` reads `ULocalPlayer::UserId` at `main+0x1825274` and passes it as a byte. With −1 this becomes 255, so `CreateFileWriterForUser` formats `save255:/`. `CreateFileWriter` then converts that path again with index 0 at `main+0x1441468`. The converter recognises `save0:/` through `save3:/` as existing prefixes and prepends `save0:/` to `save255:/`. This happens before the call to the host's `CreateFile`.
- **Verified with main's account and filesystem behavior.** At the first checkpoint and on save-and-quit, both `SaveDataToDisk` and `CreateFileWriterForUser` received index 0. `CreateFile` succeeded for `save0:/Save0001.sav` and `save0:/Save0001.sav.tmp`. Both files committed at the mount root (2,944 and 8 bytes), and the lobby listed the level 1 Soldier after reading them back. The run logged no `save255:/` requests. Directory creation on `CreateFile` is unnecessary for this path.

## Memory

- 4 GB block (`mem_reserve 0x100000000`), `mem_alias 0x10000000`, `app_pool 0xCD500000`, `sys_resource 0x0` (the NPDM declares no system resource, so `IsVirtualAddressMemoryEnabled` is false).
- The original host refused the `app pool − 64 MB` heap and the SDK used the unwritten out-parameter, crashing at `sdk+0xbf430`. The shared accounting fix makes that request fit with the default `mem_used`. See [Heap sized from the application pool less used memory](../../../docs/reference/memory.md#heap-sized-from-the-application-pool-less-used-memory).

## Rendering

- 371 draws in a busy frame, 23,627 translated instructions per draw (the portability run, frame 600).
- In the original Mac bring-up, NVN submit+present p50 9.5 ms of a 33.4 ms frame and GPU p50 5.4 / p95 6.8 ms on the packaged payload.

## Frame rate

- The stock game asks for NVN present interval 2 on a 60 Hz tick. `UGameEngine::GetMaxTickRate` allows 62 fps in Fyrestone, so the present interval sets the effective 30 fps ceiling. `appUpdateTimeAndHandleMaxTickRate` computes `GDeltaTime` from elapsed time during gameplay. The profile uses the [existing display-paced frame cap](../../../docs/reference/frame-rate.md#a-present-interval-that-is-the-games-only-limit-in-display-mode) to present once per tick. `DM_FPS_TARGET=30` selects the two-tick cap. The profile names no executable addresses.
- Mac check on Apple M6, macOS 27.2.1, with the same save and a 19-second sample of the in-engine Fyrestone intro. Stock averaged 27.7 fps including a 0.94-second stall, with 33.37 ms median frame time. The profile default averaged 59.6 fps at 16.66 ms median, with the world clock advancing 19.93 seconds in 19.96 seconds of real time. The 30 fps override averaged 30.0 fps at 33.37 ms median. All three runs loaded the same save and logged no `[RED]` lines.
- After the ECHO tutorial handed control to the player, a separate stationary first-person run averaged 59.9 fps (16.67 ms median and 17.16 ms p95). The world advanced 19.97 seconds in 19.98 real seconds, with no `[RED]` lines.

## Mac routes

- Build with `scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> --platform mac`. The shader split needs the Mac host for Oodle (`DM_OODLE_SERVE`), which the NSP script starts itself.
- From an empty save, headless with `DM_CTL` presses: A at the title, A on START GAME, A on Play Game, A on SOLDIER, then A/A/X/A through the ECHO dialog (LEGAL and CONGRATULATIONS appear once the user is valid), `LDown` to walk. Pause menu: `Plus`, `Down`×3, A, `Up`, A — hold each press for 0.05 s; a 0.2–0.3 s hold is read as two presses (auto-repeat) and lands on View Lobby.
- Follow Claptrap to the first checkpoint on a fresh character. The pause-menu Exit confirmation shows `All progress will be saved.` once saving is enabled.

## Open

- No IPA has been built; iOS is untested on a device.
- The console's own scoped view is not captured. The game ships `0.01` in that shader's c1 and the translated shader reads it, so the console draws this reticle, but a capture from hardware would close the loop.
- The other 175 two-part vertex shaders whose c1 changed (first float 1.0 in 5, 0.001 in 4, 36.0 in 3, 0.01 in 162) are the same class of data correction. Nothing looked wrong in the frames checked — hip, HUD, load, menu, save, quit — but only the reticle's draw was looked at closely.
- None of the 251 two-part records has independent constants for part B. A synthetic record covers the rejection of that layout.
- Aiming down sights misses about half the time in scripted runs: `ret1`–`ret3` never engaged, `ret4`–`ret6` did with the same key list. The press edge lands while the intro or tutorial still has input, so re-run rather than theorise.
- 18 shaders are "not proven bounded" in `c1_guard`.

## Found on this game

- [Local account selection, sessions and separate saves](../../../docs/reference/services.md#local-account-selection-sessions-and-separate-saves) explains the open session semantics behind the empty character list and stuck saving panel.
- [VertexA and VertexB parts merged into one vertex shader](../../../docs/reference/shaders.md#vertexa-and-vertexb-parts-merged-into-one-vertex-shader): the second two-part form, where control +0x700 is the c1 offset and not part B's header.
- [Unreal Engine 3 shader caches (Oodle packages)](../../../docs/reference/shaders.md#unreal-engine-3-shader-caches-oodle-packages): a package zero-padded past its last chunk.
