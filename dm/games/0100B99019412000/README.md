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

## On Linux (the x86-64 engine)

The first game packed for Linux and started on the x86-64 engine (`scripts/build_ipa_from_nsp.py --platform linux`), from the cartridge base v0 (1.0.0): the profile registers 1.0.1 only, so v0 is packed from a derived profile, and v0's `main` (build `37ce685a99f0bc0fb72132c40015f101`) is a build the profile does not register; `sdk` (`d5a1029a57d8cd4fdf4eacad87b0cfd0`) and `subsdk0` (`34577ede441d6dddaa1af845b7869356`) are the registered builds. Runs here are headless on lavapipe (software Vulkan, no GPU).

- **Resolved: a non-local transfer in `nnosInitialize`, then a fault at address 0** (`[RED] aot: a non-local transfer to a continuation with no native frame at sdk+0x13154c`, `SIGSEGV at address 0 ... sdk+0x12dc5c`, 12 SVCs in). `AslrSpaceManagerTemplate`'s constructor (`sdk+0x131530`) calls `AslrSpaceManagerImplByHorizon`'s (`sdk+0x1315f4`) through a PLT stub and went on with that callee's SP, which the callee's call of svcGetInfo's wrapper had left in the state block: [A non-local transfer at the return of a PLT call](../../../docs/reference/threads.md#a-non-local-transfer-at-the-return-of-a-plt-call-a-callees-sp-left-in-the-block). With every instruction in the JIT fallback (`DM_AOT_JIT_ALL=1`) the same build reached `nnMain` and the NVN device before the fix.
- **Resolved: no frame after the NVN device, the main thread polling `nn::pl::GetSharedFontLoadState`** (`main+0x778cbc`, with `svcSleepThread` between calls) after `[RED] cannot open the standard shared font`: the port had no stand-in for the standard shared font. The host now takes a TrueType font of the system's: [Shared fonts on Linux and Windows](../../../docs/reference/services.md#shared-fonts-on-linux-and-windows). In Docker the image has no fonts: the runs mount the host's `/usr/share/fonts` read-only.
- **Resolved: no frame after the second movie starts** (two 960x1080 H.264 streams into textures; the main thread waits under the movie player, `main+0xe84d18`): the port's H.264 stopgap gave no picture. It now gives black pictures at the movie's times: [H.264 movies without a decoder](../../../docs/reference/movies.md#h264-movies-without-a-decoder). The movies' video and their AAC sound are not decoded on Linux yet (Phase 1).
- **Current: the route to level 1-1** (2026-11-15; lavapipe, headless, console mode, driven through `DM_CTL` from the work folder: Plus skips the opening movie, L+R on the title once it takes input, about a minute after the start, A for file 1, A for Classic and A for the dialog on a new file, then the world intro, whose Skip wants A held for about 45 s of wall time at 1 to 2 frames a second, then level 1-1). Screenshots (`DM_CTL` `shot`, [`DM_CTL`](../../../docs/env_switches.tsv)): the title, file select, the play style screen, its dialog and the level select (with the level previews) are right; the opening movie, the title's two background bands and the other movies are black (the H.264 stopgap). Frame times: 2D menus p50 33 to 37 ms; the world intro and the level 0.6 to 1 s a frame (p50 600 ms, p95 790 ms over 300 frames in the level): lavapipe at 1920x1080.
- **Current, open: 3D scenes are black on lavapipe.** In the world intro (`W1`, Mario Toy Company) only Donkey Kong's fur shells draw, and his body and the set are black; level 1-1 is black but for a few particle sprites, over 104 draws a frame. With `render_cond` off (a copy of the profile file) the level select's previews draw and the level stays black, so render conditions are not the cause. Not root-caused: the texture and pass dumps that would show which pass goes black are the Metal backend's (`DM_CTL` `passdump`, `texdump`), and a GPU run against the Metal picture was not available here.
- **Current: the JIT fallback.** The first pack's route (title, new file, world intro) ran 8.7 % of the guest threads' CPU time in the JIT fallback (4.87 million runs, 8.36 s of 96 s), with 42 addresses of `main` and 7 of `sdk` in the misses file. The pack built from that profile and misses file (`nxaot build --profile --misses`: 31 new entries, 1 resume point, `fpcr` unserved; its AOT step 4:00 against 7:49, its `.text` 481 / 169 / 87 MB for `main` / `sdk` / `subsdk0` against 166 / 57 / 28 at tier W, as tier C's code is larger) served every one of them, and the next route ran 6.2 % (7.19 s of 117 s). Its misses are new addresses next to the old ones: other cases of the same jump tables (`sdk`'s `nn::util::VFormatString` at `sdk+0x841b0`, `nn::audio::EffectManager::UpdateEffectsInParameter` at `sdk+0x1cfb44`, `main+0x48615c`, `main+0x5f4190` and its neighbours), which discovery leaves to the dispatcher: their loops call through a PLT stub or `blr`, whose effects are the union over the module's address-taken functions, which 80 % of this `sdk`'s functions make "everything", table bases in callee-saved registers included, so the table base set before the loop is unknown at the `br` (`NXAOT_DEBUG_REGION`: "the entry address depends on more than one value"). A profile serves only the cases a route reached; the fix is in discovery (open, for the lead).
- **Current: flush-to-zero.** The three `phive::DepWorker` threads set FZ around each physics job (`main+0xc53910`: `main+0xab9010` saves the environment and sets FZ, the job, `main+0xab9030` restores it) and nothing else runs with another FPCR: on the second route 458,158 such stretches, 5.6 µs each, 2.57 s of CPU time in the JIT fallback, 2.2 % of all guest CPU time and 23 to 25 % of each worker's. The return after each stretch is a `nonlocal` miss at `main+0xa01314` (the JIT ran through the job's own return under FZ and came back to the packs' FPCR before the caller's call site): the next pack's resume point there cut the rest of the workers' JIT time from about 1.35 s to 0.27 s each.
- Other red lines on these routes: `[RED] movie:` (Phase 1), the NVN lines that name Apple-only readouts, one `[hostvk] texture with the IMAGE flag in VkFormat 141, which this device cannot use as a storage image` (lavapipe).

## Found on this game

General host changes first needed here; each applies to every game:

- **32 outputs for AAC movie decoders and one AAC frame from `GetOutputBufferSize`** (`MD_NOUT_AAC` in `hle_movie.c`): [AAC decoders offer at most 32 outputs](../../../docs/reference/movies.md#aac-decoders-offer-at-most-32-outputs), [AAC output buffer size is one frame](../../../docs/reference/movies.md#aac-output-buffer-size-is-one-frame).
- **Advanced blend modes** (`blend_st`, `adv_blend`): [Advanced blend modes as Porter-Duff factors](../../../docs/reference/rendering.md#advanced-blend-modes-as-porter-duff-factors).
- **A value filter for `DM_PROBE_POLL`** (`=<value>`, for a word that changes often before the bad store): [05, Probes without a debugger](../../../docs/agents/05-debugging.md#probes-without-a-debugger-dm_probe).
- **The capability for storage images without a format, on the Vulkan chain** (`formatless` in `dmsh_spv.py`; found packing the cartridge's 1.0.0 for Linux): [Storage images without a declared format on the Vulkan chain](../../../docs/reference/shaders.md#storage-images-without-a-declared-format-on-the-vulkan-chain).
- **The rewriter refuses a capstone it was not checked with** (`dm/tools/capstone_version.py`; the first Linux pack ran with capstone 4.0.2, whose rewrite of this game's four modules is byte for byte the 5.0.9 one, compared afterwards): [The capstone version](../../../docs/reference/rewriter.md#the-capstone-version).
- **SP stored again by a function that keeps it, after its calls** (the recompiler, `lower_calls.cpp` `FnDataflow`): [A non-local transfer at the return of a PLT call](../../../docs/reference/threads.md#a-non-local-transfer-at-the-return-of-a-plt-call-a-callees-sp-left-in-the-block).
- **A system font as the standard shared font on Linux and Windows** (`hle_sdk16.c` `font_system`): [Shared fonts on Linux and Windows](../../../docs/reference/services.md#shared-fonts-on-linux-and-windows).
- **Black pictures from the port's H.264 stopgap** (`hle_movie_avc.c`): [H.264 movies without a decoder](../../../docs/reference/movies.md#h264-movies-without-a-decoder).
- **A return under another FPCR counted apart from non-local transfers** (`aot_run.c` `REASON_FPCR_RETURN`; found analysing its flush-to-zero stretches): [A return under another FPCR counted as a non-local transfer](../../../docs/reference/threads.md#a-return-under-another-fpcr-counted-as-a-non-local-transfer).
- **Screenshots without a window on the Vulkan backend** (`DM_CTL` `shot`, `vk_backend.c` `dm_gfx_shot_at`).

## Open

- `game.tsv`'s header says the game plays on the Mac and does not mention the iPhone, where it plays.
