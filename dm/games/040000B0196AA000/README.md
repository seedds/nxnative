# Red Dead Redemption, Switch 2 program (`040000B0196AA000`)

The Switch 2 program of Red Dead Redemption, update v458752 (no NACP, so no display version), SDK 20.5.17 built for the Switch 2 (`Ounce`), NVN2 shaders. Rockstar's RAGE engine in Double Eleven's port, as in the Switch 1 release ([`01007820196A6000`](../01007820196A6000/README.md)). Modules `main` and `sdk`; `subsdk0` and `subsdk1` are C libraries that `main` takes nothing from. The NUP holds the program and a licence-only upgrade add-on (`010000B0196AB001`). The profile is the NSP script's provisional one.

On the Mac (2026-11-23, Apple M5 Max, macOS 27.0.1, window): the opening movie and the game start normally, and about 5 minutes were played. The longest run (NX2Apple's engine with the changes in [Found on this game](#found-on-this-game)) held 60 fps after loading (frame time p50 16.67 ms, p95 16.7 ms; GPU p50 about 6 ms) with a footprint up to 3.5 GB. Its only `[RED]` lines are four DLSS debug overlay setters (`nvnDlssParametersSetOverlay*`), called once each. The game's DLSS runs on MetalFX's temporal scaler (1280×720 to 2560×1440). The unsigned IPA was not built.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `mem_reserve 0x230000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0x240000000` | [`game.tsv`](game.tsv) | the provisional layout: the Switch 2's 9 GB pool; `main` sets a heap of at most the pool − 64 MB − its 1209 MB of bss |
| `sys_resource 0x0` | `game.tsv` | the NPDM declares 0 MB of system resource |
| `modules main sdk` | `game.tsv` | `main` imports only from `sdk` |
| `romfs_marker audio_ounce.rpf` | `game.tsv` | the Switch 2 program's audio archive (the Switch 1 release has `audio_switch.rpf`) |
| `zip_store .bk2:.pat:.pkdounce:.rpf:.metallib` | `game.tsv` | the archives and movies are already compressed |

It runs at 60 fps by itself: the game sets present interval 1, and the profile sets no `fps_target`.

## Files

- RomFS: 140 files, 11.30 GB: 135 `.rpf`, two `.bk2` movies, one `.pat`, and `d11extra/langselect.pkdounce` with its index `.pkiounce`.
- The RPF6 table key is built by a key setup in `main` from `mov` / `movk` immediates ([RAGE RPF6 archives and `rgxd` effects](../../../docs/reference/shaders.md#rage-rpf6-archives-and-rgxd-effects)).

## Shaders

- 4,062 shaders (2,593 vertex, 1,469 pixel): 3,890 from 143 RAGE effects in `shaders.rpf`, 170 from effects in `main`, and 2 from the language selection's Double Eleven pack.
- **Open:** the prewarm list (`pso_manifest.tsv`) is empty.

## Found on this game

- Resolved: the RPF6 archives did not open: the key setup differs from the Switch 1 builds' → [RAGE RPF6 archives and `rgxd` effects](../../../docs/reference/shaders.md#rage-rpf6-archives-and-rgxd-effects). The effects' NVN2 control sections (0xe0 bytes) are read in the same entry.
- Resolved: the language selection's shaders were missing: the Double Eleven pack is named `.pkdounce` → [Double Eleven `.pkdswitch` packs](../../../docs/reference/shaders.md#double-eleven-pkdswitch-packs).
- Resolved: a thread spun forever at start: the host offered only cores 0 to 2 → [A Switch 2 application runs on six cores](../../../docs/reference/threads.md#a-switch-2-application-runs-on-six-cores).
- Resolved: without a NACP, the add-on base was the program's ID + 0x1000, which the upgrade add-on does not use: it keeps the Switch 1 application's ID (`010000B0196AA000` + 0x1001), and `pack_nup.py` now takes that base when every add-on falls in its range.

## Open

- iPhone: no IPA built.
- Only the opening and about 5 minutes of play were checked: riding, gunfights, the wider map and reloading a save are untested.
- The empty prewarm list.
