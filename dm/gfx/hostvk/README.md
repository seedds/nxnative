# dm/gfx/hostvk: the Vulkan backend (Windows and Linux)

The host's Vulkan backend of the Windows and Linux port, designed in [vulkan-backend.md](../../../docs/design/vulkan-backend.md). The directory is `hostvk` because `dm/gfx/vk` is the guest's Vulkan API on Metal. Apple builds never compile it: `dm/host/Makefile` and `dm/gfx/tests/Makefile` take `dm/gfx`, `nvn`, `tex`, `vk` and `gl` only.

## What exists

The prerequisites of phase P3 (design 11.1), headless, in plain C11 over [volk](../../third_party/volk/VENDOR.md), which loads the Vulkan loader at run time (nothing links against `libvulkan.so.1` or `vulkan-1.dll`):

- [vk_device.h](vk_device.h), [vk_device.c](vk_device.c): `hostvk_init` makes the instance (the window's instance extensions from `front_vk_instance_extensions` when a surface is wanted; the validation layer, with synchronization validation and a debug-utils messenger that counts what it reports, when asked), picks the physical device (`device_index`, or the first that meets the baseline), checks the baseline of design 9.1 (Vulkan 1.3, the feature list, `VK_EXT_external_memory_host` with host allocations importable as pool buffers, `VK_EXT_transform_feedback`, which decision D6 makes required, `VK_KHR_swapchain` with a surface), records which optional extensions of 9.1 the device has, derives the level of design 3.6, makes the logical device with one graphics, compute and transfer queue (a transfer-only family is recorded for later) and fills `hostvk_caps`: the `be_caps` fields of design 1.5 and the backend's own (import alignment, allocation limit, the memory type imports get and whether it is coherent, timeline semaphores, buffer device address, the level). It logs one line, `[hostvk] device '<name>' driver <version> (<driver name>: <driver info>) Vulkan <x.y.z> level <A-D>: <missing optional extensions>`. Its log lines go to stderr with the host's `[dm] ` prefix, or to the function `hostvk_set_log` names: `LOG` (`dm/host/dm.h`) needs the generated game profile header and declares the whole host.
- [vk_import.h](vk_import.h), [vk_import.c](vk_import.c): `hostvk_import` imports a page-aligned host range as a `VkDeviceMemory` with one `VkBuffer` over it with every usage a pool address can have (`hostvk_pool_usage`) and its `VkDeviceAddress`; it refuses a misaligned range and an import past `maxMemoryAllocationCount` itself, before Vulkan is called. `hostvk_import_release` frees it.

The levels, from what the device offers (`vk_device.h` lists the dynamic states): **A** pipeline libraries, dynamic vertex input and every dynamic state of the pipeline description that Vulkan 1.3 does not make dynamic (extended dynamic state 2's logic op and patch control points, and ten states of extended dynamic state 3); **B** pipeline libraries without all of those; **C** no pipeline libraries, but dynamic vertex input or some of those states; **D** none of them.

Not here yet: AMD's Vulkan Memory Allocator (decision D2, with the memory layer), textures, shaders and pipelines, the descriptor scheme, the `be_*` functions of `nvn_be.h`, the presenter. The host (`nxhost`) does not link this directory yet; the two tests do.

## Tests

`t_vk_device` and `t_vk_import` (in `tests/`, registered in [dm/host/CMakeLists.txt](../../host/CMakeLists.txt)) run with the other host tests, in the `nxnative-build` image ([docker/README.md](../../../docker/README.md)), which carries Mesa's software Vulkan driver lavapipe, the Vulkan loader and the Khronos validation layer:

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

## Next in P3

Textures with the CPU deswizzle, shaders from the translator's SPIR-V, monolithic pipelines, draws, copies, clears, queries and synchronization, the descriptor microbenchmark (design 11.1), and the memory layer with VMA.
