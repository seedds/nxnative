# Pokémon FireRed Version (Spanish Ver.) (`0100EB702342C000`)

Base game 1.0.0 (`v0`), SDK 20.5.19. It is a Game Boy Advance game in Nintendo's own player: `main` (1.4 MB of code) is the player, the RomFS holds a single file, the game's ROM `FireRed_s.gba` (16 MB), and the save is `FireRed_s.sav` (128 KB). The player draws with NVN; its two shaders are GLSL in `main`, compiled at run time through GLSLC (`subsdk0`), as in [Fire Emblem: Three Houses](../010055D009F78000/README.md). It also imports `nn::ldn` (local wireless, for trading), which nothing here exercises.

## What it needs

- `mem_used 0x7000000`. The NPDM declares 2 MB of system resource and `main` sizes its heap as the pool minus the used memory. With the default 64 MB it asked for 0xc9400000, which with the 0x6600000 of modules is more than the 0xcd500000 pool (`[mem] SetHeapSize 0xc9400000 + modules 0x6600000 > app pool 0xcd500000`), and it aborted at `main+0x15fa70`. The provisional profile did not set it: the heap check sizes `mem_alias` from the request, and the rule that sets `mem_used` after the rewrite covers games without a system resource only. With it the heap is 3.10 GB in a 3.25 GB heap region (`mem_alias 0x50000000`, from the heap check).
- The host's no-op for `audio::SetDeviceOutputVolumeAutoTuneModeEnabled`, which SDK 20 sends to `audren:u` ([Audio device output volume and names](../../../docs/reference/audio.md#audio-device-output-volume-and-names)). Before it: `game called nn::diag abort: Result 0xe15, call site sdk+0x225bac`, under `main+0x1a88`.
- The rest is the provisional profile as `profile_guess.py` wrote it: modules `main subsdk0 sdk`, Engage's 5 GB block with the hardware pool, `romfs_marker FireRed_s.gba`.

No `patches.py`.

## Files and shaders

- Shaders: 2 (a vertex and a pixel shader, one pair), captured by the NSP script's GLSLC boot in about 30 s.
- The log has eight `[RED] nvnDeviceGetInteger unknown item 0x60` … `0x68` lines at start (SDK 20 queries the host does not know, answered 0); nothing else red in a session of several minutes.

## Performance

- On the Mac (headless host, the Mac app's shader libraries): 60 fps, frame time p50 16.66 ms, p95 17.0–17.4 ms over 900 frames, about 0.1 W of CPU.

## Mac routes

Driven with `DM_CTL` (`A`, `Plus`, the D-pad, `shot`), with the Mac app's `dmhost`, `Resources/dm`, `msl`, `pso` and `romfs.img`:

- The intro (Game Freak logo, Gengar and Nidorino) plays from start; `Plus` twice reaches the title screen and the menu.
- New game: the button tutorial, Professor Oak's speech, boy or girl, the name keyboard (D-pad and `Plus` to OK), the rival's name from the list, then the bedroom in Pallet Town. Walking and the start menu work.
- Saving from the start menu writes `FireRed_s.sav` and commits it; after a restart the menu offers CONTINUAR with the saved player, and it loads into the bedroom.

Not checked: sound heard through the speakers (the renderer opens and runs with no error in the windowed app), a controller, the iPhone or iPad build, and the other languages' releases (their `main` is likely the same build, so the NSP script would pack them from this profile by build, but `romfs_marker` names the Spanish ROM).
