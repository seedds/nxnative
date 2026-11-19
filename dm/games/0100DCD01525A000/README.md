# Persona 3 Portable (`0100DCD01525A000`)

Update 1.0.2 (`v131072`), SDK 14.3.0. The same Atlus engine with CRI middleware
(CRIWARE) on NVN as [Persona 4 Golden](../010062B01525C000/README.md): the
shaders even sit under `sysdat.cpk/shaders/P4G/`, and `main`'s imports are a
strict subset of P4G's (265 of its 441, none new). Modules `main`, `subsdk0`
(NintendoSDK_movie) and `sdk`.

Started from the NSP script's provisional profile, which booted to the opening
text at 60 fps but showed garbled textures on 3D surfaces and erratic analog
sticks with a DualSense on the Mac (the D-pad was fine).

## What it needs

| Setting | Where | Why |
|---|---|---|
| `tex_flush_upload on`, `tex_dirty_upload on` | [`game.tsv`](game.tsv) | the provisional run logged `packaged textures: first uploads 0` with 314 textures in use: the game fills its textures from the CPU, as FINAL FANTASY TACTICS does, so the host has to upload what the CPU writes or the 3D surfaces sample stale storage |
| `shader_groups /sysdat.cpk/=sysdat` | `game.tsv` | every shader is a BNSH entry under `sysdat.cpk` (the guess named the `data*` folders, which hold none) |
| `pad_layout full` | `game.tsv` | 3D movement with the camera on the right stick, as P4G (the iPhone's virtual pad only) |
| `sys_resource 0x0` | `game.tsv` | the NPDM declares 0 MB of system resource |
| `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | `game.tsv` | the provisional layout: `main` carries about 149 MB of bss |
| `modules main subsdk0 sdk` | `game.tsv` | `main` imports from all three |
| `romfs_marker data/umd0.cpk` | `game.tsv` | the RomFS is the `data*` folders of CPKs and `sysdat.cpk` at the root |

## Files

- RomFS: 11 CPKs, 7.09 GB: `data/` (2 files, 6.26 GB), one folder per
  language (`data_CH`, `data_CK`, `data_DE`, `data_EN`, `data_ES`, `data_FR`,
  `data_IT`, `data_KR`) and the small `sysdat.cpk` that holds the shaders.

## Shaders

- 30 BNSH shaders under `sysdat.cpk/shaders` (`P4G/`, `system/`,
  `system/posteffect/hdr/`), one of them a geometry shader
  (`custom_Prim3D_Flat_geom`), which P4G does not have.
- `pso_manifest.tsv` holds the 27 shaders and 31 pipelines recorded on the Mac
  (`pso_user.tsv`) in a session from boot into the first scenes.

## On the Mac

- With this profile the game holds 60 fps from boot past frame 15,000 (Apple
  M2 Pro), at about 27% of one core. The texture log reads `first uploads 220`
  where the provisional run read 0, with about 30 CPU-written textures alive at
  a time.

## Open

- Analog sticks: with a DualSense on the Mac, both sticks moved erratically on
  the provisional build while the D-pad worked. No profile key governs stick
  input, and the game reads the sticks only through `GetNpadState` (full key,
  handheld, joy dual), the same calls P4G makes. A Mac controller's axes each
  reach full tilt on their own, so a diagonal reads up to about 1.15 of it,
  past the circle a Switch stick reports; holding each stick inside that circle
  in the host is a candidate, kept out of this profile because it changes every
  game. Not yet confirmed.
- Only the 1.0.2 build (`v131072`) is registered.
- iPhone: not built, not tested.
