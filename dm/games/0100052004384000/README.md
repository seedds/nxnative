# Axiom Verge (`0100052004384000`)

Update 1.0.5 (v262144), SDK 12.3.3. Thomas Happ Games, MonoGame for the Switch (Microsoft.Xna.Framework over NVN), MonoGame's and the game's C# compiled to C++ by Sickhead's BRUTE into `main`, Opus audio (`nn::codec::OpusDecoder`), on NVN. Modules `main` and `sdk` only. Brought up with no host change; plays on the Mac.

On the Mac: boots natively, disassembles, passes memory checks, plays the opening cutscenes and ingame at 60 fps with sound. Packages a standalone Mac application (`Axiom Verge.app`, 233.1 MB).

## What it needs

The provisional profile's guesses held: no key needed changing.

| Setting | Where | Why |
|---|---|---|
| `sys_resource 0x0` | [`game.tsv`](game.tsv) | the NPDM declares no system resource: `IsVirtualAddressMemoryEnabled` is false, as on Horizon: no virtual address memory. The game sets a 3.25 GB heap (pool − used memory) |
| 4 GB layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | `game.tsv` | 256 MB in front + 256 MB alias region + 256 MB stack region + 3.25 GB heap region |
| `mem_used 0x5e00000` (94 MB) | `game.tsv` | the rewritten modules are larger than the default 64 MB of used memory, so the heap request plus the modules would exceed the pool |
| `shader_groups /Content/=content` | `game.tsv` | the MonoGame effects; see [Shaders](#shaders) |

## Shaders

- 120 shaders (61 pixel, 59 vertex) in MonoGame effects (MGFX version 8): the 20 XNB files in `Content/Shaders` and SpriteBatch's built-in effect in `main`, read by `shader_split.py` (`mgfx_effect`).
- All 120 shaders compile with Apple Metal and pack into the offline pipeline archive (`applegpu_g13g`, 1.7 MB).

## Input

- In menus and dialogues, Jump / OK is tied to the **B button** (`K` on the Mac keyboard) and Cancel is tied to the **A button** (`J` on the Mac keyboard).
- Alternatively, clicking on screen options with the mouse interacts via the touchscreen.
- Arrow keys / D-pad for direction and movement.

## Mac route

- Build with `scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> --platform mac-app --romfs bundle`.
- The application is created in the game folder as `Axiom Verge.app`.
- Save files live in `~/Library/Application Support/nxnative/0100052004384000/`.
- Logs write to `~/Library/Logs/nxnative/0100052004384000.log`.

## Found on this game

General host and tool changes first needed here; each applies to every game:

- **MonoGame effects (MGFX version 8)** in XNB files and in `main` (`mgfx_effect` in `shader_split.py`): [MGFX effects in MonoGame XNB files](../../../docs/reference/shaders.md#mgfx-effects-in-monogame-xnb-files).

