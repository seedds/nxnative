# dm/gfx/hostvk: the Vulkan backend (Windows and Linux)

The host's Vulkan backend of the Windows and Linux port, designed in [vulkan-backend.md](../../../docs/design/vulkan-backend.md). The directory is `hostvk` because `dm/gfx/vk` is the guest's Vulkan API on Metal. Apple builds never compile it: `dm/host/Makefile` and `dm/gfx/tests/Makefile` take `dm/gfx`, `nvn`, `tex`, `vk` and `gl` only.

## What exists

The core of phase P3 (design 11.1): the backend behind [nvn_be.h](../nvn/nvn_be.h), headless, in plain C11 over [volk](../../third_party/volk/VENDOR.md), which loads the Vulkan loader at run time (nothing links against `libvulkan.so.1` or `vulkan-1.dll`). The library `nvn_vk` of [dm/host/CMakeLists.txt](../../host/CMakeLists.txt), beside `nvn_null`: a binary links one of them (the host takes this one at step M7):

- [vk_device.h](vk_device.h), [vk_device.c](vk_device.c): `hostvk_init` makes the instance (the window's instance extensions from `front_vk_instance_extensions` when a surface is wanted; the validation layer, with synchronization validation and a debug-utils messenger that counts what it reports, when asked), picks the physical device (`device_index`, or the first that meets the baseline), checks the baseline of design 9.1 (Vulkan 1.3, the feature list, `VK_EXT_external_memory_host` with host allocations importable as pool buffers, `VK_EXT_transform_feedback`, which decision D6 makes required, `VK_KHR_swapchain` with a surface), records which optional extensions of 9.1 the device has, derives the level of design 3.6, makes the logical device with one graphics, compute and transfer queue (a transfer-only family is recorded for later) and fills `hostvk_caps`: the `be_caps` fields of design 1.5 and the backend's own (import alignment, allocation limit, the memory type imports get and whether it is coherent, timeline semaphores, buffer device address, the level). It logs one line, `[hostvk] device '<name>' driver <version> (<driver name>: <driver info>) Vulkan <x.y.z> level <A-D>: <missing optional extensions>`. Its log lines go to stderr with the host's `[dm] ` prefix, or to the function `hostvk_set_log` names: `LOG` (`dm/host/dm.h`) needs the generated game profile header and declares the whole host.
- [vk_import.h](vk_import.h), [vk_import.c](vk_import.c): `hostvk_import` imports a page-aligned host range as a `VkDeviceMemory` with one `VkBuffer` over it with every usage a pool address can have (`hostvk_pool_usage`) and its `VkDeviceAddress`; it refuses a misaligned range and an import past `maxMemoryAllocationCount` itself, before Vulkan is called. `hostvk_import_release` frees it.

The levels, from what the device offers (`vk_device.h` lists the dynamic states): **A** pipeline libraries, dynamic vertex input and every dynamic state of the pipeline description that Vulkan 1.3 does not make dynamic (extended dynamic state 2's logic op and patch control points, and ten states of extended dynamic state 3); **B** pipeline libraries without all of those; **C** no pipeline libraries, but dynamic vertex input or some of those states; **D** none of them.

- [vk_backend.h](vk_backend.h), [vk_backend.c](vk_backend.c): the `be_*` functions. The device comes up on the first call that needs it (`DM_VK_DEVICE=<index>` picks it, `DM_VK_VALIDATION=1` turns validation on), `be_get_caps` reports it; with no device every function takes the null form (`nvn_null.c`'s: a `[RED]` line on its first call)
- [vk_memory.c](vk_memory.c): memory pools as imports (`be_guest_map` / `be_guest_unmap`, `be_addr`), the ring of the open submission (driver banks, converted indices, copied vertex streams, staging), the release of what the GPU may still be using after its fence
- [vk_texture.c](vk_texture.c), with the format table [nvn_fmt_vk.c](../tex/nvn_fmt_vk.c): images in the table's formats; textures the CPU fills deswizzled on the CPU and uploaded, then write-protected by the dirty tracker; samplers (exact border colours, integer borders); views with the composed channel mapping; CPU reads (ReadTexels) from the image of a texture the GPU wrote
- [vk_shader.c](vk_shader.c): modules from `DM_MSL_DIR/vk-<DMSH_CHAIN_VK>/<key>.spv` and their reflection (`<key>.json`) through `nvn_refl_from_json_text`, checked against the device's capabilities
- [vk_pipeline.c](vk_pipeline.c): the four universal pipeline layouts of design 2.3 (the driver banks c0 and c2 as dynamic uniform buffers in the ring), monolithic graphics and compute pipelines by content, descriptor sets cached by content per submission slot, the backend's own pipelines (the clear triangle, the counter-report, predicate and quad-index kernels)
- [vk_commands.c](vk_commands.c): submissions on a timeline semaphore with a completion thread (events, CPU-side reports), render passes (dynamic rendering, images in `GENERAL`, a full barrier between operations), the draw state, draws (indexed, instanced, indirect, indirect-count; quads, fans and restart converted; sources the GPU writes first copied or expanded on the GPU), clears, copies, barriers, syncs, counter reports, conditional rendering, transform feedback
- [shaders/](shaders): the backend's GLSL; `shaders/gen_spirv.sh` writes the SPIR-V headers of `shaders/gen/` and of the GPU tests (`dm/gfx/tests/vk/shaders/gen/`), committed, so building needs no glslang

Declared with a `[RED]` line, for later phases: the presenter and the vsync source (P4); GPU conversions of indices other than quads without restart, draw-based copies between formats a blit cannot convert (P4); pipeline libraries and the disk cache (P5); discrete-GPU mirrors (P6); ASTC without the device's support, depth-stencil uploads (P7); counters other than samples passed and timestamps (P8); external textures of the guest Vulkan and GL layers (P9). Not here: AMD's Vulkan Memory Allocator (decision D2).

## Tests

`t_vk_device` and `t_vk_import` (in `tests/`), the GPU tests of the backend (`t_vk_*` in [dm/gfx/tests/vk](../tests/vk), which drive the real NVN frontend over it and read pixels back; [t_vk_harness.h](../tests/vk/t_vk_harness.h) says what they share), the format table's `t_fmt_vk`, `t_vk_spirv` (the committed SPIR-V headers rebuild byte for byte; skipped where glslang is missing or another version) and the descriptor microbenchmark `t_vk_descbench` (run without the validation layer, whose cost it would measure) are registered in [dm/host/CMakeLists.txt](../../host/CMakeLists.txt) and run with the other host tests, in the `nxnative-build` image ([docker/README.md](../../../docker/README.md)), which carries Mesa's software Vulkan driver lavapipe, the Vulkan loader and the Khronos validation layer:

```sh
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$(pwd):/src" -w /src nxnative-build sh -c \
  'cmake -S dm/host -B build/linux -G Ninja -DGAME=010015100B514000 && cmake --build build/linux && ctest --test-dir build/linux -R t_vk_ --output-on-failure'
```

CTest sets `VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json` and `VK_LOADER_DRIVERS_SELECT=lvp*` (lavapipe only) and `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`. With those set, a missing driver or layer fails the test. The runners (`run_host_tests.sh`, `run_win64_tests.ps1`) do not set them: the tests then take the loader's default drivers (in the container lavapipe is the only one with a device) and turn the validation layer on themselves when it is installed. Without a Vulkan loader, without a driver or without a device, they print `note: no Vulkan device: skipped` and pass, which is what the Windows CI runner does: it has no Vulkan driver. Any validation-layer warning or error fails a test; each test first provokes one on purpose (an invalid buffer, a copy hazard) to show the messenger counts it.

`tests/t_vk_import_add1.comp` is the compute shader `t_vk_import` runs; its SPIR-V is committed as `tests/t_vk_import_add1.h`, so building needs no glslang. After editing the shader, regenerate and validate it in the image: `docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$(pwd):/src" -w /src nxnative-build sh dm/gfx/hostvk/tests/gen_spirv.sh`.

## What lavapipe answers

Mesa 25.2.8 (`mesa-vulkan-drivers 25.2.8-0ubuntu0.24.04.4`, LLVM 20.1.2), device `llvmpipe (LLVM 20.1.2, 256 bits)`, through the loader 1.3.275 and the validation layer 1.3.275 of Ubuntu 24.04, measured on 2026-10-29:

| Question | Answer |
|---|---|
| Baseline of 9.1 | met, every feature and required extension |
| Vulkan version | the device reports 1.4.318; used at 1.3, because Ubuntu 24.04's loader is 1.3.275 |
| Level | **A**: pipeline libraries, dynamic vertex input and all twelve dynamic states |
| Optional extensions of 9.1 missing | Vulkan 1.4 (the loader), `VK_KHR_present_id`, `VK_KHR_present_wait`, `VK_EXT_conservative_rasterization`, `VK_EXT_sample_locations`, `VK_EXT_blend_operation_advanced`, `VK_KHR_pipeline_binary`, ASTC (`textureCompressionASTC_LDR`) |
| Device type, queues | CPU (so `unified_memory`), one queue family with graphics, compute and transfer, no transfer-only family |
| `minImportedHostPointerAlignment` | 4,096 bytes, the platform layer's page; a misaligned address or size is refused by `hostvk_import` before Vulkan |
| `maxMemoryAllocationCount` | 4,294,967,295 (no practical limit); 1,000 adjacent 4 KiB imports take about 30 us each, a release about 13 us |
| Memory type of an import | one type for host memory, type 0: `DEVICE_LOCAL`, `HOST_VISIBLE`, `HOST_COHERENT`, `HOST_CACHED` |
| Host coherence | a CPU write after the import, even after the command buffer was recorded, is seen by the next submission's GPU copy without a flush; the GPU's writes are seen by the CPU after the fence without an invalidate |
| Overlapping imports | allowed: a range inside a live import and the same range again both import, a GPU write through one is read through the other, and releasing the first leaves the others working |
| Device address | an import's `VkDeviceAddress` equals its host address |
| Pool buffer over a host allocation | importable, not dedicated-only (`VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT`) |
| `MAP_SHARED` memfd mappings (the recompiler's guest arena may use them) | import; two views of one memfd imported separately alias on the GPU |
| Synchronization validation | reports a write-after-write between two copies; did not report two dispatches writing the same storage buffer with no barrier (layer 1.3.275) |

Lavapipe is a software driver for continuous integration (design 9.2): its answers show the backend's code is correct Vulkan, not what a GPU will demand. The owner's RTX 4060 laptop (and its integrated GPU) answers go in a second table here; the design's open questions on them are its section 4.2 and risk 6 of 11.2.

## What the backend's tests found on lavapipe

- A `DEPTH16` clear stored one unit off a fragment writing the same depth for 7 of `t_vk_depth16_clear`'s 1,042 depths: lavapipe rounds a fragment's float product half to even and converts a clear value in double. `hvk_depth_clear_value` (`vk_commands.c`) quantizes a `DEPTH16` clear as the fragment does and moves it a quarter unit inside that value's bin, for every clear path ([rendering.md, Depth16 load-clears match fragment writes](../../../docs/reference/rendering.md#depth16-load-clears-match-fragment-writes)).
- The validation layer 1.3.275 rejects the KHR names of `VK_KHR_index_type_uint8`, `VK_KHR_calibrated_timestamps` and `VK_KHR_vertex_attribute_divisor` (promoted in header 1.3.280 with the same structures and values): under an older layer the device enables the EXT names where it has them.
- A pipeline with no colour attachment must not declare the colour blend states dynamic (no `vkCmdSetColorBlend*EXT` call can name an attachment); a pipeline with the patch size dynamic needs it set before every draw.
- `t_vk_descbench`, 4,900 draws a frame (medians of 20 frames): cached sets with dynamic offsets record a frame in 2.1 ms (435 ns a draw, 161 descriptor writes, 56 set allocations), push descriptors in 3.7 ms (747 ns a draw, 24,500 writes); the frame takes 130 to 140 ms on lavapipe either way. The backend keeps the cached sets; the RTX 4060's numbers decide it for good (P4).

## Next

Phase P4 (design 11.1): the window, the presenter and the vsync source on the RTX 4060; the frontend's uniform deferral and shadows; the GPU conversions declared above.
