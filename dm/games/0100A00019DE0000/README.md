# Hades II (`0100A00019DE0000`)

Base game v0 (the NACP's display version is 0.0.126424), SDK 19.3.9. Supergiant Games' engine on **NVN**, unlike Hades ([0100535012974000](../0100535012974000/README.md)), which renders with Vulkan. The ExeFS holds only `main` and `sdk`; this `sdk` is byte-identical to FINAL FANTASY TACTICS - The Ivalice Chronicles' ([010038B015560000](../010038B015560000/README.md)). The NSP script's provisional profile booted it on the Mac with no host change: the title, a new save, the opening and Erebus at 60 fps. Plays on the iPhone.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `mem_used 0x6c00000` | [`game.tsv`](game.tsv) | `main` has an 82 MB bss: the rewritten modules take more than the default 64 MB of used memory, so `SetHeapSize(pool − 64 MB)` would not fit; see [Memory](#memory) |
| 4 GB layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | `game.tsv` | no system resource; the 3.25 GB heap region holds the 3.10 GB heap the game asks for |
| `pad_layout full` | `game.tsv` | the 3D gamepad layout: the right stick aims (the weapons with `ManualAiming` in `Game/Weapons/PlayerWeapons.sjson`) |
| `shader_groups /Shaders/=shaders` | `game.tsv` | the shader split's group for `Shaders/NVN/bin` |

## Memory

- No system resource (META +0x14 = 0): the game uses `SetHeapSize` and asks for 3.10 GB (pool − used memory), which the 4 GB layout's 3.25 GB heap region holds.
- `main` has an 82 MB bss, so the profile's `mem_used` is 108 MB (`0x6c00000`).
- Resident memory levels off at about 3.4 GB in Erebus on the Mac.

## Rendering

- **Current: textures at fixed, sparse slots.** Its 88 shaders bind combined textures at slots 0, 1, 3, 6, 8, 11, 15, 21, 25, 26, 30, 31 and 34. Slot 34 (`SceneFade.frag`, c2 0x130; `fp_t_tcb_4C`: c2 word 0x4C = 0x20 + 8 × 34) lies past the driver's 32 combined slots, inside the image table at 0x120. With a 32-slot table, `dmsh_build.py` reported that binding as not decoded (`shaders with a binding whose slot was not decoded 1`, which prints a red line when the shader loads). The combined table now runs up to the separate textures at 0x168 (41 slots); past slot 31 it shares its words with the image table, and the replay writes a bound texture there when the image slot is unbound (`nvn_mtl.m`). The game has no image bindings, so a bound texture there wins. The census of `fp_t_*` names in the translated libraries gives the slots in use.
- **Not a bug: two long lines through Melinoë.** While the staff's Omega attack charges, two thin white lines run from her to the screen edges, one forward and one back. They are that weapon's aim lines (`WeaponStaffSwing5`: `AimLineCountOverride = 2`, `AimLineAngleOffsetOverride = 180`, animation `AuraAimLineNoMovementStaff`). The sprite is a long arrow with white edges, a dark fill drawn emissive and its tip off-screen, and the game hides it while she moves. Traced with `DM_NVN_CBDUMP` vertex dumps and `DM_PROBE` on the aim-line update (`main+0x39ade0`); a scripted run that taps Y shows them often.

## Services

- **Current: offline.** Its `EOS_*` imports (Epic Online Services, `nro/EOSSDK-Switch-Shipping.nro`, loaded through `nn::ro`) stay offline, as in Hades: `ro::RegisterModuleInfo` fails (red line), so the game never loads the NRO.
- **Resolved: SDK 19's `nn::ro::LoadModule`** takes one more bool (`_ZN2nn2ro10LoadModuleEPNS0_6ModuleEPKvPvmib`), so the hook for the older form did not catch it and the SDK's own code would have run. The host hooks that form and `ro::Finalize` too, so neither reaches the SDK's `ldr:ro` client.

## Files and shaders

- 88 BNSH files in `Shaders/NVN/bin` (`.vert` / `.frag`; the `.shader` files list each program's pair).
- `romfs_marker` is `Audio/Switch/Music.bank`.

## Mac routes

- `DM_KEYS="f700:A,f900:A,f1100:A,f1300:A,f1500:A,f1800:A,f2100:A,f2400:A,f2700:A"` goes through Play, the first save slot and the opening into Erebus (control around frame 3000).

## Found on this game

General host changes first needed here; each applies to every game:

- **Combined texture slots past 31** (the combined table up to 0x168, 41 slots, sharing words with the image table; a bound texture written there when the image slot is unbound): [04, Find the binding layout by census](../../../docs/agents/04-graphics.md#3-find-the-binding-layout-by-census).
- **The SDK 19 `nn::ro::LoadModule` form** and `ro::Finalize` hooked (`hle_ro.c`): [`nn::ro` and its port](../../../docs/reference/services.md#nnro-and-its-port).
- **Fullscreen startup from another fullscreen app (resolved):** the NX2Apple reporter reproduced the rollback on four of four launches and confirmed the deferred request succeeds: [A game opened from a fullscreen app stays a window](../../../docs/reference/frame-rate.md#a-game-opened-from-a-fullscreen-app-stays-a-window).

## Open

- `game.tsv` says values marked "Mario's" are not measured for this game; none of its rows is marked so.
