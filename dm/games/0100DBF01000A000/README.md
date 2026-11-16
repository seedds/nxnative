# Burnout Paradise Remastered (`0100DBF01000A000`)

The base game, 1.0.0 (v0, no update), SDK 9.3.0, NVN. Criterion's engine, with Criterion's Bundle 2 files (`.BNDL`, `.BUNDLE`, `.BIN`, `.DAT`) and MP4 movies. Modules `main`, `subsdk0` (NintendoSDK_movie) and `sdk`. The RomFS has 8,152 files, 3.96 GB.

Status: **plays on the Mac** (Apple M6, 16 GB, macOS 27.2, 2026-11-16). From a new save it reaches the title, the main menu, the intro flyover of Paradise City, the junkyard and a drive on the I-88, with the world, the player's car and traffic drawn and the game's autosaves. A restart loads that save into the city. Over about seven minutes on the Mac (the two recording runs below), frames held 16.67 ms (p50 in every 300-frame window while the window was in front), and the GPU took 2–5 ms a frame while driving (p50). No shader was missing from the map, and the host's footprint stayed under 2.6 GB. No iPhone or iPad run, and no race or event was played here. A contributor's earlier report ([#1219](https://github.com/seedds/nxnative/issues/1219), before the version 2 reader) won a Road Rage event, with menus, HUD, minimap, music and events working.

## What it needs

The NSP script's provisional profile as it was made. There is no game-specific host code, rewriter cut or frame-rate key.

| Setting | Where | Why |
|---|---|---|
| The shaders of its Bundle 2 files, version 2 | `shader_split.py` `bnd2_resources` | the model and material shaders are whole GLSLC outputs in zlib resources of Criterion's Bundle 2 version 2, whose resources have three blocks and 0x40-byte entries ([GLSLC outputs in Criterion's Bundle 2 resources](../../../docs/reference/shaders.md#glslc-outputs-in-criterions-bundle-2-resources)) |
| `modules main subsdk0 sdk` | [`game.tsv`](game.tsv) | `main` imports both |
| `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | `game.tsv` | `profile_guess.py`'s hardware pool: the game sets a 3.14 GB heap in the 3.25 GB heap region |
| `sys_resource 0x0` | `game.tsv` | the NPDM declares no system resource, so no virtual address memory |
| [`pso_manifest.tsv`](pso_manifest.tsv) | profile | the prewarm list: 229 shaders and 156 render pipelines, recorded on the Mac over a new save from boot to the first drive and a load of that save |

## Shaders

376 (192 pixel, 184 vertex): 138 from `main` and 238 from the Bundle 2 files. The 184 vertex shaders are single-stage programs, each paired with one pixel cover for padding (`c1a608e6add64c26`). Before the version 2 layout was read, the split read 2,882 of the game's files as damaged and kept only `main`'s 138. Those are the effect, sky and HUD shaders, so the road, buildings, cars and traffic were never drawn (#1219).

## Frame rate

**Current: 60 fps on the Mac**, the game's own rate. It asks for present interval 1 (`present tick 60 Hz, interval mode game … present interval set by the game 1`), and the profile has no `fps_target` or `present_interval` key. Game speed was not compared with a Switch.

A window hidden behind another one takes the display link to about one tick a second (`front end: window covered … frame times measure nothing`). The frame times then read about 34.5 ms, and `DM_CTL` screenshots are not written. That is the host's covered-window state, not the game.

## Services

- The game runs offline. On a later start it shows `Connection Problem` (the Burnout Paradise server is not available) after the title; A dismisses it, and the main menu follows.
- A new save shows EA's Privacy and Cookie Policy and then its Legal Agreements before the main menu. The policy can be declined (`Do Not Accept/Continue`); the agreement has only OK.

## Rendering

- **Current:** `nvnCommandBufferSetConservativeRasterDilate` and `nvnCommandBufferSetSubpixelPrecisionBias` are unimplemented. Each is called once at boot, with no visible effect.
- **Open, not checked:** the #1219 report mentions vehicles jittering back and forth in the story intro, seen while another build was compiling. No such run was watched for it here.

## Driving it on the Mac

`scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> --platform mac` builds it in `0100DBF01000A000_build` beside the NSPs. The work folder `0100DBF01000A000-work` beside it holds the scripts:

- `boot.sh` runs the build with a renamed host copy (`bphost`), the window in front with the Metal HUD, sound and `DM_CTL=ctl.txt`. `SAVE=<dir>` picks the save, `EXTRA="K=V …"` adds environment (no spaces in its values), and `BPHOST` picks another host.
- `ctl.sh <line>…` appends presses to `ctl.txt`; `see <name>` writes `shots/<name>.png` and waits for it.
- `mash.sh <name> <seconds> [key]` presses A (or the key) every 2 s with a shot every 10 s.

From a new save:

1. The logos and an attract movie run for about 45 s before `Press + button`. Press Plus.
2. A continues past the saving-icon notice.
3. Down and A decline the privacy policy, and A takes the Legal Agreements' OK.
4. A on `Enter Paradise City` starts the intro flyover. A every few seconds runs through it, the driver's licence and the junkyard, to the car at the junkyard gate.
5. ZR accelerates.

`rec/save` holds a save after the first drive, from which `Enter Paradise City` goes straight to the city.

## Found on this game

General changes first needed here; each applies to every game:

- [GLSLC outputs in Criterion's Bundle 2 resources](../../../docs/reference/shaders.md#glslc-outputs-in-criterions-bundle-2-resources): the version 2 layout (`shader_split.py`)
