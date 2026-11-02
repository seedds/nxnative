# Hogwarts Legacy (`0100F7E00C70E000`)

Update 1.0.5 (v327680) over the base game; SDK 17.5.4. Avalanche Software (Warner Bros. Games), Unreal Engine 4 (version 11 paks with IoStore containers and shader code archives), NVN, Wwise sound, Bink 2 movies. Modules `main` (120.6 MB of code), `subsdk0` (the movie library, which `main` imports from) and `sdk`. The five add-on content NSPs are not packed yet (they sit in a subfolder the NSP script does not read).

Status: brought up on the Mac (an M1 Max, 64 GB) from the NSP script's provisional profile: the opening movie, the menus, character creation and the first gameplay play with the game's own 30 fps cap, with no red line (a 50-second gameplay run after the audio fix, 3.2 GB resident after 600 frames). Four host changes were needed: an `nsd` answer at boot, two shader binding forms (one a chain change) and a private libopus for the SDK's software Opus decoder. Not tried on an iPhone (see [Open](#open)).

## What it needs

| Setting | Where | Why |
|---|---|---|
| `nn::oe::GetServerEnvironmentType` and `nn::nsd::GetApplicationServerEnvironmentType` answered with 1 (retail `lp1`) | `hle_offline.c` | the SDK's call is an IPC to `nsd:a`; without it the game aborted at boot. See [Services](#services) |
| Texture handles of type 1 whose sampler word is in c2's separate sampler table decoded as a uniform-buffer texture plus a separate sampler | `dmsh_build.py` `decode_ubo_tex` (a chain change) | 46,782 of the 65,047 shaders bind their material textures this way; undecoded, gameplay drew black. See [Shaders](#shaders) |
| A separate sampler binding may be an array | `nvn_refl.m`, `nvn_mtl.m` | the volumetric lightmap shader pairs an array of 32 3D texture handles with an array of 32 samplers; the host stopped at its first load. See [Shaders](#shaders) |
| The sdk's libopus bound to its own definitions | `loader.c` `bind_private_opus` | `main` bundles an older float libopus and exports some of its symbols; the SDK's software Opus decoder, standing in for the hwopus service, read them and every voice line came out garbled. See [Audio](#audio) |
| `depth_mode zero` | [`game.tsv`](game.tsv) | the game sets near = 0; without the key the bundled pipeline archive was built for the GL convention and rejected at start |
| 64 GB alias region: `mem_reserve 0x1028000000`, `mem_alias 0x1000000000` | `game.tsv` (the provisional values) | Unreal's `nninitStartup` reserves a 62 GB engine region and the allocator maps 37 GB of it; the rewriter's allocator cut does not recognize this build. Address space only: see [Memory](#memory) |

No `fps_target` key: the game runs at its own 30 (it sets present interval 2 itself).

## Services

- **Resolved: an `nn::diag` abort at boot (Result `0xe8d`, under `sdk+0x3a1748`), after `[RED] sf: the game connects to service nsd:a itself` and the same for `nsd:u`.** The game asks `nn::oe::GetServerEnvironmentType()` early in `nnMain` and decodes the answer with a 5-entry table and a switch (`main+0x42e6c20` / `+0x42e6c50`: 0 `dd1`, 1 `lp1`, 2 `sd1`, 3 `sp1`, 4 `dp1`, anything above 4 "unknown"). In SDK 17 that call is a tail call into `nn::nsd::GetApplicationServerEnvironmentType()`, whose IPC goes to `nsd:a`; the host served no `nsd` and the SDK aborted. Both are answered with 1, retail `lp1`, the environment `GetEnvironmentIdentifier` already reported ([Server environment type asked of nsd](../../../docs/reference/services.md#server-environment-type-asked-of-nsd)).
- Nothing else: the heavy online imports the survey flagged (`socket`, `ssl`, `ec`, `friends`, `web`, `ldn`, `account`) are all met by the existing offline answers; the run logged no red line after this.

## Shaders

- 65,047 shaders in the IoStore containers' shader code archives (`ShaderArchive-Global-…` and `ShaderArchive-…`, the profile's `global` and `game` groups), each one Oodle-compressed and decoded on the Mac by the game's own Oodle, as Octopath Traveler 0's ([Unreal Engine 5 shader code archives (Oodle)](../../../docs/reference/shaders.md#unreal-engine-5-shader-code-archives-oodle)). The first build compiled them in 3 h 02 min on an M1 Max; the chain change below rebuilt the libraries in 38 min with the Metal compiles cached. `cb_extent.tsv` covers 64,698 of them; `c1_full.tsv` leaves 242 not proven bounded.
- **Resolved: gameplay drew black, with faint outlines, while text and menus were right** (the shader build's `shaders with a binding whose slot was not decoded 46782`). The game keeps its material textures' 64-bit NVN handles in its own uniform buffers (8 bytes apart) and binds a few samplers with `BindSeparateSampler`; the translator names such a texture `fp_t_cb196612_10568002` (handle type 1: c[4] word 2 for the texture, c2 word `0x15A` for the sampler, separate sampler slot 0), a form `decode_ubo_tex` did not take, since it expected both words in game uniform buffers. With both masks from constant buffers the translator keeps the OR's order, so about 12,000 of these names carry the c2 word in the texture field (`fp_t_cb589826_1002015A`). `decode_ubo_tex` now decodes the form whichever field the c2 word is in (the texture from the uniform buffer, `tex_ubo`; the sampler as the separate sampler slot, `samp`), a chain change (the libraries of this bring-up were built as `c29a8a4dc6d1` on the Xcode 27.1 chain of October 31): [Texture handles in a game uniform buffer with the sampler word in c2](../../../docs/reference/shaders.md#texture-handles-in-a-game-uniform-buffer-with-the-sampler-word-in-c2). One binding stays undecoded, a storage buffer `cp_s4` of one compute shader.
- **Resolved: the host stopped at start with `unexpected argument buffer layout encodedLength=584 maxAid=41`**, right after the chain change: the volumetric lightmap shader `96e024d2b42c1cb1` samples its bricks through `array<texture3d<float>, 32> cp_t3d_cb196612_10568000` with `array<sampler, 32> …Smplr`, and the new decode gave the sampler array the plain separate-sampler kind, which the host's reflection reader took as one sampler. It now keeps the array length, and the replay fills every element with the one sampler of that slot ([An array of one separate sampler](../../../docs/reference/shaders.md#an-array-of-one-separate-sampler)).
- Translator limits reported by the build, not followed up: 174 shaders use the `Sam` / `Ram` instructions (not implemented), 105 to 166 shaders each have a bindless access whose handle source the translator does not find (`imageBuffer`, `textureCube`, `texture3D`, `texture2D`, `textureBuffer`), and 5 a global memory load without a storage buffer. None showed as a red line in play so far; see [Open](#open).
- Depth mode near = 0 (`nvnDeviceSetDepthMode`), hence `depth_mode zero`.

## Audio

- Wwise mixes through the SDK's AudioRenderer, run on the CPU by the host: 48 kHz, 240 samples per frame, 32 voices, 6 mix buffers, one device sink, to a stereo AudioOut the game polls. SDK 17.5.4's `AudioRenderSystem` has the SDK 16 layout (`+0x60` in `AudioRendererImpl`, update lock at `+0x1f8`).
- Its voices, music and cutscene tracks are Opus, decoded through the hardware decoder's C interface (`InitializeHardwareOpusDecoder` / `DecodeOpusInterleavedWithHardware`): 48 kHz stereo decoders for the music and the movies' tracks, 24 kHz mono ones for the voices. The host forwards them to the SDK's software `OpusDecoder` ([Hardware Opus decoders built on the SDK's software decoders](../../../docs/reference/audio.md#hardware-opus-decoders-built-on-the-sdks-software-decoders)).
- **Resolved: every voice line muffled and garbled, "deep inside a tunnel", at normal speed and in sync with the subtitles, while most sound effects were right.** Not the output path (the AudioOut stream dumped with `DM_AUDIO_DUMP` was already muffled), not the output device name (`DM_AUDIO_DEVICE=AudioTvOutput` changed nothing), not the game's audio configuration (Home Cinema, TV and Headphones sound the same) and not the 20–60 Hz rumble track in the mix (86% of the energy; filtered out, the voices stayed bad): the decoders' own output, dumped with `DM_OPUS_DUMP`, was already wrong. `main` bundles its own libopus (an older, floating-point build) and exports 112 symbols the `sdk`'s libopus also defines; by load order (Horizon's rule, which the loader copies) 85 of the sdk's relocations bound to `main`'s: `eMeans` (25 floats against the sdk's 25 signed chars), `silk_NLSF_CB_NB_MB` / `silk_NLSF_CB_WB` (64-byte structs against 72), `_celt_lpc` / `_celt_autocorr` (float against fixed point). On the console the same binding happens and does not matter: the hwopus service decodes in its own process. The loader now binds the libopus symbols of the module holding the SDK's software decoder to its own definitions (69 relocations here): [Two libopus copies: the SDK's software decoder read the game's](../../../docs/reference/audio.md#two-libopus-copies-the-sdks-software-decoder-read-the-games).
- `subsdk0`, the movie library, carries a third libopus (its `silk_*` tables and `downmix_int` bind to `main`'s too); it is not the decoder the host runs, and the movies' sound is fine.

## Memory

- Unreal's `nninitStartup` reserves a 62 GB engine region; the rewriter's engine-wide gate on the store of its size applies (`main+0x19ed298`, `dm/tools/ue_region.py`) and the host gives it what the 64 GB alias region has room for. The allocator then maps 37 GB of it (`alias region mapped up to +37.25 GB`): the rewriter's FMallocBinned3 cut (`dm/tools/ue_malloc.py`) does not recognize this build (`main.cuts.tsv` is empty, no `[mem] Unreal heap` line), so the game runs its own allocator in a 64 GB alias region. Address space only on the Mac: 2.55 GB resident and 1.03 GB of GPU allocations at frame 300, 3.17 GB and 1.28 GB at frame 600.
- The 16 MB of system resource the NPDM declares is `sys_resource`.

## Files

- RomFS: 200 files, 13.21 GB. `Phoenix/Content/Paks/`: 12 version 11 paks with 13 IoStore containers (`.ucas` / `.utoc`; the profile's `romfs_marker` is `pakchunk10-Switch.ucas`), 140 Bink 2 movies (`.bk2`), fonts and PNGs. The NSP script reads the paks and containers with `uepak.py` ([IoStore patch containers keep their shader groups in the base container](../../../docs/reference/shaders.md#iostore-patch-containers-keep-their-shader-groups-in-the-base-container)).
- The update is 10.24 GB over a 6.83 GB base; the Mac app is 18.8 GB with the 1.2 GB pipeline archive.

## Mac routes

- No scripted route yet. From a fresh save: the opening movie, the main menu, character creation, the first gameplay scenes.

## Found on this game

General host changes first needed here; each applies to every game:

- **The server environment type without an `nsd` service** (`hle_offline.c`): [Server environment type asked of nsd](../../../docs/reference/services.md#server-environment-type-asked-of-nsd).
- **Type 1 texture handles with the sampler word in c2** (`dmsh_build.py`, a chain change) and **an array of one separate sampler** (`nvn_refl.m`, `nvn_mtl.m`): [Texture handles in a game uniform buffer with the sampler word in c2](../../../docs/reference/shaders.md#texture-handles-in-a-game-uniform-buffer-with-the-sampler-word-in-c2), [An array of one separate sampler](../../../docs/reference/shaders.md#an-array-of-one-separate-sampler).
- **A private libopus for the SDK's software Opus decoder** (`loader.c` `bind_private_opus`), with the diagnostics that found it (`DM_OPUS_DUMP` in `hle_codec.c`, `DM_AUDIO_DEVICE` in `hle_audren.c`): [Two libopus copies: the SDK's software decoder read the game's](../../../docs/reference/audio.md#two-libopus-copies-the-sdks-software-decoder-read-the-games).

## Open

- **Stutters when effects first appear**: pipelines built at first sight. `pso_manifest.tsv` is still the provisional empty list; the Mac keeps a user list (`pso_user.tsv`, 1,708 lines after two runs), which the second start prepared in 0.15 s. Recording a seed list (`DM_PSO_MANIFEST=off DM_PSO_RECORD`, [Pipelines and caches](../../../docs/reference/shaders.md#pipelines-and-caches)) over a route through the first hours would remove most of them.
- **An iPhone**: the allocator cut does not recognize this build, so the guest block stays at 64 GB of address space; an iPhone has about 5 GB. `dm/tools/ue_malloc.py` would need this build's FMallocBinned3 fingerprint (or whichever allocator it is), then a smaller `mem_alias` as Octopath Traveler 0's.
- The translator's reported limits in [Shaders](#shaders) (`Sam` / `Ram`, bindless handle sources) have not been matched to anything visible yet; the image quality is the Switch version's.
- The add-on content NSPs are not packed (move them from their subfolder beside the base and update NSPs and rebuild).
- `shader_note` still reads "(provisional profile)": the key is part of the shader library inputs digest, and changing it rebuilds the libraries.
- Not run with `DM_SYMCHECK` or `DM_LOADHASH`; no benchmark route.
