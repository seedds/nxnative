# Persona 5 Strikers (`0100801011C3E000`)

Base game 1.0.0 (`v0`), SDK 8.3.2. Koei Tecmo's KTGL on NVN. Modules `main`, `subsdk0` (NintendoSDK_movie) and `sdk`. The two DLC packages provide licenses for indices 1 and 2.

## Memory and rendering

- The NPDM declares no system resource. The SDK sets a 3.14 GB heap in the same 4 GiB partition used by other games without virtual address memory.
- NVN sets the upper-left window origin and near = 0 depth mode. The profile's `depth_mode zero` makes the pipeline prebuild use the same vertex entry points as the running game.
- The shader split found 23,092 unique keys: 20,241 pixel, 2,653 vertex, 196 compute and two geometry. The KTGL RDB archives supply 22,976 keys and `main` supplies 116. The existing [RDB reader](../../../docs/reference/shaders.md#g2s-containers-in-ktgl-rdb-archives) reads the archives.

## Mac validation

On an Apple M6 with Xcode 27.2 (27B52), all 23,092 Mac shaders compiled, with no undecoded bindings or reported translation losses. The windowed run rendered the startup logos, title and menu, granted the DLC items on New Game, and reached the opening Shibuya battle. A 300-frame standing-still window in the battle measured frame time p50 33.34 ms and p95 34.81 ms, with 3.3 GB resident and 2.3 GB allocated by Metal. This diagnostic launch disabled pipeline prebuild and paused for 15.5 seconds at the battle transition while compiling 48 first-use pipelines.

## Running

Point [`scripts/build_ipa_from_nsp.py`](../../../scripts/build_ipa_from_nsp.py) at the NSP folder with the local keys file and `--title-id 0100801011C3E000` if it holds more than one title. The default output is `Persona5Strikers.ipa`. Use `--platform mac` for the Mac host and its run command. Add `DM_METAL_HUD=1` to that command to show the performance HUD.

From the title screen, choose New Game and dismiss the DLC notices. At the opening cutscene's skip prompt, choose Yes to reach the battle. On a Mac keyboard, J confirms, WASD moves, and Shift with the arrow keys moves the camera.

## Found on this game

- [Play statistics without a user ID](../../../docs/reference/services.md#play-statistics-without-a-user-id): startup dereferenced a null applet proxy. The host now returns zero play records from the SDK overload the game calls.

## Open

- Verify combat beyond the opening battle's standing-still check, later areas and save/load on the Mac.
- The iOS build and device validation remain open.
