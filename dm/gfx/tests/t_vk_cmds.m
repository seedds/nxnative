// The Vulkan layer's commands and pipeline rules, read back from the GPU (no game files, no shader library: the shaders are Metal source compiled here and wrapped as shader
// modules the way the layer wraps a library function):
//   · a pipeline that requests a fragment stage whose function the library lacks is refused and its draws are counted as skipped, while one with no fragment stage at all (a
//     depth-only pipeline) is built, and so is one with a geometry stage: requested and not available is refused, not requested is built
//   · viewport selection and clipping, buffer fill and update, image resolve and same-size blit, indirect draws, dynamic blend constants, secondary command buffers, events and query copies
#include "../vk/hle_vk.m"
int dm_guest_as_contains(uintptr_t address) { (void)address; return 0; }
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
void dm_vi_layer_crop(int c[4]) { c[0] = c[1] = c[2] = c[3] = 0; }
int dm_opmode(void) { return 1; }
static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); } } while (0)

static VkDevice dev; static VkQueue queue; static VkCommandPool pool; static VkDeviceMemory mem; static VkDeviceSize mem_top;
static id<MTLLibrary> lib;

static VkBuffer new_buf(VkDeviceSize size, int fill) {   // host-visible memory, its bytes set to `fill`
    VkBufferCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                             .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT};
    VkBuffer b; h_vkCreateBuffer(dev, &ci, NULL, &b); h_vkBindBufferMemory(dev, b, mem, mem_top);
    mem_top += (size + 255) & ~(VkDeviceSize)255;
    memset((uint8_t *)mem_buf(mem).contents + b->off, fill, size);
    return b;
}
static uint8_t *bytes(VkBuffer b) { return (uint8_t *)mem_buf(b->mem).contents + b->off; }
static VkCommandBuffer new_cb(VkCommandBufferLevel level) {
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = level, .commandBufferCount = 1};
    VkCommandBuffer cb; h_vkAllocateCommandBuffers(dev, &ai, &cb);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    h_vkBeginCommandBuffer(cb, &bi);
    return cb;
}
static void run(VkCommandBuffer cb) {   // submit and wait for the queue
    h_vkEndCommandBuffer(cb);
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    h_vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE); h_vkQueueWaitIdle(queue);
}
static VkImage new_img(uint32_t w, uint32_t h, VkFormat fmt, VkSampleCountFlagBits samples, VkImageUsageFlags usage) {
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
                            .samples = samples, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage};
    VkImage im = NULL; VkResult r = h_vkCreateImage(dev, &ci, NULL, &im); CHECK(r == VK_SUCCESS && im && im->tex, "image %ux%u format %d created", w, h, fmt);
    return im;
}
static VkImageView new_view(VkImage im, VkFormat fmt, VkImageAspectFlags aspect) {
    VkImageViewCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt, .subresourceRange = {aspect, 0, 1, 0, 1}};
    VkImageView v; h_vkCreateImageView(dev, &ci, NULL, &v); return v;
}
static VkBufferImageCopy whole(uint32_t w, uint32_t h) { return (VkBufferImageCopy){.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}}; }
static void upload(VkImage im, uint32_t w, uint32_t h, const uint8_t *px) {   // RGBA8 texels through a buffer
    VkBuffer b = new_buf(w * h * 4, 0); memcpy(bytes(b), px, w * h * 4);
    VkCommandBuffer cb = new_cb(VK_COMMAND_BUFFER_LEVEL_PRIMARY); VkBufferImageCopy r = whole(w, h);
    h_vkCmdCopyBufferToImage(cb, b, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r); run(cb);
}
static void download(VkImage im, uint32_t w, uint32_t h, uint8_t *px) {
    VkBuffer b = new_buf(w * h * 4, 0xEE);
    VkCommandBuffer cb = new_cb(VK_COMMAND_BUFFER_LEVEL_PRIMARY); VkBufferImageCopy r = whole(w, h);
    h_vkCmdCopyImageToBuffer(cb, im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b, 1, &r); run(cb);
    memcpy(px, bytes(b), w * h * 4);
}
static int near(const uint8_t *p, int r, int g, int b, int a) { return abs(p[0] - r) <= 1 && abs(p[1] - g) <= 1 && abs(p[2] - b) <= 1 && abs(p[3] - a) <= 1; }
static int all_near(const uint8_t *px, int n, int r, int g, int b, int a) { for (int i = 0; i < n; i++) if (!near(px + 4 * i, r, g, b, a)) return 0; return 1; }

static VkRenderPass color_pass(VkFormat fmt, VkSampleCountFlagBits samples) {   // one colour attachment, cleared on load and stored
    VkAttachmentDescription at = {.format = fmt, .samples = samples, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
    VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sp = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &ref};
    VkRenderPassCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &at, .subpassCount = 1, .pSubpasses = &sp};
    VkRenderPass rp; h_vkCreateRenderPass(dev, &ci, NULL, &rp); return rp;
}
static VkRenderPass depth_pass(VkFormat fmt) {   // a depth attachment and no colour: the pass of a depth-only pipeline
    VkAttachmentDescription at = {.format = fmt, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                  .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sp = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .pDepthStencilAttachment = &ref};
    VkRenderPassCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &at, .subpassCount = 1, .pSubpasses = &sp};
    VkRenderPass rp; h_vkCreateRenderPass(dev, &ci, NULL, &rp); return rp;
}
static VkFramebuffer new_fb(VkRenderPass rp, VkImageView v, uint32_t w, uint32_t h) {
    VkFramebufferCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = 1, .pAttachments = &v, .width = w, .height = h, .layers = 1};
    VkFramebuffer fb; h_vkCreateFramebuffer(dev, &ci, NULL, &fb); return fb;
}
static void begin_pass(VkCommandBuffer cb, VkRenderPass rp, VkFramebuffer fb, uint32_t w, uint32_t h, VkSubpassContents contents) {
    VkClearValue clear = rp->ncol ? (VkClearValue){.color = {{0, 0, 1, 1}}} : (VkClearValue){.depthStencil = {1, 0}};   // blue; depth 1 (attachment 0 is the colour or the depth)
    VkRenderPassBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb, .renderArea = {{0, 0}, {w, h}}, .clearValueCount = 1, .pClearValues = &clear};
    h_vkCmdBeginRenderPass(cb, &bi, contents);
}

// A shader module as the layer makes it from its library: the function by key, no table row (these shaders take no resources). A module without a function is a shader the library lacks
static VkShaderModule module_of_fn(const char *name, const char *key) {
    VkShaderModule m = zalloc(sizeof *m); strlcpy(m->key, key, sizeof m->key);
    if (name) m->fn = [lib newFunctionWithName:@(name)];
    return m;
}
static VkPipeline make_pipe_with_topology(VkRenderPass rp, const VkPipelineShaderStageCreateInfo *st, uint32_t nst, int blend_constant, VkPrimitiveTopology topology) {
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = topology};
    VkViewport vp = {0, 0, 4, 4, 0, 1}; VkRect2D sc = {{0, 0}, {4, 4}};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState ba = {.blendEnable = blend_constant, .srcColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_COLOR, .dstColorBlendFactor = VK_BLEND_FACTOR_ZERO,
                                              .colorBlendOp = VK_BLEND_OP_ADD, .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO, .alphaBlendOp = VK_BLEND_OP_ADD,
                                              .colorWriteMask = 0xF};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ba};   // constants: 0, 0, 0, 0
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 1, .pDynamicStates = dyn};
    VkGraphicsPipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = nst, .pStages = st, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = rp->ncol ? &cb : NULL, .pDynamicState = blend_constant ? &dy : NULL, .renderPass = rp};
    VkPipeline p; h_vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &p); return p;
}
static VkPipeline make_pipe(VkRenderPass rp, const VkPipelineShaderStageCreateInfo *st, uint32_t nst, int blend_constant) {
    return make_pipe_with_topology(rp, st, nst, blend_constant, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
}
static VkPipelineShaderStageCreateInfo stage(VkShaderStageFlagBits s, VkShaderModule m) { return (VkPipelineShaderStageCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = s, .module = m, .pName = "main"}; }

static void test_indirect_fans(VkRenderPass rp, VkFramebuffer fb, VkImage target, VkShaderModule fs) {
    VkShaderModule vs = module_of_fn("t_fan", "0000000000000009");
    VkPipelineShaderStageCreateInfo stages[] = {stage(VK_SHADER_STAGE_VERTEX_BIT, vs), stage(VK_SHADER_STAGE_FRAGMENT_BIT, fs)};
    VkPipeline p = make_pipe_with_topology(rp, stages, 2, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN);
    VkBuffer args = new_buf(64, 0), indices = new_buf(16, 0);
    const uint32_t corners[] = {0, 1, 2, 3};
    memcpy(bytes(indices), corners, sizeof corners);
    VkDrawIndirectCommand draw = {4, 1, 0, 0};
    VkDrawIndexedIndirectCommand indexed = {4, 1, 0, 0, 0};
    const VkMemoryBarrier dep = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                 .dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT};
    for (int i = 0; i < 2; i++) {
        // Exercise an indirect count produced in this submission instead of a stale CPU count.
        VkCommandBuffer cb = new_cb(VK_COMMAND_BUFFER_LEVEL_PRIMARY);
        h_vkCmdUpdateBuffer(cb, args, 0, i ? sizeof indexed : sizeof draw, i ? (void *)&indexed : (void *)&draw);
        h_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, 0, 1, &dep, 0, NULL, 0, NULL);
        begin_pass(cb, rp, fb, 4, 4, VK_SUBPASS_CONTENTS_INLINE);
        h_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
        if (i) {
            h_vkCmdBindIndexBuffer(cb, indices, 0, VK_INDEX_TYPE_UINT32);
            h_vkCmdDrawIndexedIndirect(cb, args, 0, 1, sizeof indexed);
        } else h_vkCmdDrawIndirect(cb, args, 0, 1, sizeof draw);
        h_vkCmdEndRenderPass(cb); run(cb);
        uint8_t px[64]; download(target, 4, 4, px);
        CHECK(all_near(px, 16, 255, 128, 0, 255), "%s indirect fan covers all 16 pixels", i ? "indexed" : "non-indexed");
    }
    h_vkDestroyPipeline(dev, p, NULL); h_vkDestroyShaderModule(dev, vs, NULL);
    h_vkDestroyBuffer(dev, args, NULL); h_vkDestroyBuffer(dev, indices, NULL);
}

static void test_geometry_viewports(VkRenderPass rp, VkFramebuffer fb, VkImage target) {
    if (max_viewports() == 1) return;
    const shinfo route = {.geom_vp = 2};   // a geomvp:1 table row
    VkShaderModule gs = module_of_fn(NULL, "000000000000000a");
    gs->si = &route;
    VkShaderModule vs = module_of_fn("t_vs", "000000000000000b");
    VkShaderModule fs = module_of_fn("t_vp_fs", "000000000000000c");
    VkPipelineShaderStageCreateInfo stages[] = {stage(VK_SHADER_STAGE_GEOMETRY_BIT, gs), stage(VK_SHADER_STAGE_VERTEX_BIT, vs),
                                                stage(VK_SHADER_STAGE_FRAGMENT_BIT, fs)};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState ba = {.colorWriteMask = 0xF};
    VkPipelineColorBlendStateCreateInfo blend = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ba};
    VkViewport viewports[] = {{0, 0, 2, 4, 0, 1}, {2, 0, 2, 4, 0, 1}};
    VkRect2D scissors[] = {{{1, 0}, {1, 4}}, {{3, 0}, {1, 4}}};
    VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                            .viewportCount = 2, .pViewports = viewports, .scissorCount = 2, .pScissors = scissors};
    VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = states};
    VkGraphicsPipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 3, .pStages = stages,
                                       .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms,
                                       .pColorBlendState = &blend, .renderPass = rp};
    for (int dynamic = 0; dynamic < 2; dynamic++) {
        ci.pDynamicState = dynamic ? &dyn : NULL;
        VkPipeline p;
        h_vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &p);
        CHECK(p->pso != nil, "the geometry pipeline picks the matching vertex variant");
        VkCommandBuffer cb = new_cb(VK_COMMAND_BUFFER_LEVEL_PRIMARY);
        begin_pass(cb, rp, fb, 4, 4, VK_SUBPASS_CONTENTS_INLINE);
        h_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
        if (dynamic) {
            const VkViewport initial[] = {viewports[0], viewports[0]};
            const VkRect2D clips[] = {scissors[0], scissors[0]};
            h_vkCmdSetViewport(cb, 0, 2, initial);
            h_vkCmdSetScissor(cb, 0, 2, clips);
            h_vkCmdSetViewport(cb, 1, 1, &viewports[1]);
            h_vkCmdSetScissor(cb, 1, 1, &scissors[1]);
        }
        h_vkCmdDraw(cb, 3, 2, 0, 0);
        h_vkCmdEndRenderPass(cb); run(cb);
        uint8_t pixels[64]; download(target, 4, 4, pixels);
        const uint8_t row[] = {0, 0, 255, 255, 255, 0, 0, 255, 0, 0, 255, 255, 0, 255, 0, 255};
        int match = 1;
        for (unsigned y = 0; y < 4; y++) match &= !memcmp(pixels + y * sizeof row, row, sizeof row);
        CHECK(match, "%s viewport and scissor arrays route instances to the expected pixels", dynamic ? "partially updated dynamic" : "static");
        h_vkDestroyPipeline(dev, p, NULL);
    }
    h_vkDestroyShaderModule(dev, gs, NULL); h_vkDestroyShaderModule(dev, vs, NULL); h_vkDestroyShaderModule(dev, fs, NULL);
}

// VK_EXT_extended_dynamic_state: all twelve dynamic states of the extension, read back from the GPU.
static void test_extended_dynamic_state(VkRenderPass rp, VkFramebuffer fb, VkImage target, VkShaderModule fs) {
    // 1. Extension advertisement, feature query, non-mutation of vkCreateDevice's const pNext, and lookup of all 12 entry points
    uint32_t next_ext = 0; h_vkEnumerateDeviceExtensionProperties(&g_pdev, NULL, &next_ext, NULL);
    VkExtensionProperties *exts = calloc(next_ext ? next_ext : 1, sizeof *exts);
    int listed = 0;
    if (exts) {
        h_vkEnumerateDeviceExtensionProperties(&g_pdev, NULL, &next_ext, exts);
        for (uint32_t i = 0; i < next_ext; i++) if (!strcmp(exts[i].extensionName, "VK_EXT_extended_dynamic_state")) listed = 1;
        free(exts);
    }
    CHECK(listed, "the device lists VK_EXT_extended_dynamic_state among its %u extensions", next_ext);
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT q = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT};
    VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &q};
    h_vkGetPhysicalDeviceFeatures2(&g_pdev, &f2);
    CHECK(q.extendedDynamicState == VK_TRUE, "vkGetPhysicalDeviceFeatures2 answers extendedDynamicState = VK_TRUE");
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT cq = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT, .extendedDynamicState = VK_FALSE};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &cq};
    VkDevice d2 = VK_NULL_HANDLE; h_vkCreateDevice(&g_pdev, &dci, NULL, &d2); (void)d2;
    CHECK(cq.extendedDynamicState == VK_FALSE, "vkCreateDevice does not mutate const pNext feature structs");
    const char *const eds_fns[12] = {
        "vkCmdSetCullModeEXT", "vkCmdSetFrontFaceEXT", "vkCmdSetPrimitiveTopologyEXT",
        "vkCmdSetViewportWithCountEXT", "vkCmdSetScissorWithCountEXT", "vkCmdBindVertexBuffers2EXT",
        "vkCmdSetDepthTestEnableEXT", "vkCmdSetDepthWriteEnableEXT", "vkCmdSetDepthCompareOpEXT",
        "vkCmdSetDepthBoundsTestEnableEXT", "vkCmdSetStencilTestEnableEXT", "vkCmdSetStencilOpEXT",
    };
    int all_resolved = 1;
    for (int i = 0; i < 12; i++) all_resolved &= h_vkGetDeviceProcAddr(dev, eds_fns[i]) != NULL;
    CHECK(all_resolved, "vkGetDeviceProcAddr resolves all twelve VK_EXT_extended_dynamic_state commands");

    // 2. Dynamic vertex stride (including pStrides == NULL preservation and stride == 0), with-count viewport/scissor, and no leakage to static-count pipelines
    VkShaderModule vs = module_of_fn("t_poscol_vs", "000000000000000d"), pfs = module_of_fn("t_col_fs", "000000000000000e");
    VkPipelineShaderStageCreateInfo stages[] = {stage(VK_SHADER_STAGE_VERTEX_BIT, vs), stage(VK_SHADER_STAGE_FRAGMENT_BIT, pfs)};
    VkVertexInputBindingDescription binds[2] = {
        {.binding = 0, .stride = 8, .inputRate = VK_VERTEX_INPUT_RATE_VERTEX},
        {.binding = 1, .stride = 16, .inputRate = VK_VERTEX_INPUT_RATE_VERTEX},
    };
    VkVertexInputAttributeDescription attrs[2] = {
        {.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 0},
        {.location = 1, .binding = 1, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 0},
    };
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .vertexBindingDescriptionCount = 2,
                                               .pVertexBindingDescriptions = binds, .vertexAttributeDescriptionCount = 2, .pVertexAttributeDescriptions = attrs};
    // Spec-required viewportCount = 0 / scissorCount = 0 when WITH_COUNT is dynamic
    VkPipelineViewportStateCreateInfo vsi0 = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 0, .scissorCount = 0};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL,
                                                 .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState ba = {.colorWriteMask = 0xF};
    VkPipelineColorBlendStateCreateInfo blend = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ba};
    VkDynamicState states[] = {VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE, VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT, VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT,
                               VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 6, .pDynamicStates = states};
    VkGraphicsPipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = stages, .pVertexInputState = &vi,
                                       .pInputAssemblyState = &ia, .pViewportState = &vsi0, .pRasterizationState = &rs, .pMultisampleState = &ms,
                                       .pColorBlendState = &blend, .pDynamicState = &dy, .renderPass = rp};
    VkPipeline p; h_vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &p);
    CHECK(p->pso != nil && (p->eds & (EDS_STRIDE | EDS_VP_COUNT | EDS_SC_COUNT | EDS_CULL | EDS_WIND | EDS_PRIM)) == (EDS_STRIDE | EDS_VP_COUNT | EDS_SC_COUNT | EDS_CULL | EDS_WIND | EDS_PRIM),
          "pipeline with dynamic stride, with-count viewport/scissor, cull, front face and topology is built");

    // Mixed pipeline: VIEWPORT_WITH_COUNT (viewportCount = 0) + static scissors (scissorCount = 2 where supported) must build
    if (max_viewports() > 1) {
        VkRect2D two_sc[2] = {{{0, 0}, {4, 4}}, {{0, 0}, {4, 4}}};
        VkPipelineViewportStateCreateInfo vsi_mix = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 0, .scissorCount = 2, .pScissors = two_sc};
        VkDynamicState one_vp[] = {VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT};
        VkPipelineDynamicStateCreateInfo dy_mix = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 1, .pDynamicStates = one_vp};
        VkGraphicsPipelineCreateInfo cm = ci; cm.pViewportState = &vsi_mix; cm.pDynamicState = &dy_mix;
        VkPipeline pm; h_vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &cm, NULL, &pm);
        CHECK(pm->pso != nil && pm->nviewport == 2, "a mixed pipeline with dynamic viewport count (0) and 2 static scissors builds");
        h_vkDestroyPipeline(dev, pm, NULL);
    }

    // Vertex buffers: right quad at stride 12 (3 floats/vert), left quad at stride 24 (6 floats/vert), color buffer with red at vertex 0 and green at vertices 1..5
    VkBuffer vba = new_buf(96, 0), vbb = new_buf(160, 0), vbc = new_buf(128, 0);
    const float right_quad[18] = {0,-1,0.5f, 1,-1,0.5f, 0,1,0.5f, 1,-1,0.5f, 1,1,0.5f, 0,1,0.5f};
    const float left_quad[18]  = {-1,-1,0.5f, 0,-1,0.5f, -1,1,0.5f, 0,-1,0.5f, 0,1,0.5f, -1,1,0.5f};
    memcpy(bytes(vba), right_quad, sizeof right_quad);
    for (int i = 0; i < 6; i++) memcpy((float *)bytes(vbb) + i * 6, left_quad + 3 * i, 3 * sizeof(float));
    float cols[24];
    for (int i = 0; i < 6; i++) {
        cols[4 * i + 0] = i == 0 ? 1.0f : 0.0f; cols[4 * i + 1] = i == 0 ? 0.0f : 1.0f; cols[4 * i + 2] = 0.0f; cols[4 * i + 3] = 1.0f;
    }
    memcpy(bytes(vbc), cols, sizeof cols);

    VkDeviceSize offs[2] = {0, 0}, st12_0[2] = {12, 0}, st24_0[2] = {24, 0};
    VkBuffer bufs_a[2] = {vba, vbc};
    const VkViewport full = {0, 0, 4, 4, 0, 1};
    const VkRect2D all = {{0, 0}, {4, 4}}, row1 = {{0, 1}, {4, 1}};
    uint8_t px[64]; int ok;

    // Stride 12 on binding 0 (right half) and stride 0 on binding 1 (constant red from vertex 0)
    VkCommandBuffer cb = new_cb(VK_COMMAND_BUFFER_LEVEL_PRIMARY);
    begin_pass(cb, rp, fb, 4, 4, VK_SUBPASS_CONTENTS_INLINE);
    h_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    h_vkCmdSetViewportWithCountEXT(cb, 1, &full); h_vkCmdSetScissorWithCountEXT(cb, 1, &all);
    h_vkCmdSetCullModeEXT(cb, VK_CULL_MODE_NONE); h_vkCmdSetFrontFaceEXT(cb, VK_FRONT_FACE_COUNTER_CLOCKWISE);
    h_vkCmdSetPrimitiveTopologyEXT(cb, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    h_vkCmdBindVertexBuffers2EXT(cb, 0, 2, bufs_a, offs, NULL, st12_0);
    h_vkCmdDraw(cb, 6, 1, 0, 0);
    h_vkCmdEndRenderPass(cb); run(cb); download(target, 4, 4, px);
    ok = 1;
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++)
        ok &= x >= 2 ? near(px + 4 * (y * 4 + x), 255, 0, 0, 255) : near(px + 4 * (y * 4 + x), 0, 0, 255, 255);
    CHECK(ok, "dynamic stride 12 draws the right half and dynamic stride 0 broadcasts vertex 0's red color");

    // Stride 24 on binding 0 (left half), then rebind with pStrides == NULL (must preserve stride 24!) + one-row scissor-with-count
    cb = new_cb(VK_COMMAND_BUFFER_LEVEL_PRIMARY);
    begin_pass(cb, rp, fb, 4, 4, VK_SUBPASS_CONTENTS_INLINE);
    h_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    h_vkCmdSetViewportWithCountEXT(cb, 1, &full); h_vkCmdSetScissorWithCountEXT(cb, 1, &row1);
    h_vkCmdSetCullModeEXT(cb, VK_CULL_MODE_NONE); h_vkCmdSetFrontFaceEXT(cb, VK_FRONT_FACE_COUNTER_CLOCKWISE);
    h_vkCmdSetPrimitiveTopologyEXT(cb, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    h_vkCmdBindVertexBuffers2EXT(cb, 0, 2, bufs_a, offs, NULL, st24_0);
    h_vkCmdBindVertexBuffers2EXT(cb, 0, 1, &vbb, &offs[0], NULL, NULL);   // pStrides == NULL preserves stride 24
    h_vkCmdDraw(cb, 6, 1, 0, 0);
    h_vkCmdEndRenderPass(cb); run(cb); download(target, 4, 4, px);
    ok = 1;
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++)
        ok &= (x < 2 && y == 1) ? near(px + 4 * (y * 4 + x), 255, 0, 0, 255) : near(px + 4 * (y * 4 + x), 0, 0, 255, 255);
    CHECK(ok, "vkCmdBindVertexBuffers2EXT with pStrides == NULL preserves dynamic stride 24 and with-count scissor clips to row 1");

    // Switching from a 2-viewport with-count draw to a 1-viewport regular dynamic pipeline in the same command buffer must not leak count 2
    if (max_viewports() > 1) {
        VkDynamicState reg_dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dy_reg = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = reg_dyn};
        VkPipelineViewportStateCreateInfo vsi1 = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
        VkShaderModule plain_vs = module_of_fn("t_vs", "0000000000000001");
        VkPipelineShaderStageCreateInfo plain_st[] = {stage(VK_SHADER_STAGE_VERTEX_BIT, plain_vs), stage(VK_SHADER_STAGE_FRAGMENT_BIT, fs)};
        VkGraphicsPipelineCreateInfo cr = ci; cr.stageCount = 2; cr.pStages = plain_st; cr.pVertexInputState = NULL; cr.pViewportState = &vsi1; cr.pDynamicState = &dy_reg;
        VkPipeline pr; h_vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &cr, NULL, &pr);
        const VkViewport two_vp[2] = {{0, 0, 2, 4, 0, 1}, {2, 0, 2, 4, 0, 1}};
        const VkRect2D two_sc[2] = {{{0, 0}, {2, 4}}, {{2, 0}, {2, 4}}};
        cb = new_cb(VK_COMMAND_BUFFER_LEVEL_PRIMARY);
        begin_pass(cb, rp, fb, 4, 4, VK_SUBPASS_CONTENTS_INLINE);
        h_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
        h_vkCmdSetViewportWithCountEXT(cb, 2, two_vp); h_vkCmdSetScissorWithCountEXT(cb, 2, two_sc);
        h_vkCmdSetCullModeEXT(cb, VK_CULL_MODE_NONE); h_vkCmdSetPrimitiveTopologyEXT(cb, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
        h_vkCmdBindVertexBuffers2EXT(cb, 0, 2, bufs_a, offs, NULL, st12_0);
        h_vkCmdDraw(cb, 6, 1, 0, 0);
        h_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pr);
        h_vkCmdSetViewport(cb, 0, 1, &full); h_vkCmdSetScissor(cb, 0, 1, &all);
        h_vkCmdDraw(cb, 3, 1, 0, 0);
        h_vkCmdEndRenderPass(cb); run(cb); download(target, 4, 4, px);
        CHECK(all_near(px, 16, 255, 128, 0, 255), "a 1-viewport pipeline after a 2-viewport with-count draw restores viewport/scissor count 1");
        h_vkDestroyPipeline(dev, pr, NULL); h_vkDestroyShaderModule(dev, plain_vs, NULL);
    }

    // 3. Dynamic cull mode and front face (left_quad triangles are wound CCW in Metal framebuffer space: (-1,-1),(0,-1),(-1,1) -> signed area > 0 -> CW/CCW tested directly)
    // First confirm which winding is front with CULL_MODE_FRONT_BIT + COUNTER_CLOCKWISE vs CLOCKWISE
    uint8_t px_ccw[64], px_cw[64], px_both[64];
    for (int mode = 0; mode < 3; mode++) {
        cb = new_cb(VK_COMMAND_BUFFER_LEVEL_PRIMARY);
        begin_pass(cb, rp, fb, 4, 4, VK_SUBPASS_CONTENTS_INLINE);
        h_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
        h_vkCmdSetViewportWithCountEXT(cb, 1, &full); h_vkCmdSetScissorWithCountEXT(cb, 1, &all);
        h_vkCmdSetPrimitiveTopologyEXT(cb, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
        h_vkCmdSetCullModeEXT(cb, mode == 2 ? VK_CULL_MODE_FRONT_AND_BACK : VK_CULL_MODE_FRONT_BIT);
        h_vkCmdSetFrontFaceEXT(cb, mode == 1 ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE);
        h_vkCmdBindVertexBuffers2EXT(cb, 0, 2, bufs_a, offs, NULL, st12_0);
        h_vkCmdDraw(cb, 6, 1, 0, 0);
        h_vkCmdEndRenderPass(cb); run(cb);
        download(target, 4, 4, mode == 0 ? px_ccw : mode == 1 ? px_cw : px_both);
    }
    int ccw_drawn = near(px_ccw + 4 * 2, 255, 0, 0, 255), cw_drawn = near(px_cw + 4 * 2, 255, 0, 0, 255);
    CHECK(ccw_drawn != cw_drawn, "vkCmdSetCullModeEXT and vkCmdSetFrontFaceEXT flip which winding is culled");
    CHECK(all_near(px_both, 16, 0, 0, 255, 255), "vkCmdSetCullModeEXT(VK_CULL_MODE_FRONT_AND_BACK) culls triangles");

    // 4. Dynamic primitive topology: TRIANGLE_STRIP (4 verts -> full target) and indexed TRIANGLE_FAN (4 verts -> full target)
    VkBuffer vstrip = new_buf(64, 0), vfan = new_buf(64, 0), fan_idx = new_buf(16, 0);
    const float strip_q[12] = {-1,-1,0.5f, 1,-1,0.5f, -1,1,0.5f, 1,1,0.5f};
    const float fan_q[12]   = {-1,-1,0.5f, 1,-1,0.5f, 1,1,0.5f, -1,1,0.5f};
    const uint32_t fi4[4] = {0, 1, 2, 3};
    memcpy(bytes(vstrip), strip_q, sizeof strip_q); memcpy(bytes(vfan), fan_q, sizeof fan_q); memcpy(bytes(fan_idx), fi4, sizeof fi4);
    VkBuffer bufs_strip[2] = {vstrip, vbc}, bufs_fan[2] = {vfan, vbc};
    cb = new_cb(VK_COMMAND_BUFFER_LEVEL_PRIMARY);
    begin_pass(cb, rp, fb, 4, 4, VK_SUBPASS_CONTENTS_INLINE);
    h_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    h_vkCmdSetViewportWithCountEXT(cb, 1, &full); h_vkCmdSetScissorWithCountEXT(cb, 1, &all);
    h_vkCmdSetCullModeEXT(cb, VK_CULL_MODE_NONE);
    h_vkCmdSetPrimitiveTopologyEXT(cb, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);
    h_vkCmdBindVertexBuffers2EXT(cb, 0, 2, bufs_strip, offs, NULL, st12_0);
    h_vkCmdDraw(cb, 4, 1, 0, 0);
    h_vkCmdEndRenderPass(cb); run(cb); download(target, 4, 4, px);
    CHECK(all_near(px, 16, 255, 0, 0, 255), "vkCmdSetPrimitiveTopologyEXT(TRIANGLE_STRIP) covers all 16 pixels from 4 vertices");

    cb = new_cb(VK_COMMAND_BUFFER_LEVEL_PRIMARY);
    begin_pass(cb, rp, fb, 4, 4, VK_SUBPASS_CONTENTS_INLINE);
    h_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    h_vkCmdSetViewportWithCountEXT(cb, 1, &full); h_vkCmdSetScissorWithCountEXT(cb, 1, &all);
    h_vkCmdSetCullModeEXT(cb, VK_CULL_MODE_NONE);
    h_vkCmdSetPrimitiveTopologyEXT(cb, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN);
    h_vkCmdBindVertexBuffers2EXT(cb, 0, 2, bufs_fan, offs, NULL, st12_0);
    h_vkCmdBindIndexBuffer(cb, fan_idx, 0, VK_INDEX_TYPE_UINT32);
    h_vkCmdDrawIndexed(cb, 4, 1, 0, 0, 0);
    h_vkCmdEndRenderPass(cb); run(cb); download(target, 4, 4, px);
    CHECK(all_near(px, 16, 255, 0, 0, 255), "vkCmdSetPrimitiveTopologyEXT(TRIANGLE_FAN) converts and draws an indexed fan");

    // 5. Dynamic depth test/write/compare, depth bounds enable, and stencil test/op (on a color + D32S8 render pass)
    VkAttachmentDescription dsatts[2] = {
        {.format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE},
        {.format = VK_FORMAT_D32_SFLOAT_S8_UINT, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
         .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE},
    };
    VkAttachmentReference cref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, dsref = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription dssp = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &cref, .pDepthStencilAttachment = &dsref};
    VkRenderPassCreateInfo dsrci = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 2, .pAttachments = dsatts, .subpassCount = 1, .pSubpasses = &dssp};
    VkRenderPass dsrp; h_vkCreateRenderPass(dev, &dsrci, NULL, &dsrp);
    VkImage dsc = new_img(4, 4, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    VkImage dsz = new_img(4, 4, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    VkImageView dsviews[2] = {new_view(dsc, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT),
                              new_view(dsz, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)};
    VkFramebufferCreateInfo dsfci = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = dsrp, .attachmentCount = 2, .pAttachments = dsviews, .width = 4, .height = 4, .layers = 1};
    VkFramebuffer dsfb; h_vkCreateFramebuffer(dev, &dsfci, NULL, &dsfb);
    VkStencilOpState st_base = {.failOp = VK_STENCIL_OP_KEEP, .passOp = VK_STENCIL_OP_KEEP, .depthFailOp = VK_STENCIL_OP_KEEP, .compareOp = VK_COMPARE_OP_ALWAYS,
                                .compareMask = 0xFF, .writeMask = 0xFF, .reference = 1};
    VkPipelineDepthStencilStateCreateInfo dsi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                 .depthTestEnable = VK_FALSE, .depthWriteEnable = VK_FALSE, .depthCompareOp = VK_COMPARE_OP_NEVER,
                                                 .depthBoundsTestEnable = VK_FALSE, .stencilTestEnable = VK_FALSE,
                                                 .front = st_base, .back = st_base, .minDepthBounds = 0.1f, .maxDepthBounds = 0.3f};
    VkDynamicState ds_dyn[] = {VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE, VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT, VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT,
                               VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
                               VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP};
    VkPipelineDynamicStateCreateInfo dy_ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 9, .pDynamicStates = ds_dyn};
    VkGraphicsPipelineCreateInfo cd = ci; cd.pDepthStencilState = &dsi; cd.pDynamicState = &dy_ds; cd.renderPass = dsrp;
    VkPipeline pds; h_vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &cd, NULL, &pds);
    CHECK(pds->pso != nil && (pds->eds & (EDS_DSS_MASK | EDS_DB_EN)) == (EDS_DSS_MASK | EDS_DB_EN),
          "pipeline with dynamic depth test/write/compare, depth bounds enable and stencil test/op is built");

    // Full-screen quads at z = 0.25 (near), 0.40 (mid), 0.75 (far), and green color buffer
    VkBuffer vnear = new_buf(96, 0), vmid = new_buf(96, 0), vfar = new_buf(96, 0), vgreen = new_buf(64, 0);
    const float qnear[18] = {-1,-1,0.25f, 1,-1,0.25f, -1,1,0.25f, 1,-1,0.25f, 1,1,0.25f, -1,1,0.25f};
    const float qmid[18]  = {-1,-1,0.40f, 1,-1,0.40f, -1,1,0.40f, 1,-1,0.40f, 1,1,0.40f, -1,1,0.40f};
    const float qfar[18]  = {-1,-1,0.75f, 1,-1,0.75f, -1,1,0.75f, 1,-1,0.75f, 1,1,0.75f, -1,1,0.75f};
    const float green4[4] = {0, 1, 0, 1};
    memcpy(bytes(vnear), qnear, sizeof qnear); memcpy(bytes(vmid), qmid, sizeof qmid); memcpy(bytes(vfar), qfar, sizeof qfar); memcpy(bytes(vgreen), green4, sizeof green4);
    VkClearValue dsclears[2] = {{.color = {{0, 0, 1, 1}}}, {.depthStencil = {0.5f, 0}}};
    VkRenderPassBeginInfo dsbi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = dsrp, .framebuffer = dsfb,
                                  .renderArea = {{0, 0}, {4, 4}}, .clearValueCount = 2, .pClearValues = dsclears};

    // Depth test: z=0.75 rejected against cleared 0.5 with LESS; depthTestEnable=FALSE lets it draw without writing depth (even with depthWriteEnable=TRUE)
    cb = new_cb(VK_COMMAND_BUFFER_LEVEL_PRIMARY);
    h_vkCmdBeginRenderPass(cb, &dsbi, VK_SUBPASS_CONTENTS_INLINE);
    h_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pds);
    h_vkCmdSetViewportWithCountEXT(cb, 1, &full); h_vkCmdSetScissorWithCountEXT(cb, 1, &all);
    h_vkCmdSetDepthBoundsTestEnableEXT(cb, VK_FALSE); h_vkCmdSetStencilTestEnableEXT(cb, VK_FALSE);
    h_vkCmdSetDepthTestEnableEXT(cb, VK_TRUE); h_vkCmdSetDepthWriteEnableEXT(cb, VK_FALSE); h_vkCmdSetDepthCompareOpEXT(cb, VK_COMPARE_OP_LESS);
    VkBuffer bf_far_r[2] = {vfar, vbc}, bf_near_r[2] = {vnear, vbc}, bf_mid_g[2] = {vmid, vgreen};
    h_vkCmdBindVertexBuffers2EXT(cb, 0, 2, bf_far_r, offs, NULL, st12_0);
    h_vkCmdDraw(cb, 6, 1, 0, 0);   // 0.75 < 0.5 is false -> rejected
    h_vkCmdEndRenderPass(cb); run(cb); download(dsc, 4, 4, px);
    CHECK(all_near(px, 16, 0, 0, 255, 255), "dynamic depthTestEnable + LESS rejects z=0.75 against cleared 0.5");

    // Depth write + compare op: draw red at z=0.25 writing depth, then green at z=0.40 rejected by LESS and accepted by GREATER
    cb = new_cb(VK_COMMAND_BUFFER_LEVEL_PRIMARY);
    h_vkCmdBeginRenderPass(cb, &dsbi, VK_SUBPASS_CONTENTS_INLINE);
    h_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pds);
    h_vkCmdSetViewportWithCountEXT(cb, 1, &full); h_vkCmdSetScissorWithCountEXT(cb, 1, &all);
    h_vkCmdSetDepthBoundsTestEnableEXT(cb, VK_FALSE); h_vkCmdSetStencilTestEnableEXT(cb, VK_FALSE);
    h_vkCmdSetDepthTestEnableEXT(cb, VK_TRUE); h_vkCmdSetDepthWriteEnableEXT(cb, VK_TRUE); h_vkCmdSetDepthCompareOpEXT(cb, VK_COMPARE_OP_LESS);
    h_vkCmdBindVertexBuffers2EXT(cb, 0, 2, bf_near_r, offs, NULL, st12_0);
    h_vkCmdDraw(cb, 6, 1, 0, 0);   // writes red and depth = 0.25
    h_vkCmdSetDepthWriteEnableEXT(cb, VK_FALSE);
    h_vkCmdBindVertexBuffers2EXT(cb, 0, 2, bf_mid_g, offs, NULL, st12_0);
    h_vkCmdDraw(cb, 6, 1, 0, 0);   // 0.40 < 0.25 is false -> stays red
    h_vkCmdEndRenderPass(cb); run(cb); download(dsc, 4, 4, px);
    CHECK(all_near(px, 16, 255, 0, 0, 255), "dynamic depthWriteEnable writes z=0.25 so z=0.40 fails LESS");

    cb = new_cb(VK_COMMAND_BUFFER_LEVEL_PRIMARY);
    h_vkCmdBeginRenderPass(cb, &dsbi, VK_SUBPASS_CONTENTS_INLINE);
    h_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pds);
    h_vkCmdSetViewportWithCountEXT(cb, 1, &full); h_vkCmdSetScissorWithCountEXT(cb, 1, &all);
    h_vkCmdSetDepthBoundsTestEnableEXT(cb, VK_FALSE); h_vkCmdSetStencilTestEnableEXT(cb, VK_FALSE);
    h_vkCmdSetDepthTestEnableEXT(cb, VK_TRUE); h_vkCmdSetDepthWriteEnableEXT(cb, VK_TRUE); h_vkCmdSetDepthCompareOpEXT(cb, VK_COMPARE_OP_LESS);
    h_vkCmdBindVertexBuffers2EXT(cb, 0, 2, bf_near_r, offs, NULL, st12_0);
    h_vkCmdDraw(cb, 6, 1, 0, 0);   // writes red and depth = 0.25
    h_vkCmdSetDepthCompareOpEXT(cb, VK_COMPARE_OP_GREATER);
    h_vkCmdBindVertexBuffers2EXT(cb, 0, 2, bf_mid_g, offs, NULL, st12_0);
    h_vkCmdDraw(cb, 6, 1, 0, 0);   // 0.40 > 0.25 is true -> green
    h_vkCmdEndRenderPass(cb); run(cb); download(dsc, 4, 4, px);
    CHECK(all_near(px, 16, 0, 255, 0, 255), "vkCmdSetDepthCompareOpEXT(GREATER) lets z=0.40 pass over z=0.25");

    // Depth test disabled must also disable depth writes even when depthWriteEnable is TRUE
    cb = new_cb(VK_COMMAND_BUFFER_LEVEL_PRIMARY);
    h_vkCmdBeginRenderPass(cb, &dsbi, VK_SUBPASS_CONTENTS_INLINE);
    h_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pds);
    h_vkCmdSetViewportWithCountEXT(cb, 1, &full); h_vkCmdSetScissorWithCountEXT(cb, 1, &all);
    h_vkCmdSetDepthBoundsTestEnableEXT(cb, VK_FALSE); h_vkCmdSetStencilTestEnableEXT(cb, VK_FALSE);
    h_vkCmdSetDepthTestEnableEXT(cb, VK_FALSE); h_vkCmdSetDepthWriteEnableEXT(cb, VK_TRUE); h_vkCmdSetDepthCompareOpEXT(cb, VK_COMPARE_OP_LESS);
    h_vkCmdBindVertexBuffers2EXT(cb, 0, 2, bf_near_r, offs, NULL, st12_0);
    h_vkCmdDraw(cb, 6, 1, 0, 0);   // depth test off -> draws red, must NOT write depth 0.25 (depth stays 0.5)
    h_vkCmdSetDepthTestEnableEXT(cb, VK_TRUE);
    h_vkCmdBindVertexBuffers2EXT(cb, 0, 2, bf_mid_g, offs, NULL, st12_0);
    h_vkCmdDraw(cb, 6, 1, 0, 0);   // 0.40 < 0.50 is true -> green (would have failed if 0.25 had been written)
    h_vkCmdEndRenderPass(cb); run(cb); download(dsc, 4, 4, px);
    CHECK(all_near(px, 16, 0, 255, 0, 255), "vkCmdSetDepthTestEnableEXT(FALSE) suppresses depth writes even when depthWriteEnable is TRUE");

    // Depth bounds test enable: static bounds [0.1, 0.3] are resolved even when static depthBoundsTestEnable is FALSE, and vkCmdSetDepthBoundsTestEnableEXT toggles live state
    CHECK(pds->db_ok && !pds->db_en && pds->dbk == DBND_TEST && pds->db[0] == 0.1f && pds->db[1] == 0.3f,
          "pipeline with dynamic depthBoundsTestEnable resolves static bounds [0.1, 0.3] even when statically disabled");
    {
        VkCommandBuffer dcb = new_cb(VK_COMMAND_BUFFER_LEVEL_PRIMARY);
        h_vkCmdSetDepthBoundsTestEnableEXT(dcb, VK_TRUE);
        rs_t rs_db = {0}; replay_ops(&rs_db, dcb);
        CHECK(rs_db.have_db_en && rs_db.db_en == 1, "vkCmdSetDepthBoundsTestEnableEXT(TRUE) enables dynamic depth bounds state");
        dcb = new_cb(VK_COMMAND_BUFFER_LEVEL_PRIMARY);
        h_vkCmdSetDepthBoundsTestEnableEXT(dcb, VK_FALSE);
        replay_ops(&rs_db, dcb);
        CHECK(rs_db.have_db_en && rs_db.db_en == 0, "vkCmdSetDepthBoundsTestEnableEXT(FALSE) disables dynamic depth bounds state");
    }
    if (mtl_depth_bounds_hw()) {
        cb = new_cb(VK_COMMAND_BUFFER_LEVEL_PRIMARY);
        h_vkCmdBeginRenderPass(cb, &dsbi, VK_SUBPASS_CONTENTS_INLINE);
        h_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pds);
        h_vkCmdSetViewportWithCountEXT(cb, 1, &full); h_vkCmdSetScissorWithCountEXT(cb, 1, &all);
        h_vkCmdSetDepthTestEnableEXT(cb, VK_FALSE); h_vkCmdSetStencilTestEnableEXT(cb, VK_FALSE);
        h_vkCmdSetDepthBoundsTestEnableEXT(cb, VK_TRUE);
        h_vkCmdBindVertexBuffers2EXT(cb, 0, 2, bf_near_r, offs, NULL, st12_0);
        h_vkCmdDraw(cb, 6, 1, 0, 0);   // stored depth 0.5 is outside [0.1, 0.3] -> rejected
        h_vkCmdEndRenderPass(cb); run(cb); download(dsc, 4, 4, px);
        CHECK(all_near(px, 16, 0, 0, 255, 255), "vkCmdSetDepthBoundsTestEnableEXT(TRUE) rejects stored depth 0.5 outside [0.1, 0.3]");
    }

    // Stencil test enable + stencil op: write stencil = 1 on right half (ALWAYS + REPLACE), then draw full-screen green with EQUAL (ref 1) -> green on right half only
    cb = new_cb(VK_COMMAND_BUFFER_LEVEL_PRIMARY);
    h_vkCmdBeginRenderPass(cb, &dsbi, VK_SUBPASS_CONTENTS_INLINE);
    h_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pds);
    h_vkCmdSetViewportWithCountEXT(cb, 1, &full); h_vkCmdSetScissorWithCountEXT(cb, 1, &all);
    h_vkCmdSetDepthTestEnableEXT(cb, VK_FALSE); h_vkCmdSetDepthBoundsTestEnableEXT(cb, VK_FALSE);
    h_vkCmdSetStencilTestEnableEXT(cb, VK_TRUE);
    h_vkCmdSetStencilOpEXT(cb, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS);
    h_vkCmdBindVertexBuffers2EXT(cb, 0, 2, bufs_a, offs, NULL, st12_0);
    h_vkCmdDraw(cb, 6, 1, 0, 0);   // right half -> red + stencil = 1
    h_vkCmdSetStencilOpEXT(cb, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_EQUAL);
    h_vkCmdBindVertexBuffers2EXT(cb, 0, 2, bf_mid_g, offs, NULL, st12_0);
    h_vkCmdDraw(cb, 6, 1, 0, 0);   // full screen -> green only where stencil == 1 (right half)
    h_vkCmdEndRenderPass(cb); run(cb); download(dsc, 4, 4, px);
    ok = 1;
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++)
        ok &= x >= 2 ? near(px + 4 * (y * 4 + x), 0, 255, 0, 255) : near(px + 4 * (y * 4 + x), 0, 0, 255, 255);
    CHECK(ok, "vkCmdSetStencilTestEnableEXT and vkCmdSetStencilOpEXT stamp and mask the right half only");

    h_vkDestroyPipeline(dev, pds, NULL); h_vkDestroyFramebuffer(dev, dsfb, NULL); h_vkDestroyRenderPass(dev, dsrp, NULL);
    h_vkDestroyImageView(dev, dsviews[0], NULL); h_vkDestroyImageView(dev, dsviews[1], NULL);
    h_vkDestroyImage(dev, dsc, NULL); h_vkDestroyImage(dev, dsz, NULL);
    h_vkDestroyBuffer(dev, vnear, NULL); h_vkDestroyBuffer(dev, vmid, NULL); h_vkDestroyBuffer(dev, vfar, NULL); h_vkDestroyBuffer(dev, vgreen, NULL);
    h_vkDestroyPipeline(dev, p, NULL);
    h_vkDestroyBuffer(dev, vba, NULL); h_vkDestroyBuffer(dev, vbb, NULL); h_vkDestroyBuffer(dev, vbc, NULL);
    h_vkDestroyBuffer(dev, vstrip, NULL); h_vkDestroyBuffer(dev, vfan, NULL); h_vkDestroyBuffer(dev, fan_idx, NULL);
    h_vkDestroyShaderModule(dev, vs, NULL); h_vkDestroyShaderModule(dev, pfs, NULL);
}

int main(void) {
    dev = &g_device; queue = &g_queue;
    @autoreleasepool {
        dev_init();
        NSError *err = nil;
        lib = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
                "vertex float4 t_vs(uint v [[vertex_id]]) { float2 p = float2((v << 1) & 2, v & 2); return float4(p * 2.0 - 1.0, 0.0, 1.0); }\n"   // one triangle over the whole target
                "vertex float4 t_fan(uint v [[vertex_id]]) { const float2 p[4] = {float2(-1,-1),float2(1,-1),float2(1,1),float2(-1,1)}; return float4(p[v],0,1); }\n"
                "struct Routed { float4 p [[position]]; uint i [[viewport_array_index]]; };\n"
                "vertex Routed vk_000000000000000b_vp1(uint v [[vertex_id]], uint i [[instance_id]]) { float2 p = float2((v << 1) & 2, v & 2); return {float4(p * 2.0 - 1.0, 0.0, 1.0), i}; }\n"
                "fragment float4 t_vp_fs(uint i [[viewport_array_index]]) { return i ? float4(0, 1, 0, 1) : float4(1, 0, 0, 1); }\n"
                "struct TPosColIn { float3 p [[attribute(0)]]; float4 c [[attribute(1)]]; };\n"
                "struct TPosColOut { float4 p [[position]]; float4 c; };\n"
                "vertex TPosColOut t_poscol_vs(TPosColIn in [[stage_in]]) { return {float4(in.p, 1.0), in.c}; }\n"
                "fragment float4 t_col_fs(TPosColOut in [[stage_in]]) { return in.c; }\n"
                "fragment float4 t_fs() { return float4(1.0, 0.5, 0.0, 1.0); }\n" options:nil error:&err];
        CHECK(lib != nil, "the test shaders compile: %s", err.localizedDescription.UTF8String);
        g_lib = lib;
        VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; h_vkCreateCommandPool(dev, &pci, NULL, &pool);
        VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = 1 << 20, .memoryTypeIndex = 1}; h_vkAllocateMemory(dev, &mai, NULL, &mem);
        VkShaderModule vs = module_of_fn("t_vs", "0000000000000001"), fs = module_of_fn("t_fs", "0000000000000002");
        CHECK(vs->fn && fs->fn, "the shader functions exist");

        // ---- Pipelines: requested and not available is refused, not requested is built ----
        VkRenderPass crp = color_pass(VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT), drp = depth_pass(VK_FORMAT_D32_SFLOAT);
        VkPipelineShaderStageCreateInfo ok_st[2] = {stage(VK_SHADER_STAGE_VERTEX_BIT, vs), stage(VK_SHADER_STAGE_FRAGMENT_BIT, fs)};
        VkPipeline good = make_pipe(crp, ok_st, 2, 0);
        CHECK(good->pso != nil, "a pipeline whose stages the library has is built");

        VkShaderModule nofs = module_of_fn(NULL, "0000000000000003");   // a fragment shader that is not in the library
        VkPipelineShaderStageCreateInfo lack_st[2] = {stage(VK_SHADER_STAGE_VERTEX_BIT, vs), stage(VK_SHADER_STAGE_FRAGMENT_BIT, nofs)};
        VkPipeline lacking = make_pipe(crp, lack_st, 2, 0);
        CHECK(lacking->pso == nil, "a pipeline with a requested but missing fragment function is refused");
        CHECK(!strcmp(lacking->keys, "0000000000000001 0000000000000003"), "the refused pipeline keeps its shaders' keys for the report (got '%s')", lacking->keys);

        VkShaderModule geom = module_of_fn(NULL, "0000000000000004");   // Metal has no geometry stage: no function
        VkPipelineShaderStageCreateInfo geo_st[3] = {stage(VK_SHADER_STAGE_VERTEX_BIT, vs), stage(VK_SHADER_STAGE_GEOMETRY_BIT, geom), stage(VK_SHADER_STAGE_FRAGMENT_BIT, fs)};
        VkPipeline withgeo = make_pipe(crp, geo_st, 3, 0);
        CHECK(withgeo->pso != nil, "a pipeline with a stage Metal has none of (geometry) is built without it, as before");

        VkPipelineShaderStageCreateInfo depth_st[1] = {stage(VK_SHADER_STAGE_VERTEX_BIT, vs)};
        VkPipeline depth_only = make_pipe(drp, depth_st, 1, 0);
        CHECK(depth_only->pso != nil, "a pipeline with no fragment stage at all (depth only) is built");

        VkImage target = new_img(4, 4, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        VkFramebuffer cfb = new_fb(crp, new_view(target, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT), 4, 4);
        VkImage depth = new_img(4, 4, VK_FORMAT_D32_SFLOAT, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
        VkFramebuffer dfb = new_fb(drp, new_view(depth, VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT), 4, 4);
        uint8_t px[64];

        uint64_t skip0 = g_n_skip, draw0 = g_n_draw;
        VkCommandBuffer cb = new_cb(VK_COMMAND_BUFFER_LEVEL_PRIMARY);
        begin_pass(cb, crp, cfb, 4, 4, VK_SUBPASS_CONTENTS_INLINE);
        h_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, lacking); h_vkCmdDraw(cb, 3, 1, 0, 0); h_vkCmdDraw(cb, 3, 1, 0, 0);
        h_vkCmdEndRenderPass(cb); run(cb);
        CHECK(g_n_skip - skip0 == 2, "the draws of the refused pipeline are counted as skipped (2, got %llu)", (unsigned long long)(g_n_skip - skip0));
        CHECK(g_n_draw == draw0, "and are not drawn");
        CHECK(lacking->told, "a refused pipeline is reported once");
        download(target, 4, 4, px);
        CHECK(all_near(px, 16, 0, 0, 255, 255), "a refused pipeline leaves the cleared target as it was: its draws are not drawn depth-only");

        skip0 = g_n_skip; draw0 = g_n_draw;
        cb = new_cb(VK_COMMAND_BUFFER_LEVEL_PRIMARY);
        begin_pass(cb, drp, dfb, 4, 4, VK_SUBPASS_CONTENTS_INLINE);
        h_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, depth_only); h_vkCmdDraw(cb, 3, 1, 0, 0);
        h_vkCmdEndRenderPass(cb); run(cb);
        CHECK(g_n_skip == skip0 && g_n_draw - draw0 == 1, "the depth-only pipeline's draw runs and is not counted as skipped");

        // ---- Buffer fill and update ----
        VkBuffer fb1 = new_buf(96, 0x11);
        cb = new_cb(VK_COMMAND_BUFFER_LEVEL_PRIMARY);
        h_vkCmdFillBuffer(cb, fb1, 0, 16, 0xAABBCCDDu);   // bytes that differ: Metal fills bytes, so the pattern comes from a copy
        h_vkCmdFillBuffer(cb, fb1, 16, 16, 0x5A5A5A5Au);  // one byte repeated: a Metal fill
        h_vkCmdFillBuffer(cb, fb1, 32, 32, 0);
        run(cb);
        uint8_t *f = bytes(fb1); int ok = 1;
        for (int i = 0; i < 16; i++) ok &= f[i] == (uint8_t[]){0xDD, 0xCC, 0xBB, 0xAA}[i & 3];
        CHECK(ok, "a four-byte fill pattern lands in little-endian order");
        for (int i = 16; i < 32; i++) ok &= f[i] == 0x5A;
        for (int i = 32; i < 64; i++) ok &= f[i] == 0;
        for (int i = 64; i < 96; i++) ok &= f[i] == 0x11;
        CHECK(ok, "a repeated-byte fill, a zero fill and the bytes outside every fill read back");
        VkBuffer fb2 = new_buf(32, 0x11);
        cb = new_cb(VK_COMMAND_BUFFER_LEVEL_PRIMARY); h_vkCmdFillBuffer(cb, fb2, 16, VK_WHOLE_SIZE, 0x01020304u); run(cb);
        f = bytes(fb2); ok = 1;
        for (int i = 0; i < 16; i++) ok &= f[i] == 0x11;
        for (int i = 16; i < 32; i++) ok &= f[i] == (uint8_t[]){0x04, 0x03, 0x02, 0x01}[i & 3];
        CHECK(ok, "VK_WHOLE_SIZE fills from the offset to the buffer's end");

        VkBuffer ub = new_buf(32, 0x11); uint8_t payload[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
        cb = new_cb(VK_COMMAND_BUFFER_LEVEL_PRIMARY); h_vkCmdUpdateBuffer(cb, ub, 4, sizeof payload, payload);
        memset(payload, 0xFF, sizeof payload);   // the data is copied when the command is recorded
        run(cb);
        f = bytes(ub); ok = 1;
        for (int i = 0; i < 32; i++) ok &= f[i] == (i >= 4 && i < 16 ? i - 3 : 0x11);
        CHECK(ok, "an update writes the bytes recorded, at its offset, and nothing around them");

        // ---- Resolve and blit ----
        VkImage ms = new_img(4, 4, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_4_BIT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        VkImage one = new_img(4, 4, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        VkRenderPass mrp = color_pass(VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_4_BIT);
        VkFramebuffer mfb = new_fb(mrp, new_view(ms, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT), 4, 4);
        uint8_t zero[64] = {0}; upload(one, 4, 4, zero);
        cb = new_cb(VK_COMMAND_BUFFER_LEVEL_PRIMARY);
        begin_pass(cb, mrp, mfb, 4, 4, VK_SUBPASS_CONTENTS_INLINE); h_vkCmdEndRenderPass(cb);   // the multisampled image is cleared to blue
        VkImageResolve rr = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .extent = {4, 4, 1}};
        h_vkCmdResolveImage(cb, ms, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, one, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rr);
        run(cb); download(one, 4, 4, px);
        CHECK(all_near(px, 16, 0, 0, 255, 255), "a resolve writes the multisampled image's samples into the single-sample image (first texel %u,%u,%u,%u)", px[0], px[1], px[2], px[3]);

        uint8_t src_px[64], blank[64] = {0};
        for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) memcpy(src_px + 4 * (y * 4 + x), (uint8_t[]){10 + x, 20 + y, 30, 255}, 4);
        VkImage bs = new_img(4, 4, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        VkImage bd = new_img(4, 4, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        upload(bs, 4, 4, src_px); upload(bd, 4, 4, blank);
        VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{1, 1, 0}, {3, 3, 1}}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 2, 0}, {2, 4, 1}}};
        cb = new_cb(VK_COMMAND_BUFFER_LEVEL_PRIMARY);
        h_vkCmdBlitImage(cb, bs, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, bd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_NEAREST); run(cb);
        download(bd, 4, 4, px); ok = 1;
        for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
            const uint8_t *p = px + 4 * (y * 4 + x), *want = x < 2 && y >= 2 ? src_px + 4 * ((y - 1) * 4 + x + 1) : blank;
            ok &= !memcmp(p, want, 4);
        }
        CHECK(ok, "a same-size blit copies the source rectangle to the destination one and touches no other texel");
        upload(bd, 4, 4, blank);
        VkImageBlit scaled = bl; scaled.dstOffsets[0] = (VkOffset3D){0, 0, 0}; scaled.dstOffsets[1] = (VkOffset3D){4, 4, 1};
        cb = new_cb(VK_COMMAND_BUFFER_LEVEL_PRIMARY);
        h_vkCmdBlitImage(cb, bs, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, bd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &scaled, VK_FILTER_LINEAR); run(cb);
        download(bd, 4, 4, px);
        ok = 1;
        for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++)
            ok &= near(px + 4 * (y * 4 + x), 11 + (x >= 2), 21 + (y >= 2), 30, 255);
        CHECK(ok, "a scaled linear blit fills the entire destination with interpolated source texels");

        // ---- Indirect draws ----
        VkBuffer args = new_buf(64, 0);
        VkDrawIndirectCommand *dic = (VkDrawIndirectCommand *)bytes(args); dic[0] = (VkDrawIndirectCommand){0, 1, 0, 0}; dic[1] = (VkDrawIndirectCommand){3, 1, 0, 0};
        cb = new_cb(VK_COMMAND_BUFFER_LEVEL_PRIMARY);
        begin_pass(cb, crp, cfb, 4, 4, VK_SUBPASS_CONTENTS_INLINE); h_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, good);
        h_vkCmdDrawIndirect(cb, args, 0, 1, sizeof *dic);   // no vertices
        h_vkCmdEndRenderPass(cb); run(cb); download(target, 4, 4, px);
        CHECK(all_near(px, 16, 0, 0, 255, 255), "an indirect draw of 0 vertices draws nothing");
        draw0 = g_n_draw;
        cb = new_cb(VK_COMMAND_BUFFER_LEVEL_PRIMARY);
        begin_pass(cb, crp, cfb, 4, 4, VK_SUBPASS_CONTENTS_INLINE); h_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, good);
        h_vkCmdDrawIndirect(cb, args, sizeof *dic, 1, sizeof *dic);   // the second command, at its offset: 3 vertices
        h_vkCmdEndRenderPass(cb); run(cb); download(target, 4, 4, px);
        CHECK(all_near(px, 16, 255, 128, 0, 255), "an indirect draw reads its arguments at the offset given (first texel %u,%u,%u,%u)", px[0], px[1], px[2], px[3]);
        CHECK(g_n_draw - draw0 == 1, "and is counted as a draw");

        VkBuffer iargs = new_buf(64, 0), idx = new_buf(16, 0);
        uint32_t *ix = (uint32_t *)bytes(idx); ix[0] = 0; ix[1] = 1; ix[2] = 2;
        VkDrawIndexedIndirectCommand *dii = (VkDrawIndexedIndirectCommand *)bytes(iargs); dii[0] = (VkDrawIndexedIndirectCommand){0, 1, 0, 0, 0}; dii[1] = (VkDrawIndexedIndirectCommand){3, 1, 0, 0, 0};
        cb = new_cb(VK_COMMAND_BUFFER_LEVEL_PRIMARY);
        begin_pass(cb, crp, cfb, 4, 4, VK_SUBPASS_CONTENTS_INLINE); h_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, good);
        h_vkCmdBindIndexBuffer(cb, idx, 0, VK_INDEX_TYPE_UINT32);
        h_vkCmdDrawIndexedIndirect(cb, iargs, sizeof *dii, 1, sizeof *dii);
        h_vkCmdEndRenderPass(cb); run(cb); download(target, 4, 4, px);
        CHECK(all_near(px, 16, 255, 128, 0, 255), "an indexed indirect draw reads its arguments and the index buffer (first texel %u,%u,%u,%u)", px[0], px[1], px[2], px[3]);
        test_indirect_fans(crp, cfb, target, fs);
        test_geometry_viewports(crp, cfb, target);
        test_extended_dynamic_state(crp, cfb, target, fs);

        // ---- Dynamic blend constants: the pipeline blends with CONSTANT_COLOR, the constants come from the command ----
        VkPipeline blended = make_pipe(crp, ok_st, 2, 1);
        CHECK(blended->pso != nil && blended->dyn_blend, "a pipeline with dynamic blend constants is built and takes them from the command");
        float bc[4] = {0.5f, 0.5f, 0.5f, 1.0f};
        cb = new_cb(VK_COMMAND_BUFFER_LEVEL_PRIMARY);
        begin_pass(cb, crp, cfb, 4, 4, VK_SUBPASS_CONTENTS_INLINE); h_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, blended);
        h_vkCmdSetBlendConstants(cb, bc); h_vkCmdDraw(cb, 3, 1, 0, 0);
        h_vkCmdEndRenderPass(cb); run(cb); download(target, 4, 4, px);
        CHECK(all_near(px, 16, 128, 64, 0, 255), "the dynamic blend constants (0.5) scale the shader's colour (first texel %u,%u,%u,%u)", px[0], px[1], px[2], px[3]);

        // ---- Secondary command buffers ----
        VkBuffer sb = new_buf(16, 0x11);
        VkCommandBuffer sec = new_cb(VK_COMMAND_BUFFER_LEVEL_SECONDARY);
        h_vkCmdFillBuffer(sec, sb, 0, 16, 0x77777777u); h_vkEndCommandBuffer(sec);
        cb = new_cb(VK_COMMAND_BUFFER_LEVEL_PRIMARY); h_vkCmdExecuteCommands(cb, 1, &sec); run(cb);
        f = bytes(sb); ok = 1; for (int i = 0; i < 16; i++) ok &= f[i] == 0x77;
        CHECK(ok, "a secondary command buffer's commands run where vkCmdExecuteCommands names it");
        VkCommandBuffer sec2 = new_cb(VK_COMMAND_BUFFER_LEVEL_SECONDARY);
        h_vkCmdBindPipeline(sec2, VK_PIPELINE_BIND_POINT_GRAPHICS, good); h_vkCmdDraw(sec2, 3, 1, 0, 0); h_vkEndCommandBuffer(sec2);
        cb = new_cb(VK_COMMAND_BUFFER_LEVEL_PRIMARY);
        begin_pass(cb, crp, cfb, 4, 4, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS); h_vkCmdExecuteCommands(cb, 1, &sec2); h_vkCmdEndRenderPass(cb); run(cb);
        download(target, 4, 4, px);
        CHECK(all_near(px, 16, 255, 128, 0, 255), "a draw recorded in a secondary buffer lands in the primary's render pass (first texel %u,%u,%u,%u)", px[0], px[1], px[2], px[3]);

        // ---- Events ----
        VkEventCreateInfo eci = {.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO}; VkEvent ev, ev2; h_vkCreateEvent(dev, &eci, NULL, &ev); h_vkCreateEvent(dev, &eci, NULL, &ev2);
        CHECK(h_vkGetEventStatus(dev, ev) == VK_EVENT_RESET, "a new event is reset");
        h_vkSetEvent(dev, ev); CHECK(h_vkGetEventStatus(dev, ev) == VK_EVENT_SET, "vkSetEvent sets it for the host");
        h_vkResetEvent(dev, ev); CHECK(h_vkGetEventStatus(dev, ev) == VK_EVENT_RESET, "vkResetEvent resets it");
        cb = new_cb(VK_COMMAND_BUFFER_LEVEL_PRIMARY); h_vkCmdSetEvent(cb, ev, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        CHECK(h_vkGetEventStatus(dev, ev) == VK_EVENT_RESET, "an event a command buffer sets stays reset until the submit runs");
        run(cb);
        CHECK(h_vkGetEventStatus(dev, ev) == VK_EVENT_SET, "vkCmdSetEvent sets it once its submit has completed");
        cb = new_cb(VK_COMMAND_BUFFER_LEVEL_PRIMARY); h_vkCmdResetEvent(cb, ev, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT); run(cb);
        CHECK(h_vkGetEventStatus(dev, ev) == VK_EVENT_RESET, "vkCmdResetEvent resets it");
        cb = new_cb(VK_COMMAND_BUFFER_LEVEL_PRIMARY); h_vkCmdSetEvent(cb, ev2, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT); h_vkCmdResetEvent(cb, ev2, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT); run(cb);
        CHECK(h_vkGetEventStatus(dev, ev2) == VK_EVENT_RESET, "a set followed by a reset in one submit leaves it reset");
        // vkCmdWaitEvents waits for nothing: a submit that waits on an event no one sets completes (the documented limit: a GPU wait on an event the host sets later is not honoured)
        VkEvent never; h_vkCreateEvent(dev, &eci, NULL, &never); VkBuffer wb = new_buf(16, 0);
        cb = new_cb(VK_COMMAND_BUFFER_LEVEL_PRIMARY);
        h_vkCmdWaitEvents(cb, 1, &never, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, NULL, 0, NULL, 0, NULL);
        h_vkCmdFillBuffer(cb, wb, 0, 16, 0x33333333u); run(cb);
        CHECK(bytes(wb)[0] == 0x33 && h_vkGetEventStatus(dev, never) == VK_EVENT_RESET, "a wait on an event nothing sets does not block the queue");
        h_vkDestroyEvent(dev, ev, NULL); h_vkDestroyEvent(dev, ev2, NULL); h_vkDestroyEvent(dev, never, NULL);

        // ---- Query copies ----
        VkQueryPoolCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 3}; VkQueryPool qp; h_vkCreateQueryPool(dev, &qci, NULL, &qp);
        VkBuffer q64 = new_buf(48, 0xEE), q32 = new_buf(24, 0xEE), consumed = new_buf(16, 0xCC);
        cb = new_cb(VK_COMMAND_BUFFER_LEVEL_PRIMARY);
        h_vkCmdResetQueryPool(cb, qp, 0, 3);
        h_vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, qp, 0); h_vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, qp, 1);   // query 2 is not written
        h_vkCmdCopyQueryPoolResults(cb, qp, 0, 3, q64, 0, 16, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        h_vkCmdCopyQueryPoolResults(cb, qp, 0, 3, q32, 0, 8, VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        VkMemoryBarrier dep = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT};
        h_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &dep, 0, NULL, 0, NULL);
        VkBufferCopy read_query = {0, 0, 16};
        h_vkCmdCopyBuffer(cb, q64, consumed, 1, &read_query);
        run(cb);
        uint64_t host[2]; h_vkGetQueryPoolResults(dev, qp, 0, 2, sizeof host, host, 8, VK_QUERY_RESULT_64_BIT);
        const uint64_t *r64 = (const uint64_t *)bytes(q64); const uint32_t *r32 = (const uint32_t *)bytes(q32);
        CHECK(r64[0] && r64[0] == host[0] && r64[2] && r64[2] == host[1] && r64[1] == 1 && r64[3] == 1, "copied 64-bit timestamps equal what vkGetQueryPoolResults gives, with availability 1 (%llu %llu / %llu %llu)",
              (unsigned long long)r64[0], (unsigned long long)r64[1], (unsigned long long)host[0], (unsigned long long)r64[2]);
        CHECK(r64[5] == 0 && r64[4] == 0xEEEEEEEEEEEEEEEEull, "a query that was not written is unavailable and its value is not written (a result is written only when available)");
        CHECK(r32[0] == (uint32_t)host[0] && r32[1] == 1 && r32[2] == (uint32_t)host[1] && r32[3] == 1 && r32[4] == 0xEEEEEEEEu && r32[5] == 0, "the 32-bit copy holds the low words and availability");
        const uint64_t *gpu = (const uint64_t *)bytes(consumed);
        CHECK(gpu[0] == host[0] && gpu[1] == 1, "a later GPU command sees the query value and availability in the same submit (availability %#llx)",
              (unsigned long long)gpu[1]);
    }
    printf("Vulkan commands: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
