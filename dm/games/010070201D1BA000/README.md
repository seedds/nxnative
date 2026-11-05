# Core Keeper (`010070201D1BA000`)

Update 1.2.1.5 (v1) over the base game, SDK 20.5.21. Unity 2022.3.50f1 IL2CPP with FMOD on NVN. Modules `main`, `subsdk0` and `sdk`, and one NRO that the game registers through `nn::ro`. Reaches its world on the Mac: the title, world and character creation, then walking at the Core, at 30 fps. Not tried on the iPhone.

## What it needs

| Setting | Where | Why |
|---|---|---|
| A middle patch area in `main` | `dm/tools/patch_nso.py` `MID`, `dm/tools/nso_shift.py` (general: any module the two areas leave sites out of reach in) | `main` has 302 MB of text and 98 MB of ro, data and bss after it, so neither the front nor the back patch area reaches the middle of the text; see [Rewriting](#rewriting) |
| Node area cut 1 GB → 62 MB | `dm/tools/sdk_patches.py` SDK-wide rule, `sdk` build `c668ae02…` (implementation 5, `sdk+0x145d0c`, `+0x145d78`) | the SDK-wide rule; the build has no registered cuts |
| `sys_resource 0x1000000` | [`game.tsv`](game.tsv) | the NPDM's 16 MB of system resource: virtual address memory |
| 5 GB guest layout: `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | `game.tsv` | the NSP script's guess for a Unity game that maps its own memory; see [Memory](#memory) |
| `depth_mode zero` | `game.tsv` | the game calls `nvnDeviceSetDepthMode` with near = 0 |
| `shader_groups /Data/=data /.nrr/=nrr /Plugins/=plugins` | `game.tsv` | the NSP script's split groups |

No code of the game is cut. The profile is the NSP script's provisional one, with only its header rewritten.

## Rewriting

- **Resolved: `no split reaches` at the start of the rewrite.** `main`'s text is 302.0 MB, the longest so far; EA SPORTS FC 27's 185.6 MB was the longest before. With two patch areas, the back one reached no site below 272 MB and the front one none above about 111 MB. The rewriter now opens a 16 MB gap at a function start for a third area: [A middle patch area for a text that neither end reaches](../../../docs/reference/rewriter.md#a-middle-patch-area-for-a-text-that-neither-end-reaches).
  - The split is at `0xbfb0000`. 91,503 branches cross it, 24,786 of them through veneers.
  - The stubs take 3.7 MB in the front area, 2.2 MB in the middle and 165 KB at the back.
  - The rewrite takes about 4.5 minutes on an M1 Pro.
- Offsets in this game's logs, crash reports and rewriter tables are those of the shifted `main`. A `main` offset of `0xbfb0000` or more is `0x1000000` lower in the file's module.

## Memory

- **Current:** the alias region reaches +3.76 GB of its 4.38 GB once the world is loaded, with at most 2.02 GB mapped. That is about 3.2 GB resident and 0.77 GB of Metal allocations on the Mac.
- **Open:** that leaves 0.6 GB of alias region. Cult of the Lamb's Unity ran out of its alias region with the hardware pool and runs with a 1 GB one ([its note](../01002E7016C46000/README.md#memory)). A longer session, or a later part of the game, may need the same here. Not seen yet.

## Services

- **Resolved: `nn::diag abort` at startup.** The game aborts when `nn::friends::UserPresence::Initialize` fails, and the host failed it: [The player's own presence offline](../../../docs/reference/services.md#the-players-own-presence-offline).
- The software keyboard names the world and the character. Headless runs answer it with `DM_KBD=<text>`; without that, it counts as cancelled and **Create** stays greyed out.

## Frame rate

- **Current:** 30 fps on the Mac (M1 Pro, headless) at the title and in the world: frame time p50 33.3 ms, p95 33.6 ms. GPU time is 4.4 ms a frame at the title, 47 draws a frame. Whether the Switch version runs at 30 or 60 fps has not been checked.

## Mac routes

Headless, from an empty save, driven with `DM_CTL` and `DM_KBD=Cave`:

1. At the title, from about frame 2400: A seven times, 6 s apart. The world slots open, then world creation; the seventh opens the name field's keyboard.
2. Down four times (the focus wraps to the GENERAL tab), Up once to **Create**, then A. The stick (`LDown`) did not move the menu focus; the D-pad does.
3. At character select: A (slot 1). On character type, the first A toggles Standard / Hardcore, so press A again, then Down to **Done** and A.
4. At character creation: Down eight times to the name field, A, Right to **Done**, A. The world loads in under a minute.

## Remaining issues

- One `[RED] texture→texture copy case not implemented: 384x256x1→384x256x1 flags 0 format 92→92 3D 1→0` at the title. Nothing wrong was seen on screen.
- Not tested: audio output (headless runs use a silent sink), saving and loading a world, mining, combat, the iPhone.

## Found on this game

- [A middle patch area for a text that neither end reaches](../../../docs/reference/rewriter.md#a-middle-patch-area-for-a-text-that-neither-end-reaches)
- [The player's own presence offline](../../../docs/reference/services.md#the-players-own-presence-offline)
