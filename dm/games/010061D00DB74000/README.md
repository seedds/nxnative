# Ori and the Blind Forest: Definitive Edition (`010061D00DB74000`)

Update 1.0.2 (v131072), SDK 8.2.0. Moon Studios / Xbox Game Studios. Unity IL2CPP with FMOD audio on NVN. Modules `main`, `subsdk0` (movie decoder), and `sdk`.

On the Mac: boots natively, unpacks and streams asset bundles, renders at 60 fps (1080p upscaled cleanly via MetalFX) with sound and controller or keyboard support.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `present_interval_zero on` | [`game.tsv`](game.tsv) | Ori sets `nvnWindowSetPresentInterval(0)` during boot and loading screens to stream asset bundles unthrottled; bypassing the 16.67 ms display-link wait drops initial load time from ~3 minutes to ~10–15 seconds |
| 5 GB layout: `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000`, `app_pool 0xcd500000` | `game.tsv` | standard 5 GB block for Unity IL2CPP games with virtual address memory |
| `sys_resource 0x1000000` | `game.tsv` | 16 MB system resource so `IsVirtualAddressMemoryEnabled` is true |
| `op_mode console` | `game.tsv` | docked 1080p rendering |

## Shaders

- 11,053 shaders translated from Switch originals to Metal Shading Language and bundled into the offline pipeline archive (`applegpu_g13g`, 95.6 MB).
- Initial boot prewarms render and compute pipelines from the bundled archive.

## Input

- Controller (Pro Controller, Xbox, DualSense) supported out of the box with rumble and motion.
- Keyboard: Arrow keys for movement, `Z` / `Space` for jump, `X` for spirit flame attack.
- Press `A` (or Controller `A`) at the title screen to proceed to save slots.

## Mac route

- Build with:
  ```bash
  python scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> --platform mac-app
  ```
- Save files live in `~/Library/Application Support/nxnative/010061D00DB74000/`.
- Logs write to `~/Library/Logs/nxnative/010061D00DB74000.log`.
