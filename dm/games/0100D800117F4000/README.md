# Re:ZERO -Starting Life in Another World- The Prophecy of the Throne (`0100D800117F4000`)

Update 1.0.2 (v131072) uses The Forge renderer on Vulkan and Nintendo SDK 9.5.0. `main` imports `vkGetInstanceProcAddr` and resolves its Vulkan commands through it. The host runs `main`, `sdk` and the movie module `subsdk2`.

## What it needs

- [The registered build and settings](game.tsv) use the 4 GiB layout with no NPDM system resource. The Mac boot requested a 3.14 GiB heap in the 3.25 GiB region. There are no executable patches.
- The RomFS contains 41 SPIR-V modules. The Vulkan tool builds 40 Metal shaders for macOS and iPhone and folds the [shadow geometry shader's viewport selection](../../../docs/reference/rendering.md#vulkan-geometry-shaders-that-select-a-viewport) into a vertex variant.
- Compute dispatches perform skinning and build the light clusters. The skinning shader uses a 1,024-thread group. The larger descriptor sets use Metal Tier 2 argument buffers.

## Mac route

Pack with the NSP script and `--platform mac-app`. Open the app and choose Play. On the keyboard, J confirms, K goes back, the arrow keys select options, and WASD moves the left stick.

On Apple M6 with macOS 27.2.1, the local integration reached the opening story and recorded 20,400 frames without a skipped draw or dispatch. This was the integration before the landing revisions. The revisions were checked at the GPU command and shader boundaries. iPhone Metal compilation passed. iPhone runtime is open.

With the viewport fix, both shadow pipelines built during a headless boot with Metal validation on the same Mac. The boot reached 600 frames without a skipped draw or dispatch. Visible shadow rendering in gameplay has not been retested.

## Found on this game

- [SPIR-V compiled by DXC](../../../docs/reference/rendering.md#spir-v-compiled-by-dxc)
- [Commands reached through the Vulkan loader](../../../docs/reference/rendering.md#commands-reached-through-the-vulkan-loader)
- [Vulkan query copies consumed by the GPU](../../../docs/reference/rendering.md#vulkan-query-copies-consumed-by-the-gpu)
- [Vulkan compute dispatch](../../../docs/reference/rendering.md#vulkan-compute-dispatch)
- [Vulkan geometry shaders that select a viewport](../../../docs/reference/rendering.md#vulkan-geometry-shaders-that-select-a-viewport)
- [Vulkan descriptor arrays over Metal's direct limits](../../../docs/reference/rendering.md#vulkan-descriptor-arrays-over-metals-direct-limits)
- [Attachments opened with `LOAD_OP_DONT_CARE`](../../../docs/reference/rendering.md#attachments-opened-with-load_op_dont_care)
