# Stardew Valley (`0100E65002BB8000`)

Update 1.6.15.13 (v1310720; the base game v0 is not extracted), SDK 20.5.27. MonoGame for the Switch, as Celeste ([01002B30028F6000](../01002B30028F6000/README.md)): MonoGame's and the game's C# compiled to C++ by Sickhead's BRUTE into `main` (124 MB of code, 230 MB with its data and bss) with a Boehm collector; XACT sound (`Content/XACT`, 0.48 GB of wave banks) through the audio renderer; NVN. Modules `main` and `sdk` only. It links Pia and NPLN for online co-op and LDN for local co-op. Plays on the iPhone (IPA 0.6 GB); no host change in this profile's change.

## Status on the Mac

- The title, a new farm (character creation with the software keyboard, Skip Intro), the farmhouse, the farm and its tools, the bus stop, Pelican Town by day and night (lamp light), Pierre's shop and a purchase, the game menu, the mail, a night's sleep and its save, Load from the title, and the Co-op menu (Online Communication goes back to the title with no network). No red line on that route.
- 60 fps: frame time p50 16.67 ms, p95 16.8 ms; GPU about 0.6 ms a frame on an M6; CPU 8% of a core, 0.27 W. Sound.
- 0.46 GB resident at the title and 0.93 GB in play, flat over the last 10 of 23 minutes; 0.3 GB of Metal allocations.
- The title is up about 20 s after launch.

## What it needs

The provisional profile's guesses held.

| Setting | Where | Why |
|---|---|---|
| `sys_resource 0x0` | [`game.tsv`](game.tsv) | the NPDM declares no system resource, so `IsVirtualAddressMemoryEnabled` is false, as on Horizon: no virtual address memory. The game sets a 2.96 GB heap (pool − used memory) |
| 4 GB layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | `game.tsv` | 256 MB in front + 256 MB alias region + 256 MB stack region + 3.25 GB heap region |
| `mem_used 0xda5c000` (218 MB) | `game.tsv` | the rewritten modules are larger than the default 64 MB of used memory (214 MB without `main`'s 29.6 MB of bss, + 4 MB: `profile_guess.py` `mem_used`), so the heap request plus the modules would exceed the pool |
| `pad_layout full` | `game.tsv` | the 3D layout: the D-pad for menus and the right stick for the cursor |
| `shader_groups /Content/=content` | `game.tsv` | the MonoGame effects; see [Files and shaders](#files-and-shaders) |

## Files and shaders

- 3,549 XNB files, 3,535 of them LZ4 and 8 LZX (MonoGame's compressed XNB; LZX is flag 0x80, LZ4 flag 0x40).
- **Current:** 65 shaders (23 pixel, 42 vertex): 64 in the MonoGame effects in `main` (SpriteBatch's and the game's) and one in `Content/Effects/ShadowRemove.xnb`, an LZ4 XNB that `shader_split.py` decompresses before reading it (`xnb_decompress`).
- Nearly everything draws with SpriteBatch: the prewarm list is its two shaders and four pipelines, and with it nothing was built on demand over the route above.

## Input

- The pad only (`main` imports no touch screen or mouse). The D-pad moves between menu items, the right stick moves the cursor, ZL / ZR change the toolbar slot (empty slots are skipped), X opens the menu, Y uses the tool, A checks.
- The game drops presses while a menu fades in or out (about a second after it opens or closes).
- **Resolved (PR #120): the Mac text box.** Character creation names the farmer and the farm through the software keyboard, and it re-opened the keyboard after every OK. The Mac text box (`front_mac.m`) now releases the buttons that opened it, and stays up while the text is shorter than the minimum length.

## Mac routes

- The work folder's `run_v/boot.sh <log> [secs] VAR=…` runs `build/valleyhost` (`SV=<save dir>` for a copy of `run_v/save_ref`, the morning of day 2 in the farmhouse); `run_v/ctl.sh` drives a run started with `DM_CTL`; `run_v/step.sh <shot> [wait] <presses…>` presses and takes a screenshot.
- The title is up about 20 s into a boot: A (Load) and, a second later, A (the save).
- Menu presses held 0.3 s or more repeat, so hold 0.2 s. Walking takes the left stick (`LLeft~1.3` is about five tiles).
- In character creation with `DM_KBD=<text>`, A on each of the three text boxes fills it, and Skip Intro is the last item above OK.
- Out of the farmhouse (its door is below the gift box), the path east above the weeds, past the mailbox, leads to the bus stop, and east from there to Pelican Town.

## Found on this game

General host changes first needed here (PR #120, before this profile's change); each applies to every game:

- **Compressed XNB** decompressed before the MGFX reader (`xnb_decompress` in `shader_split.py`): [MGFX effects in MonoGame XNB files](../../../docs/reference/shaders.md#mgfx-effects-in-monogame-xnb-files).
- **The Mac software keyboard's text box** releases the buttons that opened it and stays up below the minimum length (`front_mac.m`): described in [Input](#input) above.
- **Spin waits through stores** (`patch_nso.py` "Spin waits"): one more loop in this game's `main`: [development.md, Power](../../../docs/development.md#power).

## Open

- About one frame in 800 takes 24–47 ms (nine in two idle minutes in the farmhouse): game CPU time, with no pipeline build, upload or present in it, while the main thread otherwise sleeps 95% of each frame in `SleepThread` (sampled), whose host sleep is exact. Probably BRUTE's Boehm collector; not checked further.
