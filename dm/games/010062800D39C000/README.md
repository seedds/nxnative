# SpongeBob SquarePants: Battle for Bikini Bottom - Rehydrated (`010062800D39C000`)

Purple Lamp Studios / THQ Nordic, Unreal Engine 4 (`ue4`). Modules `main` and `sdk` on NVN. Profile configured for Apple Silicon on macOS.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `pso_mtl4 on` | [`game.tsv`](game.tsv) | pipeline archives and hybrid pipelines through MTL4Compiler |
| `fps_target 60` | [`game.tsv`](game.tsv) | 60 fps target cap |
| `mem_alias 256M` | [`game.tsv`](game.tsv) | 256 MB guest alias memory region |

## Engine and graphics

- Unreal Engine 4 (`ue4`) with Metal 4 pipeline state compilation (`pso_mtl4 on`).
- Targeted frame rate of 60 fps (`fps_target 60`).

## Memory

- 4 GB guest memory layout with a 256 MB alias region (`mem_alias 256M` / `0x10000000`), 256 MB stack, and 3.2 GB application pool (`app_pool 0xcd500000`).
