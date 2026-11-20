#include "../vk/hle_vk.m"
#include <assert.h>

int dm_guest_as_contains(uintptr_t address) { (void)address; return 0; }
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
void dm_vi_layer_crop(int c[4]) { c[0] = c[1] = c[2] = c[3] = 0; }
int dm_opmode(void) { return 1; }

static const uint32_t extent = 4;
static const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
static const VkImageSubresourceLayers layer = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
static VkCommandPool pool;

static VkCommandBuffer begin_commands(void) {
    VkCommandBuffer cb;
    VkCommandBufferAllocateInfo info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .commandBufferCount = 1};
    assert(h_vkAllocateCommandBuffers(&g_device, &info, &cb) == VK_SUCCESS);
    VkCommandBufferBeginInfo start = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    assert(h_vkBeginCommandBuffer(cb, &start) == VK_SUCCESS);
    return cb;
}

static VkImage image(VkFormat format, VkSampleCountFlagBits samples, VkImageUsageFlags usage) {
    VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = format,
        .extent = {extent, extent, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = samples, .usage = usage};
    VkImage result;
    assert(h_vkCreateImage(&g_device, &info, NULL, &result) == VK_SUCCESS);
    return result;
}

static VkBuffer buffer(VkDeviceSize size, VkBufferUsageFlags usage) {
    VkBuffer b;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage};
    assert(h_vkCreateBuffer(&g_device, &bi, NULL, &b) == VK_SUCCESS);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = 256, .memoryTypeIndex = 1};
    VkDeviceMemory m;
    assert(h_vkAllocateMemory(&g_device, &ai, NULL, &m) == VK_SUCCESS);
    assert(h_vkBindBufferMemory(&g_device, b, m, 0) == VK_SUCCESS);
    return b;
}

static void free_buffer(VkBuffer b) {
    VkDeviceMemory m = b->mem;
    h_vkDestroyBuffer(&g_device, b, NULL);
    h_vkFreeMemory(&g_device, m, NULL);
}

static id<MTLBuffer> readback(VkCommandBuffer cb, VkImage im) {
    assert(h_vkEndCommandBuffer(cb) == VK_SUCCESS);
    subq_t sq = {0};
    rs_t rs = {.cb = [g_q commandBuffer], .sq = &sq};
    replay(&rs, cb);
    NSUInteger row = extent * sizeof(uint32_t);
    id<MTLBuffer> out = [g_dev newBufferWithLength:row * extent options:MTLResourceStorageModeShared];
    id<MTLBlitCommandEncoder> be = [rs.cb blitCommandEncoder];
    [be copyFromTexture:im->tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
              sourceSize:MTLSizeMake(extent, extent, 1) toBuffer:out destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:0];
    [be endEncoding]; [rs.cb commit]; [rs.cb waitUntilCompleted];
    assert(rs.cb.status == MTLCommandBufferStatusCompleted);
    h_vkFreeCommandBuffers(&g_device, pool, 1, &cb);
    return out;
}

static void check_regions(void) {
    VkImageUsageFlags transfer = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImage ms = image(VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_4_BIT, transfer);
    VkImage dst = image(VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, transfer);
    VkImage src = image(VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, transfer);
    VkCommandBuffer cb = begin_commands();
    VkClearColorValue red = {.float32 = {1, 0, 0, 1}}, blue = {.float32 = {0, 0, 1, 1}}, green = {.float32 = {0, 1, 0, 1}};
    h_vkCmdClearColorImage(cb, ms, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &red, 1, &range);
    h_vkCmdClearColorImage(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &blue, 1, &range);
    h_vkCmdClearColorImage(cb, src, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &green, 1, &range);
    VkImageResolve resolve = {layer, {1, 1, 0}, layer, {2, 1, 0}, {1, 2, 1}};
    h_vkCmdResolveImage(cb, ms, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &resolve);
    VkImageBlit blit = {layer, {{0, 0, 0}, {1, 1, 1}}, layer, {{0, 0, 0}, {2, 2, 1}}};
    h_vkCmdBlitImage(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

    // A Metal-compatible texture can still lack the required Vulkan usage or layout.
    size_t used = cb->n;
    h_vkCmdResolveImage(cb, ms, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &resolve);
    VkImage no_transfer = image(VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    h_vkCmdBlitImage(cb, no_transfer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    assert(cb->n == used);
    h_vkDestroyImage(&g_device, no_transfer, NULL);
    id<MTLBuffer> out = readback(cb, dst);
    const uint32_t *pixels = out.contents;
    for (uint32_t y = 0; y < extent; y++) for (uint32_t x = 0; x < extent; x++) {
        uint32_t want = x < 2 && y < 2 ? 0xff00ff00 : x == 2 && (y == 1 || y == 2) ? 0xff0000ff : 0xffff0000;
        assert(pixels[y * extent + x] == want);
    }
    h_vkDestroyImage(&g_device, src, NULL); h_vkDestroyImage(&g_device, dst, NULL); h_vkDestroyImage(&g_device, ms, NULL);
}

static void check_bone_index(uint16_t index) {
    NSString *msl = @"#include <metal_stdlib>\nusing namespace metal;\n"
                    "struct In { uint a_indices [[attribute(0)]]; };\n"
                    "struct Out { float4 pos [[position]]; int index [[flat]]; };\n"
                    "vertex Out bone(In in [[stage_in]], uint i [[vertex_id]]) {"
                    " float2 p = float2((i << 1) & 2, i & 2); return {float4(p * 2 - 1, 0, 1), int(in.a_indices)}; }\n"
                    "fragment uint result(Out in [[stage_in]]) { return uint(in.index); }";
    NSError *err = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:msl options:nil error:&err];
    assert(lib);
    struct VkShaderModule_T vert = {.fn = [lib newFunctionWithName:@"bone"]};
    struct VkShaderModule_T frag = {.fn = [lib newFunctionWithName:@"result"]};
    VkPipelineShaderStageCreateInfo stages[] = {{.stage = VK_SHADER_STAGE_VERTEX_BIT, .module = &vert}, {.stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = &frag}};
    VkVertexInputAttributeDescription attr = {0, 0, VK_FORMAT_R16_UINT, 0};
    VkVertexInputBindingDescription bind = {0, 4, VK_VERTEX_INPUT_RATE_VERTEX};
    VkPipelineVertexInputStateCreateInfo vi = {.vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &bind,
        .vertexAttributeDescriptionCount = 1, .pVertexAttributeDescriptions = &attr};
    struct VkRenderPass_T rp = {.natt = 1, .ncol = 1, .depth = -1, .col = {0}, .res = {-1},
        .att = {{.fmt = VK_FORMAT_R32_UINT, .load = VK_ATTACHMENT_LOAD_OP_CLEAR, .store = VK_ATTACHMENT_STORE_OP_STORE}}};
    VkGraphicsPipelineCreateInfo gi = {.stageCount = 2, .pStages = stages, .pVertexInputState = &vi, .renderPass = &rp};
    VkPipeline pipeline = pipe_make(&gi);
    assert(pipeline->pso);
    VkBuffer b = buffer(12, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    uint32_t *indices = b->mem->buf.contents;
    indices[0] = indices[1] = indices[2] = index;
    VkImage im = image(VK_FORMAT_R32_UINT, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    struct VkImageView_T view = {.tex = im->tex, .img = im};
    struct VkFramebuffer_T fb = {.n = 1, .w = extent, .h = extent, .layers = 1, .v = {&view}};
    VkClearValue zero = {0};
    VkRenderPassBeginInfo begin = {.renderPass = &rp, .framebuffer = &fb, .renderArea = {{0, 0}, {extent, extent}}, .clearValueCount = 1, .pClearValues = &zero};
    VkCommandBuffer cb = begin_commands();
    VkDeviceSize offset = 0;
    h_vkCmdBeginRenderPass(cb, &begin, VK_SUBPASS_CONTENTS_INLINE);
    h_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    h_vkCmdBindVertexBuffers(cb, 0, 1, &b, &offset);
    h_vkCmdDraw(cb, 3, 1, 0, 0);
    h_vkCmdEndRenderPass(cb);
    id<MTLBuffer> out = readback(cb, im);
    const uint32_t *pixels = out.contents;
    for (uint32_t i = 0; i < extent * extent; i++) assert(pixels[i] == index);
    h_vkDestroyPipeline(&g_device, pipeline, NULL); h_vkDestroyImage(&g_device, im, NULL); free_buffer(b);
}

static void submit(VkCommandBuffer cb) {
    assert(h_vkEndCommandBuffer(cb) == VK_SUCCESS);
    VkSubmitInfo info = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    assert(h_vkQueueSubmit(&g_queue, 1, &info, NULL) == VK_SUCCESS);
    assert(h_vkQueueWaitIdle(&g_queue) == VK_SUCCESS);
    h_vkFreeCommandBuffers(&g_device, pool, 1, &cb);
}

static void check_query_copy(void) {
    VkQueryPool q;
    VkQueryPoolCreateInfo info = {.queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2};
    assert(h_vkCreateQueryPool(&g_device, &info, NULL, &q) == VK_SUCCESS);
    VkBuffer dst = buffer(48, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    uint64_t *results = dst->mem->buf.contents;
    for (uint32_t i = 0; i < 6; i++) results[i] = UINT64_MAX;
    VkCommandBuffer cb = begin_commands();
    h_vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, q, 0);
    h_vkCmdCopyQueryPoolResults(cb, q, 0, 2, dst, 0, 24, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    submit(cb);
    assert(results[0] > 0 && results[0] != UINT64_MAX && results[1] == 1);
    assert(results[2] == UINT64_MAX && results[3] == UINT64_MAX && results[4] == 0 && results[5] == UINT64_MAX);
    uint64_t timestamp = 0;
    assert(h_vkGetQueryPoolResults(&g_device, q, 0, 1, sizeof timestamp, &timestamp, sizeof timestamp, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS);
    assert(timestamp == results[0]);
    h_vkDestroyQueryPool(&g_device, q, NULL); free_buffer(dst);
}

int main(void) {
    @autoreleasepool {
        dev_init();
        VkCommandPoolCreateInfo info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        assert(h_vkCreateCommandPool(&g_device, &info, NULL, &pool) == VK_SUCCESS);
        check_regions();
        check_bone_index(0x8000);
        check_bone_index(0xffff);
        check_query_copy();
        h_vkDestroyCommandPool(&g_device, pool, NULL);
    }
    puts("Vulkan partial resolve, scaled blit, unsigned widening and query copy: OK");
}
