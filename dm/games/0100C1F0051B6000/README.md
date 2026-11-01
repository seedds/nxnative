# Donkey Kong Country: Tropical Freeze (`0100C1F0051B6000`)

Update 1.0.2 (v131072) over the base game, SDK 4.4.0, Retro Studios' own engine (RFRM forms, the Wii U game's engine), NVN. Modules `main`, `subsdk0` (the SDK's movie library) and `sdk`; `sdk` and `subsdk0` are the same builds as [Mario Kart 8 Deluxe](../0100152000022000/README.md)'s. `main` exports its own C++ names (76,513 dynamic symbols: `CShaderCore_NVN`, `NMaterialArchive`, `CAxalSystem` …), so Ghidra names nearly every function. RomFS is 83 files: `MaterialArchive.arc`, `PreLoadPak.pak`, `MiscData.pak`, `UI/UIPak.pak`, a `.pak` per world and level, and seven `.mp4` movies.

Brought up on the Mac from 1.0.2: boot, the title, Game Select, Play Mode, Controller Select, the intro cinematic (rendered in real time), the plane and the first level, Mangrove Cove: running, jumping, the slide down the waterfall, the swim, losing a life and restarting. 60 fps (p50 16.67 ms; the long frames are first uses of pipelines), no red line; the audio renderer produces sound (34,894 of the first 36,000 rendered frames not silent; not listened to); about 2 GB resident and 1 GB of Metal allocations in the level. The IPA (about 7.3 GB, iOS shader libraries for all 24,098 shaders) is built but not yet tested on the iPhone.

## What it needs

| Setting | Where | Why |
|---|---|---|
| 4 GB layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | [`game.tsv`](game.tsv) | no virtual address memory: the game finds its heap by a binary search ([Memory](#memory)) and settles on 3.09 GB in the 3.25 GB heap region |
| `mem_used 0x7c00000` | `game.tsv` | the rewritten modules are larger than the default 64 MB of used memory |
| `sys_resource 0x0` | `game.tsv` | the NPDM declares 0 MB of system resource |
| `modules main subsdk0 sdk` | `game.tsv` | every module `main` imports from |
| `shader_groups` (`/Worlds/`, `/UI/`, `/movies/`) | `game.tsv` | the shader split's groups ([Shaders](#shaders)) |

No rendering key: the title logo and menu panels that needed `tex_flush_upload` / `tex_dirty_upload` at first are covered by a general fill ([Rendering](#rendering)).

## Memory

- **Resolved: `Break` in `nninitStartup` after `SetHeapSize OOM: req 0x120000000`.** `nninitStartup` (`main+0x1e84c`) binary-searches `SetMemoryHeapSize` between 3 and 6 GB and narrows on each failure; the host answered a size beyond the heap region with a result the SDK aborts on. Now ResultOutOfMemory, as Horizon: [A heap request beyond the heap region returns ResultOutOfMemory](../../../docs/reference/memory.md#a-heap-request-beyond-the-heap-region-returns-resultoutofmemory). The search ends at 3.09 GB (pool less the rewritten modules).

## Shaders

- **Resolved: magenta 3D scene, `[RED] shader … is not in the shader map`.** The material shaders are SNVN program records in Retro's RFRM forms: `MaterialArchive.arc` (3,468 stages beside the same materials' DXBC), the 795 `MTRL` resources of the packs, and a startup material archive LZSS-compressed into `main` (`main+0x1510cf8`), whose four programs draw the title logo: [SNVN program records in Retro Studios' RFRM forms](../../../docs/reference/shaders.md#snvn-program-records-in-retro-studios-rfrm-forms). 24,098 shaders: 11,644 vertex, 11,734 pixel, 720 geometry (programs of `MaterialArchive.arc` with a geometry stage; they run as compute kernels) and 294 GLSLC outputs in `main`; 11,640 vertex / pixel pairs.
- The four startup programs share code areas with `MaterialArchive.arc` programs but differ in control +0x7d0, so a dump of them (`DM_NVN_SHDUMP`) matched those code areas and none of their keys; `BuildFromDelayedData` (`main+0x550d8`) passes the control sections to `nvnProgramSetShaders` exactly as they come out of the zlib stream. A probe there (`DM_PROBE=main:550d8@*x2+28`) gave the compressed bytes, which are in no RomFS file; `NMaterialArchive::ImportStartupArchive` (`main+0xaccb0`) named their source.
- Each shader build's Mac outputs: 24,098 libraries, no unresolved binding, no driver-bank read the replay cannot fill.

## Rendering

- **Resolved: the title logo and the menus' panels magenta** (no red line). The 912×512 ASTC 5×5 logo sits in a `CPU_UNCACHED | GPU_CACHED` pool (flags 0x22) whose storage holds its texels at first use (`DM_NVN_TEXINFO=912x512`: 242,374 of 376,832 bytes non-zero) and is never flushed. The profile keys `tex_flush_upload` and `tex_dirty_upload` brought the button icons but not the logo. A texture in a CPU-uncached pool now starts with its storage: [Storage of a CPU-uncached pool at first use](../../../docs/reference/nvn-objects.md#storage-of-a-cpu-uncached-pool-at-first-use).
- **Resolved: DK wrapped in a brown box while he moves** (sliding, swimming, running; fine standing still). `DM_NVN_HILITE` showed it is his fur shells: an instanced draw (`648304c43bbe3463` / `5b5ffba62bca9e0a`, three instances on the body's index buffer, after the body `659ce20d37af3304` and the blended fur `2d24f1883b9f626f` / `3637414591888cf6`). The shell vertex shader pulls each shell toward a lagged position, `c3` rows 13–15 applied to vertex attribute 6, and that stream is a buffer the frame's vertex-only draw (`6979dd3cdbd62373` / `7dcdaed991c74bfe`, 3,400 vertices, `fs=-`) fills by transform feedback; `DM_NVN_KEYDUMP=5b5ffba62bca9e0a` showed it held zeros and `2.3e24`. Its feedback buffer captures two outputs (xyz each, stride 24), a form the build did not decode, and the game draws the capture with rasterization on, which the host did not capture. With both general fixes DK's fur follows him: [Transform feedback captured by a vertex entry point](../../../docs/reference/rendering.md#transform-feedback-captured-by-a-vertex-entry-point).

## Audio and services

- **Resolved: the frame loop stopped for good after the last menu (Continue), with audio still running.** The main thread waited in `CAudioMemoryPool::Shutdown` (`main+0x6fa8c`, detaching the menu's audio pool as the first level loads) on the renderer event, which the sound thread (`main+0x971e8`, priority 29) also waits on every frame; the host's CPU renderer made that event a local one, which wakes only the higher-priority thread: [The renderer event as the service gives it: a kernel event](../../../docs/reference/audio.md#the-renderer-event-as-the-service-gives-it-a-kernel-event).
- **Resolved: SIGSEGV at `sdk+0x15b73c`** as the intro cinematic starts: `oe::GetUserInactivityDetectionTimeExtensionMode`, called from `CScreenDimmingUtil::DisableScreenDimming` (`main+0x647d0`) whenever a cinematic camera starts: [oe calls answered without an applet proxy](../../../docs/reference/services.md#oe-calls-answered-without-an-applet-proxy).

## Mac routes

The work folder is `<NSP folder>/0100C1F0051B6000-work`: `run/boot.sh <log> [secs] VAR=value…` runs `build/tfhost` (a renamed copy of the worktree's host, `host.sh` copies it), `run/ctl.sh press|snap` drives a run started with `DM_CTL=run/ctl.txt`, `ghidra.sh analyse|decompile main <offsets>` uses a 6 GB Java heap (the default 2 GB runs out on `main`).

- From a fresh save: `DM_KEYS=f3000:L+R~0.3,f3700:L+R~0.3,f4500:A,f5400:Right,f5800:A,f6400:A,f7600:A,f9100:A`: the title (L+R; the first press is sometimes too early), Game Select slot 1, Play Mode (Right: Original Mode), Controller Select, single player, the controls notice; then the intro cinematic plays.
- The intro cinematic (about 4.5 minutes) does not skip with Plus. At the plane, X has to be pressed repeatedly (about 30 presses, one every 0.2 s) before DK falls into the level; he has control about 15 s later. The work folder's `run/save_ref` is the save just after the new file was made: from it, the intro plays again.

## Found on this game

General host and tool changes first needed here; each applies to every game:

- **A heap request beyond the heap region returns ResultOutOfMemory**: [memory.md](../../../docs/reference/memory.md#a-heap-request-beyond-the-heap-region-returns-resultoutofmemory).
- **SNVN program records in RFRM forms, and the startup archive LZSS-compressed in `main`** (`rfrm_shaders`, `retro_main_forms`): [shaders.md](../../../docs/reference/shaders.md#snvn-program-records-in-retro-studios-rfrm-forms).
- **Storage of a CPU-uncached pool taken at a texture's first use** (`tex_init_fill`): [nvn-objects.md](../../../docs/reference/nvn-objects.md#storage-of-a-cpu-uncached-pool-at-first-use).
- **The renderer event as the service gives it** (`ar_open`): [audio.md](../../../docs/reference/audio.md#the-renderer-event-as-the-service-gives-it-a-kernel-event).
- **`oe::GetUserInactivityDetectionTimeExtensionMode`**: [services.md](../../../docs/reference/services.md#oe-calls-answered-without-an-applet-proxy).
- **Transform feedback buffers of several outputs (`tf_layout`, `vs_tf`), and feedback captured from draws that rasterize**: [rendering.md](../../../docs/reference/rendering.md#transform-feedback-captured-by-a-vertex-entry-point).

## Open

- The iPhone: the IPA is untested on a device, and the pipeline prewarm list `pso_manifest.tsv` is still empty (first uses of pipelines take up to about 80 ms on the Mac).
- Only the start of the first level has been played (about two minutes); later levels, the world map, bosses and the movies (`movies/*.mp4`, the SDK movie library in `subsdk0`) are untested.
- The four programs of the startup archive in `main` are found by their `CBufferedCompression` header and LZSS type 1 only; other compression types (2–13) are not read, and none were met.
