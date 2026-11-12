# FAST Fusion (`040041E0212AE000`)

A Switch 2 game: v262144 (packed from a `.nup`), SDK 20.5.13 built for the Switch 2 (`Ounce`), NVN2. Shin'en Multimedia's own engine. Modules `main` and `sdk`; `subsdk0` and `subsdk1` (C / libc) are not loaded. It plays on the Mac: the title menu with the ship, and races with the sky, the track and the HUD drawn (a Mac app built by the NSP script, played by the user; played on chain `5a0eb80d20cc`, before the rebase onto `09a91c5c`). Tested on the Mac only, an M1 Max (64 GB).

## What it needs

| Setting | Where | Why |
|---|---|---|
| `mem_reserve 0x270000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0x240000000` | [`game.tsv`](game.tsv) | the Switch 2's 9 GB pool; `main` sets a heap of at most the pool less 64 MB |
| `sys_resource 0x0` | `game.tsv` | the NPDM declares none, so virtual address memory is off |
| `modules main sdk` | `game.tsv` | `main` takes nothing from the two subsdks that the host does not supply |
| `romfs_marker terrain/10_materials/00_snow_normal.bc7u.tex`, `zip_store .otf:.tex:.vma:.metallib` | `game.tsv` | the NSP script's guesses |
| `depth_mode zero` | `game.tsv` | the game calls `nvnDeviceSetDepthMode` with near = 0 |
| `shader_groups` (the RomFS's top folders) | `game.tsv` | the shaders are in [SHN! resource files](../../../docs/reference/shaders.md#shn-resource-files-shinen) all over the RomFS |

There is no game-specific patch and no rewriter cut. The profile is the NSP script's guess from the survey with its name filled in; only v262144's build row exists.

## Shaders

2,976 in [`shader_map.tsv`](shader_map.tsv): 1,431 pixel, 1,398 vertex, 85 compute, 31 task, 31 mesh. 2,491 come from the `.w3d` models (their materials' programs), 457 from the `.shdr` shader sets and 28 from the NDSR file `ounce-nvn2-upper_left.ndsr`. All build, with no binding left unresolved.

Before the SHN! reader the split found only the 28 NDSR shaders: the game ran with sound over a black screen.

## Driving it on the Mac

`python3 scripts/build_ipa_from_nsp.py <folder> --platform mac-app --app-name "FAST Fusion"` builds the app from the folder holding the `.nup`. The game draws to 3840x2160 window textures with a 1920x1080 crop, so a `DM_CTL` `shot` holds the picture in its top-left quarter. The title menu is up within about a minute of boot, at about 60 fps (frame 3,300 at 58 s). [`smoke.tsv`](smoke.tsv), the route `scripts/regress.py` runs, boots a fresh save to it.

## Found on this game

- [SHN! resource files (Shin'en)](../../../docs/reference/shaders.md#shn-resource-files-shinen): the black screen.
- [Half words of a task's payload (ISBEWR 0xb4, ISBERD 0x34)](../../../docs/reference/shaders.md#half-words-of-a-tasks-payload-isbewr-0xb4-isberd-0x34): the menu and HUD, drawn by task and mesh shaders.
- [Ampere forms met on PRAGMATA](../../../docs/reference/shaders.md#ampere-forms-met-on-pragmata-tld-through-a-uniform-register-i2f-of-an-immediate-suatomcas): 0xf66 in 8 terrain vertex shaders (`terrain.shdr`).
- [A compute shader's c0 handle table read at a computed offset](../../../docs/reference/shaders.md#a-compute-shaders-c0-handle-table-read-at-a-computed-offset): the volumetric fog `cded8d0899ba4a6d`; the race's sky was black.
- [The DLSS output keeps the input's alpha](../../../docs/reference/rendering.md#the-dlss-output-keeps-the-inputs-alpha): the title menu hidden behind the ship.
- [Per-frame rows lost on an exFAT drive](../../../docs/reference/frame-rate.md#per-frame-rows-lost-on-an-exfat-drive): the first regression run's frame counts.
- [DLSS scalers released under their encoders](../../../docs/reference/rendering.md#dlss-scalers-released-under-their-encoders): a crash after about 9 minutes of play.

## Open

- `[RED] NVN unimplemented function called: nvnCommandBufferSetProvokingVertexMode` (mode 0, [the first vertex](../../../docs/reference/rendering.md#the-provoking-vertex-is-the-first)), `nvnDlssParametersSetOverlayFullScreen` (0) and `nvnDlssParametersSetOverlayDebugLayer`: once each per run, not looked at.
- Once each per run, not looked at: `[RED] stage 0 cb1 has no uniform buffer bound`; `[RED] same-pass feedback sampling: the sampled view is not a single-level 2D view of the attachment's level and layer (type 3 levels 0+1 layer 0, …, shader 7890df2c983ed35c_b); left as is`; `[RED] cube texture view of 0 + 1 layers past the 1 faces of texture ID …: read as a 2D array`.
- The display version is not known (the `.nup` names v0, the package v262144). Multiplayer, a long session, frame rate and iOS are not tested; a `pso_manifest.tsv` has not been recorded.
