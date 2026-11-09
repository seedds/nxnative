# Farming Simulator 26: Nintendo Switch Edition (`0100D440269E8000`)

Update 1.0.1 (v65536) over the base game, SDK 20.5.4. GIANTS Engine on NVN. Modules `main` and `sdk`. Runs on Apple silicon Macs and iOS.

Brought up on the Mac: loads the initial splash screens, title screen, menu, and enters 3D simulation gameplay.

## What it needs

| Setting | Where | Why |
|---|---|---|
| 4 GB layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | [`game.tsv`](game.tsv) | standard layout for games without virtual address memory; main reserves up to 3328 MB heap |
| GIANTS Engine `.sharc` shader reader | `dm/gfx/tools/shader_split.py` (`sharc_shaders`) | shaders are packaged in `shared/shader_switch.sharc` (`RAHS` magic, version 11) using raw LZMA compression |
| Dual-part vertex merger with intermediate c1 | `dm/gfx/tools/dmsh_build.py` (`vertex_ab`) | vertex shaders use Maxwell type 0 (`VertexA`) and type 1 (`VertexB`), with constant buffer c1 positioned between Part A and Part B (`c1o + 0x30 < b_sph`) |

## Shaders

- 2,209 GLSLC outputs (843 vertex, 1,181 pixel, 185 compute) packaged inside `shared/shader_switch.sharc`.
- **SHARC archive reader:** `shader_split.py` detects the `RAHS` container header, parses metadata table entries, and decompresses the raw LZMA payload (`lzma.FORMAT_RAW`) before scanning for GLSLC compiler outputs (`0x19866891`).
- **Dual-part VertexA/B merger:** Every vertex shader uses dual-part programs. `dmsh_build.py` merges `VertexA` and `VertexB` into one unified vertex shader, rewriting Part A's `EXIT` instructions into branches to Part B. In this engine, constant buffer c1 (`ctl[0x700]`) precedes Part B, sliced from `code[c1o : b_sph - 0x30]`.

## Memory

- `mem_reserve 0x100000000` with 3328 MB heap allocation.
- NPDM specifies no system resource reservations (`sys_resource 0x0`).
