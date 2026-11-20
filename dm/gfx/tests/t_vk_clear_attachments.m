// GPU contracts for vkCmdClearAttachments: exact regions, typed values, view-relative layers, independent aspects and state/order across an encoder split.
#include "../vk/hle_vk.m"
#include <assert.h>

int dm_guest_as_contains(uintptr_t address) { (void)address; return 0; }
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
void dm_vi_layer_crop(int c[4]) { memset(c, 0, 4 * sizeof *c); }
int dm_opmode(void) { return 1; }

static VkCommandPool pool;
static int cases;
typedef struct { VkImage im; VkImageView view; struct VkRenderPass_T rp; struct VkFramebuffer_T fb; } target;

static target make_target(VkFormat format, uint32_t size, uint32_t mips, uint32_t layers, uint32_t mip, uint32_t base, uint32_t count, VkSampleCountFlagBits samples) {
    target t = {0};
    const fmt_t *f = fmt_of(format); int ds = fmt_has_depth(f) || fmt_has_stencil(f);
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = format, .extent = {size, size, 1},
        .mipLevels = mips, .arrayLayers = layers, .samples = samples,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | (ds ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)};
    assert(h_vkCreateImage(&g_device, &ci, NULL, &t.im) == VK_SUCCESS);
    VkImageAspectFlags aspects = ds ? (fmt_has_depth(f) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (fmt_has_stencil(f) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0) : VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = t.im, .viewType = count > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D,
        .format = format, .subresourceRange = {aspects, mip, 1, base, count}};
    assert(h_vkCreateImageView(&g_device, &vi, NULL, &t.view) == VK_SUCCESS && t.view->tex);
    t.rp = (struct VkRenderPass_T){.natt = 1, .ncol = ds ? 0 : 1, .depth = ds ? 0 : -1, .col = {0}, .res = {-1},
        .att = {{.fmt = format, .load = VK_ATTACHMENT_LOAD_OP_CLEAR, .store = VK_ATTACHMENT_STORE_OP_STORE,
                 .sload = VK_ATTACHMENT_LOAD_OP_CLEAR, .sstore = VK_ATTACHMENT_STORE_OP_STORE}}};
    t.fb = (struct VkFramebuffer_T){.n = 1, .w = size >> mip, .h = size >> mip, .layers = count, .v = {t.view}};
    return t;
}
static void free_target(target *t) { h_vkDestroyImageView(&g_device, t->view, NULL); h_vkDestroyImage(&g_device, t->im, NULL); }
static VkCommandBuffer commands(VkCommandBufferLevel level, VkRenderPass rp, VkFramebuffer fb) {
    VkCommandBuffer cb;
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = level, .commandBufferCount = 1};
    assert(h_vkAllocateCommandBuffers(&g_device, &ai, &cb) == VK_SUCCESS);
    VkCommandBufferInheritanceInfo inherit = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO, .renderPass = rp, .framebuffer = fb};
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = level == VK_COMMAND_BUFFER_LEVEL_SECONDARY ? VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT : 0, .pInheritanceInfo = &inherit};
    assert(h_vkBeginCommandBuffer(cb, &bi) == VK_SUCCESS); return cb;
}
static VkCommandBuffer begin(target *t, VkClearValue value) {
    VkCommandBuffer cb = commands(VK_COMMAND_BUFFER_LEVEL_PRIMARY, NULL, NULL);
    VkRenderPassBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = &t->rp, .framebuffer = &t->fb,
        .renderArea = {{0, 0}, {t->fb.w, t->fb.h}}, .clearValueCount = 1, .pClearValues = &value};
    h_vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE); return cb;
}
static void submit(VkCommandBuffer cb) {
    assert(h_vkEndCommandBuffer(cb) == VK_SUCCESS);
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    assert(h_vkQueueSubmit(&g_queue, 1, &si, NULL) == VK_SUCCESS);
    assert(h_vkQueueWaitIdle(&g_queue) == VK_SUCCESS);
    h_vkFreeCommandBuffers(&g_device, pool, 1, &cb);
}
static id<MTLBuffer> pixels(VkImage im, uint32_t mip, uint32_t layer, uint32_t bytes, MTLBlitOption option) {
    uint32_t w = im->w >> mip, h = im->h >> mip;
    id<MTLCommandBuffer> cb = [g_q commandBuffer];
    id<MTLTexture> tex = im->tex;
    if (im->samples > 1) {
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:tex.pixelFormat width:w height:h mipmapped:NO];
        td.storageMode = MTLStorageModePrivate; td.usage = MTLTextureUsageRenderTarget;
        id<MTLTexture> resolved = [g_dev newTextureWithDescriptor:td];
        MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
        d.colorAttachments[0].texture = tex; d.colorAttachments[0].resolveTexture = resolved;
        d.colorAttachments[0].loadAction = MTLLoadActionLoad; d.colorAttachments[0].storeAction = MTLStoreActionMultisampleResolve;
        [[cb renderCommandEncoderWithDescriptor:d] endEncoding]; tex = resolved;
    }
    id<MTLBuffer> out = [g_dev newBufferWithLength:w * h * bytes options:MTLResourceStorageModeShared];
    id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
    [be copyFromTexture:tex sourceSlice:layer sourceLevel:mip sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
              toBuffer:out destinationOffset:0 destinationBytesPerRow:w * bytes destinationBytesPerImage:w * h * bytes options:option];
    [be endEncoding]; [cb commit]; [cb waitUntilCompleted]; assert(cb.status == MTLCommandBufferStatusCompleted); return out;
}
static int inside(uint32_t x, uint32_t y) { return x >= 1 && x < 3 && y >= 1 && y < 3; }

static void typed_colors(void) {
    static const struct { VkFormat format; VkClearColorValue value; uint32_t bits; } formats[] = {
        {VK_FORMAT_R32_UINT, {.uint32 = {0x80000000}}, 0x80000000},
        {VK_FORMAT_R32_UINT, {.uint32 = {0xffffffff}}, 0xffffffff},
        {VK_FORMAT_R32_SINT, {.int32 = {-5}}, (uint32_t)-5},
        {VK_FORMAT_R32_SFLOAT, {.float32 = {0.5f}}, 0x3f000000},
        {VK_FORMAT_R8G8B8A8_SRGB, {.float32 = {0.5f, 0.5f, 0.5f, 1}}, 0xffbcbcbc},
    };
    for (uint32_t k = 0; k < sizeof formats / sizeof *formats; k++) for (int whole = 0; whole < 2; whole++) {
        target t = make_target(formats[k].format, 4, 1, 1, 0, 0, 1, VK_SAMPLE_COUNT_1_BIT);
        VkCommandBuffer cb = begin(&t, (VkClearValue){0});
        VkClearAttachment a = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .clearValue.color = formats[k].value};
        VkClearRect r = {whole ? (VkRect2D){{0, 0}, {4, 4}} : (VkRect2D){{1, 1}, {2, 2}}, 0, 1};
        h_vkCmdClearAttachments(cb, 1, &a, 1, &r); h_vkCmdEndRenderPass(cb); submit(cb);
        id<MTLBuffer> out = pixels(t.im, 0, 0, 4, MTLBlitOptionNone); const uint32_t *p = out.contents;
        for (uint32_t y = 0; y < 4; y++) for (uint32_t x = 0; x < 4; x++) assert(p[y * 4 + x] == (whole || inside(x, y) ? formats[k].bits : 0));
        free_target(&t); cases++;
    }
}
static void attachments_and_rectangles(void) {
    target a = make_target(VK_FORMAT_R32_UINT, 4, 1, 1, 0, 0, 1, VK_SAMPLE_COUNT_1_BIT);
    target b = make_target(VK_FORMAT_R32_UINT, 4, 1, 1, 0, 0, 1, VK_SAMPLE_COUNT_1_BIT);
    a.rp.natt = 2; a.rp.ncol = 3; a.rp.col[1] = -1; a.rp.col[2] = 1; a.rp.res[1] = a.rp.res[2] = -1; a.rp.att[1] = a.rp.att[0];
    a.fb.n = 2; a.fb.v[1] = b.view;
    VkCommandBuffer cb = commands(VK_COMMAND_BUFFER_LEVEL_PRIMARY, NULL, NULL);
    VkClearValue initial[] = {{.color.uint32 = {11}}, {.color.uint32 = {22}}};
    VkRenderPassBeginInfo bi = {.renderPass = &a.rp, .framebuffer = &a.fb, .renderArea = {{0, 0}, {4, 4}}, .clearValueCount = 2, .pClearValues = initial};
    h_vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    VkClearAttachment clears[] = {{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = 0, .clearValue.color.uint32 = {0x80000000}},
        {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = 1},   // an unused subpass slot must not discard the other clears
        {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = 2, .clearValue.color.uint32 = {0xffffffff}}};
    VkClearRect rects[] = {{{{1, 1}, {2, 2}}, 0, 1}, {{{0, 0}, {1, 1}}, 0, 1}};
    h_vkCmdClearAttachments(cb, 3, clears, 2, rects); h_vkCmdEndRenderPass(cb); submit(cb);
    id<MTLBuffer> pa = pixels(a.im, 0, 0, 4, MTLBlitOptionNone), pb = pixels(b.im, 0, 0, 4, MTLBlitOptionNone);
    const uint32_t *p = pa.contents, *q = pb.contents;
    for (uint32_t y = 0; y < 4; y++) for (uint32_t x = 0; x < 4; x++) {
        int changed = inside(x, y) || (!x && !y);
        assert(p[y * 4 + x] == (changed ? 0x80000000 : 11)); assert(q[y * 4 + x] == (changed ? 0xffffffff : 22));
    }
    free_target(&a); free_target(&b); cases++;
}
static void view_layers(void) {
    target t = make_target(VK_FORMAT_R32_UINT, 8, 2, 4, 1, 1, 2, VK_SAMPLE_COUNT_1_BIT);
    t.rp.att[0].load = VK_ATTACHMENT_LOAD_OP_LOAD;
    VkCommandBuffer cb = commands(VK_COMMAND_BUFFER_LEVEL_PRIMARY, NULL, NULL);
    VkClearColorValue old = {.uint32 = {7}}; VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 2, 0, 4};
    h_vkCmdClearColorImage(cb, t.im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &old, 1, &all);
    VkRenderPassBeginInfo bi = {.renderPass = &t.rp, .framebuffer = &t.fb, .renderArea = {{0, 0}, {4, 4}}};
    h_vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    VkClearAttachment a = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .clearValue.color.uint32 = {0xffffffff}};
    VkClearRect r[] = {{{{1, 1}, {2, 2}}, 1, 1}, {{{3, 0}, {1, 1}}, 0, 2}};
    h_vkCmdClearAttachments(cb, 1, &a, 2, r); h_vkCmdEndRenderPass(cb); submit(cb);
    for (uint32_t mip = 0; mip < 2; mip++) for (uint32_t layer = 0; layer < 4; layer++) {
        id<MTLBuffer> out = pixels(t.im, mip, layer, 4, MTLBlitOptionNone); const uint32_t *p = out.contents; uint32_t w = 8 >> mip;
        for (uint32_t y = 0; y < w; y++) for (uint32_t x = 0; x < w; x++) {
            int changed = mip == 1 && ((layer == 2 && inside(x, y)) || ((layer == 1 || layer == 2) && x == 3 && y == 0));
            assert(p[y * w + x] == (changed ? 0xffffffff : 7));
        }
    }
    free_target(&t); cases++;
}
static void multisample(void) {
    target t = make_target(VK_FORMAT_R8G8B8A8_UNORM, 4, 1, 1, 0, 0, 1, VK_SAMPLE_COUNT_4_BIT);
    VkCommandBuffer cb = begin(&t, (VkClearValue){.color.float32 = {0, 0, 1, 1}});
    VkClearAttachment a = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .clearValue.color.float32 = {1, 0, 0, 1}};
    VkClearRect r = {{{1, 1}, {2, 2}}, 0, 1};
    h_vkCmdClearAttachments(cb, 1, &a, 1, &r); h_vkCmdEndRenderPass(cb); submit(cb);
    id<MTLBuffer> out = pixels(t.im, 0, 0, 4, MTLBlitOptionNone); const uint32_t *p = out.contents;
    for (uint32_t y = 0; y < 4; y++) for (uint32_t x = 0; x < 4; x++) assert(p[y * 4 + x] == (inside(x, y) ? 0xff0000ff : 0xffff0000));
    free_target(&t); cases++;
}
static void depth_stencil(void) {
    const VkFormat formats[] = {VK_FORMAT_D32_SFLOAT, VK_FORMAT_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT};
    for (uint32_t k = 0; k < 3; k++) {
        target t = make_target(formats[k], 4, 1, 1, 0, 0, 1, VK_SAMPLE_COUNT_1_BIT);
        const fmt_t *f = fmt_of(formats[k]); int depth = fmt_has_depth(f), stencil = fmt_has_stencil(f);
        VkCommandBuffer cb = begin(&t, (VkClearValue){.depthStencil = {1, 0x35}});
        VkClearAttachment a = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .clearValue.depthStencil = {0.25f, 0xa7}};
        VkClearRect r = {{{1, 1}, {2, 2}}, 0, 1};
        if (depth) h_vkCmdClearAttachments(cb, 1, &a, 1, &r);
        a.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT; r.rect = (VkRect2D){{0, 0}, {4, 1}};
        if (stencil) h_vkCmdClearAttachments(cb, 1, &a, 1, &r);
        a.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT; a.clearValue.depthStencil = (VkClearDepthStencilValue){0.75f, 0x81};
        r.rect = (VkRect2D){{0, 3}, {1, 1}};
        h_vkCmdClearAttachments(cb, 1, &a, 1, &r);   // absent aspects have no effect
        h_vkCmdEndRenderPass(cb); submit(cb);
        if (depth) {
            id<MTLBuffer> out = pixels(t.im, 0, 0, 4, stencil ? MTLBlitOptionDepthFromDepthStencil : MTLBlitOptionNone); const float *p = out.contents;
            for (uint32_t y = 0; y < 4; y++) for (uint32_t x = 0; x < 4; x++) assert(p[y * 4 + x] == (!x && y == 3 ? 0.75f : inside(x, y) ? 0.25f : 1));
        }
        if (stencil) {
            id<MTLBuffer> out = pixels(t.im, 0, 0, 1, depth ? MTLBlitOptionStencilFromDepthStencil : MTLBlitOptionNone); const uint8_t *p = out.contents;
            for (uint32_t y = 0; y < 4; y++) for (uint32_t x = 0; x < 4; x++) assert(p[y * 4 + x] == (!x && y == 3 ? 0x81 : !y ? 0xa7 : 0x35));
        }
        free_target(&t); cases++;
    }
}
static void secondary_clear(void) {
    target t = make_target(VK_FORMAT_R32_UINT, 4, 1, 1, 0, 0, 1, VK_SAMPLE_COUNT_1_BIT);
    VkCommandBuffer secondary = commands(VK_COMMAND_BUFFER_LEVEL_SECONDARY, &t.rp, &t.fb);
    VkClearAttachment a = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .clearValue.color.uint32 = {0xffffffff}};
    VkClearRect r = {{{1, 1}, {2, 2}}, 0, 1}; h_vkCmdClearAttachments(secondary, 1, &a, 1, &r);
    assert(h_vkEndCommandBuffer(secondary) == VK_SUCCESS);
    VkCommandBuffer cb = commands(VK_COMMAND_BUFFER_LEVEL_PRIMARY, NULL, NULL); VkClearValue zero = {0};
    VkRenderPassBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = &t.rp, .framebuffer = &t.fb,
        .renderArea = {{0, 0}, {4, 4}}, .clearValueCount = 1, .pClearValues = &zero};
    h_vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
    h_vkCmdExecuteCommands(cb, 1, &secondary); h_vkCmdEndRenderPass(cb); submit(cb);
    id<MTLBuffer> out = pixels(t.im, 0, 0, 4, MTLBlitOptionNone); const uint32_t *p = out.contents;
    for (uint32_t y = 0; y < 4; y++) for (uint32_t x = 0; x < 4; x++) assert(p[y * 4 + x] == (inside(x, y) ? 0xffffffff : 0));
    h_vkFreeCommandBuffers(&g_device, pool, 1, &secondary); free_target(&t); cases++;
}
static void draw_clear_draw(void) {
    target t = make_target(VK_FORMAT_R32_UINT, 4, 1, 1, 0, 0, 1, VK_SAMPLE_COUNT_1_BIT);
    NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct In { uint v [[attribute(0)]]; }; struct Out { float4 p [[position]]; uint v [[flat]]; };\n"
        "vertex Out vs(In in [[stage_in]], uint i [[vertex_id]]) { return {float4(float2((i << 1) & 2, i & 2)*2-1,0,1),in.v}; }\n"
        "fragment uint fs(Out in [[stage_in]]) { return in.v; }";
    NSError *err = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:source options:nil error:&err]; assert(lib);
    struct VkShaderModule_T vs = {.fn = [lib newFunctionWithName:@"vs"]}, fs = {.fn = [lib newFunctionWithName:@"fs"]};
    VkPipelineShaderStageCreateInfo stages[] = {{.stage = VK_SHADER_STAGE_VERTEX_BIT, .module = &vs}, {.stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = &fs}};
    VkVertexInputAttributeDescription attr = {0, 0, VK_FORMAT_R32_UINT, 0}; VkVertexInputBindingDescription bind = {0, 4, VK_VERTEX_INPUT_RATE_VERTEX};
    VkPipelineVertexInputStateCreateInfo vi = {.vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &bind, .vertexAttributeDescriptionCount = 1, .pVertexAttributeDescriptions = &attr};
    VkGraphicsPipelineCreateInfo gi = {.stageCount = 2, .pStages = stages, .pVertexInputState = &vi, .renderPass = &t.rp};
    VkPipeline p = pipe_make(&gi); assert(p->pso);
    VkBuffer vb[2]; VkDeviceMemory mem[2];
    for (int i = 0; i < 2; i++) {
        VkBufferCreateInfo bi = {.size = 12, .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT};
        VkMemoryAllocateInfo ai = {.allocationSize = 256, .memoryTypeIndex = 1};
        assert(h_vkCreateBuffer(&g_device, &bi, NULL, &vb[i]) == VK_SUCCESS); assert(h_vkAllocateMemory(&g_device, &ai, NULL, &mem[i]) == VK_SUCCESS);
        assert(h_vkBindBufferMemory(&g_device, vb[i], mem[i], 0) == VK_SUCCESS);
        uint32_t *v = mem[i]->buf.contents; v[0] = v[1] = v[2] = i ? 29 : 19;
    }
    VkQueryPool q; VkQueryPoolCreateInfo qi = {.queryType = VK_QUERY_TYPE_OCCLUSION, .queryCount = 1};
    assert(h_vkCreateQueryPool(&g_device, &qi, NULL, &q) == VK_SUCCESS);
    VkClearAttachment a = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .clearValue.color.uint32 = {0xffffffff}};
    VkClearRect r = {{{1, 1}, {2, 2}}, 0, 1};
    VkCommandBuffer cb = begin(&t, (VkClearValue){0}); VkDeviceSize offset = 0;
    VkViewport vp = {0, 0, 4, 4, 0, 1}; VkRect2D sc = {{0, 0}, {4, 4}};
    h_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p); h_vkCmdBindVertexBuffers(cb, 0, 1, &vb[0], &offset);
    h_vkCmdSetViewport(cb, 0, 1, &vp); h_vkCmdSetScissor(cb, 0, 1, &sc);
    h_vkCmdBeginQuery(cb, q, 0, VK_QUERY_CONTROL_PRECISE_BIT); h_vkCmdDraw(cb, 3, 1, 0, 0);
    sc.extent.height = 1; h_vkCmdSetScissor(cb, 0, 1, &sc);   // this must not clip the clear, and must survive it
    h_vkCmdClearAttachments(cb, 1, &a, 1, &r);
    h_vkCmdBindVertexBuffers(cb, 0, 1, &vb[1], &offset); h_vkCmdDraw(cb, 3, 1, 0, 0);
    h_vkCmdEndQuery(cb, q, 0); h_vkCmdEndRenderPass(cb); submit(cb);
    id<MTLBuffer> out = pixels(t.im, 0, 0, 4, MTLBlitOptionNone); const uint32_t *px = out.contents;
    for (uint32_t y = 0; y < 4; y++) for (uint32_t x = 0; x < 4; x++) assert(px[y * 4 + x] == (!y ? 29 : inside(x, y) ? 0xffffffff : 19));
    uint64_t samples = 0;
    assert(h_vkGetQueryPoolResults(&g_device, q, 0, 1, sizeof samples, &samples, sizeof samples, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS);
    fprintf(stderr, "occlusion samples across attachment clear: %llu\n", (unsigned long long)samples);
    assert(samples == 20);   // both game encoders contribute; the helper pass does not overwrite either slot
    h_vkDestroyQueryPool(&g_device, q, NULL); h_vkDestroyPipeline(&g_device, p, NULL);
    for (int i = 0; i < 2; i++) { h_vkDestroyBuffer(&g_device, vb[i], NULL); h_vkFreeMemory(&g_device, mem[i], NULL); }
    free_target(&t); cases++;
}
int main(void) {
    @autoreleasepool {
        dev_init(); VkCommandPoolCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        assert(h_vkCreateCommandPool(&g_device, &ci, NULL, &pool) == VK_SUCCESS);
        typed_colors(); attachments_and_rectangles(); view_layers(); multisample(); depth_stencil(); secondary_clear(); draw_clear_draw();
        h_vkDestroyCommandPool(&g_device, pool, NULL);
    }
    printf("Vulkan attachment clears: %d GPU cases passed\n", cases); return 0;
}
