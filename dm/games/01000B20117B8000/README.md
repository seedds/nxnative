# Re:ZERO -Starting Life in Another World- The Prophecy of the Throne (`01000B20117B8000`)

Base game 1.0.0 (v0, no update), SDK 9.5.0. The game's own engine on **The Forge** renderer (the Vulkan instance names the application `ReZERO-TPT` and the engine `TheForge`, API 1.1), drawing with **Vulkan**, not NVN: `main` imports one GPU function, `vkGetInstanceProcAddr`, and loads every other Vulkan function through it. The host implements them on Metal (`dm/gfx/vk/hle_vk.m`). Modules `main`, `subsdk2` (NintendoSDK_movie) and `sdk`; `subsdk0` (NintendoSDK_Vulkan) and `subsdk1` (NintendoSDK_OpenGL) are never loaded. Brought up on the Mac only; nothing here was run on an iPhone or iPad.

Build IDs: `main` `1ef665d78f3de35c86c26f7b91d92784`, `subsdk2` `7a5e537af42c5847f02410d81096ff0e`, `sdk` `f93ce78898c2583ae08d87d3f2c2e4d6`.

**It needs four open pull requests.** On the Mac with all four it shows the legal screen and the title menu, plays the story's 2D scenes (backgrounds, Spine character art, name plates, text box) and draws a 3D hallway scene with its lights, HUD and a Spine model. Without them the build stops or the world is black ([What it needs](#what-it-needs)). Sound, saves and input beyond the scripted `A` presses and one walked route were not checked.

## What it needs

| Setting / change | Where | Why |
|---|---|---|
| `modules main subsdk2 sdk` | [`game.tsv`](game.tsv) | The provisional profile's guess, unchanged: `subsdk2` is the movie module `main` imports from (the RomFS holds 3 WebM files). The Vulkan and OpenGL driver modules program the GPU through NvRm, which the host does not have |
| `sys_resource 0x0`; `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | `game.tsv` | The provisional profile's guesses, unchanged: the NPDM declares no system resource, so `main` sets a heap of at most the pool − 64 MB (a 3328 MB heap region). Every boot below ran on them |
| No NVN shader tables | `game.tsv` `shader_note`, the empty tables | The game has no NVN shaders; `vk_shaders.py` translates its SPIR-V ([Shaders](#shaders)) |
| #1370: DXC SPIR-V translation, loader-only Vulkan detection | `vk_shaders.py`, `profile_guess.py` | Without the detection the NSP script takes the game for NVN and stops: "found no shaders in main or the RomFS". Without the translation fixes `vk_shaders.py` stops at the first shader whose entry point is not named `main` (33 of the 41 are DXC's `PSMain`, `ps_*`, `VSFullscreen`), and the push-constant block stays at `buffer(0)` |
| #1371: Vulkan commands and pipeline refusal | `hle_vk.m` | The game looks up and records commands the layer did not have: indirect draws, fill and update, resolve, same-size blit, secondary command buffers, events, query copies, dynamic depth bias and blend constants (`dynamic state 3 not implemented` at the first pipeline). A pipeline whose fragment shader is missing from the library is refused and counted, where it used to be built depth-only: the 2D batches drew nothing and every frame stayed black with no report |
| #1539: compute pipelines, argument buffers | `hle_vk.m`, `vk_shaders.py` | The Forge skins every 3D character in a compute kernel and builds its clustered lights in another, and its materials read `textures[512]` and `samplers[64]`, past Metal's binding limits. Without compute the guest jumps to address 0 through its Vulkan function table after pipeline creation (`pc=0`, `lr=main+0x31b2a8`; the call is `vkCreateComputePipelines`, inferred, not caught in a debugger); without argument buffers 8 shaders do not translate |
| #1500: attachments opened with `LOAD_OP_DONT_CARE` are loaded, colour, depth and stencil | `hle_vk.m` `load_a` | Without it the 3D world is black and the HUD draws over nothing, and the effects pass samples a half-res depth the particle pass stored undefined ([below](#the-dont_care-finding)). The 2D scenes are not affected |

## Shaders

- 41 SPIR-V files in `fx/spirv/` (`.frag.spv`, `.cs.spv`, …), compiled by DXC from HLSL (33 entry points are not named `main`). Every shader module the game creates comes from these files; none is generated at run time.
- By kind (spirv-cross `--reflect`, observed): 9 fragment shaders with bindless arrays (`textures[512]` in set 1, `samplers[64]` and `genericSamplers[64]` in set 0), 7 compute (`skinning-p`, `-pn`, `-pnt`, `-pnts`, `ClusteredLight`, `SMAA-EdgeDetection` and one more), 1 geometry (`shadow-render.gs`), 3 vertex shaders reading a storage-buffer array `positionAttrIn[2]`, the other 21 plain (post effects, bloom, depth of field, motion blur, SMAA, copy, YUV).
- **The geometry shader has no Metal counterpart and is not translated.** `shadow-render.gs` only sets the viewport index of a 4-split shadow map (static). The layer logs three `[RED]` lines once, at pipeline creation: `shader 287e364d4b97a5be (1680 bytes of SPIR-V) is not in the library` twice (1,680 bytes is the size of `shadow-render.gs.spv`) and `shader stage 0x8 not implemented`. They are the only `[RED]` lines of a 60 s boot to the title and of the walked hallway; no frame line of the title or story boots counts a skipped draw. Shadows are not drawn; a replacement with `[[viewport_array_index]]` from the vertex stage is not written.
- `dm/gfx/tools/vk_shaders.py` writes the library and the slot table; the NVN tables of this folder are empty, as for [Hades](../0100535012974000/README.md).
- **Storage images:** The Forge creates default UAV textures and asserts (`Vulkan.cpp:3675`, "Format is not supported for GPU local images") when the format lacks `VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT`. #1539 advertises the bit per format from Metal's read-write texture tier.

## What renders

Mac, M4 Pro, macOS 27.0, boots of 45 to 100 s, frame counts from the layer's own frame line:

| Scene | Per frame | Notes |
|---|---|---|
| Legal screen (frame 300), title menu (from frame 1500) | 30 draws, 2 dispatches, 13 passes, 1 submit, 0 skipped | the menu over the palace background |
| New game: the prologue's sky scene, then village scenes with character art (frames to 5700) | 25 to 27 draws, 4 dispatches, 0 skipped | `A` pressed every few seconds from 27 s on; the final `sig=14` of the run is the boot script's time limit |
| 3D hallway, standing (integration build) | 52 draws, 18 dispatches, 13 passes, 1 submit, 0 skipped | world, lights, HUD and chibis correct; the characters here are Spine 2D (`SpineSkeletonRenderer::DrawSkeleton` in a live `sample`), not Live2D |
| 3D hallway with a dialog open (one frame holds the hallway, the 2D scene layer, the text box and a Spine model) | 14 passes | vsync-bound at 16.7 ms; GPU 4.4 to 4.9 ms a frame, about 2.5× the empty hallway |

Sixteen pipelines are built at the title menu (`pipelines so far 16`). About 3,900 frames of the integration build under Metal API and shader validation gave 0 errors.

## The DONT_CARE finding

The 3D world was black while the HUD and the 2D scenes drew. What was ruled out first, each observed on a pass dump or a readback: the skipped shadow pipelines (a depth map cleared to 1.0 and compared LESS reads as fully lit); skinning (the compute output holds plausible world-space positions and unit normals, and the draws read exactly those buffers); shading (with the toon fragment shader replaced by solid magenta the world stays black). A readback of the scene's own colour target held the whole scene.

The post target read back all zero. The copy pass (`copy.frag`, scene to post target), the effects blend after it (`pfx.frag`) and the HUD pass into the swap chain image each open their colour target with `LOAD_OP_DONT_CARE`, because the engine's render-target wrapper can only ask for CLEAR or DONT_CARE. The layer mapped that to `MTLLoadActionDontCare`, whose tiles start without the image, so every pass that draws over earlier contents lost them. #1500 loads such attachments.

- **On the Switch the contents survive by construction, not by a promise.** The NVIDIA driver the game ships (`subsdk1`, version 421.03) has no explicit discard for a colour DONT_CARE on the path examined (`subsdk1+0xc6550c`, the colour loop at `+0xc81c00`) and resets only ZCULL state for depth (`+0xc820b8`, method `0x801904b2`), all static. The render-pass macro MME `0xB2` decodes to a no-op with the driver's arguments (static). The submit-time processing is not decoded.
- **Switching the loads off brings the black world back** (observed in the game, on a build with the policy switch of #1659, mode `none`), which confirms #1500 on the game itself.
- **What loading costs** (in-process A/B, one build switching the policy every 300 frames, GPU time p50, the first quarter of each phase left out; the switch is in #1659, not in #1500):

  | Scene | colour only (#1500's first version) | all (colour, depth, stencil; #1500) | none |
  |---|---|---|---|
  | hallway, standing | 1.76–1.81 ms, 27 MB loaded per frame | 1.77–1.83 ms, 32 MB | 1.55–1.60 ms, 0 MB |
  | hallway with a dialog | about +0.24 ms over none (median of 88 paired cycles, IQR 0.15–0.30) | +0.24 ms over none (IQR 0.18–0.28), −0.00 ms against colour only (IQR −0.09 … +0.04); 36 MB | 0 MB |

  The frame is vsync-bound at 16.7 ms and the GPU is about 11 % busy in the standing hallway and about 28 % with the dialog open; these are GPU times, not frame rates. The dialog row is `gpuprof.py --ab-paired` (#1686) over 270 phases of 300 frames, each configuration against none within its cycle of three phases; it drops one cycle whose pass count changed. Loading depth and stencil too costs nothing measurable. Comparing two separate runs gave the opposite order (1.87–2.07 ms with loading, 2.85–3.35 ms without): DVFS at low utilisation, so only in-process comparisons count.
- **Oracle** (`DM_CTL dcoracle 3`, the same result in every frame checked): the colour of p4 (640×360), p6, p11 and p12 (1280×720) depends on the old contents; p2 (R8), p3, p5, p8 and p9 do not, so about 12 of the 27 MB of colour loaded per frame are not needed (a fast path that dropped them would save at most about 0.09 ms, scaled from the A/B, not measured). Depth or stencil is stored with contents the pass never wrote in p2 (stencil), p4 and p12 (both); with colour-only loading those are undefined on Apple. p4 is the particle pass: it reopens the half-res depth with DONT_CARE and STORE, draws nothing, and the effects pass (`pfx.frag`) samples that depth, which is why #1500 loads depth and stencil too. In the pass dump with a dialog open, the 1280×720 depth reopened the same way at the end of the frame is not read again before the next frame clears it; whether anything reads p2's stencil was not traced. With a dialog open the same checks give 10 passes, the new p10 (1440×810) among them. Pass numbers are the oracle's per-frame order.

## Memory

No heap check or address-region measurement was made for this profile: the provisional guesses boot and run, and the NPDM declares no system resource. How far `main`'s allocations go into the 3328 MB heap region in long play is not measured.

## Open

- **Shadows:** `shadow-render.gs` is not replaced, so no shadow map is drawn (the layer logs the stage once).
- **Barriers:** `vkCmdPipelineBarrier` and `vkCmdWaitEvents` are no-ops in the layer; it relies on one serial queue, hazard-tracked allocations and `useResources`. That holds for the game's pattern (events set and waited in submission order, compute-to-graphics barriers only between passes); a dependency inside one render pass is not honoured.
- **Not checked:** saves, sound, a controller or touch input, the three WebM movies through `subsdk2`, play beyond the first village scenes and one hallway route, the iPhone and iPad.
