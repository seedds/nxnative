# Mario Kart World (`0400C3F00006E000`)

Update 1.8.0 (917504), SDK 21.4.0, on NVN2 (Switch 2). In bring-up on the Mac: it reaches the title, the menus and free roam, lit and textured, and is not playable to the end. The first Switch 2 title here.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `device_api 231.39` | [`game.tsv`](game.tsv) | the game's own check; with 55.15 it leaves NVN setup silently and crashes on a null global |
| `render_cond on` | `game.tsv` | the 3D scene was black: its clears are conditional, as on Tears of the Kingdom |
| 20 GB guest layout, `app_pool 0x263000000` | `game.tsv` | the game is told 9.55 GiB and plans all of it |

Shaders come from `dm/gfx/tools/nx2_split.py` (SM86 lowered to Maxwell by `nx2_lower`, which is GPL-3.0-or-later, see its README), then `dmsh_build.py`, then `nx2_res.py` and `nx2_reflect.py`. About 240 of them exist only at run time: a run with `DM_NVN_SHDUMP` saves them and `NX2_DUMPS` adds them, so the map here holds keys no game file gives.

## Open

- The ground: flat colour in hard rectangular patches. Layers of the terrain arrays past 768 MB stay zero (`sparse_layers_upload`); suspected, not confirmed.
- Task and mesh shaders: `DrawMeshTasks` draws nothing, and those programs do not lower.
- Movies: `nv::InitCheck` aborts when the title's attract movie starts, about 20 s in.
- Shading rate images, `SetGeometryProcessingMode` and `PushGraphicsConstants` do nothing; 32 shaders do not lower.
- Tested on the Mac only. No `regress.py` run yet.

## Found on this game

- [`render_cond`](../README.md): the profile key, here for NVN2's conditional clears.
