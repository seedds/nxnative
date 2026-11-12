# Minecraft Dungeons II (`0100A7C01B792000`)

Update 1.1.3.0 (v131072) over the base game v65536, SDK 20.5.15. Mojang's Unreal Engine 5 game on the engine's desktop renderer (`SF_GLSL_SWITCH` shaders, not the mobile one), NVN, Wwise sound, Havok, Xbox network services (offline here), H.264 / AAC logo movies through the SDK's movie library. Modules `main`, `subsdk0` (the movie library) and `sdk`. The base game is sparse: its Program and one HtmlDocument NCA leave out data its update carries, so only the update is packed ([Sparse base games](../../../docs/reference/packing.md#sparse-base-games)).

Status: on the Mac (2026-11-06, M4 Pro), from a fresh save: the Xbox Game Studios logo movie, the title, the first-run accessibility, brightness and terms screens, the online / offline choice (online is unavailable, as the host is offline; Play Offline), the main menu, Entering World, the in-engine opening cutscene (skipped with B) and first control in Escape from Squid Coast, walking and attacking. 30 fps (the game's own rate) at 1920x1080 with no `[RED]` line, about 2.6 GB resident at frame 4,200. 60 fps, and up to 120 on the Mac, since 2026-11-08. The IPA is not built.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `tex_dyn_slots on` | [`game.tsv`](game.tsv) | 2 particle vertex shaders pick a buffer texture at run time; see [Shaders](#shaders) |
| 64 GB alias region: `mem_reserve 0x1028000000`, `mem_alias 0x1000000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | `game.tsv` | the provisional Mac layout for Unreal: the rewriter's engine-wide rules cut FMallocBinned3's constructor (`dm/tools/ue_malloc.py`) and give the engine region what the alias region has room for (`dm/tools/ue_region.py`), 62 GB here. An iPhone needs a smaller block, as [Octopath Traveler 0](../01005270232F2000/README.md#memory)'s; not measured yet |
| `sys_resource 0x1400000` | `game.tsv` | the NPDM declares 20 MB of system resource |
| `shader_groups ShaderArchive-Global-=global ShaderArchive-=game`, `depth_mode zero` | `game.tsv` | the two shader code archives; the game sets near = 0 depth |

No host or rewriter rule is specific to this game.

## Frame rate

- **Resolved: the game stopped presenting right after the logo movie** (game thread in `FlushRenderingCommands`, `main+0x14fd670`, waking each millisecond in `FFrameEndSync::Sync`, `main+0x15015f0`; render, RHI, frame-flip and interrupt threads idle). The RHI's "Viewport Thread" (`main+0x142afb0`) learns of each flip from `nn::vi::ListFrameHistory` after each vsync (`main+0x142b034`, `+0x142b094`), and the host answered with no entries, so the swap-chain fences never completed: [Layer frame history](../../../docs/reference/services.md#layer-frame-history). Found by the thread dumps at one-second steps (`DM_CTL` `dump`), the event's never-set trigger word, and reading the waiting functions (no symbols; this part of `main` has no unwind entries).
- **Current: 60 fps, up to 120 on the Mac** (`fps_target 60`, `fps_max 120`). The game's config sets `rhi.SyncInterval` 2 and `t.MaxFPS` 0 (no limit), and its `bUseFixedFrameRate` is off, so its engine steps by the time that really passed and the present interval the host writes is all it needs ([Unreal's frame limit and present interval, found by name](../../../docs/reference/frame-rate.md#unreals-frame-limit-and-present-interval-found-by-name); `rhi.SyncInterval` at `main+0xac96a10`). Measured on an M4 Pro (2026-11-08, chain `89f44f9b6bb0`, headless, a copy of the same save past the first-run screens, title → Play → cutscene skipped → right 2 s, down 1.5 s): frame time p50 33.4 ms at 30, 16.7 at 60, 8.3 at 120 (118.5 fps), no `[RED]` line, and the walk ends at the same spot at all three. At 120 the title and loads drop to 60 for a moment (a logo movie holds 60). The user played Squid Coast at 120 in a window and judged the speed right.
- **Open:** the engine's `UEngine::FixedFrameRate` reflection entry has a layout `dm/tools/ue_fixed_rate.py` does not read (the setter and getter at +32 / +40, a 16-bit array size at +48, the offset at +52: `+0xc38`, its flag bit 0x40 of `+0xc34`, `GEngine` at `main+0xae26e30`), so it finds no fixed frame rate here. It makes no difference to this game, whose flag is off; a game on this engine version with the flag on would stay at its fixed rate.

## Shaders

- 16,751 shaders in two shader code archives of the IoStore containers (`ShaderArchive-Dungeons-SF_GLSL_SWITCH-SP_GLSL_SWITCH.ushaderbytecode` 13,318, `ShaderArchive-Global-…` 3,433), decoded by the NSP script; 15,826 unique after the split, all translated for the Mac. The translator deletes one bindless access in `ccd515f16b0f63a6` (a `texture2DArray`, which reads 0); no run has used that shader.
- **Resolved: the title's campfire flame was black** (`[RED] shader 210d8abb3b9996c5_b reflection conversion failed: a texture handle the translator could not trace to one slot`, the same for `d1c8e612ec7462b6`). The two particle vertex shaders read a buffer texture whose handle they pick at run time: a c2 texture-table word ORed with a sampler-table word. Their only c2 reads are texture table words 0x170-0x188 and sampler table words 0x568 / 0x578, so `tex_dyn_slots on` is right here, as for Octopath Traveler 0's and FINAL FANTASY RESONANCE's.

## On the Mac

- After the NSP script's `--platform mac` build, its printed command runs the game; with `DM_FOCUS_KEEP=1 DM_WINDOW_TOP=1` a host started from a background shell keeps presenting.
- From a fresh save: A at the title, X on the accessibility and brightness screens, X to agree to the terms, Right and A for Play Offline, A on Play, then B held to skip the opening cutscene.

## Found on this game

- [Unreal's frame limit and present interval, found by name](../../../docs/reference/frame-rate.md#unreals-frame-limit-and-present-interval-found-by-name): 60 and 120 by `rhi.SyncInterval` alone
- [Layer frame history](../../../docs/reference/services.md#layer-frame-history)
- [The offline pages of a sparse base game stop the build as a damaged NSP](../../../docs/reference/packing.md#the-offline-pages-of-a-sparse-base-game-stop-the-build-as-a-damaged-nsp)
- [The survey's build ID lookup stops on the base game alone with a hash error](../../../docs/reference/packing.md#the-surveys-build-id-lookup-stops-on-the-base-game-alone-with-a-hash-error)
- [A shader build leaves a file per shader output while it runs or after a stop](../../../docs/reference/packing.md#a-shader-build-leaves-a-file-per-shader-output-while-it-runs-or-after-a-stop): measured here, its 61,655 library files the same under the new chain
- [Writes beside a packaged texture in a shared host page](../../../docs/reference/nvn-objects.md#writes-beside-a-packaged-texture-in-a-shared-host-page): after GPU stores into packaged textures' storage, 17 small textures were uploaded again by writes beside them, until the stores wrote their edge bytes too

## Open

- iPhone: no IPA built; the guest block needs measuring for a phone ([What it needs](#what-it-needs)).
- The prewarm list (`pso_manifest.tsv`) is empty.
- Only the start of the first level was played: later levels, couch co-op (more controllers), saves across boots and the other languages are untested.
