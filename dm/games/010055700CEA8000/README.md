# SD GUNDAM G GENERATION CROSS RAYS (`010055700CEA8000`)

Platinum Edition, all in one cartridge dump: base game v0 + update 1.6.3 (v720896) + 31 DLC.
TOM CREATE / Bandai Namco, **SDK 9.4.1**, NVN. Modules `main`, `rtld`, `sdk`, `subsdk0` (NintendoSDK_movie).

Status: **plays on the iPhone 16 Pro and on Apple-silicon Macs**, 60 fps (frame time p50 16.66 ms,
p95 16.79 ms, no dropped frames), footprint about 1.8 GB on the Mac and 1.3 GB on the phone. Title,
menus, battles, textures, effects and sound all work; the tutorial overlays and the stage-start panel
draw correctly. Not checked: an hour or more of continuous play, the 31 DLC's content one by one.

## What it needed to boot

Three fixes; the first is a profile value, the other two are general host changes (not workarounds).

| # | Symptom | Root cause | Fix |
|---|---|---|---|
| 1 | `nninitStartup` crashed at `main+0x5cc508`: `str x10, [x8]` with `x8 = 0` | `nn::os::AllocateMemoryBlock` returned 0. `nninitStartup` sizes its heap as `pool − used memory`, and the host reported 64 MB used while this game's modules take `0x7800000` (120 MB), so `SetHeapSize` failed. The game never checks the result. | profile `mem_used 0x8000000` (128 MB), the same fix as Hyrule Warriors: Age of Calamity (SDK 9.6.0, `docs/reference/memory.md`) |
| 2 | `nn::fs::ReadFile` faulted at `sdk+0x62a78`: `ldr w8, [x0, #0x24]` with `x0 = 0x4000000000000390` | `main` imports `nn::fs::ReadFile(FileHandle, s64, void*, size_t, const ReadOption&)` — the overload **with a ReadOption and no bytes-read output**. The host hooked its `<size_t*, …>` twin and `MountRom`'s cache-less overload but not this one, so the call fell through to the SDK's own implementation, which dereferences the handle as a real `FileAccessor*`. The host's handle is a synthesised image-entry id (`1<<58 \| mount<<32 \| index`, `<<4` for the guest), so it is not a valid pointer. `GetFileSize`, hooked, had answered 589868 correctly just before. | `dm/host/hle_fs.c`: `HLE_FORCE` for `_ZN2nn2fs8ReadFileENS0_10FileHandleElPvmRKNS0_10ReadOptionE`, reading in full like its twin |
| 3 | the whole screen black but for text; with that fixed, one overlay kept its text but lost its panel | This game sets `NVN_TEXTURE_FLAGS_COMPRESSIBLE` (0x8) on **every** texture it creates. That is a Switch memory-compression hint, not a data format: in its `CPU_UNCACHED` pools (flags 0xa2) the texels are plain. Two host paths excluded such a texture — `tex_init_fill` (content at creation) and `ft_add` (the CPU-write tracking that `tex_flush_upload` / `tex_dirty_upload` rely on). The overlay's panel is created as zeros and written a frame later, so the second path kept it out, unregistered and never uploaded. | both now exclude the flag only in a `CPU_NO_ACCESS` pool, where the compression tags may really be there (`docs/reference/nvn-objects.md`, "`COMPRESSIBLE` in a pool the CPU writes is only a hint") |

Fix 1 was found by reading the crash site with `objdump` (`adrp`/`ldr` around `0x5cc4e8`) and resolving the GOT slot
through the module's `DT_JMPREL` relocations; fix 2 the same way, by naming every `nn::fs` import of `main` and
diffing it against the host's `HLE_FORCE` list. Fix 3 came from a diagnostic of every texture at creation
(`DM_NVN_TEXINFO`) and of every skip it took (`first uploads 0`, then `write-protection faults 0`).

## Why this title is a good candidate

It is structurally the same shape as **13 Sentinels: Aegis Rim**, which plays on the iPhone:

| | Cross Rays | 13 Sentinels |
|---|---|---|
| SDK | 9.4.1 | 11.4.3 |
| Graphics | NVN | NVN |
| Middleware | CRIWARE (`.hca` x21831, `.usm` x114) | CRIWARE (CPK, ADX2, USM) |
| Modules | `main` `sdk` `subsdk0`(movie) | `main` `sdk` `subsdk0`(movie) |
| Shaders | 1090 GLSLC outputs in `main`'s read-only segment | 838 GLSLC outputs in `main`'s read-only segment |
| Assets | `/Data.bin` 15.14 GB | `ROBO_*.CPK` 4.3 GB |

SDK 9.4.1 is `<= 10`, so the host reports **NVN API 53.313** by itself (`hle_nvn.c` `nvn_api`,
`DM_SDK_OLD`). Hyrule Warriors: Age of Calamity (SDK 9.6.0, also Bandai Namco / Koei Tecmo) already
runs on that path, so no `device_api` override is needed.

The engine is **not Unity or Unreal**: CRIWARE plus compiled Lua 5.2, with PMDL models and DDS/TXD
textures. Nothing in RomFS is a shader container, so the split reads `main` alone.

## What was measured

### Survey (`010055700CEA8000_survey/survey.md`)

- SDK 9_4_1; components `NintendoSDK_NVN-9_4_1`, `libcurl`, `libz`, `movie`, `nnSdk`.
- Engine markers: NVN (all three modules), CRIWARE (main).
- RomFS: 22,100 files, 18.91 GB. `/Data.bin` 15.14 GB, `data/sound` 21,179 files (`.hca`), `data/movie` 114 `.usm`.
- `main.npdm`: program ID `010055700CEA8000`, **0 MB system resource** (no virtual address memory; the game sets a heap, as 13 Sentinels does).

### Rewrite-site census

`main` has **no `svc`, no `mrs tpidr*`** — every system call lives in `sdk`. Only 2019 `x18` uses.
That keeps the rewrite surface in `sdk` (61 `svc`, 56 `tpidrro`, 32 `cntpct`).

| module | plain | svc | cuts | gates |
|---|---|---|---|---|
| main | 1871 | 0 | 0 | 9 |
| sdk | 11021 | 61 | 5 | 109 |
| subsdk0 | 10447 | 0 | 0 | 13 |
| rtld | 18 | 4 | 0 | 4 |

`sdk` matched two SDK-wide patches by **function fingerprint** (`MATCH_CUTS`, which hashes each
registered implementation's body, not the module's build ID, so it spans SDK versions). Its build ID
`290c9a04…` was never registered, yet both rules applied: `implementation 1 of 6` for the node area and
`implementation 1 of 4` for the alignment, five cuts in all.

- `AddressRegionManager node area 1 GB → 62 MB` at `sdk+0x336b50`, `sdk+0x336bf0`
- `AllocateAddressRegion: regions of 4 MB and more aligned to 2 MB, as SDK 20` at `sdk+0x336d54/0x336d5c/0x336d60`

This matters for the memory layout: without the cut the SDK reserves about 1 GB of the 64 GB alias
region for its node area, and every `AllocateAddressRegion` of 4 MB or more lands at an alignment the
host's GPU addresses do not have. Both are handled, so the profile needs no `patches.py` of its own.

### Shaders

1,090 GLSLC compiler outputs (magic `0x19866891`) in `main`'s read-only segment: **173 `vs`, 917 `ps`**.
All 1,090 pass `shader_split.py`'s triple check, which leaves **639 unique** records (127 `vs`, 512 `ps`).

The whole chain was verified by hand on the Mac:

```
639 records → dmsh_xlate (SPIR-V):  639 ok, 0 failed   (1.3 s)
            → spirv-cross (MSL):    639 ok, 0 failed   (2.1 s)
```

Zero failures at both stages. `main`'s `vs` were paired with a single pixel-shader cover
(`0e4f9b825efa5453`), as 13 Sentinels does.

### Imports vs the host's hooks

`main` imports **658 symbols**; the host has **1040 HLE hooks**, and **776 of them resolve in this
game's `sdk`** (75%). The largest areas, and what each costs:

| area | imports | what it needs |
|---|---|---|
| other C++ | 245 | `_ZN2nv18InitializeGraphics`, `nv::SetGraphicsAllocator`, `movie::SetCoreMask` — NVN's own allocator hooks |
| C / libc | 222 | `__cxa_*` exceptions, `__assert_fail`; the host's C library |
| `nn::os` | 51 | events, multi-wait, thread info — the core path already exercised |
| `nn::fs` | 25 | `CreateFile`, `DeleteFile`, `RenameFile`, `GetFileSize` — the save path |
| `nn::audio` | 15 | `StartAudioOut`, `GetAudioOutName`, … — CRIWARE's output |
| `nn::hid` | 14 | `GetNpadState` for FullKey / JoyDual / Handheld |
| `nn::oe` | 13 | `GetOperationMode`, `FinishStartupLogo` |
| `nn::fontll` | 13 | `ScalableFontEngine` — Japanese text |
| `nn::account` | 12 | `Initialize`, `AsyncContext` |
| `nn::vi` | 8 | layer and display setup |
| `nn::time` | 7 | `ToCalendarTime`, system clocks |
| `nn::swkbd` | 6 | `ShowKeyboard` — character naming |
| `nn::nifm` / `pl` / `settings` / `aoc` / `socket` / … | 1–5 each | network, profile, 31 DLC indices, BSD sockets |

Every one of these areas already has host code for another title; the work is checking the SDK 9.4.1
layouts, not writing new subsystems.

## What was checked, and what was not

Checked on the Mac (M5 Ultra, macOS 27.2) and on an iPhone 16 Pro (A18 Pro, iOS 27.2): the boot, the
title, the menus, the first scenario's opening movie and its stage-start tutorial overlay, the stage
itself with 3D units, and the character-scout screen. 60 fps, no dropped frames, no `[RED]` lines,
footprint 1.8 GB / 1.3 GB. Audio opens six channels and the movie decoder produces 1920×1080 frames.

Not checked, and worth knowing before claiming more:

1. **Long runs.** An hour or more of continuous play, and whether memory grows over it.
2. **The 31 DLC** mount and are reported as owned (`aoc: reporting 31 add-on contents`), but their
   content was not played through.
3. **`nn::swkbd`** — 6 imports (character naming); the host's keyboard path was never exercised.
4. **Content past the first scenario.** A shader that the first stage never draws could still fail.
5. **`--romfs separate` on the phone** — the RomFS is 20.3 GB, so it is pushed once with
   `scripts/push_romfs.sh` (or Finder; `devicectl` truncates past about 10 GB and the app then rejects
   the image with `EINVAL`).

## Reproduce

```bash
# from /Users/g1n/dev/SWC2IPHOPNE
./run_nxnative.sh Game -k keys/prod.keys --survey-only          # survey (~45 s)
./run_nxnative.sh Game -k keys/prod.keys --platform mac          # Mac shaders + host, then run the command it prints
./run_nxnative.sh Game -k keys/prod.keys --romfs separate        # the IPA (1.9 GB, game files separate)
```

The Mac run needs `DM_WINDOW=1` to show its window and `DM_AUDIO=1` for sound. Save data is kept in
`run/save` between runs.

Xcode 27.2 with `xcodebuild -downloadComponent MetalToolchain` is required for the shader libraries:
`dmsh_chain.lock` pins the Metal compiler at `32023.922` (27B52 ships exactly that, so no
`--allow-other-metal`).
