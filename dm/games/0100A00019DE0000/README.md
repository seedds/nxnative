# Hades II (`0100A00019DE0000`)

Base game v0 (the NACP's display version is 0.0.126424), SDK 19.3.9, and update 1.0.141667 (v655360), SDK 20.5.15; both are registered ([Versions](#versions)). Supergiant Games' engine on **NVN**, unlike Hades ([0100535012974000](../0100535012974000/README.md)), which renders with Vulkan. The ExeFS holds only `main` and `sdk`; this `sdk` is byte-identical to FINAL FANTASY TACTICS - The Ivalice Chronicles' ([010038B015560000](../010038B015560000/README.md)). The NSP script's provisional profile booted it on the Mac with no host change: the title, a new save, the opening and Erebus at 60 fps. Plays on the iPhone.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `mem_used 0x6c00000` | [`game.tsv`](game.tsv) | `main` has an 82 MB bss: the rewritten modules take more than the default 64 MB of used memory, so `SetHeapSize(pool − 64 MB)` would not fit; see [Memory](#memory) |
| 4 GB layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | `game.tsv` | no system resource; the 3.25 GB heap region holds the 3.10 GB heap the game asks for |
| `pad_layout full` | `game.tsv` | the 3D gamepad layout: the right stick aims (the weapons with `ManualAiming` in `Game/Weapons/PlayerWeapons.sjson`) |
| `shader_groups /Shaders/=shaders` | `game.tsv` | the shader split's group for `Shaders/NVN/bin` |

## Versions

- **Current: v0 and 1.0.141667 registered.** `game.tsv` has a `build` row for each (1.0.141667: main `a862861fa2eeedea1d873c94324fca7b`, sdk `190030518b5d64e5d87b84bc54661af9`, read from the update's own ExeFS), so the NSP script packs either from the profile as it is, without the red line for unregistered modules. `package` and `shader_versions` stay v0: the update's 55 shaders all have keys among v0's 88 (its shader split gave a subset of v0's `shader_map.tsv`, and neither version has facts, pairs or `bf_want` rows), so v0's tables are already the union of both. The update's `main` gets the render size stub by its fingerprint ([Rendering](#rendering)); its `sdk` gets the SDK-wide AddressRegionManager node area patch, and the SDK 20 alignment patch has no reviewed implementation for it, as before.
- 1.0.141667 ran on the Mac through the opening into Erebus at 60 fps, from a derived profile and again from this one (2026-11-10); an iPhone run is not recorded. 1.0.131248 and 1.0.137848 (PR #485) are not registered: no copies here.

## Memory

- No system resource (META +0x14 = 0): the game uses `SetHeapSize` and asks for 3.10 GB (pool − used memory), which the 4 GB layout's 3.25 GB heap region holds.
- `main` has an 82 MB bss, so the profile's `mem_used` is 108 MB (`0x6c00000`).
- Resident memory levels off at about 3.4 GB in Erebus on the Mac.

## Rendering

- **Current: render size up to 3840x2160 (issue [#772](https://github.com/seedds/nxnative/issues/772)).** The game renders everything, the HUD and text included, at 1280x720 docked and handheld. The size is one constant in the app's init, the function that also runs the main loop: `mov x8,#0x500; movk x8,#0x2d0,lsl #32` (1280 | 720 << 32) right after `nn::vi::CreateLayer`, stored to the app settings (+8) and the window rectangle; the swapchain, every screen-sized target and the HUD's layout follow it. The game asks no service for a size (no vi or oe resolution import), so only its code can change it. The render size stub ([`patches.py`](patches.py), `MATCH_FRAME_STEP`) multiplies it by `DM_RENDER_SCALE`, each side rounded to a multiple of 4, from 0.5 to 3.0 (the launcher's Render scale, shown in pixels through `render_base`); 1.0, the default, keeps the game's constant. The function is found as the one that calls `nn::vi::CreateLayer` ([anchored on an import](../../../docs/reference/rewriter.md#frame-step-stubs-placed-by-their-function)), not by a fingerprint: v0 and 1.0.141667 already differed in one global's member offset there (`0xb40` / `0xb10`), which gave each its own fingerprint. The rule's checks, in each build's code: one call to CreateLayer, after it the function's one `movz xN,#0x500` / `movk xN,#0x2d0,lsl #32` pair, the pair once in `main`'s text, no branch onto the `movk`, and none of x16, x17, v16-v18 (the stub's) used in the function; otherwise a red line in the rewrite log and 1280x720. Reviewed: v0 `main+0x2a4410` (the size at `+0x2a45a0`, x8), 1.0.141667 (v655360) `main+0x2a5790` (`+0x2a5920`, x8); the modules came out byte-identical to the fingerprinted rules this replaced. An update that keeps these facts gets the stub with no change here. padsiwono found the site on 1.0.131248 (v196608), not checked here (no copy): the instructions they report there, at `main+0x2a4eb0`, are the same pair. PR #485's v0 address for it, `0x2a6080`, is not v0's. Its v0 crash at 1600x900 (`main+0x48e5a4`) was not seen at any size here.
- **Measured (2026-11-10, Apple M6, 16 GB, 1.0.141667, headless, the route below into Erebus, frames 3900-5400):** at every scale the game held 60 fps (frame p50 16.67 ms) with no red line, and the shots came out at the scaled size. GPU p50 per frame: 1.9 ms at 0.5 (640x360), 2.4 at 1.0, 4.0 at 1.5 (1920x1080), 6.0-6.3 at 2.0 (2560x1440, two runs), 6.7 at 3.0 (3840x2160). v0 at 1.5 the same way: 1920x1080, 60 fps, GPU p50 3.75 ms. The M6's GPU raises its clock under load, so GPU time grows less than the pixel count. Shots of native 2560x1440 against 1280x720 scaled to 2560x1440 by MetalFX and bilinearly (`DM_NVN_SHOT_OUT`, frame 4500, Melinoë in the first room): the native picture keeps thin outlines and fine foliage that both scaled ones lose; MetalFX gives hard but blocky, posterized edges, bilinear a soft picture. The HUD is not on screen there. A player on an 8 GB MacBook Air M1 ran 1080p at a locked 60 fps and found the difference slight on its 13-inch screen (#772).
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
- **A frame step state page where the patch area starts** (the render size stub's state page on v0's layout; the render scale's top from the stub): [A frame step state page where the patch area starts](../../../docs/reference/rewriter.md#a-frame-step-state-page-where-the-patch-area-starts).
- **Movie backgrounds on the iPhone (fixed, iPhone retest open):** the title screen's background grey and flickering, green and magenta blocks behind the options menu (GitHub issue #1956). The game decodes its Bink movies (`MainMenu.bik` and the menu backgrounds) on the CPU into 736x480 `IMAGE | LINEAR` R8 planes in a `CPU_UNCACHED` pool and only samples them. iOS wants 64-byte rows for a view on the pool's buffer, and 736 is not a multiple of 64 (the Mac asks for 16), so they became private textures filled once. Now a linear texture whose storage the GPU has not written is uploaded from it and again after each CPU write, whatever its `IMAGE` flag: [Linear textures a device cannot view on their pool's buffer](../../../docs/reference/nvn-objects.md#linear-textures-a-device-cannot-view-on-their-pools-buffer).

## Open

- `game.tsv` says values marked "Mario's" are not measured for this game; none of its rows is marked so.
- The iPhone retest of the movie backgrounds (#1956). The game is on this Mac since 2026-11-10 (v0 and 1.0.141667), but only a Mac run was made then.
- Render size: no iPhone run above 1.0 yet (GPU time and heat there), no play past the first Erebus room at a raised size, the anchored rule not run on 1.0.131248 or 1.0.137848 (no copies here).
