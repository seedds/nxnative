// Vulkan fans compared with explicit triangle lists: flat attributes, restart, offsets, instancing and GPU-written indices.
#include "../vk/hle_vk.m"
int dm_guest_as_contains(uintptr_t address) { (void)address; return 0; }
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
void dm_vi_layer_crop(int c[4]) { memset(c, 0, 4 * sizeof *c); }
int dm_opmode(void) { return 1; }
static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)
enum { W = 64, H = 32, PIXELS = W * H * 4 };
static VkDevice dev;
static VkRenderPass rp;
static VkFramebuffer fb;
static VkImage im;
static VkCommandPool pool;
static VkQueryPool query;
static VkShaderModule stages[2];
static VkBuffer vertices, instances, indices, source, output;
static VkDeviceMemory memories[5];

static VkBuffer buffer(size_t size, int slot) {
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkMemoryAllocateInfo mi = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = size + 16, .memoryTypeIndex = 1};
    VkBuffer b; h_vkCreateBuffer(dev, &bi, NULL, &b); h_vkAllocateMemory(dev, &mi, NULL, &memories[slot]);
    h_vkBindBufferMemory(dev, b, memories[slot], 16);
    memset((uint8_t *)mem_buf(b->mem).contents, 0, size + 16);
    return b;
}
static void *bytes(VkBuffer b) { return (uint8_t *)mem_buf(b->mem).contents + b->off; }
static VkPipeline pipeline(int fan, int restart, VkCullModeFlags cull) {
    VkPipelineShaderStageCreateInfo ss[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = stages[0], .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = stages[1], .pName = "main"}};
    VkVertexInputBindingDescription bindings[] = {{0, 8, VK_VERTEX_INPUT_RATE_VERTEX}, {1, 8, VK_VERTEX_INPUT_RATE_INSTANCE}};
    VkVertexInputAttributeDescription attrs[] = {{0, 0, VK_FORMAT_R32G32_SFLOAT, 0}, {1, 1, VK_FORMAT_R32G32_SFLOAT, 0}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 2, .pVertexBindingDescriptions = bindings, .vertexAttributeDescriptionCount = 2, .pVertexAttributeDescriptions = attrs};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = fan ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, .primitiveRestartEnable = restart};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = cull, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE};
    VkViewport viewport = {0, 0, W, H, 0, 1}; VkRect2D scissor = {{0, 0}, {W, H}};
    VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &viewport, .scissorCount = 1, .pScissors = &scissor};
    VkGraphicsPipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = ss,
        .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pRasterizationState = &rs, .pViewportState = &vp, .renderPass = rp};
    VkPipeline p; h_vkCreateGraphicsPipelines(dev, NULL, 1, &ci, NULL, &p); CHECK(p->pso != nil, "fan/list pipeline built"); return p;
}
static uint64_t render(int fan, unsigned isz, int restart, int copied, uint32_t first, int32_t base, uint32_t inst,
                       const uint32_t *data, uint32_t n, VkCullModeFlags cull, uint8_t out[PIXELS]) {
    VkPipeline p = pipeline(fan, restart, cull);
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .commandBufferCount = 1};
    VkCommandBuffer cb; h_vkAllocateCommandBuffers(dev, &ai, &cb);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; h_vkBeginCommandBuffer(cb, &bi);
    // Memory binding + index binding + firstIndex are distinct offsets; leave decoys before the actual data.
    unsigned width = isz ? isz : 4; VkDeviceSize binding = 3 * width;
    memset(bytes(indices), 0, 512); memset(bytes(source), 0, 512);
    uint8_t *dst = bytes(copied ? source : indices);
    for (uint32_t i = 0; i < n; i++) {
        if (width == 2) { uint16_t v = data[i]; memcpy(dst + binding + (first + i) * width, &v, 2); }
        else memcpy(dst + binding + (first + i) * width, &data[i], 4);
    }
    if (copied) { VkBufferCopy region = {0, 0, 512}; h_vkCmdCopyBuffer(cb, source, indices, 1, &region); }
    h_vkCmdResetQueryPool(cb, query, 0, 1); h_vkCmdBeginQuery(cb, query, 0, VK_QUERY_CONTROL_PRECISE_BIT);
    VkClearValue clear = {.color.float32 = {0, 0, 0, 1}};
    VkRenderPassBeginInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb,
        .renderArea = {{0, 0}, {W, H}}, .clearValueCount = 1, .pClearValues = &clear};
    h_vkCmdBeginRenderPass(cb, &ri, VK_SUBPASS_CONTENTS_INLINE);
    VkBuffer vb[2] = {vertices, instances}; VkDeviceSize offsets[2] = {8, 8};
    h_vkCmdBindVertexBuffers(cb, 0, 2, vb, offsets); h_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    h_vkCmdBindIndexBuffer(cb, indices, binding, width == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
    // Two draws force a query to span multiple conversion-induced render encoders.
    for (int i = 0; i < 2; i++) {
        if (isz) h_vkCmdDrawIndexed(cb, n, inst, first, base, 2);
        else h_vkCmdDraw(cb, n, inst, first, 2);
    }
    h_vkCmdEndRenderPass(cb); h_vkCmdEndQuery(cb, query, 0);
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {W, H, 1}};
    h_vkCmdCopyImageToBuffer(cb, im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, output, 1, &copy); h_vkEndCommandBuffer(cb);
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    CHECK(h_vkQueueSubmit(&g_queue, 1, &submit, NULL) == VK_SUCCESS, "submit succeeded"); h_vkQueueWaitIdle(&g_queue);
    memcpy(out, bytes(output), PIXELS); uint64_t samples = 0;
    CHECK(h_vkGetQueryPoolResults(dev, query, 0, 1, sizeof samples, &samples, sizeof samples, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS, "query completed");
    h_vkFreeCommandBuffers(dev, pool, 1, &cb); h_vkDestroyPipeline(dev, p, NULL); return samples;
}

// Two indexed draws in one pass. The second reads indices a copy inside the pass wrote (mid_copy: the copy splits the pass, and the second fan must be converted after it,
// the first before it), or indices after the first's (both fans converted before the pass, with no split). *resumed: the render encoders that resumed the pass
static uint64_t render_pair(int fan, int mid_copy, const uint32_t *a, uint32_t na, const uint32_t *b, uint32_t nb, uint8_t out[PIXELS], uint64_t *resumed) {
    VkPipeline p = pipeline(fan, 0, VK_CULL_MODE_NONE);
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .commandBufferCount = 1};
    VkCommandBuffer cb; h_vkAllocateCommandBuffers(dev, &ai, &cb);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; h_vkBeginCommandBuffer(cb, &bi);
    memset(bytes(indices), 0, 512); memset(bytes(source), 0, 512); memset(bytes(instances), 0, 8 + 2 * 8);
    memcpy(bytes(indices), a, na * 4);
    if (mid_copy) memcpy(bytes(source), b, nb * 4); else memcpy((uint8_t *)bytes(indices) + na * 4, b, nb * 4);
    h_vkCmdResetQueryPool(cb, query, 0, 1); h_vkCmdBeginQuery(cb, query, 0, VK_QUERY_CONTROL_PRECISE_BIT);
    VkClearValue clear = {.color.float32 = {0, 0, 0, 1}};
    VkRenderPassBeginInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb,
        .renderArea = {{0, 0}, {W, H}}, .clearValueCount = 1, .pClearValues = &clear};
    h_vkCmdBeginRenderPass(cb, &ri, VK_SUBPASS_CONTENTS_INLINE);
    VkBuffer vb[2] = {vertices, instances}; VkDeviceSize offsets[2] = {8, 8};
    h_vkCmdBindVertexBuffers(cb, 0, 2, vb, offsets); h_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    h_vkCmdBindIndexBuffer(cb, indices, 0, VK_INDEX_TYPE_UINT32);
    h_vkCmdDrawIndexed(cb, na, 1, 0, 0, 0);
    if (mid_copy) { VkBufferCopy region = {0, 0, 512}; h_vkCmdCopyBuffer(cb, source, indices, 1, &region); }
    h_vkCmdDrawIndexed(cb, nb, 1, mid_copy ? 0 : na, 0, 0);
    h_vkCmdEndRenderPass(cb); h_vkCmdEndQuery(cb, query, 0);
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {W, H, 1}};
    h_vkCmdCopyImageToBuffer(cb, im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, output, 1, &copy); h_vkEndCommandBuffer(cb);
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    uint64_t r0 = g_n_resume;
    CHECK(h_vkQueueSubmit(&g_queue, 1, &submit, NULL) == VK_SUCCESS, "submit succeeded"); h_vkQueueWaitIdle(&g_queue);
    *resumed = g_n_resume - r0;
    memcpy(out, bytes(output), PIXELS); uint64_t samples = 0;
    CHECK(h_vkGetQueryPoolResults(dev, query, 0, 1, sizeof samples, &samples, sizeof samples, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS, "query completed");
    h_vkFreeCommandBuffers(dev, pool, 1, &cb); h_vkDestroyPipeline(dev, p, NULL); return samples;
}

static void check_indices(unsigned isz, int restart, const uint32_t *input, uint32_t count, const uint32_t *expected, uint32_t n) {
    NSError *err = nil; id<MTLLibrary> lib = host_msl_lib(g_dev, nil, "vk_fan", &err);
    id<MTLFunction> fn = [lib newFunctionWithName:@"vk_fan"];
    id<MTLComputePipelineState> ps = fn ? [g_dev newComputePipelineStateWithFunction:fn error:&err] : nil;
    if (!ps) { fprintf(stderr, "index fixture: %s\n", err.localizedDescription.UTF8String); abort(); }
    id<MTLBuffer> src = [g_dev newBufferWithLength:256 options:MTLResourceStorageModeShared];
    id<MTLBuffer> out = [g_dev newBufferWithLength:512 options:MTLResourceStorageModeShared];
    memset(src.contents, 0, 256); memset(out.contents, 0xcd, 512);
    for (uint32_t i = 0; i < count; i++) {
        if (isz == 2) { uint16_t v = input[i]; memcpy((uint8_t *)src.contents + 2 + 2 * i, &v, 2); }
        else memcpy((uint8_t *)src.contents + 4 + 4 * i, &input[i], 4);
    }
    struct { uint64_t offset; uint32_t count, isz, restart, inst; int32_t base; uint32_t first_inst; } p = {isz, count, isz, (uint32_t)restart, 3, -7, 9};
    id<MTLCommandBuffer> cb = [g_q commandBuffer]; id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
    [ce setComputePipelineState:ps]; [ce setBuffer:src offset:0 atIndex:0]; [ce setBuffer:out offset:32 atIndex:1]; [ce setBuffer:out offset:0 atIndex:2];
    [ce setBytes:&p length:sizeof p atIndex:3]; NSUInteger threads = restart ? 1 : count - 2;
    [ce dispatchThreads:MTLSizeMake(threads,1,1) threadsPerThreadgroup:MTLSizeMake(MIN(threads,ps.threadExecutionWidth),1,1)];
    [ce endEncoding]; [cb commit]; [cb waitUntilCompleted];
    CHECK(cb.status == MTLCommandBufferStatusCompleted, "index conversion completed");
    uint32_t args[] = {n,3,0,(uint32_t)-7,9}; uint32_t *ix = (uint32_t *)((uint8_t *)out.contents + 32);
    CHECK(!memcmp(out.contents, args, sizeof args), "exact indirect count, instances and signed base vertex");
    CHECK(!memcmp(ix, expected, n * 4), "exact compacted indices preserve Vulkan order");
    CHECK(ix[n] == 0xcdcdcdcdu, "conversion leaves bytes after the compacted list untouched");
}

int main(void) {
    @autoreleasepool {
        dev_init(); dev = &g_device;
        NSString *msl = @"#include <metal_stdlib>\nusing namespace metal;\n"
            "struct I {float2 p [[attribute(0)]]; float2 shift [[attribute(1)]];}; struct V {float4 p [[position]]; float4 c [[flat]];};\n"
            "vertex V vs(I a [[stage_in]],uint v [[vertex_id]],uint i [[instance_id]],uint bv [[base_vertex]],uint bi [[base_instance]]) {return {float4(a.p*0.45+a.shift,0,1),float4((v%7+1)/8.0,(i+1)/8.0,(bv&15)/32.0,(bi+1)/8.0)};}\n"
            "fragment float4 fs(V a [[stage_in]],uint p [[primitive_id]]) {return a.c+float4(0,0,(p%5+1)/16.0,0);}\n";
        NSError *err = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:msl options:nil error:&err];
        if (!lib) { fprintf(stderr, "fixture shader: %s\n", err.localizedDescription.UTF8String); return 1; }
        for (int i = 0; i < 2; i++) { stages[i] = zalloc(sizeof *stages[i]); stages[i]->fn = [lib newFunctionWithName:i ? @"fs" : @"vs"]; }
        VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
            .extent = {W, H, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        h_vkCreateImage(dev, &ii, NULL, &im);
        VkImageViewCreateInfo iv = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = ii.format,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VkImageView view; h_vkCreateImageView(dev, &iv, NULL, &view);
        VkAttachmentDescription ad = {.format = ii.format, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        VkAttachmentReference ar = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}; VkSubpassDescription sp = {.colorAttachmentCount = 1, .pColorAttachments = &ar};
        VkRenderPassCreateInfo rci = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ad, .subpassCount = 1, .pSubpasses = &sp};
        h_vkCreateRenderPass(dev, &rci, NULL, &rp);
        VkFramebufferCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = 1, .pAttachments = &view, .width = W, .height = H, .layers = 1};
        h_vkCreateFramebuffer(dev, &fi, NULL, &fb);
        VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; h_vkCreateCommandPool(dev, &pi, NULL, &pool);
        VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_OCCLUSION, .queryCount = 1}; h_vkCreateQueryPool(dev, &qi, NULL, &query);
        vertices = buffer(65540 * 8, 0); instances = buffer(64, 1); indices = buffer(512, 2); source = buffer(512, 3); output = buffer(PIXELS, 4);
        float shape[6][2] = {{0,0},{-1,-1},{1,-1},{1,1},{-1,1},{-1,-1}};
        memcpy((uint8_t *)bytes(vertices) + 8 + 7 * 8, shape, sizeof shape);
        float shifts[2][2] = {{-0.5f,0},{0.5f,0}}; memcpy((uint8_t *)bytes(instances) + 8 + 2 * 8, shifts, sizeof shifts);
        uint8_t got[PIXELS], expected[PIXELS];
        for (unsigned isz = 0; isz <= 4; isz += 2) for (int positive = 0; positive <= !!isz; positive++)
        for (int restart = 0; restart < 2; restart++) for (int copied = 0; copied <= !!isz; copied++) {
            int32_t base = isz ? positive ? 4 : -3 : 0;
            uint32_t a = isz ? positive ? 3 : 10 : 7, first = isz ? 2 : 7, stop = isz == 2 ? 0xffffu : 0xffffffffu;
            uint32_t fan[] = {a,a+1,a+2,a+3,a+4,a+5};
            uint32_t split[] = {stop,a,a+1,stop,a,a+1,a+2,a+3,stop,stop,a,a+3,a+4,a+5,stop};
            // Vulkan fan p_i = (v_i+1,v_i+2,v_0): first provoking vertex is the rim, not the anchor.
            uint32_t list[] = {a+1,a+2,a, a+2,a+3,a, a+3,a+4,a, a+4,a+5,a};
            if (!isz) for (unsigned j = 0; j < 12; j++) list[j] -= first;
            const uint32_t *input = restart && isz ? split : fan; unsigned count = restart && isz ? 15 : 6;
            for (int cull = 0; cull < 3; cull++) {
                VkCullModeFlags cm = cull == 1 ? VK_CULL_MODE_BACK_BIT : cull == 2 ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_NONE;
                uint64_t want = render(0, 4, 0, 0, 2, isz ? base : (int32_t)first, 2, list, 12, cm, expected);
                uint64_t samples = render(1, isz, restart, copied, first, base, 2, input, count, cm, got);
                CHECK(!memcmp(got, expected, PIXELS), "fan pixels match explicit list, including flat attributes and primitive IDs");
                CHECK(samples == want, "query counts match the list's");
                if (!cull) CHECK(want > 0, "reference draws visible pixels");
                if (memcmp(got, expected, PIXELS)) fprintf(stderr, "  isz=%u restart=%d copied=%d cull=%d base=%d\n", isz,restart,copied,cull,base);
            }
        }
        for (unsigned count = 0; count < 3; count++) for (unsigned isz = 0; isz <= 4; isz += 2) {
            uint32_t data[] = {7,8}; render(0, 4, 0, 0, 0, 0, 2, data, 0, 0, expected);
            uint64_t n = render(1, isz, 0, 0, isz ? 0 : 7, 0, 2, data, count, 0, got);
            CHECK(!n && !memcmp(got, expected, PIXELS), "fewer than three vertices produce no primitive");
        }
        for (unsigned isz = 0; isz <= 4; isz += 2) {
            uint32_t data[] = {7,8,9}, list[] = {8,9,7};
            if (!isz) for (unsigned j = 0; j < 3; j++) list[j] -= 7;
            uint64_t want = render(0, 4, 0, 0, 0, isz ? 0 : 7, 2, list, 3, 0, expected);
            uint64_t n = render(1, isz, 1, 0, isz ? 0 : 7, 0, 2, data, 3, 0, got);
            CHECK(n == want && !memcmp(got, expected, PIXELS), "three vertices form one triangle");
            render(0, 4, 0, 0, 0, 0, 0, list, 3, 0, expected);
            n = render(1, isz, 0, 0, isz ? 0 : 7, 0, 0, data, 3, 0, got);
            CHECK(!n && !memcmp(got, expected, PIXELS), "zero instances produce no primitive");
        }
        // With restart disabled, UINT16_MAX is an ordinary index, not a marker.
        memcpy((uint8_t *)bytes(vertices) + 8 + 65535 * 8, shape[2], 8);
        uint32_t max_fan[] = {7,8,65535,10,11,12}, max_list[] = {8,65535,7, 65535,10,7, 10,11,7, 11,12,7};
        uint64_t want = render(0, 4, 0, 0, 2, 0, 2, max_list, 12, 0, expected);
        uint64_t n = render(1, 2, 0, 1, 2, 0, 2, max_fan, 6, 0, got);
        CHECK(n == want && !memcmp(got, expected, PIXELS), "restart disabled retains the full uint16 index range");
        for (unsigned isz = 2; isz <= 4; isz += 2) {
            uint32_t stop = isz == 2 ? 0xffffu : 0xffffffffu;
            uint32_t empty[] = {stop,7,stop,7,8,stop,stop};
            render(0, 4, 0, 0, 0, 0, 2, empty, 0, 0, expected);
            n = render(1, isz, 1, 1, 2, 0, 2, empty, 7, 0, got);
            CHECK(!n && !memcmp(got, expected, PIXELS), "only incomplete restart runs produce zero indexed arguments");
            uint32_t split[] = {stop,10,11,stop,10,11,12,13,stop,stop,20,21,22,stop};
            uint32_t list[] = {11,12,10, 12,13,10, 21,22,20};
            check_indices(isz, 1, split, 14, list, 9);
            check_indices(isz, 1, empty, 7, list, 0);
            uint32_t plain[] = {12,3,stop,29}, full[] = {3,stop,12, stop,29,12};
            check_indices(isz, 0, plain, 4, full, 6);
        }
        // Several fans in one pass are converted together before it, and a copy inside the pass orders them: the first fan reads the indices before it, the second after it
        for (int mid_copy = 0; mid_copy < 2; mid_copy++) {
            uint32_t fa[] = {7,8,9,10}, fb[] = {7,10,11,12}, la[] = {8,9,7, 9,10,7}, lb[] = {10,11,7, 11,12,7}; uint64_t rl, rf;
            uint64_t want = render_pair(0, mid_copy, la, 6, lb, 6, expected, &rl);
            uint64_t n = render_pair(1, mid_copy, fa, 4, fb, 4, got, &rf);
            CHECK(want > 0 && n == want && !memcmp(got, expected, PIXELS), "two fans in one pass match their lists, around a copy inside the pass");
            CHECK(rf == (uint64_t)mid_copy && rl == (uint64_t)mid_copy, "fans split the pass only where its copy does");
            if (rf != (uint64_t)mid_copy) fprintf(stderr, "  mid_copy=%d: the fans resumed the pass %llu times\n", mid_copy, (unsigned long long)rf);
        }
        VkBuffer buffers[] = {vertices, instances, indices, source, output};
        for (int i = 0; i < 5; i++) { h_vkDestroyBuffer(dev, buffers[i], NULL); h_vkFreeMemory(dev, memories[i], NULL); }
        h_vkDestroyQueryPool(dev, query, NULL); h_vkDestroyCommandPool(dev, pool, NULL); h_vkDestroyFramebuffer(dev, fb, NULL);
        h_vkDestroyRenderPass(dev, rp, NULL); h_vkDestroyImageView(dev, view, NULL); h_vkDestroyImage(dev, im, NULL);
        for (int i = 0; i < 2; i++) h_vkDestroyShaderModule(dev, stages[i], NULL);
    }
    printf("Vulkan fans: %d checks, %d failed\n", checks, fails); return fails != 0;
}
