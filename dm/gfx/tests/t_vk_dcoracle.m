// The Vulkan layer's pass-by-pass LOAD_OP_DONT_CARE oracle, exercised through the layer entrypoints.
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
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)
enum { W = 64, H = 64, BYTES = W * H * 4 };
enum { CASE_FULL, CASE_PARTIAL, CASE_DEPTH, CASE_QUERY, CASE_STORE_DONTCARE, N_REFERENCE_CASES };
static VkDevice dev;
static VkRenderPass color_rp, color_discard_rp, depth_rp;
static VkPipelineLayout layout;
static VkPipeline g_pipe;
static VkCommandPool pool;
static VkCommandBuffer cb;

static VkRenderPass color_pass(VkAttachmentStoreOp store) {
    VkAttachmentDescription ad = {.format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .storeOp = store,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference ar = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sd = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &ar};
    VkRenderPassCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ad,
        .subpassCount = 1, .pSubpasses = &sd};
    VkRenderPass rp = NULL;
    CHECK(h_vkCreateRenderPass(dev, &ci, NULL, &rp) == VK_SUCCESS && rp, "colour render pass created through Vulkan");
    return rp;
}
static VkRenderPass depth_pass(void) {
    VkAttachmentDescription ad = {.format = VK_FORMAT_D32_SFLOAT, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkAttachmentReference ar = {0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sd = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .pDepthStencilAttachment = &ar};
    VkRenderPassCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ad,
        .subpassCount = 1, .pSubpasses = &sd};
    VkRenderPass rp = NULL;
    CHECK(h_vkCreateRenderPass(dev, &ci, NULL, &rp) == VK_SUCCESS && rp, "depth render pass created through Vulkan");
    return rp;
}
static VkPipeline make_pipeline(void) {
    NSString *src = @"#include <metal_stdlib>\nusing namespace metal;\n"
                    "vertex float4 vs_tri(uint v [[vertex_id]]) { float2 p[3] = {float2(-1,-1), float2(3,-1), float2(-1,3)}; return float4(p[v],0,1); }\n"
                    "fragment float4 fs_fill() { return float4(0,1,0,1); }\n";
    NSError *error = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:src options:nil error:&error];
    CHECK(lib != nil, "synthetic Metal shader library compiled");
    if (!lib) return NULL;
    struct VkShaderModule_T vs = {0}, fs = {0};
    vs.fn = [lib newFunctionWithName:@"vs_tri"]; fs.fn = [lib newFunctionWithName:@"fs_fill"];
    strlcpy(vs.key, "oracle_vertex", sizeof vs.key); strlcpy(fs.key, "oracle_fragment", sizeof fs.key);
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = &vs, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = &fs, .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkViewport viewport = {0, 0, W, H, 0, 1}; VkRect2D scissor = {{0, 0}, {W, H}};
    VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1,
        .pViewports = &viewport, .scissorCount = 1, .pScissors = &scissor};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState ba = {.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT};
    VkPipelineColorBlendStateCreateInfo blend = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &ba};
    VkDynamicState dyn_state = VK_DYNAMIC_STATE_SCISSOR;
    VkPipelineDynamicStateCreateInfo dyn = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 1, .pDynamicStates = &dyn_state};
    VkDescriptorSetLayout no_sets[1] = {NULL};
    VkPipelineLayoutCreateInfo lci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pSetLayouts = no_sets};
    CHECK(h_vkCreatePipelineLayout(dev, &lci, NULL, &layout) == VK_SUCCESS, "pipeline layout created through Vulkan");
    VkGraphicsPipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = stages,
        .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
        .pMultisampleState = &ms, .pColorBlendState = &blend, .pDynamicState = &dyn, .layout = layout, .renderPass = color_rp};
    VkPipeline out = NULL;
    CHECK(h_vkCreateGraphicsPipelines(dev, NULL, 1, &ci, NULL, &out) == VK_SUCCESS && out && out->pso,
          "graphics pipeline created through Vulkan");
    return out;
}
static void capture_begin(FILE **file, int *saved) {
    *file = tmpfile(); *saved = dup(STDERR_FILENO);
    CHECK(*file && *saved >= 0, "oracle log capture opened");
    fflush(stderr);
    if (*file && *saved >= 0) CHECK(dup2(fileno(*file), STDERR_FILENO) >= 0, "oracle log capture redirected");
}
static void capture_end(FILE *file, int saved, char *text, size_t cap) {
    fflush(stderr); if (saved >= 0) { dup2(saved, STDERR_FILENO); close(saved); }
    if (!file || !cap) { if (file) fclose(file); if (cap) text[0] = 0; return; }
    if (fseek(file, 0, SEEK_END)) { text[0] = 0; fclose(file); return; }
    long n = ftell(file);
    if (n < 0 || (uint64_t)n >= cap) { text[0] = 0; fclose(file); return; }
    if (fseek(file, 0, SEEK_SET)) { text[0] = 0; fclose(file); return; }
    size_t got = fread(text, 1, (size_t)n, file); text[got] = 0;
    fclose(file);
}
// gate: the GPU holds the submission behind a shared event; the frame is presented while it is held, held() runs then, and the gate opens after it (a present that overtakes its
// submission's completion handler, as a game's present does when it does not wait for the GPU)
static void run_case_ex(int which, uint8_t out[BYTES], id<MTLSharedEvent> gate, void (^held)(void)) {
    int depth = which == CASE_DEPTH;
    VkFormat format = depth ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
    VkImageUsageFlags usage = depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = format,
        .extent = {W, H, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage};
    VkImage image = NULL;
    CHECK(h_vkCreateImage(dev, &ici, NULL, &image) == VK_SUCCESS && image, "attachment image created through Vulkan");
    if (!image) abort();
    VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = format,
        .subresourceRange = {depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView view = NULL;
    CHECK(h_vkCreateImageView(dev, &vci, NULL, &view) == VK_SUCCESS && view, "attachment view created through Vulkan");
    if (!view) abort();
    VkRenderPass rp = depth ? depth_rp : which == CASE_STORE_DONTCARE ? color_discard_rp : color_rp;
    VkFramebufferCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp,
        .attachmentCount = 1, .pAttachments = &view, .width = W, .height = H, .layers = 1};
    VkFramebuffer fb = NULL;
    CHECK(h_vkCreateFramebuffer(dev, &fci, NULL, &fb) == VK_SUCCESS && fb, "framebuffer created through Vulkan");
    if (!fb) abort();
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = BYTES, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer readback = NULL;
    CHECK(h_vkCreateBuffer(dev, &bci, NULL, &readback) == VK_SUCCESS && readback, "readback buffer created through Vulkan");
    if (!readback) abort();
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = BYTES, .memoryTypeIndex = 1};
    VkDeviceMemory memory = NULL;
    CHECK(h_vkAllocateMemory(dev, &mai, NULL, &memory) == VK_SUCCESS && memory, "readback memory allocated through Vulkan");
    if (!memory) abort();
    CHECK(h_vkBindBufferMemory(dev, readback, memory, 0) == VK_SUCCESS, "readback memory bound through Vulkan");
    VkCommandBufferBeginInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(h_vkBeginCommandBuffer(cb, &cbi) == VK_SUCCESS, "command buffer begun through Vulkan");
    VkImageSubresourceRange range = {depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (depth) {
        VkClearDepthStencilValue old = {0.25f, 0};
        h_vkCmdClearDepthStencilImage(cb, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &old, 1, &range);
    } else {
        VkClearColorValue red = {.float32 = {1, 0, 0, 1}};
        h_vkCmdClearColorImage(cb, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &red, 1, &range);
    }
    VkRenderPassBeginInfo rbi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb,
        .renderArea = {{0, 0}, {W, H}}};
    h_vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    VkQueryPool query = NULL;
    if (which == CASE_QUERY) {
        VkQueryPoolCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_OCCLUSION, .queryCount = 1};
        CHECK(h_vkCreateQueryPool(dev, &qci, NULL, &query) == VK_SUCCESS && query, "occlusion query pool created through Vulkan");
        if (!query) abort();
        h_vkCmdBeginQuery(cb, query, 0, 0);
        h_vkCmdEndQuery(cb, query, 0);
    } else if (!depth && which != CASE_STORE_DONTCARE) {
        h_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipe);
        if (which == CASE_PARTIAL) {
            VkRect2D half = {{0, 0}, {W / 2, H}};
            h_vkCmdSetScissor(cb, 0, 1, &half);
        }
        h_vkCmdDraw(cb, 3, 1, 0, 0);
    }
    h_vkCmdEndRenderPass(cb);
    VkBufferImageCopy copy = {.imageSubresource = {depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageExtent = {W, H, 1}};
    h_vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &copy);
    CHECK(h_vkEndCommandBuffer(cb) == VK_SUCCESS, "command buffer ended through Vulkan");
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    if (gate) { id<MTLCommandBuffer> w = [g_q commandBuffer]; [w encodeWaitForEvent:gate value:1]; [w commit]; }   // the queue runs in order: the submission waits behind it
    CHECK(h_vkQueueSubmit(&g_queue, 1, &submit, NULL) == VK_SUCCESS, "Vulkan queue submission accepted");
    VkPresentInfoKHR present = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    if (gate) {
        CHECK(h_vkQueuePresentKHR(&g_queue, &present) == VK_SUCCESS, "Vulkan present while the submission is held on the GPU");
        if (held) held();
        gate.signaledValue = 1;
    }
    CHECK(h_vkQueueWaitIdle(&g_queue) == VK_SUCCESS, "Vulkan queue completed without a GPU stall in replay");
    if (out) memcpy(out, (const uint8_t *)memory->buf.contents + readback->off, BYTES);
    if (!gate) CHECK(h_vkQueuePresentKHR(&g_queue, &present) == VK_SUCCESS, "empty Vulkan present advanced the layer frame counter");
    if (query) h_vkDestroyQueryPool(dev, query, NULL);
    h_vkDestroyFramebuffer(dev, fb, NULL); h_vkDestroyImageView(dev, view, NULL); h_vkDestroyImage(dev, image, NULL);
    h_vkDestroyBuffer(dev, readback, NULL); h_vkFreeMemory(dev, memory, NULL);
}
static void run_case(int which, uint8_t out[BYTES]) { run_case_ex(which, out, nil, nil); }
static void capture_peek(FILE *file, char *text, size_t cap) {   // what the capture holds so far, the capture going on
    fflush(stderr); text[0] = 0;
    if (!file || fseek(file, 0, SEEK_END)) return;
    long n = ftell(file);
    if (n < 0 || (uint64_t)n >= cap || fseek(file, 0, SEEK_SET)) return;
    size_t got = fread(text, 1, (size_t)n, file); text[got] = 0;
    fseek(file, 0, SEEK_END);
}
static void expect_log(const char *log, const char *line, const char *description) {
    CHECK(strstr(log, line) != NULL, description);
    if (!strstr(log, line)) fprintf(stderr, "  missing oracle log: %s\n", line);
}
static void expect_image(int which, const uint8_t px[BYTES]) {
    if (which == CASE_FULL) {
        for (int i = 0; i < W * H; i++) CHECK(!memcmp(px + 4 * i, (uint8_t[]){0, 255, 0, 255}, 4), "full-screen draw stored opaque green");
    } else if (which == CASE_PARTIAL) {
        int good = 1;
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
            const uint8_t *want = x < W / 2 ? (const uint8_t[]){0, 255, 0, 255} : (const uint8_t[]){255, 0, 0, 255};
            if (memcmp(px + 4 * (y * W + x), want, 4)) good = 0;
        }
        CHECK(good, "partial draw stored green over red only in the scissor");
    } else if (which == CASE_DEPTH) {
        const float *d = (const float *)px; int good = 1;
        for (int i = 0; i < W * H; i++) if (d[i] != 0.25f) good = 0;
        CHECK(good, "no-draw depth reference retained the old depth");
    }
}
int main(void) {
    dev = &g_device;
    @autoreleasepool {
        dev_init(); color_rp = color_pass(VK_ATTACHMENT_STORE_OP_STORE);
        color_discard_rp = color_pass(VK_ATTACHMENT_STORE_OP_DONT_CARE); depth_rp = depth_pass(); g_pipe = make_pipeline();
        VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        CHECK(h_vkCreateCommandPool(dev, &pci, NULL, &pool) == VK_SUCCESS, "command pool created through Vulkan");
        VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        CHECK(h_vkAllocateCommandBuffers(dev, &cai, &cb) == VK_SUCCESS, "command buffer allocated through Vulkan");

        uint8_t reference[N_REFERENCE_CASES][BYTES];
        dcoracle_test_range(0, 0); g_frames = 0; g_vk_dontcare = DC_ALL;
        for (int i = 0; i < N_REFERENCE_CASES; i++) {
            g_frames = (uint64_t)i + 1;
            run_case(i, reference[i]);
            expect_image(i, reference[i]);
        }
        dcoracle_test_range(2, 6); g_frames = 0;
        FILE *capture = NULL; int saved = -1; capture_begin(&capture, &saved);
        uint8_t actual[BYTES];
        g_vk_dontcare = DC_ALL; run_case(CASE_FULL, NULL);   // F1: outside the selected range
        g_vk_dontcare = DC_NONE; run_case(CASE_FULL, actual); expect_image(CASE_FULL, actual);
        CHECK(!memcmp(actual, reference[CASE_FULL], BYTES), "F2 oracle output equals the same-frame all-load reference");
        g_vk_dontcare = DC_NONE; run_case(CASE_PARTIAL, actual); expect_image(CASE_PARTIAL, actual);
        CHECK(!memcmp(actual, reference[CASE_PARTIAL], BYTES), "F3 oracle output equals the same-frame all-load reference");
        g_vk_dontcare = DC_NONE; run_case(CASE_DEPTH, actual); expect_image(CASE_DEPTH, actual);
        CHECK(!memcmp(actual, reference[CASE_DEPTH], BYTES), "F4 oracle output equals the same-frame all-load reference");
        g_vk_dontcare = DC_NONE; run_case(CASE_QUERY, actual);
        CHECK(!memcmp(actual, reference[CASE_QUERY], BYTES), "F5 query pass has no visible oracle side effect");
        g_vk_dontcare = DC_NONE; run_case(CASE_STORE_DONTCARE, NULL);
        g_vk_dontcare = DC_NONE; run_case(CASE_FULL, NULL);   // F7: outside the selected range
        char log[8192]; capture_end(capture, saved, log, sizeof log);
        char line[512];
        snprintf(line, sizeof line, "[vk] [dcoracle] F2 p0 c0 %lu 64x64 load DONT_CARE: identical", (unsigned long)MTLPixelFormatRGBA8Unorm);
        expect_log(log, line, "opaque full-screen DONT_CARE pass compares identical");
        snprintf(line, sizeof line, "[vk] [dcoracle] F3 p0 c0 %lu 64x64 load DONT_CARE: differs 2048 / 4096 texels (max byte difference 255)", (unsigned long)MTLPixelFormatRGBA8Unorm);
        expect_log(log, line, "partial DONT_CARE pass reports the uncovered half");
        snprintf(line, sizeof line, "[vk] [dcoracle] F4 p0 d %lu 64x64 load DONT_CARE: differs 4096 / 4096 texels", (unsigned long)MTLPixelFormatDepth32Float);
        expect_log(log, line, "unwritten DONT_CARE depth differs across the target");
        expect_log(log, "[vk] [dcoracle] F5 p0: not checked: occlusion or timestamp query in pass", "query pass is explicitly skipped");
        snprintf(line, sizeof line, "[vk] [dcoracle] F6 p0 c0 %lu 64x64 load DONT_CARE: not compared (store DONT_CARE)", (unsigned long)MTLPixelFormatRGBA8Unorm);
        expect_log(log, line, "store DONT_CARE attachment is not compared");
        expect_log(log, "[vk] [dcoracle] F2-F6: 4 passes checked, attachments 1 identical, 2 differing, 2 skipped", "range summary counts checked and skipped attachments");
        CHECK(!strstr(log, "[vk] [dcoracle] F1 p") && !strstr(log, "[vk] [dcoracle] F7 p"), "frames outside the oracle range produce no pass lines");

        // DM_CTL "dcoracle 1" asked while frame k is recorded checks frame k + 1 only, as "passdump n" starts after the frame being recorded
        uint64_t k = g_frames + 1; char ctl_log[8192];
        capture_begin(&capture, &saved);
        dm_vk_dcoracle_next(1);
        g_vk_dontcare = DC_NONE; run_case(CASE_FULL, NULL); run_case(CASE_FULL, NULL); run_case(CASE_FULL, NULL);
        capture_end(capture, saved, ctl_log, sizeof ctl_log);
        snprintf(line, sizeof line, "[vk] [dcoracle] F%llu p0 c0 %lu 64x64 load DONT_CARE: identical", (unsigned long long)(k + 1), (unsigned long)MTLPixelFormatRGBA8Unorm);
        expect_log(ctl_log, line, "DM_CTL dcoracle 1 checks the frame after the one being recorded");
        snprintf(line, sizeof line, "[vk] [dcoracle] F%llu-F%llu: 1 passes checked, attachments 1 identical, 0 differing, 0 skipped", (unsigned long long)(k + 1), (unsigned long long)(k + 1));
        expect_log(ctl_log, line, "DM_CTL range gets its own summary");
        char before[64], after[64]; snprintf(before, sizeof before, "[vk] [dcoracle] F%llu p", (unsigned long long)k); snprintf(after, sizeof after, "[vk] [dcoracle] F%llu p", (unsigned long long)(k + 2));
        CHECK(!strstr(ctl_log, before) && !strstr(ctl_log, after), "DM_CTL dcoracle 1 leaves the recorded frame and the one after its range alone");

        // The range's last frame is presented while its submission is still held on the GPU: the summary waits for that submission's completion handler and counts its pass
        uint64_t g = g_frames + 1; char gate_log[8192], held_log[8192] = {0};
        dcoracle_test_range(g, g);
        id<MTLSharedEvent> gate = [g_dev newSharedEvent];
        CHECK(gate != nil, "shared event for the GPU gate created");
        capture_begin(&capture, &saved);
        FILE *held_capture = capture; char *held_text = held_log;
        g_vk_dontcare = DC_NONE; run_case_ex(CASE_FULL, NULL, gate, ^{ capture_peek(held_capture, held_text, 8192); });
        capture_end(capture, saved, gate_log, sizeof gate_log);
        snprintf(line, sizeof line, "[vk] [dcoracle] F%llu-F%llu:", (unsigned long long)g, (unsigned long long)g);
        CHECK(!strstr(held_log, line), "no summary while the range's last submission is held on the GPU");
        snprintf(line, sizeof line, "[vk] [dcoracle] F%llu-F%llu: 1 passes checked, attachments 1 identical, 0 differing, 0 skipped", (unsigned long long)g, (unsigned long long)g);
        expect_log(gate_log, line, "a range presented before its submission completed counts that submission's pass in its summary");
        h_vkDestroyPipeline(dev, g_pipe, NULL); h_vkDestroyPipelineLayout(dev, layout, NULL);
        h_vkDestroyRenderPass(dev, color_rp, NULL); h_vkDestroyRenderPass(dev, color_discard_rp, NULL); h_vkDestroyRenderPass(dev, depth_rp, NULL);
        h_vkDestroyCommandPool(dev, pool, NULL);
    }
    printf("Vulkan DONT_CARE oracle: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
