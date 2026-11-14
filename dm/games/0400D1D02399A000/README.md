# The Duskbloods network test (`0400D1D02399A000`)

A Switch 2 game: THE DUSKBLOODS NETWORK TEST VER, base v0 only, SDK 20.5.19 built for the Switch 2 (`Ounce`). FromSoftware's engine (Dantelion2), NVN2. It boots, plays the opening movie, the title, the hub and the world draw, and with online mode it logs in to local stand-in servers.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `app_pool 0x238000000` (just under 9 GiB) | [`game.tsv`](game.tsv) | The engine plans its memory from QueryMemoryInfo (`main+0x1d7ac40`): 9 GiB or more takes plan row 4 of its table (`main+0x5a63340`), whose root block came out 20 MB short of the first four heaps (Dantelion2 Panic, CSMemory.cpp 3771); row 3 fits |
| `core_mask 0x3f` | `game.tsv` | `main.npdm`'s ThreadInfo allows cores 0-5 and the NPLN threads run on 3-4; with the Switch's 0-2, gRPC's per-CPU call counters were written past their end from core 4 and an NPLN task crashed after every request (`main+0x39cf10`) |
| `tex_dyn_slots on` | `game.tsv` | Compute shaders that pick an image from c2's table at run time bind it as a 32-entry slot array (`nvn_refl.m` / `nvn_mtl.m`) |
| `depth_mode zero` | `game.tsv` | The game sets near = 0 at boot; the pipeline archive built with the app must be made for it |
| GetGraphicsMemoryInfo with no pool | `dm/gfx/hle_nv.c` | `nv::InitializeGraphics(nullptr, 0)` is the Switch 2's remote-memory boot; a total of 0 crashed on a null pointer (+0x58) |

`subsdk0` and `subsdk1` (C library) are not loaded.

## Files

- A dump folder: `base/exefs` (`main`, `sdk`, `subsdk0`, `subsdk1`) and `base/romfs`. No NACP, so the app has no icon.

## Shaders

- [`shader_map.tsv`](shader_map.tsv) from `romfs/shader`: the eight `*.shaderbnd.dcx` archives (DCX zlib, BND4 of CGSB) and `title.shaderbnd.dcx`, and `shaderbdle.shaderbdlebnd.dcx`, the per-material bundles (DCX Oodle Kraken, 492 BND4 archives of CGSB): the world's shaders. Kraken needs an `ooz` binary (`DM_OOZ`, [`docs/env_switches.tsv`](../../../docs/env_switches.tsv)); without the bundles every world draw misses its shaders and only the HUD shows.
- The Bink 2 opening movie's shaders are NVN2 control sections embedded in `main` (`shader_split.py`, `nvn2_embedded`).
- Image loads and stores through a handle in a register (SULD / SUST 0x998, 0x99c) translate; the fog volume (`GXFogVolume_*`) loads its 3D image's handle from a storage buffer, so it indexes an image pool by the image's ID. Without it the world drew black behind the HUD.
- [`pso_manifest.tsv`](pso_manifest.tsv): a prewarm seed list recorded in play.
- Open: tessellated draws (primitive type 14) are skipped.

## Online

- Opt-in, for local stand-in servers only: [playDB-CNT](https://github.com/sikoGD/playDB-CNT)'s `npln_stub.py` (gRPC, port 18444) behind its `frontdoor.py` (TLS, port 18443). Run with `DM_ONLINE=1`, `DM_ONLINE_CA=<the servers' ca.der>` and `DM_ONLINE_BCAT=<folder>` holding `Regulation/regulation_nt_1_0.bin` (a copy of the romfs `regulation_nt.bin`; the game's "calibrations"). The game's names resolve to 127.0.0.1 and port 443 goes to the frontdoor (`DM_ONLINE_HOSTS`, `DM_ONLINE_SERVER`).
- NPLN is gRPC with its own TLS over the socket interface (`nnsocketSendMsg` / `RecvMsg`, poll with an event descriptor); FromSoftware's own server (`phx-*.fromsoftware-game.net`) is libcurl with OpenSSL in `main`, which polls its connecting socket with a zero timeout. Horizon's socket errno numbers and `O_NONBLOCK` are Linux's, not the Mac's (as #2589 found).
