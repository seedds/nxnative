# Mario vs. Donkey Kong (`0100B99019412000`)

Update 1.0.1 (v65536) over the cartridge base game (an .xci), SDK 15.3.0. Nintendo's sead / agl / gsys engine (`ainb` AI scripts, `bgyml` parameters) on NVN. Modules `main`, `subsdk0` (the SDK's movie library, NintendoSDK_movie 15.3.0: `movie::Extractor` and `movie::Decoder`) and `sdk`. Plays on the iPhone (IPA 1.6 GB); 60 fps.

On the Mac: the opening movie with its sound, the title (Press L+R), file select, the play style choice, the first cutscene, level 1-1 (both colour switches, the spring, a time-out and its restart), the pause menu, leaving the level, level select and the save it shows in file select. 60 fps (frame interval p99 about 17 ms), sound, about 1 GB resident in the level and flat while it runs (0.95 GB of Metal allocations), CPU about 30% of a core; no red line on that route.

## What it needs

The provisional profile's guesses held, with `render_cond` added and two host changes in the movie decoder.

| Setting | Where | Why |
|---|---|---|
| `render_cond on` | [`game.tsv`](game.tsv) | the game skips clears under render conditions (`SetRenderEnableConditional`), as Tears of the Kingdom ([0100F2C0115B6000](../0100F2C0115B6000/README.md)) and Animal Crossing: New Horizons ([01006F8002326000](../01006F8002326000/README.md)) do; without evaluating them every level was black behind its HUD |
| `sys_resource 0x0` | `game.tsv` | the NPDM declares no system resource, so `IsVirtualAddressMemoryEnabled` is false, as on Horizon: no virtual address memory. The game sets a 3.14 GB heap (pool − used memory) |
| 4 GB layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | `game.tsv` | 256 MB in front + 256 MB alias region + 256 MB stack region + 3.25 GB heap region |
| `shader_groups` (one group per RomFS top folder, `/Model/` … `/GameParameter/`) | `game.tsv` | see [Files and shaders](#files-and-shaders) |

No `pad_layout`: the virtual gamepad starts in the default 2D layout.

## Movies

The cutscenes are MP4 files (`Movie/*.mp4`, H.264 1080p and AAC stereo) played through the SDK's movie library, their sound through an AudioOut of the game's own.

- **Resolved: a crash in the game's heap free (`sead::ExpHeap`) on a movie thread, a second into the opening movie (`Movie/Cutscene01.mp4`), at the same address every boot** (`pc=main+0x42f980`, a free-list unlink through a link of 0x3f, `addr=0x47`). The game's movie player keeps the outputs it holds in a ring of 32 entries {u32 output index, u32 flags, u64 time} per track (`main+0xe93d4c`: 0x200 bytes) and pushes with no bounds check (`main+0xe94140`). The host's audio decoder offered 64 outputs, so the 33rd outstanding one, index 63, went past the ring into the heap. The console's AAC decoder cannot offer more than 32, or the game would overflow there too: AAC decoders now have 32 outputs (`MD_NOUT_AAC`; Monster Train 2 peaked at 30 held), Vorbis keeps 64.
- How it was traced: `DM_PROBE_WATCH` on the link broke the movie's file reads (the page also held a 64 KB read buffer, and `read()` into a protected page fails with EFAULT), so the movie stopped and nothing crashed. `DM_PROBE_POLL=<address>+8=3f` (a snapshot only at the change to that value) showed the link being written right after the block was relinked; the values around it were the ring's entries.
- **Resolved: the movie then froze on its first second, its sound decoder full and the game taking no more audio.** The player opens its own AudioOut (`main+0xe92c2c`), sizes its buffers by the audio decoder's `GetOutputBufferSize`, decodes straight into them (`SetDataAndCapacity` with that size) and appends each whole, advancing its audio clock by size / 4 samples (`main+0xe92a74`). The host answered 128 KB, a bound for any Vorbis packet, so every 4 KB AAC frame played as 0.68 s and the clock crawled (a temporary log of `AppendAudioOutBuffer` showed 131,072-byte buffers). An AAC decoder now reports one frame: 1024 samples per channel, 2048 with SBR (object type 5 or 29 in the AudioSpecificConfig), which is what each output holds; Vorbis keeps the bound, its packets vary in length.

## Rendering

- **Resolved: level scenes black behind the HUD** (`[RED] SetRenderEnableConditional: the host draws anyway`): fixed by `render_cond`.
- **Resolved: advanced blend modes.** The level sets `nvnBlendStateSetAdvancedMode 8` (SRC_OVER) on one blend state. The host used to log it and blend with the state's own functions, which the hardware ignores while an advanced mode is set. The Porter-Duff modes now blend with the fixed-function factors they amount to (`blend_st` in `nvn_obj.h`, `adv_blend` in `nvn_mtl.m`).

## Files and shaders

- 3,162 RomFS files (1.26 GB), nearly all zstd (`.zs`), as Wonder's ([010015100B514000](../010015100B514000/README.md)). The profile's `romfs_marker` is `Model/World08_Textures.bfres.zs`.
- **Current:** 12,691 BNSH / BAHS programs (8,053 pixel, 4,587 vertex, 51 compute): the models' own shader archives in `Model/` (8,390), agl's in `Lib/` (3,520), effects, `Shader/`, layouts and 40 in `main`. All translate.
- Prewarm list: recorded from a fresh save over the route above (550 lines); with it nothing was built on demand on that route.

## Input

- The pad only (`main` imports no touch screen). The title asks for L+R.

## Mac routes

- The work folder's `run/boot.sh <log> [secs] VAR=…` runs `build/konghost`; `run/ctl.sh` / `run/step.sh` drive a run started with `DM_CTL`.
- The opening movie plays at every boot and Plus skips it (about 18 s in); then L+R on the title, A for file 1 (level select) and A for level 1-1, which starts about 15 s later.
- In level 1-1, `LRight~0.3`, `LRight+A~0.25` hits the blue switch and `LRight~0.55`, `LRight+A~0.15` the red one.

## Found on this game

General host changes first needed here; each applies to every game:

- **32 outputs for AAC movie decoders and one AAC frame from `GetOutputBufferSize`** (`MD_NOUT_AAC` in `hle_movie.c`): [AAC decoders offer at most 32 outputs](../../../docs/reference/movies.md#aac-decoders-offer-at-most-32-outputs), [AAC output buffer size is one frame](../../../docs/reference/movies.md#aac-output-buffer-size-is-one-frame).
- **Advanced blend modes** (`blend_st`, `adv_blend`): [Advanced blend modes as Porter-Duff factors](../../../docs/reference/rendering.md#advanced-blend-modes-as-porter-duff-factors).
- **A value filter for `DM_PROBE_POLL`** (`=<value>`, for a word that changes often before the bad store): [05, Probes without a debugger](../../../docs/agents/05-debugging.md#probes-without-a-debugger-dm_probe).

## Open

- `game.tsv`'s header says the game plays on the Mac and does not mention the iPhone, where it plays.
