# Bayonetta Origins: Cereza and the Lost Demon (`0100CF5010FEC000`)

The base game 1.0.0 (v0), SDK 13.3.2 with the `sdk` build of [Bayonetta 3](../01004A4010FEA000/README.md) (`357efc19…`). PlatinumGames' own engine on NVN; Wwise sound through one `AudioOut`. Modules `main` and `sdk`: there is no movie module, and the movies are Bink 2 files (`.bk2`) played by code in `main`. Languages: English, Japanese, French, German, Spanish, Italian, Russian, Korean, Traditional and Simplified Chinese. It runs on the Mac with no game-specific rewriter cut and no host change of its own: the NSP script's provisional profile with `depth_mode zero` added and the App Store category set. Reached: the title, the menu, New Game, the whole storybook prologue and the first 3D scene with control of Cereza, at the game's own 30 fps; and in a 36-minute windowed session with a controller and sound, play at least as far as the Save tutorial card at a lantern. First-use pipeline compiles stutter and one tutorial-card texture is wrong (below). Not run on an iPhone or iPad here.

## What it needs

| Setting | Where | Why |
|---|---|---|
| 4 GB layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000`, `sys_resource 0x0` | [`game.tsv`](game.tsv) | the NPDM declares no system resource: no virtual address memory, as Bayonetta 3. The NSP script's guess, which held in play (heap 3.11 GB in a 3.25 GB heap region) |
| `mem_used 0x6400000` (100 MB) | `game.tsv` | the rewritten modules are larger than the default 64 MB of used memory (set by the NSP script after the rewrite) |
| `depth_mode zero` | `game.tsv` | the game calls `nvnDeviceSetDepthMode` with near = 0 ("depth mode near=0" in the host log at boot), so the pipeline bases made before it runs take the `main0_z` vertex entries and the host does not make them again; see [Pipelines](#pipelines) |

## Frame rate

- **30 fps, the game's own cap**, from the title through the prologue and the first 3D scene: frame time p50 33.3 ms in every 300-frame window from the menu on, p95 33.9–35.0 ms in all of them but the New Game load (66.7 ms) and one window in the 3D scene (56.6 ms, frames of up to 192 ms while the stick was held; cause not looked for); GPU time p50 2.4–5.8 ms a frame (headless, MacBook Pro M2 Pro, 32 GB, macOS 27.2, main `8fb5ffeb`, shader chain `5ee7c6407a57`, 2026-11-11).
- **In play with a window** (36 minutes from a new game, a DualSense, sound on, docked, main `f75ac06e`, chain `1fa63cf7c7ae`, the provisional profile plus `depth_mode zero`): frame time p50 33.3–33.6 ms in 188 of 193 windows. Three consecutive windows (frames 17,400–18,300) and the last one before quitting ran at 72–75 ms a frame with GPU time unchanged (about 9 ms); whether the window was in front then is not known. GPU time p50 up to 11 ms.
- **Stutter at first use:** 157 frames of 100 ms or more in that session, 144 of them frames in which pipelines were made at the draw; the longest 3.1 s for 68 pipelines. The draws compiled 2,468 bases in all, though the host's prebuild had made the 4,772 pair bases at the start (32 s): see [Pipelines](#pipelines).
- No frame-rate key is set. Outside this repository, FPSLocker-Warehouse has a 60 fps patch for this build (`A1CE2940D813ACAB`); it is not part of this profile.

## Memory

- Resident 1,821–1,828 MB and 1,454–1,456 MB of GPU memory as the host prints them, level from frame 4,200 (in the prologue) to frame 10,200 (in the first 3D scene) of a 6-minute run.
- In the 36-minute windowed session resident memory rose from 1,810 MB to 2,409 MB and GPU memory from 1,409 MB to 2,340 MB.

## Files and shaders

- The RomFS has 231 files, 3.17 GB: 52 PlatinumGames PKZ archives (2.62 GB, the format Bayonetta 3's profile reads; 32 of them hold Oodle Kraken entries, which the shader split decodes with the game's own decoder in the Mac host) and 179 Bink 2 movies under `movie/` (0.55 GB). No DLC.
- 7,661 shaders, all from the PKZ archives: 5,198 pixel, 2,386 vertex, 77 compute. All built for the Mac in 7 minutes, none failed. They are single-stage programs, as Bayonetta 3's: the 2,386 vertex shaders are each paired with two cover pixel shaders, 4,772 pairs.
- The movies need no shader from `main`, unlike [Advance Wars 1+2: Re-Boot Camp](../0100300012F2A000/README.md)'s Bink 2 plugin: while they play the host logs single-channel CPU-written textures of 1600x900 and 800x450, the shape of decoded Y / U / V planes, so the decoding looks to be on the CPU.
- `pso_manifest.tsv` is empty: no run recorded one.

## Rendering

- The docked picture is 1600x900 (the swap chain's size). In a 2560x1440 window with Upscaling off it is scaled bilinearly and looks soft.
- **Open: a magenta rectangle on a tutorial card.** The Save card (shown at a lantern) has a solid magenta rectangle over its lower left and a grey one beside it, on the Mac on main `f75ac06e`. PR #2268 (packaged texture width alignment) reports and fixes the same picture on an iPhone; it is not on main and was not tried here.
- One red line in 36 minutes, once: `stage 1 cb11 has no uniform buffer bound`.

## Pipelines

- Without `depth_mode zero` the Mac app's bundled pipeline archive is made for near = −w: the host says so ("the game's depth mode is not the one built for") and built all 4,772 bases itself on the first start (156 s on the M2 Pro, main `37f076a5`, 2026-11-08). With the key, the host found the stamp of an app built on main `3b6b9803` matching ("holds the bases of every shader pair", 16 of 16 probed bases found, a 131 MB archive) and ran no prebuild. In that run the draws' own archive lookups still found none (0 found, 209 not) and the draws compiled 110 bases; why was not looked into.
- On main `8fb5ffeb` and `f75ac06e` the NSP script's pipeline archive step fails for this game and the app is packed without an archive (issue #2337): `metal-tt` (AIR-TT 32023.922) stops with `cannot find private metadata at offset '8'` on the first vertex pulling library whose `main0_z` entry declares the border corrections' function constant (`dm_border_off`), which `pso_offline.py` specializes since 511a0a2c; the same library's `main0` entry specializes without error. The host's own prebuild then makes the 4,772 bases in the background on the first start (80 s headless, 32 s in the windowed session; none failed, the game running meanwhile) and keeps them in the device's archive.
- The pair bases do not cover play: in the windowed session the draws made 2,468 more bases themselves, which is the stutter under [Frame rate](#frame-rate); the host's rows name draws that waited for a pulling base not made yet. `pso_manifest.tsv` is still empty; the session recorded a `pso_user.tsv`, not turned into a seed list here.

## Sound

- The game opens one `AudioOut` (2 channels) and starts it. In the windowed session music, voices and effects were clean by ear for 34 minutes of output: 2 underruns in 408,000 device callbacks. The headless runs had `DM_AUDIO=0`.

## Mac routes

- Build with `scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> --platform mac-app`.
- The title (an animated book) is up by 22 s and waits for **ZL+ZR**; A did nothing there in a run of 2026-11-08. The menu has New Game, Continue and Options.
- New Game: a loading screen, then the storybook prologue, whose pages turn on A. With A every 4 s the last page ("Her name was Cereza.") comes about 220 s after the start, and the first 3D scene with its Move prompt at about 240 s; the left stick moves Cereza (tried from 304 s). The game wrote its save (`GameData.dat`, `SlotData_5.dat`) on the way.

## Open

- Nothing past the first half hour of play is checked, and what was fought or met in it was not written down.
- The magenta rectangle on the Save tutorial card (PR #2268), under [Rendering](#rendering).
- Stutter at first use: the draws' pipelines are not among the prebuilt bases, and there is no recorded `pso_manifest.tsv`.
- The keyboard was not tried (a DualSense was).
- iPhone and iPad runtime validation; no IPA has been built here.
- The pipeline archive step's failure on current main (issue #2337).

## Found on this game

Nothing new in the host: the general mechanisms it uses are listed above.
