# Hades II (`0100A00019DE0000`)

Base game v0 (the NACP's display version is 0.0.126424), SDK 19.3.9, and the update 1.0.137848 (v1, SDK 20.5.15; its `main` and `sdk` builds are registered in `game.tsv`, see [Versions](#versions)). Supergiant Games' engine on **NVN**, unlike Hades ([0100535012974000](../0100535012974000/README.md)), which renders with Vulkan. The ExeFS holds only `main` and `sdk`; this `sdk` is byte-identical to FINAL FANTASY TACTICS - The Ivalice Chronicles' ([010038B015560000](../010038B015560000/README.md)). The NSP script's provisional profile booted it on the Mac with no host change: the title, a new save, the opening and Erebus at 60 fps. Plays on the iPhone.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `mem_used 0x6c00000` | [`game.tsv`](game.tsv) | `main` has an 82 MB bss: the rewritten modules take more than the default 64 MB of used memory, so `SetHeapSize(pool − 64 MB)` would not fit; see [Memory](#memory) |
| 4 GB layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | `game.tsv` | no system resource; the 3.25 GB heap region holds the 3.10 GB heap the game asks for |
| `pad_layout full` | `game.tsv` | the 3D gamepad layout: the right stick aims (the weapons with `ManualAiming` in `Game/Weapons/PlayerWeapons.sjson`) |
| `shader_groups /Shaders/=shaders` | `game.tsv` | the shader split's group for `Shaders/NVN/bin` |

## Versions

- **Current: the update's builds are registered, its tables are not remade.** `game.tsv` has a `build v1 1.0.137848` row (main `8ce1ac48e9fa74479051f3520e632df4`, sdk `190030518b5d64e5d87b84bc54661af9`, read from the update's own ExeFS), so the NSP script builds the update from the profile as it is, with no red line for unregistered modules. `package` and `shader_versions` stay v0: the shader tables were made from the base game, and nobody has compared the update's 88 BNSH files with them (no key asserts the union).
- Verified in the fork LNPANDY/NX2Apple (commit 4cb1c00c, base + update 1.0.137848 from NSPs): 5,258 files packed, and the game ran on the Mac for 2,100 frames. The update needs no special extraction: `nx_extract` layers the update NCA over the base. Not re-run on this repository's current `main`; an iPhone run of the update is not recorded.

## Memory

- No system resource (META +0x14 = 0): the game uses `SetHeapSize` and asks for 3.10 GB (pool − used memory), which the 4 GB layout's 3.25 GB heap region holds.
- `main` has an 82 MB bss, so the profile's `mem_used` is 108 MB (`0x6c00000`).
- Resident memory levels off at about 3.4 GB in Erebus on the Mac.

## Rendering

- **Current: textures at fixed, sparse slots.** Its 88 shaders bind combined textures at slots 0, 1, 3, 6, 8, 11, 15, 21, 25, 26, 30, 31 and 34. Slot 34 (`SceneFade.frag`, c2 0x130; `fp_t_tcb_4C`: c2 word 0x4C = 0x20 + 8 × 34) lies past the driver's 32 combined slots, inside the image table at 0x120. With a 32-slot table, `dmsh_build.py` reported that binding as not decoded (`shaders with a binding whose slot was not decoded 1`, which prints a red line when the shader loads). The combined table now runs up to the separate textures at 0x168 (41 slots); past slot 31 it shares its words with the image table, and the replay writes a bound texture there when the image slot is unbound (`nvn_mtl.m`). The game has no image bindings, so a bound texture there wins. The census of `fp_t_*` names in the translated libraries gives the slots in use.
- **Not a bug: two long lines through Melinoë.** While the staff's Omega attack charges, two thin white lines run from her to the screen edges, one forward and one back. They are that weapon's aim lines (`WeaponStaffSwing5`: `AimLineCountOverride = 2`, `AimLineAngleOffsetOverride = 180`, animation `AuraAimLineNoMovementStaff`). The sprite is a long arrow with white edges, a dark fill drawn emissive and its tip off-screen, and the game hides it while she moves. Traced with `DM_NVN_CBDUMP` vertex dumps and `DM_PROBE` on the aim-line update (`main+0x39ade0`); a scripted run that taps Y shows them often.

- **Open: widescreen.** The swapchain size 1280x720 is written twice in `main` (v0): a rodata entry at +0x6d337c (`{u32 w, u32 h}`, dead data) and two immediates at +0x2a6080 (`movz`/`movk x8`) that write it into a config object. Patching the rodata entry changes nothing, as the immediates win. Patching the immediates too (1600x900) lets the init run, then it crashes at `main+0x48e5a4` (`ldr x2,[x21,#8]`, `x21` null) in the call chain of `0x2a7a20`: the allocation at `0x499840` fails, or a lookup misses for the wider size, so the value feeds more than the swapchain and something depends on 1280x720 itself. Real widescreen needs its resolution path reversed, as was done for Diablo II: Resurrected (PARAMS slots, the aspect cap and the derived-mask fix, each found and tested one by one); not enabled, no profile key. Found in the fork's investigation (LNPANDY/NX2Apple 44ec9c9e, 19a82b2b); the addresses are v0's, not carried to the update.

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

## Open

- `game.tsv` says values marked "Mario's" are not measured for this game; none of its rows is marked so.
