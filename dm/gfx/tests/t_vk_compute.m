// Vulkan compute and dispatch behavior through the layer's own command replay. T19, the geometry-stage pipeline policy, remains covered by t_vk_cmds.m.
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

static VkDevice dev; static VkQueue queue; static VkCommandPool cmd_pool; static VkDescriptorPool desc_pool;
static VkDeviceMemory memory; static VkDeviceSize mem_top;
static id<MTLLibrary> lib;

static uint8_t *bytes(VkBuffer b) { return (uint8_t *)mem_buf(b->mem).contents + b->off; }
static VkBuffer new_buf(VkDeviceSize size, VkBufferUsageFlags usage, int fill) {
    VkBufferCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
        .usage = usage | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT};
    VkBuffer b = NULL; VkResult r = h_vkCreateBuffer(dev, &ci, NULL, &b);
    CHECK(r == VK_SUCCESS && b, "buffer of %llu bytes created", (unsigned long long)size);
    if (!b) return NULL;
    r = h_vkBindBufferMemory(dev, b, memory, mem_top);
    CHECK(r == VK_SUCCESS, "buffer memory bound at %llu", (unsigned long long)mem_top);
    mem_top += (size + 255) & ~(VkDeviceSize)255;
    memset(bytes(b), fill, size);
    return b;
}
static VkCommandBuffer new_cb(void) {
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cmd_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer cb = NULL; h_vkAllocateCommandBuffers(dev, &ai, &cb);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    h_vkBeginCommandBuffer(cb, &bi); return cb;
}
static void run(VkCommandBuffer cb) {
    CHECK(h_vkEndCommandBuffer(cb) == VK_SUCCESS, "command buffer ended");
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    CHECK(h_vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS, "command buffer submitted");
    CHECK(h_vkQueueWaitIdle(queue) == VK_SUCCESS, "queue completed the submission");
}
static VkImage new_img(VkImageType type, VkFormat fmt, uint32_t w, uint32_t h, uint32_t d, VkImageUsageFlags usage) {
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = type, .format = fmt, .extent = {w, h, d},
        .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage};
    VkImage im = NULL; VkResult r = h_vkCreateImage(dev, &ci, NULL, &im);
    CHECK(r == VK_SUCCESS && im && im->tex, "image %ux%ux%u format %d created", w, h, d, fmt); return im;
}
static VkImageView new_view(VkImage im, VkFormat fmt, VkImageViewType type) {
    VkImageViewCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im, .viewType = type, .format = fmt,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView v = NULL; h_vkCreateImageView(dev, &ci, NULL, &v); return v;
}
static VkRenderPass color_pass(VkFormat fmt) {
    VkAttachmentDescription at = {.format = fmt, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
    VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sp = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &ref};
    VkRenderPassCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &at,
        .subpassCount = 1, .pSubpasses = &sp};
    VkRenderPass rp = NULL; h_vkCreateRenderPass(dev, &ci, NULL, &rp); return rp;
}
static VkFramebuffer new_fb(VkRenderPass rp, VkImageView v, uint32_t w, uint32_t h) {
    VkFramebufferCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = 1,
        .pAttachments = &v, .width = w, .height = h, .layers = 1};
    VkFramebuffer fb = NULL; h_vkCreateFramebuffer(dev, &ci, NULL, &fb); return fb;
}
static void begin_pass(VkCommandBuffer cb, VkRenderPass rp, VkFramebuffer fb, uint32_t w, uint32_t h) {
    VkClearValue clear = {.color = {{0, 0, 0, 0}}};
    VkRenderPassBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb,
        .renderArea = {{0, 0}, {w, h}}, .clearValueCount = 1, .pClearValues = &clear};
    h_vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
}
static void read_image(VkImage im, uint32_t w, uint32_t h, uint32_t d, uint32_t bpp, void *out) {
    VkDeviceSize size = (VkDeviceSize)w * h * d * bpp;
    VkBuffer b = new_buf(size, 0, 0xEE);
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, d}};
    VkCommandBuffer cb = new_cb(); h_vkCmdCopyImageToBuffer(cb, im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b, 1, &copy); run(cb);
    memcpy(out, bytes(b), size);
}
static void upload_image(VkImage im, uint32_t w, uint32_t h, uint32_t d, uint32_t bpp, const void *data) {
    VkDeviceSize size = (VkDeviceSize)w * h * d * bpp;
    VkBuffer b = new_buf(size, 0, 0); memcpy(bytes(b), data, size);
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, d}};
    VkCommandBuffer cb = new_cb(); h_vkCmdCopyBufferToImage(cb, b, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy); run(cb);
}
static VkDescriptorSetLayout new_set_layout(const VkDescriptorSetLayoutBinding *bindings, uint32_t n) {
    VkDescriptorSetLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = n, .pBindings = bindings};
    VkDescriptorSetLayout layout = NULL; CHECK(h_vkCreateDescriptorSetLayout(dev, &ci, NULL, &layout) == VK_SUCCESS, "descriptor-set layout created"); return layout;
}
static VkDescriptorSet new_set(VkDescriptorSetLayout layout) {
    VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = desc_pool,
        .descriptorSetCount = 1, .pSetLayouts = &layout};
    VkDescriptorSet set = NULL; CHECK(h_vkAllocateDescriptorSets(dev, &ai, &set) == VK_SUCCESS, "descriptor set allocated"); return set;
}
static void write_buffer(VkDescriptorSet set, uint32_t binding, VkDescriptorType type, VkBuffer b, VkDeviceSize off, VkDeviceSize size) {
    VkDescriptorBufferInfo info = {b, off, size};
    VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = binding,
        .descriptorCount = 1, .descriptorType = type, .pBufferInfo = &info};
    h_vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);
}
static VkPipelineLayout new_pl(const VkDescriptorSetLayout *sets, uint32_t nset, uint32_t pc_size, VkShaderStageFlags pc_stages) {
    VkPushConstantRange range = {.stageFlags = pc_stages, .offset = 0, .size = pc_size};
    VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = nset,
        .pSetLayouts = sets, .pushConstantRangeCount = pc_size ? 1u : 0u, .pPushConstantRanges = pc_size ? &range : NULL};
    VkPipelineLayout layout = NULL; CHECK(h_vkCreatePipelineLayout(dev, &ci, NULL, &layout) == VK_SUCCESS, "pipeline layout created"); return layout;
}
static VkShaderModule module(const char *name, const char *key, const shinfo *si) {
    VkShaderModule m = zalloc(sizeof *m); strlcpy(m->key, key, sizeof m->key); m->si = si; m->fn = [lib newFunctionWithName:@(name)];
    CHECK(m->fn != nil, "Metal shader %s exists", name); return m;
}
static VkPipelineShaderStageCreateInfo stage(VkShaderStageFlagBits bit, VkShaderModule m) {
    return (VkPipelineShaderStageCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = bit, .module = m, .pName = "main"};
}
static VkPipeline compute_pipe(VkPipelineLayout layout, VkShaderModule m) {
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .stage = stage(VK_SHADER_STAGE_COMPUTE_BIT, m), .layout = layout};
    VkPipeline p = NULL; CHECK(h_vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &p) == VK_SUCCESS && p, "compute pipeline created"); return p;
}
static VkPipeline graphics_pipe(VkRenderPass rp, VkPipelineLayout layout, VkShaderModule vs, VkShaderModule fs, uint32_t w, uint32_t h) {
    VkPipelineShaderStageCreateInfo stages[2] = {stage(VK_SHADER_STAGE_VERTEX_BIT, vs), stage(VK_SHADER_STAGE_FRAGMENT_BIT, fs)};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkViewport vp = {0, 0, (float)w, (float)h, 0, 1}; VkRect2D sc = {{0, 0}, {w, h}};
    VkPipelineViewportStateCreateInfo vpstate = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState ba = {.colorWriteMask = 0xF};
    VkPipelineColorBlendStateCreateInfo blend = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &ba};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = stages,
        .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vpstate, .pRasterizationState = &rs,
        .pMultisampleState = &ms, .pColorBlendState = &blend, .layout = layout, .renderPass = rp};
    VkPipeline p = NULL; CHECK(h_vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &p) == VK_SUCCESS && p, "graphics pipeline created"); return p;
}

static void test_t1(void) {
    // T1: wrong group geometry or dispatch binding leaves the storage array with missing or misplaced global IDs.
    VkDescriptorSetLayoutBinding b = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayout sl = new_set_layout(&b, 1); VkPipelineLayout pl = new_pl(&sl, 1, 0, 0);
    VkBuffer out = new_buf(32 * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0xCC); VkDescriptorSet ds = new_set(sl);
    write_buffer(ds, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, out, 0, out->size);
    static const shinfo si = {.key = "0000000000000001", .comp = 1, .nres = 1, .wg = {8, 1, 1}, .res = {{'b', 0, 0, 1, 0}}};
    VkPipeline p = compute_pipe(pl, module("k_gid", si.key, &si)); uint64_t before = g_n_disp;
    VkCommandBuffer cb = new_cb(); h_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    h_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL); h_vkCmdDispatch(cb, 4, 1, 1); run(cb);
    CHECK(g_n_disp - before == 1, "one direct dispatch is counted");
    int ok = 1; for (uint32_t i = 0; i < 32; i++) ok &= ((uint32_t *)bytes(out))[i] == i;
    CHECK(ok, "four groups of eight write global IDs 0 through 31");
}

static void test_t2(void) {
    // T2: if compute push constants are not bound at their table slot, threads beyond the requested vertex count overwrite sentinels.
    VkDescriptorSetLayoutBinding b = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayout sl = new_set_layout(&b, 1); VkPipelineLayout pl = new_pl(&sl, 1, 16, VK_SHADER_STAGE_COMPUTE_BIT);
    VkBuffer out = new_buf(64 * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0xA5); VkDescriptorSet ds = new_set(sl);
    write_buffer(ds, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, out, 0, out->size);
    static const shinfo si = {.key = "0000000000000002", .comp = 1, .nres = 2, .wg = {8, 1, 1}, .res = {{'b', 0, 0, 1, 0}, {'p', 0, 1, 1, 0}}};
    VkShaderModule sm = module("k_limit", si.key, &si); VkPipeline p = compute_pipe(pl, sm); h_vkDestroyShaderModule(dev, sm, NULL);
    uint32_t bound = 35;
    VkCommandBuffer cb = new_cb(); h_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    h_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
    h_vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof bound, &bound); h_vkCmdDispatch(cb, 8, 1, 1); run(cb);
    int ok = 1; for (uint32_t i = 0; i < 35; i++) ok &= ((uint32_t *)bytes(out))[i] == i;
    for (uint32_t i = 35; i < 64; i++) ok &= ((uint32_t *)bytes(out))[i] == 0xA5A5A5A5u;
    CHECK(ok, "the push-constant bound writes only the first 35 elements");
    h_vkDestroyPipeline(dev, p, NULL);
}

static void test_t3(void) {
    // T3: a local size above Metal's kernel limit must be refused; the dispatch is skipped and counted.
    VkPipelineLayout pl = new_pl(NULL, 0, 0, 0);
    static const shinfo si = {.key = "0000000000000003", .comp = 1, .wg = {2048, 1, 1}};
    VkShaderModule sm = module("k_noop", si.key, &si); VkPipeline p = compute_pipe(pl, sm); h_vkDestroyShaderModule(dev, sm, NULL);
    uint64_t before = g_n_disp, skipped = g_n_skip_disp;
    CHECK(p->cpso == nil, "the 2048-thread local size is refused");
    VkCommandBuffer cb = new_cb(); h_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p); h_vkCmdDispatch(cb, 1, 1, 1); run(cb);
    h_vkDestroyPipeline(dev, p, NULL);
    CHECK(g_n_disp == before, "a refused local size does not reach Metal");
    CHECK(g_n_skip_disp == skipped + 1, "a refused compute pipeline increments the skipped-dispatch count");

}

static void test_tier1_refusal(void) {
    // A Tier1 device cannot build an argument-buffer pipeline; its dispatch must be refused and counted.
    VkPipelineLayout pl = new_pl(NULL, 0, 0, 0);
    static const shinfo si = {.key = "0000000000000020", .comp = 1, .nres = 1, .wg = {1, 1, 1}, .res = {{'B', 0, 0, 1, 0}}};
    VkShaderModule sm = module("k_tier1_arg", si.key, &si); MTLArgumentBuffersTier tier = g_ab_tier;
    g_ab_tier = MTLArgumentBuffersTier1; VkPipeline p = compute_pipe(pl, sm); g_ab_tier = tier;
    h_vkDestroyShaderModule(dev, sm, NULL);
    uint64_t skipped = g_n_skip_disp;
    CHECK(p->cpso == nil, "Tier1 refuses an argument-buffer compute pipeline");
    VkCommandBuffer cb = new_cb(); h_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p); h_vkCmdDispatch(cb, 1, 1, 1); run(cb);
    CHECK(g_n_skip_disp == skipped + 1, "Tier1-refused compute dispatch is counted as skipped");
    h_vkDestroyPipeline(dev, p, NULL);
}

static void test_t4(void) {
    // T4: missing encoder hazard tracking or wrong compute/render order leaves stale or half-written vertex-pulling data across the 50 updates.
    VkDescriptorSetLayoutBinding b = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT};
    VkDescriptorSetLayout sl = new_set_layout(&b, 1); VkPushConstantRange pc = {.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = 16};
    VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &sl,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &pc}; VkPipelineLayout pl = NULL; h_vkCreatePipelineLayout(dev, &plci, NULL, &pl);
    VkBuffer vertices = new_buf(3 * 32, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0); VkDescriptorSet ds = new_set(sl);
    write_buffer(ds, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, vertices, 0, vertices->size);
    static const shinfo csi = {.key = "0000000000000004", .comp = 1, .nres = 2, .wg = {1, 1, 1}, .res = {{'b', 0, 0, 1, 0}, {'p', 0, 1, 1, 0}}};
    static const shinfo vsi = {.key = "0000000000000005", .nres = 1, .res = {{'b', 0, 0, 1, 0}}};
    static const shinfo fsi = {.key = "0000000000000006", .frag = 1};
    VkShaderModule csm = module("k_t4_comp", csi.key, &csi); VkPipeline cp = compute_pipe(pl, csm); h_vkDestroyShaderModule(dev, csm, NULL);
    VkRenderPass rp = color_pass(VK_FORMAT_R8G8B8A8_UNORM); VkImage target = new_img(VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, 4, 4, 1,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    VkFramebuffer fb = new_fb(rp, new_view(target, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_VIEW_TYPE_2D), 4, 4);
    VkShaderModule vsm = module("k_t4_vs", vsi.key, &vsi), fsm = module("k_t4_fs", fsi.key, &fsi);
    VkPipeline gp = graphics_pipe(rp, pl, vsm, fsm, 4, 4); h_vkDestroyShaderModule(dev, vsm, NULL); h_vkDestroyShaderModule(dev, fsm, NULL);
    for (uint32_t i = 0; i < 50; i++) {
        float color[4] = {(i & 1) ? 0.0f : 1.0f, (i & 2) ? 1.0f : 0.0f, (i & 4) ? 1.0f : 0.0f, 1.0f};
        VkCommandBuffer cb = new_cb(); h_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cp);
        h_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
        h_vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof color, color); h_vkCmdDispatch(cb, 3, 1, 1);
        begin_pass(cb, rp, fb, 4, 4); h_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gp);
        h_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &ds, 0, NULL); h_vkCmdDraw(cb, 3, 1, 0, 0); h_vkCmdEndRenderPass(cb); run(cb);
        uint8_t px[64]; read_image(target, 4, 4, 1, 4, px);
        int ok = 1; for (int k = 0; k < 16; k++) ok &= px[4 * k] == (color[0] ? 255 : 0) && px[4 * k + 1] == (color[1] ? 255 : 0) && px[4 * k + 2] == (color[2] ? 255 : 0) && px[4 * k + 3] == 255;
        CHECK(ok, "iteration %u draws current vertices, got %u,%u,%u,%u expected %u,%u,%u,255", i, px[0], px[1], px[2], px[3], color[0] ? 255 : 0, color[1] ? 255 : 0, color[2] ? 255 : 0);
    }
    h_vkDestroyBuffer(dev, vertices, NULL);   // the descriptor set still names it, but nothing uses the set again
}

static void test_t5(void) {
    // T5: without ending compute before resuming the render pass, the second draw is lost or the first half is cleared instead of loaded.
    VkDescriptorSetLayoutBinding b = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT};
    VkDescriptorSetLayout sl = new_set_layout(&b, 1); VkPushConstantRange pc = {.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT, .offset = 0, .size = 32};
    VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &sl,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &pc}; VkPipelineLayout pl = NULL; h_vkCreatePipelineLayout(dev, &plci, NULL, &pl);
    VkBuffer colors = new_buf(2 * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0); VkDescriptorSet ds = new_set(sl);
    write_buffer(ds, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, colors, 0, colors->size);
    static const shinfo csi = {.key = "0000000000000007", .comp = 1, .nres = 2, .wg = {1, 1, 1}, .res = {{'b', 0, 0, 1, 0}, {'p', 0, 1, 1, 0}}};
    static const shinfo vsi = {.key = "0000000000000008", .nres = 2, .res = {{'b', 0, 0, 1, 0}, {'p', 0, 1, 1, 0}}};
    static const shinfo fsi = {.key = "0000000000000009", .frag = 1};
    VkPipeline cp = compute_pipe(pl, module("k_t5_comp", csi.key, &csi));
    VkRenderPass rp = color_pass(VK_FORMAT_R8G8B8A8_UNORM); VkImage target = new_img(VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, 12, 4, 1,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    VkFramebuffer fb = new_fb(rp, new_view(target, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_VIEW_TYPE_2D), 12, 4);
    VkPipeline gp = graphics_pipe(rp, pl, module("k_t5_vs", vsi.key, &vsi), module("k_t5_fs", fsi.key, &fsi), 12, 4);
    VkCommandBuffer cb = new_cb(); begin_pass(cb, rp, fb, 12, 4); h_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gp);
    h_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &ds, 0, NULL);
    for (uint32_t i = 0; i < 2; i++) {
        struct { uint32_t index, area, pad[2]; float color[4]; } params = {.index = i, .area = i};
        params.color[i ? 2 : 0] = 1.0f; params.color[3] = 1.0f;
        h_vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof params, &params);
        h_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cp); h_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
        h_vkCmdDispatch(cb, 1, 1, 1); h_vkCmdDraw(cb, 3, 1, 0, 0);
    }
    h_vkCmdEndRenderPass(cb); run(cb);
    uint8_t px[12 * 4 * 4]; read_image(target, 12, 4, 1, 4, px);
    CHECK(px[(3 * 12 + 2) * 4] == 255 && px[(3 * 12 + 2) * 4 + 2] == 0,
        "the first draw remains in the resumed pass: left %u,%u,%u,%u, right %u,%u,%u,%u",
        px[(3 * 12 + 2) * 4], px[(3 * 12 + 2) * 4 + 1], px[(3 * 12 + 2) * 4 + 2], px[(3 * 12 + 2) * 4 + 3],
        px[(3 * 12 + 9) * 4], px[(3 * 12 + 9) * 4 + 1], px[(3 * 12 + 9) * 4 + 2], px[(3 * 12 + 9) * 4 + 3]);
    CHECK(px[(3 * 12 + 9) * 4] == 0 && px[(3 * 12 + 9) * 4 + 2] == 255, "the second dispatch and draw land in the other half: %u,%u,%u,%u",
        px[(3 * 12 + 9) * 4], px[(3 * 12 + 9) * 4 + 1], px[(3 * 12 + 9) * 4 + 2], px[(3 * 12 + 9) * 4 + 3]);
    h_vkDestroyPipeline(dev, gp, NULL); h_vkDestroyPipeline(dev, cp, NULL);
}

static void test_t6(void) {
    // T6: two consecutive dispatches must share one compute encoder and preserve the producer-consumer order.
    VkDescriptorSetLayoutBinding b = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayout sl = new_set_layout(&b, 1); VkPipelineLayout pl = new_pl(&sl, 1, 0, 0);
    VkBuffer data = new_buf(sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0); VkDescriptorSet ds = new_set(sl);
    write_buffer(ds, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, data, 0, data->size);
    static const shinfo asi = {.key = "0000000000000010", .comp = 1, .nres = 1, .wg = {1, 1, 1}, .res = {{'b', 0, 0, 1, 0}}};
    static const shinfo bsi = {.key = "0000000000000011", .comp = 1, .nres = 1, .wg = {1, 1, 1}, .res = {{'b', 0, 0, 1, 0}}};
    VkPipeline a = compute_pipe(pl, module("k_t6_a", asi.key, &asi)), bpipe = compute_pipe(pl, module("k_t6_b", bsi.key, &bsi));
    uint64_t encoders = g_n_comp_enc;
    VkCommandBuffer cb = new_cb(); h_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
    h_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, a); h_vkCmdDispatch(cb, 1, 1, 1);
    h_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, bpipe); h_vkCmdDispatch(cb, 1, 1, 1); run(cb);
    CHECK(g_n_comp_enc - encoders == 1, "two dispatches in one command buffer create one compute encoder");
    CHECK(*(uint32_t *)bytes(data) == 42, "the second dispatch reads 41 written by the first");
}

static void test_t7(void) {
    // T7: binding a compute set after the graphics set at the same index must not make the later draw read the compute set.
    VkDescriptorSetLayoutBinding b = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorSetLayout sl = new_set_layout(&b, 1); VkDescriptorSetLayout layouts[4] = {NULL, NULL, NULL, sl}; VkPipelineLayout pl = new_pl(layouts, 4, 0, 0);
    VkBuffer cdata = new_buf(sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0), gdata = new_buf(sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0);
    *(uint32_t *)bytes(cdata) = 1; *(uint32_t *)bytes(gdata) = 7;
    VkDescriptorSet cset = new_set(sl), gset = new_set(sl); write_buffer(cset, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, cdata, 0, cdata->size);
    write_buffer(gset, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, gdata, 0, gdata->size);
    static const shinfo csi = {.key = "0000000000000012", .comp = 1, .wg = {1, 1, 1}};
    static const shinfo vsi = {.key = "0000000000000013"};
    static const shinfo fsi = {.key = "0000000000000014", .frag = 1, .nres = 1, .res = {{'b', 3, 0, 1, 0}}};
    VkPipeline cp = compute_pipe(pl, module("k_noop", csi.key, &csi));
    VkRenderPass rp = color_pass(VK_FORMAT_R8G8B8A8_UNORM); VkImage target = new_img(VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, 4, 4, 1,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    VkFramebuffer fb = new_fb(rp, new_view(target, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_VIEW_TYPE_2D), 4, 4);
    VkPipeline gp = graphics_pipe(rp, pl, module("k_fullscreen_vs", vsi.key, &vsi), module("k_t7_fs", fsi.key, &fsi), 4, 4);
    uint64_t dispatches = g_n_disp;
    VkCommandBuffer cb = new_cb(); begin_pass(cb, rp, fb, 4, 4); h_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gp);
    h_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 3, 1, &gset, 0, NULL);
    h_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cp);
    h_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 3, 1, &cset, 0, NULL); h_vkCmdDispatch(cb, 1, 1, 1);
    h_vkCmdDraw(cb, 3, 1, 0, 0); h_vkCmdEndRenderPass(cb); run(cb);
    uint8_t px[64]; read_image(target, 4, 4, 1, 4, px);
    CHECK(g_n_disp - dispatches == 1, "the compute dispatch after set 3 binding runs");
    CHECK(px[1] == 255 && px[0] == 0, "graphics set 3 pixel is %u,%u,%u,%u, expected green", px[0], px[1], px[2], px[3]);
}

static void test_format_storage_features(void) {
    // The storage-image feature must follow Metal's read-write tier, not be advertised for every colour or compressed format.
    VkFormatProperties p;
    h_vkGetPhysicalDeviceFormatProperties(&g_pdev, VK_FORMAT_R8G8B8A8_UNORM, &p);
    CHECK((p.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0, "RGBA8 UNORM supports storage images on this tier-2 device");
    h_vkGetPhysicalDeviceFormatProperties(&g_pdev, VK_FORMAT_R32_UINT, &p);
    CHECK((p.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0, "R32 UINT supports storage images on tier 1 and tier 2");
    h_vkGetPhysicalDeviceFormatProperties(&g_pdev, VK_FORMAT_R32G32B32A32_UINT, &p);
    CHECK(((p.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0) == (g_rw_tex_tier == MTLReadWriteTextureTier2), "RGBA32 UINT storage support matches Metal tier 2");
    h_vkGetPhysicalDeviceFormatProperties(&g_pdev, VK_FORMAT_R8G8B8A8_SRGB, &p);
    CHECK(!(p.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT), "sRGB formats are not advertised for storage");
    h_vkGetPhysicalDeviceFormatProperties(&g_pdev, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, &p);
    CHECK(!(p.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT), "compressed formats are not advertised for storage");
}

static void test_t8(void) {
    // T8: a missing storage usage bit or a misbound texel buffer changes the 3D cluster texel copied back for every voxel.
    VkDescriptorSetLayoutBinding bindings[2] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
    };
    VkDescriptorSetLayout sl = new_set_layout(bindings, 2); VkPipelineLayout pl = new_pl(&sl, 1, 0, 0);
    VkImage im = new_img(VK_IMAGE_TYPE_3D, VK_FORMAT_R32G32B32A32_UINT, 8, 8, 4, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    CHECK(im->tex.usage & MTLTextureUsageShaderWrite, "the layer creates the 3D storage image with Metal shader-write usage");
    VkImageView view = new_view(im, VK_FORMAT_R32G32B32A32_UINT, VK_IMAGE_VIEW_TYPE_3D);
    VkBuffer input = new_buf(8 * 8 * 4 * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT, 0);
    for (uint32_t i = 0; i < 8 * 8 * 4; i++) ((uint32_t *)bytes(input))[i] = i * 3 + 7;
    VkBufferViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, .buffer = input, .format = VK_FORMAT_R32_UINT, .offset = 0, .range = input->size};
    VkBufferView texel = NULL; CHECK(h_vkCreateBufferView(dev, &vci, NULL, &texel) == VK_SUCCESS && texel && bview_tex(texel), "R32 UINT storage texel view created");
    VkDescriptorSet ds = new_set(sl); VkDescriptorImageInfo ii = {NULL, view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[2] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &ii},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, .pTexelBufferView = &texel},
    };
    h_vkUpdateDescriptorSets(dev, 2, w, 0, NULL);
    static const shinfo si = {.key = "0000000000000015", .comp = 1, .nres = 2, .wg = {1, 1, 1}, .res = {{'t', 0, 0, 1, 0}, {'t', 0, 1, 1, 1}}};
    VkPipeline p = compute_pipe(pl, module("k_cluster", si.key, &si)); VkCommandBuffer cb = new_cb();
    h_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p); h_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
    VkBuffer readback = new_buf(8 * 8 * 4 * 16, 0, 0xEE);
    h_vkCmdDispatch(cb, 8, 8, 4); h_vkCmdCopyImageToBuffer(cb, im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1,
        &(VkBufferImageCopy){.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {8, 8, 4}});
    run(cb);
    uint32_t values[8 * 8 * 4 * 4]; memcpy(values, bytes(readback), sizeof values);
    int ok = 1;
    for (uint32_t z = 0; z < 4; z++) for (uint32_t y = 0; y < 8; y++) for (uint32_t x = 0; x < 8; x++) {
        uint32_t i = z * 64 + y * 8 + x; uint32_t *v = &values[i * 4];
        ok &= v[0] == i * 3 + 7 && v[1] == x && v[2] == y && v[3] == z;
    }
    CHECK(ok, "the 3D image stores texel-buffer values at each x,y,z coordinate");
}

static void test_t9(void) {
    // T9: a compute image write sampled before its producer encoder ends shows stale pixels if storage usage or encoder ordering is wrong.
    VkDescriptorSetLayoutBinding b = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorSetLayout sl = new_set_layout(&b, 1); VkPipelineLayout pl = new_pl(&sl, 1, 0, 0);
    VkImage image = new_img(VK_IMAGE_TYPE_2D, VK_FORMAT_R32G32_SFLOAT, 4, 4, 1, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    VkImageView iv = new_view(image, VK_FORMAT_R32G32_SFLOAT, VK_IMAGE_VIEW_TYPE_2D); VkDescriptorSet ds = new_set(sl);
    VkDescriptorImageInfo ii = {NULL, iv, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &ii}; h_vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);
    static const shinfo csi = {.key = "0000000000000016", .comp = 1, .nres = 1, .wg = {1, 1, 1}, .res = {{'t', 0, 0, 1, 0}}};
    static const shinfo vsi = {.key = "0000000000000017"};
    static const shinfo fsi = {.key = "0000000000000018", .frag = 1, .nres = 1, .res = {{'t', 0, 0, 1, 0}}};
    VkPipeline cp = compute_pipe(pl, module("k_smaa_comp", csi.key, &csi));
    VkRenderPass rp = color_pass(VK_FORMAT_R8G8B8A8_UNORM); VkImage target = new_img(VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, 4, 4, 1,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    VkFramebuffer fb = new_fb(rp, new_view(target, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_VIEW_TYPE_2D), 4, 4);
    VkPipeline gp = graphics_pipe(rp, pl, module("k_fullscreen_vs", vsi.key, &vsi), module("k_smaa_fs", fsi.key, &fsi), 4, 4);
    VkCommandBuffer cb = new_cb(); h_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cp);
    h_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL); h_vkCmdDispatch(cb, 4, 4, 1);
    begin_pass(cb, rp, fb, 4, 4); h_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gp);
    h_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &ds, 0, NULL); h_vkCmdDraw(cb, 3, 1, 0, 0); h_vkCmdEndRenderPass(cb); run(cb);
    uint8_t px[64]; read_image(target, 4, 4, 1, 4, px);
    int ok = 1; for (uint32_t y = 0; y < 4; y++) for (uint32_t x = 0; x < 4; x++) {
        const uint8_t *p = px + 4 * (y * 4 + x); ok &= abs((int)p[0] - (int)(x * 85)) <= 1 && abs((int)p[1] - (int)(y * 85)) <= 1 && p[2] == 0 && p[3] == 255;
    }
    CHECK(ok, "the fragment pass samples each compute-written RG32F texel");
}

static void test_t10(void) {
    // T10: using the wrong indirect offset or running a non-zero base at workgroup zero writes the wrong set of IDs.
    VkDescriptorSetLayoutBinding b = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayout sl = new_set_layout(&b, 1); VkPipelineLayout pl = new_pl(&sl, 1, 0, 0);
    VkBuffer out = new_buf(8 * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0xA5); VkDescriptorSet ds = new_set(sl);
    write_buffer(ds, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, out, 0, out->size);
    VkBuffer args = new_buf(32, 0, 0x5A); uint32_t groups[3] = {2, 3, 1}; memcpy(bytes(args) + 16, groups, sizeof groups);
    static const shinfo si = {.key = "0000000000000019", .comp = 1, .nres = 1, .wg = {1, 1, 1}, .res = {{'b', 0, 0, 1, 0}}};
    VkPipeline p = compute_pipe(pl, module("k_gid3", si.key, &si)); uint64_t before = g_n_disp;
    VkCommandBuffer cb = new_cb(); h_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    h_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
    h_vkCmdDispatchIndirect(cb, args, 16); h_vkCmdDispatchBase(cb, 1, 0, 0, 1, 1, 1); h_vkCmdDispatchBase(cb, 0, 0, 0, 1, 1, 1); run(cb);
    CHECK(g_n_disp - before == 2, "the indirect and zero-base dispatch run; the non-zero base is skipped");
    uint32_t *result = (uint32_t *)bytes(out); int ok = 1;
    for (uint32_t i = 0; i < 6; i++) ok &= result[i] == i;
    for (uint32_t i = 6; i < 8; i++) ok &= result[i] == 0xA5A5A5A5u;
    CHECK(ok, "indirect/base dispatch output is %u,%u,%u,%u,%u,%u,%u,%u", result[0], result[1], result[2], result[3], result[4], result[5], result[6], result[7]);
}

int main(void) {
    dev = &g_device; queue = &g_queue;
    @autoreleasepool {
        dev_init(); NSError *err = nil;
        NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
            "kernel void k_gid(device uint* out [[buffer(0)]], uint id [[thread_position_in_grid]]) { out[id] = id; }\n"
            "kernel void k_gid3(device uint* out [[buffer(0)]], uint3 id [[thread_position_in_grid]]) { uint i = id.z * 6 + id.y * 2 + id.x; out[i] = i; }\n"
            "kernel void k_limit(device uint* out [[buffer(0)]], constant uint& n [[buffer(1)]], uint id [[thread_position_in_grid]]) { if (id < n) out[id] = id; }\n"
            "kernel void k_noop(uint id [[thread_position_in_grid]]) { (void)id; }\n"
            "struct TierArgs { device uint* data [[id(0)]]; };\n"
            "kernel void k_tier1_arg(constant TierArgs& args [[buffer(0)]], uint id [[thread_position_in_grid]]) { args.data[id] = 1; }\n"
            "struct PullVertex { float4 position; float4 color; }; struct PullOut { float4 position [[position]]; float4 color; };\n"
            "kernel void k_t4_comp(device PullVertex* out [[buffer(0)]], constant float4& color [[buffer(1)]], uint id [[thread_position_in_grid]]) { if (id >= 3) return; float2 p[3] = {float2(-1,-1), float2(3,-1), float2(-1,3)}; out[id].position = float4(p[id],0,1); out[id].color = color; }\n"
            "vertex PullOut k_t4_vs(uint id [[vertex_id]], device const PullVertex* in [[buffer(0)]]) { PullOut o; o.position = in[id].position; o.color = in[id].color; return o; }\n"
            "fragment float4 k_t4_fs(PullOut in [[stage_in]]) { return in.color; }\n"
            "struct T5Params { uint index; uint area; uint2 pad; float4 color; }; struct T5Out { float4 position [[position]]; float4 color; };\n"
            "kernel void k_t5_comp(device float4* colors [[buffer(0)]], constant T5Params& p [[buffer(1)]], uint id [[thread_position_in_grid]]) { if (id == 0) colors[p.index] = p.color; }\n"
            "vertex T5Out k_t5_vs(uint id [[vertex_id]], device const float4* colors [[buffer(0)]], constant T5Params& p [[buffer(1)]]) { float2 xy; if (p.area == 0) { float2 v[3] = {float2(-1,-1),float2(0,-1),float2(-1,1)}; xy = v[id]; } else { float2 v[3] = {float2(0,-1),float2(1,-1),float2(1,1)}; xy = v[id]; } T5Out o; o.position = float4(xy,0,1); o.color = colors[p.index]; return o; }\n"
            "fragment float4 k_t5_fs(T5Out in [[stage_in]]) { return in.color; }\n"
            "kernel void k_t6_a(device uint* data [[buffer(0)]], uint id [[thread_position_in_grid]]) { if (id == 0) data[0] = 41; }\n"
            "kernel void k_t6_b(device uint* data [[buffer(0)]], uint id [[thread_position_in_grid]]) { if (id == 0) data[0] += 1; }\n"
            "vertex float4 k_fullscreen_vs(uint id [[vertex_id]]) { float2 p[3] = {float2(-1,-1),float2(3,-1),float2(-1,3)}; return float4(p[id],0,1); }\n"
            "fragment float4 k_t7_fs(float4 p [[position]], device const uint* data [[buffer(0)]]) { return data[0] == 7 ? float4(0,1,0,1) : float4(1,0,0,1); }\n"
            "kernel void k_cluster(texture3d<uint, access::write> out [[texture(0)]], texture_buffer<uint, access::read> src [[texture(1)]], uint3 id [[thread_position_in_grid]]) { uint i = id.z * 64 + id.y * 8 + id.x; out.write(uint4(src.read(i).x, id.x, id.y, id.z), id); }\n"
            "kernel void k_smaa_comp(texture2d<float, access::write> out [[texture(0)]], uint2 id [[thread_position_in_grid]]) { out.write(float4(float(id.x)/3.0f,float(id.y)/3.0f,0,1),id); }\n"
            "fragment float4 k_smaa_fs(float4 p [[position]], texture2d<float, access::read> image [[texture(0)]]) { return float4(image.read(uint2(p.xy)).xy,0,1); }\n";
        lib = [g_dev newLibraryWithSource:source options:nil error:&err]; CHECK(lib != nil, "compute and graphics shaders compile: %s", err.localizedDescription.UTF8String);
        VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; h_vkCreateCommandPool(dev, &pci, NULL, &cmd_pool);
        VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = 16 << 20, .memoryTypeIndex = 1};
        CHECK(h_vkAllocateMemory(dev, &mai, NULL, &memory) == VK_SUCCESS, "host-visible GPU memory allocated");
        VkDescriptorPoolCreateInfo dpci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 128}; h_vkCreateDescriptorPool(dev, &dpci, NULL, &desc_pool);
        test_t1(); test_t2(); test_t3(); test_tier1_refusal(); test_t4(); test_t5(); test_t6(); test_t7(); test_format_storage_features(); test_t8(); test_t9(); test_t10();
    }
    printf("Vulkan compute: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
