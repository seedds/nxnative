// Vulkan strips and primitive restart: what Metal assembles for a line or triangle strip whose index buffer holds the
// index type's maximum value. Metal restarts strips there itself, so the layer's single indexed draw already matches
// Vulkan's assembly for a restart-enabled pipeline; a pipeline that disabled restart still gets the restart, which is
// the one divergence these cases pin. Every strip is compared pixel for pixel (and sample for sample, by an occlusion
// query) with an explicit line/triangle list of the primitives Vulkan assembles from it, at firstIndex, baseVertex and
// instanceCount, direct and indirect.
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
static VkBuffer vertices, instances, indices, arguments, output;
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

// Seven positions in a compact block: 0,1,2 and 2,1,3 tile its left half, 4,5,6 is a triangle in its right half.
// Everything past 7 is the same block shifted by a band, so a baseVertex the layer dropped lands somewhere else.
// The maximum index holds the centre, where a strip's triangles across a marker show.
static void pos_of(uint32_t v, float out[2]) {
    static const float t[7][2] = {{-0.40f,-0.40f},{-0.10f,-0.40f},{-0.40f,0.40f},{-0.10f,0.40f},{0.10f,-0.40f},{0.40f,-0.40f},{0.10f,0.40f}};
    if (v == 0xffffu || v == 0xffffffffu) { out[0] = 0; out[1] = 0; return; }
    uint32_t band = (v / 7) % 3;
    out[0] = t[v % 7][0] + (band == 1 ? 0.55f : band == 2 ? -0.55f : 0.0f);
    out[1] = t[v % 7][1];
}
static VkPipeline pipeline(VkPrimitiveTopology topo, int restart) {
    VkPipelineShaderStageCreateInfo ss[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = stages[0], .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = stages[1], .pName = "main"}};
    VkVertexInputBindingDescription bindings[] = {{0, 8, VK_VERTEX_INPUT_RATE_VERTEX}, {1, 8, VK_VERTEX_INPUT_RATE_INSTANCE}};
    VkVertexInputAttributeDescription attrs[] = {{0, 0, VK_FORMAT_R32G32_SFLOAT, 0}, {1, 1, VK_FORMAT_R32G32_SFLOAT, 0}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 2, .pVertexBindingDescriptions = bindings, .vertexAttributeDescriptionCount = 2, .pVertexAttributeDescriptions = attrs};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = topo, .primitiveRestartEnable = restart};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE};
    VkViewport viewport = {0, 0, W, H, 0, 1}; VkRect2D scissor = {{0, 0}, {W, H}};
    VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &viewport, .scissorCount = 1, .pScissors = &scissor};
    VkGraphicsPipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = ss,
        .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pRasterizationState = &rs, .pViewportState = &vp, .renderPass = rp};
    VkPipeline p; h_vkCreateGraphicsPipelines(dev, NULL, 1, &ci, NULL, &p); CHECK(p->pso != nil, "strip/list pipeline built"); return p;
}
// One indexed draw into the W x H target, returning the occlusion query's samples. `first` and the index binding offset
// are distinct, so firstIndex has to be added to both the data the CPU writes and the offset Metal is given.
static uint64_t render(VkPrimitiveTopology topo, int restart, unsigned isz, int indirect, uint32_t first,
                       int32_t base, uint32_t inst, const uint32_t *data, uint32_t n, uint8_t out[PIXELS]) {
    VkPipeline p = pipeline(topo, restart);
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .commandBufferCount = 1};
    VkCommandBuffer cb; h_vkAllocateCommandBuffers(dev, &ai, &cb);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; h_vkBeginCommandBuffer(cb, &bi);
    VkDeviceSize binding = 3 * (VkDeviceSize)isz;
    memset(bytes(indices), 0, 1024); memset(bytes(arguments), 0, 64);
    uint8_t *dst = bytes(indices);
    for (uint32_t i = 0; i < n; i++) {
        if (isz == 2) { uint16_t v = (uint16_t)data[i]; memcpy(dst + binding + (first + i) * 2, &v, 2); }
        else memcpy(dst + binding + (first + i) * 4, &data[i], 4);
    }
    VkDrawIndexedIndirectCommand arg = {n, inst, first, base, 0};
    if (indirect) memcpy(bytes(arguments), &arg, sizeof arg);
    h_vkCmdResetQueryPool(cb, query, 0, 1); h_vkCmdBeginQuery(cb, query, 0, VK_QUERY_CONTROL_PRECISE_BIT);
    VkClearValue clear = {.color.float32 = {0, 0, 0, 1}};
    VkRenderPassBeginInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb,
        .renderArea = {{0, 0}, {W, H}}, .clearValueCount = 1, .pClearValues = &clear};
    h_vkCmdBeginRenderPass(cb, &ri, VK_SUBPASS_CONTENTS_INLINE);
    VkBuffer vb[2] = {vertices, instances}; VkDeviceSize offsets[2] = {8, 8};
    h_vkCmdBindVertexBuffers(cb, 0, 2, vb, offsets); h_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    h_vkCmdBindIndexBuffer(cb, indices, binding, isz == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
    if (indirect) h_vkCmdDrawIndexedIndirect(cb, arguments, 0, 1, sizeof(VkDrawIndexedIndirectCommand));
    else h_vkCmdDrawIndexed(cb, n, inst, first, base, 0);
    h_vkCmdEndRenderPass(cb); h_vkCmdEndQuery(cb, query, 0);
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {W, H, 1}};
    h_vkCmdCopyImageToBuffer(cb, im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, output, 1, &copy); h_vkEndCommandBuffer(cb);
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    CHECK(h_vkQueueSubmit(&g_queue, 1, &submit, NULL) == VK_SUCCESS, "submit succeeded"); h_vkQueueWaitIdle(&g_queue);
    memcpy(out, bytes(output), PIXELS); uint64_t samples = 0;
    CHECK(h_vkGetQueryPoolResults(dev, query, 0, 1, sizeof samples, &samples, sizeof samples, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS, "query completed");
    h_vkFreeCommandBuffers(dev, pool, 1, &cb); h_vkDestroyPipeline(dev, p, NULL); return samples;
}

// A strip draw against the explicit list of the primitives Vulkan assembles from it: equal pixels and equal samples.
// `drawn` says the reference must have drawn something, so a blank target cannot make two draws equal.
static void same(const char *what, const uint32_t *list, uint32_t nl, const uint32_t *strip, uint32_t ns, int drawn,
                 VkPrimitiveTopology ltopo, VkPrimitiveTopology stopo, int restart, unsigned isz, int indirect,
                 uint32_t first, int32_t base, uint32_t inst, uint8_t want[PIXELS], uint8_t got[PIXELS]) {
    uint64_t w = render(ltopo, 0, isz, 0, first, base, inst, list, nl, want);
    uint64_t g = render(stopo, restart, isz, indirect, first, base, inst, strip, ns, got);
    if (drawn) CHECK(w > 0, what);
    CHECK(g == w, what);
    CHECK(!memcmp(got, want, PIXELS), what);
    if (g != w || memcmp(got, want, PIXELS)) {
        fprintf(stderr, "  %s: %llu samples against %llu\n", what, (unsigned long long)g, (unsigned long long)w);
        int shown = 0;
        for (int i = 0; i < PIXELS && shown < 4; i += 4)
            if (memcmp(got + i, want + i, 4)) { fprintf(stderr, "    px %d,%d got %u,%u,%u,%u want %u,%u,%u,%u\n", (i / 4) % W, (i / 4) / W, got[i], got[i+1], got[i+2], got[i+3], want[i], want[i+1], want[i+2], want[i+3]); shown++; }
    }
}

int main(void) {
    @autoreleasepool {
        dev_init(); dev = &g_device;
        NSString *msl = @"#include <metal_stdlib>\nusing namespace metal;\n"
            "struct I {float2 p [[attribute(0)]]; float2 shift [[attribute(1)]];}; struct V {float4 p [[position]]; float4 c;};\n"
            "vertex V vs(I a [[stage_in]],uint v [[vertex_id]],uint i [[instance_id]],uint bv [[base_vertex]],uint bi [[base_instance]]) {return {float4(a.p+a.shift,0,1),float4((v%7+1)/8.0,(i+1)/8.0,(bv&15)/32.0,(bi+1)/8.0)};}\n"
            "fragment float4 fs(V a [[stage_in]]) {return a.c;}\n";
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
        vertices = buffer(65540 * 8, 0); instances = buffer(64, 1); indices = buffer(1024, 2); arguments = buffer(64, 3); output = buffer(PIXELS, 4);
        for (uint32_t v = 0; v < 16; v++) { float p[2]; pos_of(v, p); memcpy((uint8_t *)bytes(vertices) + 8 + (size_t)v * 8, p, sizeof p); }
        { float c[2]; pos_of(0xffffu, c); memcpy((uint8_t *)bytes(vertices) + 8 + (size_t)0xffff * 8, c, sizeof c); }
        float shifts[2][2] = {{-0.5f,0},{0.5f,0}}; memcpy((uint8_t *)bytes(instances) + 8, shifts, sizeof shifts);
        uint8_t got[PIXELS], want[PIXELS], first_px[PIXELS];
        // The explicit lists of the primitives Vulkan assembles: 0,1,2,3 is a two-triangle run and 4,5,6 one triangle,
        // and a strip that restarts at a marker resumes even parity at its next index, as these lists do.
        uint32_t tri_list[] = {0,1,2, 2,1,3, 4,5,6};
        uint32_t line_list[] = {0,1, 1,2, 2,3, 4,5, 5,6};
        for (unsigned isz = 2; isz <= 4; isz += 2) {
            uint32_t stop = isz == 2 ? 0xffffu : 0xffffffffu;
            uint32_t tri[] = {0,1,2,3,stop,4,5,6};
            same("a triangle strip restarts at the index type's maximum and draws the runs between them", tri_list, 9, tri, 8, 1,
                 VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, 1, isz, 0, 2, 0, 2, want, got);
            memcpy(first_px, got, PIXELS);
            uint32_t line[] = {0,1,2,3,stop,4,5,6};
            same("a line strip restarts at the index type's maximum and draws the runs between them", line_list, 10, line, 8, 1,
                 VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_STRIP, 1, isz, 0, 2, 0, 2, want, got);
            // The two pipelines differ only in primitiveRestartEnable, and the draw is the same either way: Metal has no
            // way to turn restart off. Vulkan reads this buffer as one strip when the flag is clear.
            uint64_t off = render(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, 0, isz, 0, 2, 0, 2, tri, 8, got);
            uint64_t on = render(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, 1, isz, 0, 2, 0, 2, tri, 8, want);
            CHECK(on > 0 && off == on && !memcmp(got, want, PIXELS), "restart disabled or enabled, Metal draws the same runs: it cannot be turned off");
            CHECK(!memcmp(want, first_px, PIXELS), "the restart flag does not change the strip's pixels");
            // A marker after the last run ends the strip with nothing left to draw.
            uint32_t tri_trail[] = {0,1,2,3,stop,4,5,6,stop};
            same("a marker after the last run draws nothing more", tri_list, 9, tri_trail, 9, 1,
                 VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, 1, isz, 0, 2, 0, 2, want, got);
            uint32_t tri_lead[] = {stop,4,5,6};
            same("a marker as the first index starts the strip after it", tri_list + 6, 3, tri_lead, 4, 1,
                 VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, 1, isz, 0, 2, 0, 2, want, got);
            // A run shorter than the topology's primitive draws nothing: a triangle needs three vertices, a line two.
            uint32_t tri_short[] = {0,1,stop,4,5,6};
            same("a triangle run shorter than three vertices draws nothing", tri_list + 6, 3, tri_short, 6, 1,
                 VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, 1, isz, 0, 2, 0, 2, want, got);
            uint32_t tri_none[] = {0,1,stop,4,5,stop};
            same("markers between short runs draw nothing at all", tri_list + 6, 0, tri_none, 6, 0,
                 VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, 1, isz, 0, 2, 0, 2, want, got);
            uint32_t line_short[] = {0,stop,4,5,6};
            same("a line run shorter than two vertices draws nothing", line_list + 6, 4, line_short, 5, 1,
                 VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_STRIP, 1, isz, 0, 2, 0, 2, want, got);
            uint32_t line_none[] = {0,stop,3,stop};
            same("single-vertex line runs draw nothing at all", line_list, 0, line_none, 4, 0,
                 VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_STRIP, 1, isz, 0, 2, 0, 2, want, got);
            // No marker in the range: the whole strip, one primitive per extra vertex.
            uint32_t tri_plain[] = {0,1,2,3,4,5,6}, tri_plain_list[] = {0,1,2, 2,1,3, 2,3,4, 4,3,5, 4,5,6};
            same("a strip with no marker draws the whole run", tri_plain_list, 15, tri_plain, 7, 1,
                 VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, 1, isz, 0, 2, 0, 2, want, got);
            // baseVertex reaches every run, negative and positive, and instanceCount draws each of them twice.
            uint32_t tri_base[] = {9,10,11,12,stop,13,14,15}, tri_base_list[] = {9,10,11, 11,10,12, 13,14,15};
            same("baseVertex -9 reaches every run of the strip", tri_base_list, 9, tri_base, 8, 1,
                 VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, 1, isz, 0, 2, -9, 2, want, got);
            same("baseVertex +7 reaches every run of the strip", tri_list, 9, tri, 8, 1,
                 VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, 1, isz, 0, 2, 7, 2, want, got);
            // The indirect path takes indexCount, firstIndex, vertexOffset and instanceCount from the argument buffer.
            same("an indirect triangle strip restarts at the same markers", tri_list, 9, tri, 8, 1,
                 VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, 1, isz, 1, 2, 0, 2, want, got);
            same("an indirect line strip restarts at the same markers", line_list, 10, line, 8, 1,
                 VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_STRIP, 1, isz, 1, 2, 0, 2, want, got);
            same("an indirect strip carries a negative baseVertex", tri_base_list, 9, tri_base, 8, 1,
                 VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, 1, isz, 1, 2, -9, 2, want, got);
        }
        // The marker is the index type's maximum, not the value 0xFFFF: a UINT32 index buffer holding 0xFFFF keeps it as
        // an ordinary vertex and the whole strip assembles.
        uint32_t tri_wide[] = {0,1,2,3,0xffff,4,5,6};
        uint32_t tri_joined[] = {0,1,2, 2,1,3, 2,3,0xffff, 0xffff,3,4, 0xffff,4,5, 5,4,6};
        same("a UINT32 strip keeps 0xFFFF as an ordinary index and joins across it", tri_joined, 18, tri_wide, 8, 1,
             VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, 1, 4, 0, 2, 0, 2, want, got);
        // ...where a list draws the maximum index as a vertex at all, and the strip restarts at it. That is the one place
        // Vulkan and Metal disagree: with restart disabled, Vulkan reads the maximum index as a vertex too.
        uint64_t wl = render(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, 2, 0, 2, 0, 2, tri_joined, 18, want);
        uint64_t gl = render(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, 0, 2, 0, 2, 0, 2, tri_wide, 8, got);
        uint64_t gs = render(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, 1, 2, 0, 2, 0, 2, tri_wide, 8, first_px);
        CHECK(wl > 0 && gl == gs && memcmp(got, want, PIXELS), "a list draws the maximum index as a vertex where the strip restarts at it");
        VkBuffer buffers[] = {vertices, instances, indices, arguments, output};
        for (int i = 0; i < 5; i++) { h_vkDestroyBuffer(dev, buffers[i], NULL); h_vkFreeMemory(dev, memories[i], NULL); }
        h_vkDestroyQueryPool(dev, query, NULL); h_vkDestroyCommandPool(dev, pool, NULL); h_vkDestroyFramebuffer(dev, fb, NULL);
        h_vkDestroyRenderPass(dev, rp, NULL); h_vkDestroyImageView(dev, view, NULL); h_vkDestroyImage(dev, im, NULL);
        for (int i = 0; i < 2; i++) h_vkDestroyShaderModule(dev, stages[i], NULL);
    }
    printf("Vulkan strips and primitive restart: %d checks, %d failed\n", checks, fails); return fails != 0;
}
