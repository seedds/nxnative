# Mario Kart World (`0400C3F00006E000`)

A Switch 2 game: update 1.8.0 (v917504) over the base game, SDK 21.4.0 built for the Switch 2 (`sdk` names its spec `Ounce`), NVN2. Nintendo EPD's engine (sead / agl / gsys, Phive), modules `main` + `subsdk0` (NintendoSDK movie) + `subsdk1` (C library) + `sdk`. **In bring-up**: the profile and the rewritten modules exist; the game does not reach its frame loop yet. iOS is untested.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `sys_resource 0x4000000` | [`game.tsv`](game.tsv) | the NPDM declares 64 MB of system resource: virtual address memory on, and `main` sets a heap |
| `mem_alias 0xb0000000` (with `mem_reserve 0x140000000`) | `game.tsv` | the heap check: `nninitStartup` asks for a 1.68 GB heap (`0x6b800000`), which the guessed 128 MB heap region refused; a 1.75 GB heap region holds it, and the alias region gets the rest of the 5 GB block |
| `app_pool 0xCD500000` | `game.tsv` | the Switch 1 pool, not a Switch 2 measurement. The game sizes its heap from the pool (the pool less about 1.53 GB: with a 9 GB pool it asks for 7.47 GB), so the pool decides how much memory the game gets. Open |
| `romfs_marker Movie/StaffRoll_00.webm` | `game.tsv` | a file of the 1.8.0 RomFS |
| NVN2 API 231.39 | `hle_nvn.c` `nvn_api` | the game requires major 231 and minor ≥ 39 (`main+0xdd0890`, after `nvnDeviceGetInteger` items 0 and 1); its own `sdk` answers 231.39 (`DeviceGetInteger` at `sdk+0x65b680`; Donkey Kong Bananza's sdk answers 231.30). The host gives 231.39 to every Switch 2 SDK, which also meets Donkey Kong Bananza's minor ≥ 30 |

## Files

- Packed from a dump folder (`base/`, `updates/917504/`) with [`scripts/pack_dump.py`](../../../scripts/pack_dump.py): the update's RomFS is the patched one (206,450 entries, 21.9 GB), packed on its own.
- Build IDs of 1.8.0: `main` `d00e4f399e411d0613aa8d26b1c02fef`, `subsdk0` `746302f49f095cfca3d2188423c2bd77`, `subsdk1` `8dec9c0b1bed821364e77b10a07b9a28`, `sdk` `7a68b13d88183767fb9c7babffe89f3d`.
- The rewriter (`dm/tools/patch_nso.py`) rewrites the four modules with no cut. The SDK-wide address region rules of `dm/tools/sdk_patches.py` (the node area cut and the 2 MB alignment) match no function of this `sdk`, so its address region node area keeps 1 GB of the alias region (the heap check: alias region mapped up to +1.00 GB).
- Shaders: `Shader/*.Nin_Ounce_NVN2.bfsha.zs` (six files) and `Shader/Drive.Nin_Ounce_NVN2.release.sarc.zs`, the effects in `Effect/static.Nin_Ounce_NVN2.esetb.byml.zs`, the layouts' shaders in 90 `Layout/*.blarc.zs`, agl's and gsys's archives in `Lib/`.

## Shaders

- The split (`dm/gfx/tools/shader_split.py`, no new reader) finds 11,604 shaders (compute 730, domain 13, pixel 7,301, vertex 3,560): BNSH in the `bfsha` files, the effects and the layouts (and 40 in `main`), agl's BAHS archives (`sharcb`, in `Lib/` and in `Shader/Drive…sarc.zs`) and agl's NDSR pack (36). Groups: shader 4,252, other 4,071, effect 3,136, layout 105, main 40; 5,180 variation pairs.
- Full Mac build (chain 3c66991eb439, about 25 minutes from an empty cache): 11,522 of 11,604 translate. The 82 failures are Ampere instructions the translator does not decode ([Ampere shader code](../../../docs/reference/shaders.md#ampere-shader-code-switch-2)). Open: which of them the races need.

  | Opcode | Shaders | Stages | Source |
  |---|---|---|---|
  | 0x883 | 29 | pixel | `DrivePassShaderDefault.sharcb` |
  | 0x2be | 25 | compute 13, vertex 7, domain 5 | `DrivePassShaderDefault.sharcb` |
  | 0x927 | 14 | domain 8, vertex 6 | `DrivePassShaderDefault.sharcb` |
  | 0x992 | 10 | compute | `DrivePassShaderDefault.sharcb` |
  | 0x593 | 2 | compute | agl `agl_technique_pfx.sharcb` (as in Donkey Kong Bananza) |
  | 0x481 | 1 | compute | `DrivePassShaderDefault.sharcb` |
  | 0x59f | 1 | compute | `DrivePassShaderDefault.sharcb` |

- The translator warns `Attribute offset 0x5C is not valid.` in 2,838 shaders (Donkey Kong Bananza's build has none). Open: not analysed.
