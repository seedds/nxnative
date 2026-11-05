# Star Fox (`0400E88025B1A000`)

A Switch 2 game: update v131072 over the base game, SDK 21.4.4 built for the Switch 2 (`Ounce`), NVN2 with DLSS. A custom engine (its own prefixes: `vfs`, `vrender`, `vjob`, `vscript`, `vspirv`, `veffects`, `vphysics`; project name `eclipse`, built on Windows), with Wwise 2024.1 for audio, Bullet physics and NPLN for online play. Modules `main`, `sdk`, `subsdk0` (the SDK's movie module), `subsdk3` (`nn::camera`); `subsdk1` (the C library) and `subsdk2` (`NintendoSDK_DLSS`) are shipped but `main` imports nothing from them. **Plays on the Mac** (M5 Max): the title (`Nintendo presents STAR FOX`, `A Start`), the prologue cinematic (the Lylat System, James McCloud's Arwing, the cockpit with General Pepper's briefing and subtitles) and the tutorial flight after it (`map_tutorial`: Peppy's lessons over the holographic city), 7 minutes with a controller at 60 fps (frame time p50 16.67 ms, p95 17.1 ms; a few dropped frames at scene changes and loads), with sound, the characters' fur, the Opus voices, the camera, GameChat and the network layers answered offline. iOS is not tested.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `mem_alias 0x400000000`, `mem_reserve 0x550000000`, `mem_stack 0x10000000`, `app_pool 0x240000000` | [`game.tsv`](game.tsv) | Fire Emblem Fortune's Weave's split as a starting point (the Switch 2's 9 GB, a DLSS game); not measured yet |
| `sys_resource 0x1000000` | `game.tsv` | the NPDM declares 16 MB, so virtual address memory is on; `main` never sets a heap (`profile_guess.py`) |
| `modules main subsdk0 subsdk3 sdk` | `game.tsv` | `main` imports 17 movie functions from `subsdk0` and 19 `nn::camera` functions from `subsdk3`, nothing from `subsdk1` or `subsdk2` |
| `romfs_marker data.zip` | `game.tsv` | the RomFS has two files, and this is the game |
| `zip_store .zip:.metallib` | `game.tsv` | the archive's members are LZ4-compressed already |
| `shader_groups /data.zip=shader` | `game.tsv` | every shader is a member of the archive |

No `render_cond`: not tried.

What the host needed for it, all general (the reference entries are linked under *Found on this game*): SDK 21's socket initialization against host bsd sessions and the C library's `poll` routed to the host; SDK 21's hardware Opus decoder manager on the SDK's software decoder; SDK 21's output streams on the host's AudioOut; 33 GameChat calls answered as "no chat"; the camera module's commands answered as a console with no camera plugged in; `nvnDlssNetworkResize`; the translator's TXD through a uniform-register handle (0xf6c, also fixed on main meanwhile).

## Files

- Packed from a NUP (`scripts/pack_nup.py`): a base package (v0, 13.4 GB) and an update package (v131072, 14.5 GB), each with its own ExeFS and RomFS. The NUP carries the base game's icon but no NACP, so the name comes from the profile and the display version is the placeholder `v131072`.
- The RomFS is two files: `data.zip` (14.5 GB) and `veclipse_build.cvar` (a JSON line: `system.is_shipping_build: true`). `main` opens `rom:/data.zip` (and `rom:/headless.zip`, which is not there) through its own virtual file system (`vfs.zip.c`). The archive is a zip behind an 18 MB `VZIP` preamble (the engine's own index); zipfile reads it through the central directory. Its 114,154 members are named by a 16-digit hash with no folders or extensions, and every one is listed as stored (method 0), but a member whose size on disk differs from its file size is a chain of 64 KB chunks, each a u32 length and an LZ4 block of that length compressed against the previous chunk as its dictionary (a chunk that would not shrink is stored raw); the directory's CRC is the decoded data's. 92,069 members are chained, 22,085 (the Wwise `RIFF` sound files and `BKHD` banks) stored plain; all 33.4 GB decode and check ([`viper_zip_member`](../../gfx/tools/shader_split.py)). What is inside: 24,802 JSON documents (the engine's assets), 24,650 `VIPR` containers, 23,590 index or mesh buffers, 21,918 Wwise sounds, 163 Wwise banks, 2,828 shader binaries, and a few thousand raw texture mips.
- `main` is 192 MB of code with 1,036 imports, of which the host and the other modules supply all but two (gRPC thread-local initialisers, `_ZTH…`). No shader is in any code module.

## Shaders

2,856 in [`shader_map.tsv`](shader_map.tsv): 1,483 vertex, 1,305 pixel, 68 compute; 2,828 one per archive member, no task or mesh shaders, and the rest from the two NDSR upscaler networks (`ounce-nvn2-upper_left_ndsr.bin`, 40 shaders each, 28 of them new) embedded in two `VIPR` resource containers. Each member is the engine's shader binary (`_vrender_shader_binary_t`): the GLSLC output magic `0x19866891`, the code length, the control length, a zero and a 64-bit hash (0x18 bytes), then the Ampere code area (its 0x80-byte SPH first) and the NVN2 control section (version 0xc9; 0xe0 or 0xf8 bytes, up to 0x9f68 for 172 compute and vertex shaders that carry more). `shader_split.py` reads them as single-stage programs (`viper_shader`, `#VIPER`), paired at draw time as GLSLC outputs are (`separable_pairs`: 1,480 cover pairs). The engine also carries a SPIR-V interpreter (`vspirv`, ISPC) for its scripts; those SPIR-V modules (11 in the archive) are not GPU shaders. The DLSS and NDSR networks are the SDK's (`subsdk2`, `SMAA_For_NDSR`), which the host answers on MetalFX ([NVN2's DLSS on MetalFX](../../../docs/reference/rendering.md#nvn2s-dlss-on-metalfx)).

## Driving it on the Mac

`python3 scripts/build_ipa_from_nsp.py <folder>/StarFox\ [0400E88025B1A000].nup --platform mac` builds the Mac shaders and the host and prints the run command; `--platform mac-app` packs the Mac app. `DM_KEYS=12:A` starts the game from the title; the prologue cinematic follows. `DM_NVN_SHOT=600,1500:<dir>/f_%d.png` saves frames (the picture is the top-left 1920x1080 of the 4K swapchain).

## Found on this game

- [SDK 21 socket initialization and overloads](../../../docs/reference/services.md#sdk-21-socket-initialization-and-overloads) and [Socket `Poll` and `Select` with nothing to wait on](../../../docs/reference/services.md#socket-poll-and-select-with-nothing-to-wait-on): the game's HTTP thread aborted on a failed `poll`.
- [SDK 21's hardware Opus decoder manager](../../../docs/reference/audio.md#sdk-21s-hardware-opus-decoder-manager) and [SDK 21 output streams on the host's AudioOut](../../../docs/reference/audio.md#sdk-21-output-streams-on-the-hosts-audioout): its voice and music.
- [GameChat queries from the title screen (SDK 21)](../../../docs/reference/services.md#gamechat-queries-from-the-title-screen-sdk-21): a null read in `nn::chat::GetChattingUid`.
- [The camera commands of SDK 21's own camera module](../../../docs/reference/services.md#the-camera-commands-of-sdk-21s-own-camera-module): the game's own camera module, a console with no camera plugged in.
- [The DLSS network resized](../../../docs/reference/rendering.md#the-dlss-network-resized): `nvnDlssNetworkResize`.
- [TXD through a uniform register kept no handle](../../../docs/reference/shaders.md#txd-through-a-uniform-register-kept-no-handle): 281 pixel shaders (the fur shells) read 0, the same fix taken in from PR 1261 meanwhile.
- [A handle pair ORed sampler word first](../../../docs/reference/shaders.md#a-handle-pair-ored-sampler-word-first): 62 pixel shaders.

## Open

- Once in a 7-minute gameplay session, a GPU hang in one submission (`segment command buffer execution error … kIOGPUCommandBufferCallbackErrorHang`, at 178 s, in the prologue); the game went on. Not reproduced.
- The game opens a new output stream each time the window regains focus and never closes the old one (the console would keep them too): each orphaned output plays on empty. Harmless so far.
- One compute shader (`1002069a331ceded`) fails at load (`reflection conversion failed: B binding cp_t_cb196610_10280074Smplr has no resolved slot number`): its sampler words lie at c[2] 0x1028 and 0x1e0, past the separate sampler table the host decodes (NVN2 compute's own handle layout, mapped from c[0]). What it computes has not been seen missing.
- 3 shaders do not translate: Ampere opcodes 0x925 (2 compute shaders) and 0x21b (1). (On the chain before main's surface-atomic and TLD4 changes of 2026-11-04 it was 13: SURED form 0310e000 and 0xf63 too.)
- The memory split is Fortune's Weave's, not measured (the address region check runs only for a provisional profile; the game mapped up to alias +7.0 GB of 16 at the title).
- `render_cond` not tried. Mario Kart World 1.8.0 is not re-tested with the camera answers changed for this game.
- The game's wake-up eventfd fails (the host refuses `nn::socket::EventFd`), so its HTTP thread polls without a wake-up; harmless offline.
