# MARVEL Cosmic Invasion (`010059D020C26000`)

Update 1.0.7 (v393216) over the base game, SDK 20.5.15. Tribute Games' engine ("Paris") on **FNA**, compiled with .NET NativeAOT (NativeAOT-NX), with FNA3D's **SDL_GPU** driver on SDL's **Vulkan** backend, FMOD for sound and Epic Online Services in an NRO. Modules `main` and `sdk`; NVIDIA's Vulkan and OpenGL drivers (`subsdk0`, `subsdk1`) are not loaded. The DLC (Cyclops and The Thing) is a licence only.

Plays on the Mac from the logos through the intro movie, the title screen, the menus and the mission select into a stage, at 60 fps, with sound (0 underruns). Not tried on the iPhone.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `modules main sdk` | [`game.tsv`](game.tsv) | the host's Vulkan layer answers SDL's `vkGetInstanceProcAddr`, so the drivers stay out |
| 5 GB block: `mem_reserve 0x140000000`, `mem_alias 0x118000000` | `game.tsv` | virtual address memory and no heap: the game maps its memory in the alias region itself |
| `game_env DOTNET_GCRegionRange=60000000`, `stdalloc_cap 0xa0000000` | `game.tsv` | the .NET GC's 1.5 GB region after malloc's 2.5 GB, as Absolum's; without them the runtime fails to start and the game calls `exit(1)` at the top of `nnMain` ([memory.md](../../../docs/reference/memory.md#net-nativeaot-malloc-and-the-gc-region-range)) |
| `vk_capture.tsv` | profile | the keys of the SPIR-V modules MOJOSHADER makes at run time, which the NSP script captures; see [Shaders](#shaders) |
| Offline Epic Online Services | `dm/host/hle_eos.c` | the game uses its EOS platform without checking it; see [Online](#online) |

## Shaders

- FNA's MOJOSHADER translates the effects' D3D9 bytecode (`Content/Shaders/*.fxb`, 14 effects, and FNA's own) to SPIR-V when an effect is first used; the RomFS has no SPIR-V file, so the NSP script boots the game on the Mac with `DM_VK_CAPTURE` and `vk_shaders.py` translates the capture. 5 modules from boot into a stage: 2 at the logos, 2 in the intro movie, 1 at the title screen behind "press any button" (the capture boot presses A after 15 s without a new module). A stage made none of its own.
- `main` has no `vk*` import besides `vkGetInstanceProcAddr`, and SDL's EGL support names `glShaderSource`; `profile_guess.py` tells the game by FNA3D's SDL_GPU driver (`SDL_GPU Driver`) and MOJOSHADER's bare `#version` lines are not GLSL sources ([rendering.md](../../../docs/reference/rendering.md#fna3d-on-sdl_gpu-a-vulkan-game-with-no-vk-imports)). MOJOSHADER names its entry points `ShaderFunction<n>`.
- SDL_GPU sets blend constants and the stencil reference in every render pass, and FNA3D presents its backbuffer with `vkCmdBlitImage` ([rendering.md](../../../docs/reference/rendering.md#blend-constants-stencil-reference-and-depth-bias-as-dynamic-state)).
- If a later stage, character or effect draws nothing (an invisible sprite, a magenta box) and the log has `[vk] [RED] shader … is not in the library`, capture it: run the game with `DM_VK_CAPTURE=<dir>`, add the new keys to `vk_capture.tsv` and put the files in the build's `extract/v393216/vk_capture`.

## Online

The host never loads the EOS module ([services.md](../../../docs/reference/services.md#online-sdks-in-nros-are-never-loaded)). `EpicHelper.Init` uses the platform without checking it: with the `UNIMPL` stub's null platform it threw a `NullReferenceException` at boot. The imports now answer as the SDK does on a console without a network ([services.md](../../../docs/reference/services.md#epic-online-services-offline)); the game plays offline with local saves (`save:/GameSave.sav`).

## Not checked

- Later stages, every character, the cutscenes after the intro, co-op.
- The iPhone.
