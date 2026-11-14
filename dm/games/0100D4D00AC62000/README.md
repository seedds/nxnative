# SEGA AGES Out Run (`0100D4D00AC62000`)

Update 1.1.0 (v65536), SDK 5.5.0, on NVN (14 shaders, all built into `main`). Modules `main` and `sdk`. SEGA's 1986 arcade game in M2's SEGA AGES series: the build path in `main` names M2's engine (`sega_ages_outrun\m2engage\…`), and the game ships Quazal's NEX library for its online rankings ("BEST OUTRUNNERS"). The standard host and rewriter run this build with no game-specific fix: no shader overrides, rewriter cuts or host changes. The profile is the NSP script's provisional one with no key changed, plus a prewarm list recorded on the Mac ([Shaders](#shaders)). Only this update is registered in the profile.

On the Mac (2026-11-13, an Apple silicon MacBook on macOS 27.2.1, a Mac app from the NSP script): boot, the Start Menu, the credit dialog, the arcade game's music select and races on Stage 1, over six launches; the longest ran 29,100 frames (about 8 minutes) of menus and races. The keyboard, the save (three commits in that run), audio and the window and fullscreen presents all worked, with no `[RED]` line in any run. The unsigned IPA has not been built from this profile or tested on an iPhone.

## What it needs

| Setting | Where | Why |
|---|---|---|
| 3.25 GB layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | [`game.tsv`](game.tsv) | the provisional layout, which holds: `main` sets a heap of at most the pool − 64 MB |
| `sys_resource 0x0` | `game.tsv` | the NPDM declares 0 MB of system resource: `IsVirtualAddressMemoryEnabled` is false, as on Horizon |
| `shader_groups /204/=204` | `game.tsv` | the provisional guess (the largest RomFS folder); it matches no file, since every shader is in `main`, and changes no shader key |
| [`pso_manifest.tsv`](pso_manifest.tsv) | profile | the prewarm list: a first start builds the game's pipelines before they are drawn ([Shaders](#shaders)) |

## Files

- RomFS: 13 files, 0.09 GB. `alldata.bin` (the RomFS marker) and a `.m` file at the root, the music as ten `.m4a` files in `204/stream/`, one file in `204/state/`.
- The save holds `data_008_0000.bin` (0.4 MB) and `data_013_0001.bin` / `data_013_0002.bin` (0.6 MB each), written through the transactional save mount.

## Shaders

- **Current:** 14 shaders (3 vertex, 11 pixel) from three BNSH containers in `main` (`main+0xcbb8a0`, `+0xcc2c08`, `+0xcc9f80`), none in RomFS; all translated for the Mac. The game draws 1 to about 66 draws a frame (65 on the Start Menu), with a GPU time of about 1.3–1.8 ms a frame.
- **Prewarm list (`pso_manifest.tsv`, current):** recorded from a cold start, with the list empty and no `pso_user.tsv`, over the boot, the Start Menu, the credit dialog and races: all 14 shaders and the game's 9 render pipelines. More than 8 minutes of later play added none. Without it, the first start built 6 pipelines on the spot in one frame on entering the Start Menu (a 48 ms frame, `[longframe] … new pipelines 6`); a later start built none. With it, a cold start (no pipeline archive, no `pso_user.tsv`) of a Mac app from this profile logged `pipeline prewarm done: shaders 14 / render pipelines 9` (2.2 ms in the background), and its long frames to the Start Menu all had `new pipelines 0`.

## Memory

- Resident memory was 430–800 MB and Metal allocations 290–480 MB, flat over the 8-minute run.

## Performance

- Frame time p50 16.67 ms at 60 fps, CPU about 15 % of one core. Frames were dropped only around window events: entering fullscreen, the window being covered or another app taking focus (the display link fires about once a second while the window is covered, `display link silent … rescue`), never in steady play. A fullscreen switch leaves about 45 frames of macOS's own animation undisplayed; the rescue it starts now ends as the window shows ([Found on this game](#found-on-this-game)). A few 30–35 ms frames in the intro come from the game itself (a GPU time of 0.2–1.7 ms on them).

## On the Mac

- After the NSP script's `--platform mac-app` build, open the app; Play (or Return) in the settings window starts the game.
- The credit dialog after the intro closes with A. Out Run is an arcade game on its own coin: X inserts a credit and X again starts the game; the music is chosen by steering.
- The keyboard plays as player 1: J is A, U is X, the arrows the D-pad and WASD the left stick. The game draws its 4:3 arcade picture inside its own black bars in the 16:9 frame; fullscreen (⌃⌘F) keeps them.

## Menus

- **The Start Menu's and pause menu's slot panel, half black (resolved, not a fault).** The panel beside the menu is the save-state slot carousel: a 4:3 screenshot on the left and the save's date and time on the right. An empty slot shows the SEGA AGES placeholder at 4:3 and an empty, black details area; after a Save in the pause menu the slot showed the race's screenshot and `11/13/2026 23:40`, and Load was enabled.

## Found on this game

- **A display-link rescue of a covered window outlasted the cover** (fullscreen switches): [frame rate](../../../docs/reference/frame-rate.md#present-thread-rescues-a-silent-display-link).

## Open

- iPhone: an IPA from this profile is not built or tested.
- The online rankings (NEX) were not tried; the log shows no network use in these runs.
- Only Stage 1 was raced; the later stages, the Start Menu's Ranking, Play Replay and Manual, and the game's own display options (its wallpaper and screen size) are untested.
