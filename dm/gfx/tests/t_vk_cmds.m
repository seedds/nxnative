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
