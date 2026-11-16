# DreamWorks Shrek 2 Remastered (`01000750246F0000`)

Version 1.0.2 (v0), SDK 21.4.2, NVN API. Custom engine with FMOD audio and Epic Online Services (EOS) integration. Modules `main`, `subsdk0` (GLSLC) and `sdk`. Plays on the iPhone and Mac at 60 fps.

## What it needs

| Setting / Fix | Where | Why |
|---|---|---|
| `pad_layout full` | [`game.tsv`](game.tsv) | 3D platformer camera and controls |
| `mem_reserve 0x140000000`, `mem_alias 0x70000000`, `app_pool 0xCD500000` | `game.tsv` | the game requests a 2.70 GB heap; verified by Mac heap check |
| `ro::RegisterModuleInfo` success | `dm/host/hle_ro.c` | the game registers EOS NRR at boot and asserts `Result == 0` |
| `ro::LoadModule` online SDK stub | `dm/host/hle_ro.c` | refuses EOS module gracefully with state 1 to play offline |
| `friends::GetFriendList` count 0 | `dm/host/hle_offline.c` | returns `*count = 0` and `RES_OK` instead of failing |
| `friend:u` service & stubs | `dm/host/hle_sf.c`, `hle_offline.c` | serves `friend:u`, stubs `EnsureBlockedUserListAvailable` |

## Rendering & Shaders

The game uses deferred/HDR rendering with a G-buffer at 1824×1024.

- **Post-processing 3D LUT & Tonemap Curve (`70a70e279d508295_b` / `post_proc.frag.nvn`):**
  The post-processing pass samples the scene color target (`fp_t_tcb_A`), a 3D color LUT (`fp_t_tcb_C`, 32×32×32, guest texture `ser=1670`), and a tone curve from uniform buffer `fp_c3` at offset 96+. In guest memory, the 3D LUT is uninitialized (`[252, 253, 255, 255]` slices) and the curve parameters in `fp_c3` evaluate to zeroes, causing the shader to multiply the rendered color by zero/monochrome threshold. Patching `70a70e279d508295_b` to pass through the full-color scene target restores complete color fidelity in the 3D world.
