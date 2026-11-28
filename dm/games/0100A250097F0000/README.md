# DRAGON BALL FighterZ (`0100A250097F0000`)

Update 1.50 (v1703936) over the base game, SDK 4.5.0. Bandai Namco / Arc System Works on Unreal Engine
(pak version 11: the shaders are kept compressed in shader code archives, decoded by booting the game on
the Mac), NVN. Modules `main`, `subsdk0` (NintendoSDK_movie) and `sdk`. The profile began as the NSP
script's provisional profile (the game has no hand-made one) and is kept here, with one class of work done
by hand: the character and UI textures draw black or in the wrong colours without `tex_flush_upload` /
`tex_dirty_upload` on (GitHub issue #1432). Everything else in the profile is still the survey's guess; the
memory values below were exercised on the Mac but not reasoned out against the game. SDK 4.5.0 is far older
than the SDK 16 the host's object and service sizes come from (D2R's SDK 12 converted to 16), so NVN/API
sizes are the SDK-4 shapes the survey found; nothing in this game has needed a host change keyed to it.

Status: plays on the Mac — the game boots, runs the menus, story mode and a versus battle at 60 fps with no
`[RED]` line, and the textures render correctly after the fix below. Not brought up on the iPhone: the two
texture keys act on the host the iOS app also builds from, so an IPA from this profile should carry the same
fix, but no iPhone has run it.

## What it needs

No `patches.py`: the modules run as built, with no cut.

| Setting | Where | Why |
|---|---|---|
| `tex_flush_upload on`, `tex_dirty_upload on` | [`game.tsv`](game.tsv) | the character and UI textures (DXT: `0x41` RGB_DXT1, `0x44` RGBA_DXT5, `0x45` DXT1_SRGB, `0x48` DXT5_SRGB; and 2D) are written by the CPU into **CPU_UNCACHED** pools (flags `0xe2`) after creation, with no `WriteTexels` and no flush; without both keys every such texture is GPU-only and reads stale (black blocks, wrong portraits, black story-mode cutscenes); see [Rendering](#rendering) |
| 4 GB guest block: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | `game.tsv` | the survey's Switch-1 4 GB layout, `main` setting a heap of at most the pool − 64 MB; the game runs with it on the Mac; see [Memory](#memory) |

## Memory

`main` declares 0 MB of system resource (`sys_resource 0x0`): `IsVirtualAddressMemoryEnabled` is false, as
on Horizon, and the game takes its heap from the 3.25 GB heap region. The provisional profile's 4 GB block
(the Switch-1 layout) served through a full play session on the Mac; the values were not checked against
`main`'s own `SetHeapSize`, so a hand-passed layout is still open.

## Rendering

The story-mode cutscenes and parts of the lobby and versus screens came out black or miscoloured, while the
game ran at a steady 60 fps with no `[RED]` line — a stale-texture problem, not a decode or pipeline one. A
`DM_NVN_FZDIAG` pass (a temporary per-texture print after `tex_from_storage` in `nvn_mtl.m`, since removed)
showed all 2,784 textures in pools with flags `0xe2` (CPU_UNCACHED | GPU_CACHED | SHADER_CODE | COMPRESSIBLE),
and every one `pkg=0` — none taken from its storage. Many of them, including the DXT character/UI textures and
a 1920×1080 R8 and a 960×540 RG8, held data in their storage (`nz=1`) yet were never uploaded: the game
writes them through the CPU into the uncached pool and never flushes, so the host filled each once
(`tex_init_fill`) or not at all and read it as a GPU texture.

`tex_flush_upload on` makes the host upload a texture from its pool storage at first use, and
`tex_dirty_upload on` re-uploads one whose pages the CPU wrote after creation. Both are the pair Final
Fantasy Tactics ([010038B015560000](../010038B015560000/README.md)), Tears of the Kingdom and ten other
profiles already set, for the same CPU-written-into-uncached-pool mechanism. Measured on the Mac after the
change: `packaged textures: first uploads` 0 → 2,590, `CPU-written textures alive` 0 → 941, `missed` 0; the
black and miscoloured scenes render correctly for a full play session.

## Files and shaders

- RomFS marked by `RED/Content/Paks/pakchunk0-Switch.pak`; `zip_store .mp4:.pak:.metallib`. The shaders are
  compressed inside the pak v11 shader code archives (2 paks) and decoded by booting the game on the Mac.
- Shaders: 12,545, split in 0:33. The Mac build compiles all 12,545 (about 4 minutes cold) and packs the app
  in about 10 minutes total.

## Build

`--platform mac-app`, `-l zh-Hans`, `--gpu g16s,g16g` (Apple M4). App 10.2 GB, 34 add-on contents embedded
(31 in the folder plus 3 licence-only from `DLC-UNLOCKER/`, whose title IDs do not overlap). DMG 9.2 GB,
`hdiutil verify` VALID.
