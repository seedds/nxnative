// Vulkan Metal argument-buffer semantics from descriptor sets, read back through the layer's own replay path.
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
    r = h_vkBindBufferMemory(dev, b, memory, mem_top); CHECK(r == VK_SUCCESS, "buffer memory bound");
    mem_top += (size + 255) & ~(VkDeviceSize)255; memset(bytes(b), fill, size); return b;
}
static VkCommandBuffer new_cb(void) {
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cmd_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer cb = NULL; h_vkAllocateCommandBuffers(dev, &ai, &cb);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; h_vkBeginCommandBuffer(cb, &bi); return cb;
}
static void run(VkCommandBuffer cb) {
    CHECK(h_vkEndCommandBuffer(cb) == VK_SUCCESS, "command buffer ended");
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    CHECK(h_vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS, "command buffer submitted");
    CHECK(h_vkQueueWaitIdle(queue) == VK_SUCCESS, "queue completed the submission");
}
static VkImage new_img(VkFormat fmt, uint32_t w, uint32_t h, VkImageUsageFlags usage) {
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt, .extent = {w, h, 1},
        .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage};
    VkImage im = NULL; VkResult r = h_vkCreateImage(dev, &ci, NULL, &im);
    CHECK(r == VK_SUCCESS && im && im->tex, "image %ux%u created", w, h); return im;
}
static VkImageView new_view(VkImage im, VkFormat fmt) {
    VkImageViewCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView v = NULL; h_vkCreateImageView(dev, &ci, NULL, &v); return v;
}
static VkImageView color_view(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    VkImage im = new_img(VK_FORMAT_R8G8B8A8_UNORM, 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    VkImageView v = new_view(im, VK_FORMAT_R8G8B8A8_UNORM); uint8_t px[4] = {r, g, b, a};
    VkBuffer upload = new_buf(sizeof px, 0, 0); memcpy(bytes(upload), px, sizeof px);
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {1, 1, 1}};
    VkCommandBuffer cb = new_cb(); h_vkCmdCopyBufferToImage(cb, upload, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy); run(cb); return v;
}
static VkSampler new_sampler(void) {
    VkSamplerCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0};
    VkSampler s = NULL; CHECK(h_vkCreateSampler(dev, &ci, NULL, &s) == VK_SUCCESS && s && s->s, "plain sampler created"); return s;
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
static void read_image(VkImage im, uint32_t w, uint32_t h, uint8_t *out) {
    VkBuffer b = new_buf((VkDeviceSize)w * h * 4, 0, 0xEE);
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
    VkCommandBuffer cb = new_cb(); h_vkCmdCopyImageToBuffer(cb, im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b, 1, &copy); run(cb);
    memcpy(out, bytes(b), (size_t)w * h * 4);
}
static VkDescriptorSetLayout new_layout(const VkDescriptorSetLayoutBinding *bindings, uint32_t n) {
    VkDescriptorSetLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = n, .pBindings = bindings};
    VkDescriptorSetLayout l = NULL; CHECK(h_vkCreateDescriptorSetLayout(dev, &ci, NULL, &l) == VK_SUCCESS, "descriptor layout created"); return l;
}
static VkDescriptorSet new_set(VkDescriptorSetLayout layout) {
    VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = desc_pool,
        .descriptorSetCount = 1, .pSetLayouts = &layout};
    VkDescriptorSet s = NULL; CHECK(h_vkAllocateDescriptorSets(dev, &ai, &s) == VK_SUCCESS, "descriptor set allocated"); return s;
}
static void write_buffer(VkDescriptorSet set, uint32_t binding, VkDescriptorType type, VkBuffer b) {
    VkDescriptorBufferInfo info = {b, 0, b->size};
    VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = binding,
        .descriptorCount = 1, .descriptorType = type, .pBufferInfo = &info}; h_vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);
}
static void write_arrays(VkDescriptorSet set, uint32_t tex_binding, uint32_t sampler_binding, const VkImageView *textures, const VkSampler *samplers) {
    VkDescriptorImageInfo ti[40] = {0}, si[20] = {0};
    for (uint32_t i = 0; i < 40; i++) ti[i] = (VkDescriptorImageInfo){.imageView = textures[i], .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    for (uint32_t i = 0; i < 20; i++) si[i] = (VkDescriptorImageInfo){.sampler = samplers[i]};
    VkWriteDescriptorSet w[2] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = tex_binding, .descriptorCount = 40,
            .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = ti},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = sampler_binding, .descriptorCount = 20,
            .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .pImageInfo = si},
    };
    h_vkUpdateDescriptorSets(dev, 2, w, 0, NULL);
}
static void write_indices(VkDescriptorSet set, uint32_t binding, VkDescriptorType type, VkBuffer indices) { write_buffer(set, binding, type, indices); }
static VkDescriptorSetLayout new_arg_layout(VkDescriptorType buffer_type) {
    VkDescriptorSetLayoutBinding b[3] = {
        {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 40, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {40, VK_DESCRIPTOR_TYPE_SAMPLER, 20, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {60, buffer_type, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
    };
    return new_layout(b, 3);
}
static VkPipelineLayout new_pl(VkDescriptorSetLayout layout, uint32_t pc_size, VkShaderStageFlags pc_stages) {
    VkPushConstantRange range = {.stageFlags = pc_stages, .offset = 0, .size = pc_size};
    VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = layout ? 1u : 0u,
        .pSetLayouts = layout ? &layout : NULL, .pushConstantRangeCount = pc_size ? 1u : 0u, .pPushConstantRanges = pc_size ? &range : NULL};
    VkPipelineLayout pl = NULL; CHECK(h_vkCreatePipelineLayout(dev, &ci, NULL, &pl) == VK_SUCCESS, "pipeline layout created"); return pl;
}
static VkShaderModule module(const char *name, const char *key, const shinfo *si) {
    VkShaderModule m = zalloc(sizeof *m); strlcpy(m->key, key, sizeof m->key); m->si = si; m->fn = [lib newFunctionWithName:@(name)];
    CHECK(m->fn != nil, "Metal shader %s exists", name); return m;
}
static VkPipelineShaderStageCreateInfo stage(VkShaderStageFlagBits bit, VkShaderModule m) {
    return (VkPipelineShaderStageCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = bit, .module = m, .pName = "main"};
}
static VkPipeline graphics_pipe(VkRenderPass rp, VkPipelineLayout layout, VkShaderModule vs, VkShaderModule fs, uint32_t w, uint32_t h) {
    VkPipelineShaderStageCreateInfo stages[2] = {stage(VK_SHADER_STAGE_VERTEX_BIT, vs), stage(VK_SHADER_STAGE_FRAGMENT_BIT, fs)};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkViewport vp = {0, 0, (float)w, (float)h, 0, 1}; VkRect2D sc = {{0, 0}, {w, h}};
    VkPipelineViewportStateCreateInfo vpstate = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState ba = {.colorWriteMask = 0xF};
    VkPipelineColorBlendStateCreateInfo blend = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ba};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = stages,
        .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vpstate, .pRasterizationState = &rs,
        .pMultisampleState = &ms, .pColorBlendState = &blend, .layout = layout, .renderPass = rp};
    VkPipeline p = NULL; CHECK(h_vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &p) == VK_SUCCESS && p, "graphics pipeline created"); return p;
}
static int color_at(const uint8_t *px, uint32_t w, uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b) {
    const uint8_t *p = px + 4 * (y * w + x); return abs((int)p[0] - r) <= 1 && abs((int)p[1] - g) <= 1 && abs((int)p[2] - b) <= 1 && p[3] == 255;
}
static void fill_arrays(VkImageView *textures, VkSampler *samplers, VkImageView base, VkSampler sampler) {
    for (uint32_t i = 0; i < 40; i++) textures[i] = base;
    for (uint32_t i = 0; i < 20; i++) samplers[i] = sampler;
}
static void draw_set(VkPipeline pipeline, VkPipelineLayout pl, VkDescriptorSet set, VkRenderPass rp, VkFramebuffer fb,
                     VkImage target, uint32_t w, uint32_t h, uint8_t *pixels) {
    VkCommandBuffer cb = new_cb(); begin_pass(cb, rp, fb, w, h); h_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    h_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &set, 0, NULL); h_vkCmdDraw(cb, 3, 1, 0, 0);
    h_vkCmdEndRenderPass(cb); run(cb); read_image(target, w, h, pixels);
}

static void test_t11(void) {
    // T11: wrong T/S argument-buffer bindings break indexed samples; samplers used only by argument sets need lazy twins, unused ones must stay plain.
    VkDescriptorSetLayout sl = new_arg_layout(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER); VkPipelineLayout pl = new_pl(sl, 0, 0);
    VkImageView red = color_view(255, 0, 0, 255), green = color_view(0, 255, 0, 255), blue = color_view(0, 0, 255, 255);
    VkSampler sampler = new_sampler(), unused = new_sampler(); VkImageView textures[40]; VkSampler samplers[20]; fill_arrays(textures, samplers, red, sampler);
    textures[17] = green; textures[39] = blue;
    struct { uint32_t tex[3], smp[3]; } indices = {{0, 17, 39}, {0, 17, 19}};
    VkBuffer index_buffer = new_buf(sizeof indices, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, 0); memcpy(bytes(index_buffer), &indices, sizeof indices);
    VkDescriptorSet set = new_set(sl); write_arrays(set, 0, 40, textures, samplers); write_indices(set, 60, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, index_buffer);
    static const shinfo vsi = {.key = "1000000000000001"};
    static const shinfo fsi = {.key = "1000000000000002", .frag = 1, .nres = 3,
        .res = {{'T', 0, 0, 40, 0}, {'S', 0, 0, 20, 40}, {'U', 0, 0, 1, 60}}};
    VkRenderPass rp = color_pass(VK_FORMAT_R8G8B8A8_UNORM); VkImage target = new_img(VK_FORMAT_R8G8B8A8_UNORM, 12, 4,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    VkFramebuffer fb = new_fb(rp, new_view(target, VK_FORMAT_R8G8B8A8_UNORM), 12, 4);
    VkPipeline gp = graphics_pipe(rp, pl, module("k_fullscreen_vs", vsi.key, &vsi), module("k_arg_table_fs", fsi.key, &fsi), 12, 4);
    uint8_t px[12 * 4 * 4]; draw_set(gp, pl, set, rp, fb, target, 12, 4, px);
    CHECK(color_at(px, 12, 1, 1, 255, 0, 0), "the first texture-array element is sampled");
    CHECK(color_at(px, 12, 5, 1, 0, 255, 0), "texture and sampler array element 17 are sampled");
    CHECK(color_at(px, 12, 10, 1, 0, 0, 255), "texture element 39 and sampler element 19 are sampled");
    CHECK(sampler->ref.argumentState != nil, "a sampler used in an argument set gets its argument-buffer twin");
    CHECK(unused->ref.argumentState == nil, "a sampler never used in an argument set stays plain");
    h_vkDestroySampler(dev, unused, NULL);
    h_vkDestroyBuffer(dev, index_buffer, NULL);
}

static void test_t12(void) {
    // T12: if vkUpdateDescriptorSets does not bump the set generation, the second submission keeps sampling the stale first texture.
    VkDescriptorSetLayout sl = new_arg_layout(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER); VkPipelineLayout pl = new_pl(sl, 0, 0);
    VkImageView red = color_view(255, 0, 0, 255), green = color_view(0, 255, 0, 255); VkSampler sampler = new_sampler();
    VkImageView textures[40]; VkSampler samplers[20]; fill_arrays(textures, samplers, red, sampler);
    struct { uint32_t tex[3], smp[3]; } indices = {{0, 0, 0}, {0, 0, 0}};
    VkBuffer index_buffer = new_buf(sizeof indices, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, 0); memcpy(bytes(index_buffer), &indices, sizeof indices);
    VkDescriptorSet set = new_set(sl); write_arrays(set, 0, 40, textures, samplers); write_indices(set, 60, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, index_buffer);
    static const shinfo vsi = {.key = "1000000000000003"};
    static const shinfo fsi = {.key = "1000000000000004", .frag = 1, .nres = 3, .res = {{'T', 0, 0, 40, 0}, {'S', 0, 0, 20, 40}, {'U', 0, 0, 1, 60}}};
    VkRenderPass rp = color_pass(VK_FORMAT_R8G8B8A8_UNORM); VkImage target = new_img(VK_FORMAT_R8G8B8A8_UNORM, 4, 4,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    VkFramebuffer fb = new_fb(rp, new_view(target, VK_FORMAT_R8G8B8A8_UNORM), 4, 4);
    VkPipeline gp = graphics_pipe(rp, pl, module("k_fullscreen_vs", vsi.key, &vsi), module("k_arg_table_fs", fsi.key, &fsi), 4, 4);
    uint8_t first[64], second[64]; draw_set(gp, pl, set, rp, fb, target, 4, 4, first);
    VkDescriptorImageInfo next = {NULL, green, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0, .dstArrayElement = 0,
        .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &next};
    h_vkUpdateDescriptorSets(dev, 1, &w, 0, NULL); draw_set(gp, pl, set, rp, fb, target, 4, 4, second);
    CHECK(color_at(first, 4, 2, 2, 255, 0, 0), "the first submission sampled the old texture");
    CHECK(color_at(second, 4, 2, 2, 0, 255, 0), "the updated descriptor is re-encoded for the second submission");
    h_vkDestroyPipeline(dev, gp, NULL); h_vkDestroyBuffer(dev, index_buffer, NULL);
    h_vkDestroyImage(dev, green->img, NULL); h_vkDestroyImageView(dev, green, NULL);
}

static void test_t13(void) {
    // T13: after free/reallocation, a pointer-keyed argument-buffer cache must not keep the old texture when the generation changes.
    VkDescriptorSetLayout sl = new_arg_layout(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER); VkPipelineLayout pl = new_pl(sl, 0, 0);
    VkImageView red = color_view(255, 0, 0, 255), green = color_view(0, 255, 0, 255); VkSampler sampler = new_sampler();
    VkImageView textures[40]; VkSampler samplers[20]; fill_arrays(textures, samplers, red, sampler);
    struct { uint32_t tex[3], smp[3]; } indices = {{0, 0, 0}, {0, 0, 0}};
    VkBuffer index_buffer = new_buf(sizeof indices, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, 0); memcpy(bytes(index_buffer), &indices, sizeof indices);
    VkDescriptorSet old = new_set(sl); write_arrays(old, 0, 40, textures, samplers); write_indices(old, 60, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, index_buffer);
    static const shinfo vsi = {.key = "1000000000000005"};
    static const shinfo fsi = {.key = "1000000000000006", .frag = 1, .nres = 3, .res = {{'T', 0, 0, 40, 0}, {'S', 0, 0, 20, 40}, {'U', 0, 0, 1, 60}}};
    VkRenderPass rp = color_pass(VK_FORMAT_R8G8B8A8_UNORM); VkImage target = new_img(VK_FORMAT_R8G8B8A8_UNORM, 4, 4,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    VkFramebuffer fb = new_fb(rp, new_view(target, VK_FORMAT_R8G8B8A8_UNORM), 4, 4);
    VkPipeline gp = graphics_pipe(rp, pl, module("k_fullscreen_vs", vsi.key, &vsi), module("k_arg_table_fs", fsi.key, &fsi), 4, 4);
    uint8_t first[64], second[64]; draw_set(gp, pl, old, rp, fb, target, 4, 4, first);
    uint64_t old_generation = old->gen; h_vkFreeDescriptorSets(dev, desc_pool, 1, &old); VkDescriptorSet fresh = new_set(sl);
    CHECK(fresh->gen > old_generation, "allocation assigns a fresh generation before descriptor writes");
    // Force the stale cache's address to alias this allocation, independent of the system allocator's reuse policy.
    abuf_t *cached = &gp->ab[ST_FRAG][0]; cached->s = fresh; cached->gen = old_generation;
    CHECK(cached->s == fresh && cached->gen == old_generation, "reuse fixture places the fresh set behind the old generation cache key");
    fill_arrays(textures, samplers, green, sampler); write_arrays(fresh, 0, 40, textures, samplers); write_indices(fresh, 60, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, index_buffer);
    draw_set(gp, pl, fresh, rp, fb, target, 4, 4, second);
    CHECK(color_at(first, 4, 2, 2, 255, 0, 0), "the original set sampled its original texture");
    CHECK(color_at(second, 4, 2, 2, 0, 255, 0), "the cached argument buffer re-encodes the new texture for a fresh set generation");
}

static void test_t14(void) {
    // T14: without resetting residency or retaining the pre-encode generation, alternating sets in one encoder leaves stale resources current.
    VkDescriptorSetLayout sl = new_arg_layout(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    VkPushConstantRange pc = {.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT, .offset = 0, .size = 4};
    VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &sl,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &pc}; VkPipelineLayout pl = NULL; h_vkCreatePipelineLayout(dev, &plci, NULL, &pl);
    VkImageView red = color_view(255, 0, 0, 255), blue = color_view(0, 0, 255, 255); VkSampler sampler = new_sampler();
    VkImageView ta[40], tb[40]; VkSampler sa[20], sb[20]; fill_arrays(ta, sa, red, sampler); fill_arrays(tb, sb, blue, sampler);
    VkDescriptorSet a = new_set(sl), b = new_set(sl); write_arrays(a, 0, 40, ta, sa); write_arrays(b, 0, 40, tb, sb);
    static const shinfo vsi = {.key = "1000000000000007"};
    static const shinfo fsi = {.key = "1000000000000008", .frag = 1, .nres = 3,
        .res = {{'T', 0, 0, 40, 0}, {'S', 0, 0, 20, 40}, {'p', 0, 1, 1, 0}}};
    VkRenderPass rp = color_pass(VK_FORMAT_R8G8B8A8_UNORM); VkImage target = new_img(VK_FORMAT_R8G8B8A8_UNORM, 12, 4,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    VkFramebuffer fb = new_fb(rp, new_view(target, VK_FORMAT_R8G8B8A8_UNORM), 12, 4);
    VkPipeline gp = graphics_pipe(rp, pl, module("k_fullscreen_vs", vsi.key, &vsi), module("k_arg_area_fs", fsi.key, &fsi), 12, 4);
    VkCommandBuffer cb = new_cb(); begin_pass(cb, rp, fb, 12, 4); h_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gp);
    for (uint32_t area = 0; area < 3; area++) {
        VkDescriptorSet ds = area == 1 ? b : a; h_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &ds, 0, NULL);
        h_vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof area, &area); h_vkCmdDraw(cb, 3, 1, 0, 0);
    }
    h_vkCmdEndRenderPass(cb); run(cb); uint8_t px[12 * 4 * 4]; read_image(target, 12, 4, px);
    CHECK(color_at(px, 12, 1, 1, 255, 0, 0), "the first set's texture is resident for draw one");
    CHECK(color_at(px, 12, 5, 1, 0, 0, 255), "the alternating set's texture is resident for draw two");
    CHECK(color_at(px, 12, 10, 1, 255, 0, 0), "the first set is resident again for draw three");
    abuf_t *arg = &gp->ab[ST_FRAG][0]; arg->enc_id = g_enc_id; uint64_t generation = b->gen; b->gen = generation + 1;
    ab_encode(arg, b, &fsi, generation);
    CHECK(arg->gen == generation, "re-encoding keeps the generation captured before encoding");
    CHECK(arg->enc_id == 0, "re-encoding clears the previous encoder's residency mark");
}

static void test_t15(void) {
    // T15: omitting argument-buffer useResources leaves a texture rendered in an earlier pass untracked when the later pass samples it.
    VkDescriptorSetLayoutBinding bindings[2] = {
        {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 40, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {40, VK_DESCRIPTOR_TYPE_SAMPLER, 20, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
    };
    VkDescriptorSetLayout sl = new_layout(bindings, 2); VkPipelineLayout pl = new_pl(sl, 0, 0);
    VkImage source = new_img(VK_FORMAT_R8G8B8A8_UNORM, 4, 4, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    VkImageView source_view = new_view(source, VK_FORMAT_R8G8B8A8_UNORM); VkRenderPass src_rp = color_pass(VK_FORMAT_R8G8B8A8_UNORM);
    VkFramebuffer src_fb = new_fb(src_rp, source_view, 4, 4);
    VkImageView textures[40]; VkSampler samplers[20]; VkSampler sampler = new_sampler(); fill_arrays(textures, samplers, source_view, sampler);
    VkDescriptorSet set = new_set(sl); write_arrays(set, 0, 40, textures, samplers);
    static const shinfo vsi = {.key = "1000000000000009"};
    static const shinfo fsi = {.key = "1000000000000010", .frag = 1, .nres = 2, .res = {{'T', 0, 0, 40, 0}, {'S', 0, 0, 20, 40}}};
    VkImage target = new_img(VK_FORMAT_R8G8B8A8_UNORM, 4, 4, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    VkRenderPass dst_rp = color_pass(VK_FORMAT_R8G8B8A8_UNORM); VkFramebuffer dst_fb = new_fb(dst_rp, new_view(target, VK_FORMAT_R8G8B8A8_UNORM), 4, 4);
    VkPipelineLayout plain = new_pl(NULL, 0, 0); static const shinfo solid_vs = {.key = "1000000000000011"}, solid_fs = {.key = "1000000000000012", .frag = 1};
    VkPipeline draw_source = graphics_pipe(src_rp, plain, module("k_fullscreen_vs", solid_vs.key, &solid_vs), module("k_solid_fs", solid_fs.key, &solid_fs), 4, 4);
    VkPipeline sample_source = graphics_pipe(dst_rp, pl, module("k_fullscreen_vs", vsi.key, &vsi), module("k_arg_single_fs", fsi.key, &fsi), 4, 4);
    VkCommandBuffer cb = new_cb(); begin_pass(cb, src_rp, src_fb, 4, 4); h_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, draw_source);
    h_vkCmdDraw(cb, 3, 1, 0, 0); h_vkCmdEndRenderPass(cb); begin_pass(cb, dst_rp, dst_fb, 4, 4);
    h_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, sample_source); h_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &set, 0, NULL);
    h_vkCmdDraw(cb, 3, 1, 0, 0); h_vkCmdEndRenderPass(cb); run(cb);
    uint8_t px[64]; read_image(target, 4, 4, px); int ok = 1;
    for (int i = 0; i < 16; i++) ok &= color_at(px, 4, i % 4, i / 4, 255, 128, 0);
    CHECK(ok, "the later pass samples the texture written in the earlier pass");
}

static void test_t16(void) {
    // T16: mishandling nil slots or q/o dynamic offsets crashes validation or makes both draws read the same uniform.
    VkDescriptorSetLayout sl = new_arg_layout(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC); VkPipelineLayout pl = new_pl(sl, 0, 0);
    VkImageView red = color_view(255, 0, 0, 255), blue = color_view(0, 0, 255, 255); VkSampler sampler = new_sampler();
    VkImageView textures[40] = {0}; VkSampler samplers[20] = {0}; textures[0] = red; textures[17] = blue; samplers[0] = sampler;
    VkBuffer data = new_buf(512, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, 0); ((uint32_t *)bytes(data))[0] = 17; ((uint32_t *)bytes(data))[64] = 23;
    VkDescriptorSet ds = new_set(sl); write_arrays(ds, 0, 40, textures, samplers); write_indices(ds, 60, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, data);
    static const shinfo vsi = {.key = "1000000000000013"};
    static const shinfo fsi = {.key = "1000000000000014", .frag = 1, .nres = 5,
        .res = {{'T', 0, 0, 40, 0}, {'S', 0, 0, 20, 40}, {'U', 0, 0, 1, 60}, {'q', 0, 1, 1, 0}, {'o', 0, 0, 1, 60}}};
    VkRenderPass rp = color_pass(VK_FORMAT_R8G8B8A8_UNORM); VkImage targets[2] = {
        new_img(VK_FORMAT_R8G8B8A8_UNORM, 4, 4, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT),
        new_img(VK_FORMAT_R8G8B8A8_UNORM, 4, 4, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)};
    VkFramebuffer fb[2] = {new_fb(rp, new_view(targets[0], VK_FORMAT_R8G8B8A8_UNORM), 4, 4),
                           new_fb(rp, new_view(targets[1], VK_FORMAT_R8G8B8A8_UNORM), 4, 4)};
    VkPipeline gp = graphics_pipe(rp, pl, module("k_fullscreen_vs", vsi.key, &vsi), module("k_arg_dyn_fs", fsi.key, &fsi), 4, 4);
    VkCommandBuffer cb = new_cb();
    for (uint32_t i = 0; i < 2; i++) {
        begin_pass(cb, rp, fb[i], 4, 4); h_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gp);
        uint32_t dyn = i ? 256 : 0; h_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &ds, 1, &dyn);
        h_vkCmdDraw(cb, 3, 1, 0, 0); h_vkCmdEndRenderPass(cb);
    }
    run(cb); uint8_t first[64], second[64]; read_image(targets[0], 4, 4, first); read_image(targets[1], 4, 4, second);
    CHECK(color_at(first, 4, 2, 2, 255, 0, 0), "dynamic offset zero selects the first uniform");
    CHECK(color_at(second, 4, 2, 2, 0, 0, 255), "dynamic offset 256 selects the second uniform despite nil array slots");
}

static void test_t17(void) {
    // T17: making the vertex set an argument buffer too loses its discrete uniform, or binding the fragment set discretely loses the indexed sample.
    VkDescriptorSetLayoutBinding bindings[3] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 40, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {41, VK_DESCRIPTOR_TYPE_SAMPLER, 20, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
    };
    VkDescriptorSetLayout sl = new_layout(bindings, 3); VkPipelineLayout pl = new_pl(sl, 0, 0);
    VkImageView red = color_view(255, 0, 0, 255); VkSampler sampler = new_sampler(); VkImageView textures[40]; VkSampler samplers[20];
    fill_arrays(textures, samplers, red, sampler); VkDescriptorSet set = new_set(sl);
    VkBuffer allow = new_buf(sizeof(uint32_t), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, 0); *(uint32_t *)bytes(allow) = 7;
    write_buffer(set, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, allow);
    write_arrays(set, 1, 41, textures, samplers);
    static const shinfo vsi = {.key = "1000000000000015", .nres = 1, .res = {{'u', 0, 0, 1, 0}}};
    static const shinfo fsi = {.key = "1000000000000016", .frag = 1, .nres = 2, .res = {{'T', 0, 0, 40, 1}, {'S', 0, 0, 20, 41}}};
    VkRenderPass rp = color_pass(VK_FORMAT_R8G8B8A8_UNORM); VkImage target = new_img(VK_FORMAT_R8G8B8A8_UNORM, 4, 4,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    VkFramebuffer fb = new_fb(rp, new_view(target, VK_FORMAT_R8G8B8A8_UNORM), 4, 4);
    VkPipeline gp = graphics_pipe(rp, pl, module("k_t17_vs", vsi.key, &vsi), module("k_arg_mixed_fs", fsi.key, &fsi), 4, 4);
    uint8_t px[64]; draw_set(gp, pl, set, rp, fb, target, 4, 4, px);
    CHECK(color_at(px, 4, 2, 2, 255, 0, 0), "the vertex stage reads the discrete uniform while fragment reads its argument-buffer texture");
}

static void test_t18(void) {
    // T18: destroy the descriptor set and its sampled objects before GPU completion; only the encoded argument buffer may keep them alive.
    VkDescriptorSetLayoutBinding bindings[2] = {
        {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 40, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {40, VK_DESCRIPTOR_TYPE_SAMPLER, 20, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
    };
    VkDescriptorSetLayout sl = new_layout(bindings, 2); VkPipelineLayout pl = new_pl(sl, 0, 0);
    VkImageView view = color_view(0, 255, 255, 255); VkImage image = view->img; VkSampler sampler = new_sampler();
    VkImageView textures[40]; VkSampler samplers[20]; fill_arrays(textures, samplers, view, sampler);
    VkDescriptorSet set = new_set(sl); write_arrays(set, 0, 40, textures, samplers);
    static const shinfo vsi = {.key = "1000000000000017"};
    static const shinfo fsi = {.key = "1000000000000018", .frag = 1, .nres = 2, .res = {{'T', 0, 0, 40, 0}, {'S', 0, 0, 20, 40}}};
    VkRenderPass rp = color_pass(VK_FORMAT_R8G8B8A8_UNORM); VkImage target = new_img(VK_FORMAT_R8G8B8A8_UNORM, 4, 4,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    VkFramebuffer fb = new_fb(rp, new_view(target, VK_FORMAT_R8G8B8A8_UNORM), 4, 4);
    VkPipeline gp = graphics_pipe(rp, pl, module("k_fullscreen_vs", vsi.key, &vsi), module("k_arg_single_fs", fsi.key, &fsi), 4, 4);
    VkCommandBuffer cb = new_cb(); begin_pass(cb, rp, fb, 4, 4); h_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gp);
    h_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &set, 0, NULL); h_vkCmdDraw(cb, 3, 1, 0, 0); h_vkCmdEndRenderPass(cb);
    CHECK(h_vkEndCommandBuffer(cb) == VK_SUCCESS, "lifetime command buffer ended");
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    CHECK(h_vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS, "lifetime command buffer submitted");
    CHECK(h_vkFreeDescriptorSets(dev, desc_pool, 1, &set) == VK_SUCCESS, "descriptor-set references released after encoding");
    h_vkDestroyImageView(dev, view, NULL); h_vkDestroyImage(dev, image, NULL); h_vkDestroySampler(dev, sampler, NULL);
    CHECK(gp->ab[ST_FRAG][0].keep.count == 2, "the argument buffer alone keeps its unique texture and sampler wrapper");
    CHECK(h_vkQueueWaitIdle(queue) == VK_SUCCESS, "GPU completes after the sampled objects and set are destroyed");
    uint8_t px[64]; read_image(target, 4, 4, px); int ok = 1;
    for (int i = 0; i < 16; i++) ok &= color_at(px, 4, i % 4, i / 4, 0, 255, 255);
    CHECK(ok, "the argument buffer retains the sampled image through command-buffer completion");
}

static void test_descriptor_generations(void) {
    // Descriptor copies and template writes each need a generation bump or a cached argument buffer remains stale.
    VkDescriptorSetLayoutBinding binding = {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    VkDescriptorSetLayout layout = new_layout(&binding, 1); VkImageView view = color_view(0, 255, 0, 255);
    VkDescriptorSet src = new_set(layout), dst = new_set(layout);
    VkDescriptorImageInfo image = {.imageView = view, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = src, .dstBinding = 0,
        .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &image};
    h_vkUpdateDescriptorSets(dev, 1, &write, 0, NULL);
    uint64_t copy_generation = dst->gen;
    VkCopyDescriptorSet copy = {.sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET, .srcSet = src, .srcBinding = 0,
        .dstSet = dst, .dstBinding = 0, .descriptorCount = 1};
    h_vkUpdateDescriptorSets(dev, 0, NULL, 1, &copy);
    CHECK(dst->gen > copy_generation, "descriptor copy advances the destination generation");

    VkDescriptorSet templated = new_set(layout); uint64_t template_generation = templated->gen;
    VkDescriptorUpdateTemplateEntry entry = {.dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        .offset = 0, .stride = sizeof image};
    VkDescriptorUpdateTemplateCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO,
        .descriptorUpdateEntryCount = 1, .pDescriptorUpdateEntries = &entry,
        .templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET, .descriptorSetLayout = layout};
    VkDescriptorUpdateTemplate update = NULL;
    CHECK(h_vkCreateDescriptorUpdateTemplate(dev, &ci, NULL, &update) == VK_SUCCESS && update, "descriptor update template created");
    h_vkUpdateDescriptorSetWithTemplate(dev, templated, update, &image);
    CHECK(templated->gen > template_generation, "template update advances the destination generation");
    h_vkDestroyDescriptorUpdateTemplate(dev, update, NULL);
}

static void test_tier1_refusal(void) {
    // A graphics pipeline with an argument-buffer set must be refused on Tier1 and its draw counted as skipped.
    VkPipelineLayout pl = new_pl(NULL, 0, 0); static const shinfo vsi = {.key = "1000000000000020"};
    static const shinfo fsi = {.key = "1000000000000021", .frag = 1, .nres = 2, .res = {{'T', 0, 0, 40, 0}, {'S', 0, 0, 20, 40}}};
    VkRenderPass rp = color_pass(VK_FORMAT_R8G8B8A8_UNORM);
    VkImage target = new_img(VK_FORMAT_R8G8B8A8_UNORM, 4, 4, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    VkFramebuffer fb = new_fb(rp, new_view(target, VK_FORMAT_R8G8B8A8_UNORM), 4, 4);
    MTLArgumentBuffersTier tier = g_ab_tier; g_ab_tier = MTLArgumentBuffersTier1;
    VkPipeline p = graphics_pipe(rp, pl, module("k_fullscreen_vs", vsi.key, &vsi), module("k_arg_single_fs", fsi.key, &fsi), 4, 4);
    g_ab_tier = tier;
    CHECK(p->pso == nil, "Tier1 refuses an argument-buffer graphics pipeline");
    uint64_t skipped = g_n_skip; VkCommandBuffer cb = new_cb(); begin_pass(cb, rp, fb, 4, 4);
    h_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p); h_vkCmdDraw(cb, 3, 1, 0, 0); h_vkCmdEndRenderPass(cb); run(cb);
    CHECK(g_n_skip == skipped + 1, "Tier1-refused graphics draw is counted as skipped");
}

static void test_ab_init_refusal(void) {
    // More than two argument buffers cannot be bound by this layer; Metal's otherwise-valid pipeline must be refused.
    VkDescriptorSetLayoutBinding binding = {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    VkDescriptorSetLayout sets[3] = {new_layout(&binding, 1), new_layout(&binding, 1), new_layout(&binding, 1)};
    VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 3, .pSetLayouts = sets};
    VkPipelineLayout pl = NULL; CHECK(h_vkCreatePipelineLayout(dev, &plci, NULL, &pl) == VK_SUCCESS, "three-set pipeline layout created");
    static const shinfo vsi = {.key = "1000000000000022"};
    static const shinfo fsi = {.key = "1000000000000023", .frag = 1, .nres = 3,
        .res = {{'T', 0, 0, 1, 0}, {'T', 1, 1, 1, 0}, {'T', 2, 2, 1, 0}}};
    VkRenderPass rp = color_pass(VK_FORMAT_R8G8B8A8_UNORM);
    VkPipeline p = graphics_pipe(rp, pl, module("k_fullscreen_vs", vsi.key, &vsi), module("k_arg_three_fs", fsi.key, &fsi), 4, 4);
    CHECK(p->pso == nil, "a pipeline requiring three argument buffers is refused");
}

int main(void) {
    dev = &g_device; queue = &g_queue;
    @autoreleasepool {
        dev_init(); NSError *err = nil;
        NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
            "vertex float4 k_fullscreen_vs(uint id [[vertex_id]]) { float2 p[3] = {float2(-1,-1),float2(3,-1),float2(-1,3)}; return float4(p[id],0,1); }\n"
            "fragment float4 k_solid_fs(float4 p [[position]]) { return float4(1.0,0.5,0.0,1.0); }\n"
            "struct SelectIndices { uint t0,t1,t2,s0,s1,s2; };\n"
            "struct BigArgs { array<texture2d<float>,40> textures [[id(0)]]; array<sampler,20> samplers [[id(40)]]; constant SelectIndices& indices [[id(60)]]; };\n"
            "fragment float4 k_arg_table_fs(float4 p [[position]], constant BigArgs& arg [[buffer(0)]]) { uint t,s; if (p.x < 4.0) { t=arg.indices.t0; s=arg.indices.s0; } else if (p.x < 8.0) { t=arg.indices.t1; s=arg.indices.s1; } else { t=arg.indices.t2; s=arg.indices.s2; } return arg.textures[t].sample(arg.samplers[s],float2(0.5)); }\n"
            "struct BigTextures { array<texture2d<float>,40> textures [[id(0)]]; array<sampler,20> samplers [[id(40)]]; };\n"
            "fragment float4 k_arg_single_fs(float4 p [[position]], constant BigTextures& arg [[buffer(0)]]) { uint i = uint(p.x) % 40; uint s = uint(p.x) % 20; return arg.textures[i].sample(arg.samplers[s],float2(0.5)); }\n"
            "struct BigTextures17 { array<texture2d<float>,40> textures [[id(1)]]; array<sampler,20> samplers [[id(41)]]; };\n"
            "fragment float4 k_arg_mixed_fs(float4 p [[position]], constant BigTextures17& arg [[buffer(0)]]) { uint i = uint(p.x) % 40; uint s = uint(p.x) % 20; return arg.textures[i].sample(arg.samplers[s],float2(0.5)); }\n"
            "struct OneArg { texture2d<float> image [[id(0)]]; };\n"
            "fragment float4 k_arg_three_fs(float4 p [[position]], constant OneArg& a [[buffer(0)]], constant OneArg& b [[buffer(1)]], constant OneArg& c [[buffer(2)]]) { return a.image.read(uint2(0)) + b.image.read(uint2(0)) + c.image.read(uint2(0)); }\n"
            "fragment float4 k_arg_area_fs(float4 p [[position]], constant BigTextures& arg [[buffer(0)]], constant uint& area [[buffer(1)]]) { if (uint(p.x) / 4 != area) discard_fragment(); return arg.textures[0].sample(arg.samplers[0],float2(0.5)); }\n"
            "struct BigDyn { array<texture2d<float>,40> textures [[id(0)]]; array<sampler,20> samplers [[id(40)]]; device const uint* data [[id(60)]]; };\n"
            "fragment float4 k_arg_dyn_fs(float4 p [[position]], constant BigDyn& arg [[buffer(0)]], constant uint* offsets [[buffer(1)]]) { if (offsets[0] == 0xffffffffu) return arg.textures[17].sample(arg.samplers[17],float2(0.5)); uint v = arg.data[offsets[0]/4]; return v == 17 ? float4(1,0,0,1) : float4(0,0,1,1); }\n"
            "vertex float4 k_t17_vs(uint id [[vertex_id]], constant uint& allow [[buffer(0)]]) { float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)}; return allow == 7 ? float4(p[id],0,1) : float4(4,4,0,1); }\n";
        lib = [g_dev newLibraryWithSource:source options:nil error:&err]; CHECK(lib != nil, "argument-buffer shaders compile: %s", err.localizedDescription.UTF8String);
        VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; h_vkCreateCommandPool(dev, &pci, NULL, &cmd_pool);
        VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = 16 << 20, .memoryTypeIndex = 1};
        CHECK(h_vkAllocateMemory(dev, &mai, NULL, &memory) == VK_SUCCESS, "host-visible GPU memory allocated");
        VkDescriptorPoolCreateInfo dpci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 128}; h_vkCreateDescriptorPool(dev, &dpci, NULL, &desc_pool);
        test_t11(); test_t12(); test_t13(); test_t14(); test_t15(); test_t16(); test_t17(); test_t18();
        test_descriptor_generations(); test_tier1_refusal(); test_ab_init_refusal();
    }
    printf("Vulkan argument buffers: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
