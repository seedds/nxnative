// VkClearColorValue is a union: a clear of an unsigned integer format reads uint32, of a signed integer format int32, of every other colour format float32 (the Vulkan
// specification, VkClearColorValue). Metal takes a clear colour as four doubles and converts them to the attachment's type, so the layer has to read the member the format
// names. Here an image of each kind is cleared both ways the layer clears, by a render pass that opens it with LOAD_OP_CLEAR and by vkCmdClearColorImage, and read back:
// the texels must hold the value the game gave. The unsigned value is above INT32_MAX and the signed one negative, so a read of the wrong member cannot pass.
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

enum { W = 16, H = 16, TEXEL_BYTES = 4 };
enum { BY_RENDER_PASS, BY_IMAGE_CLEAR };
#define UNSIGNED_ABOVE_INT32_MAX 4000000000u
#define SIGNED_NEGATIVE (-5)
#define FLOAT_HALF 0.5f

int main(void) {
    VkDevice dev = &g_device;
    @autoreleasepool {
        dev_init();
        VkCommandPoolCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        VkCommandPool cp; h_vkCreateCommandPool(dev, &cpi, NULL, &cp);
        static const struct { VkFormat format; int how; VkClearColorValue value; const char *what; } cases[] = {
            {VK_FORMAT_R32_UINT, BY_RENDER_PASS, {.uint32 = {UNSIGNED_ABOVE_INT32_MAX}}, "a render pass clears an unsigned integer attachment to the uint32 value"},
            {VK_FORMAT_R32_SINT, BY_RENDER_PASS, {.int32 = {SIGNED_NEGATIVE}}, "a render pass clears a signed integer attachment to the int32 value"},
            {VK_FORMAT_R32_SFLOAT, BY_RENDER_PASS, {.float32 = {FLOAT_HALF}}, "a render pass clears a float attachment to the float32 value"},
            {VK_FORMAT_R32_UINT, BY_IMAGE_CLEAR, {.uint32 = {UNSIGNED_ABOVE_INT32_MAX}}, "vkCmdClearColorImage clears an unsigned integer image to the uint32 value"},
            {VK_FORMAT_R32_SINT, BY_IMAGE_CLEAR, {.int32 = {SIGNED_NEGATIVE}}, "vkCmdClearColorImage clears a signed integer image to the int32 value"},
            {VK_FORMAT_R32_SFLOAT, BY_IMAGE_CLEAR, {.float32 = {FLOAT_HALF}}, "vkCmdClearColorImage clears a float image to the float32 value"},
        };
        for (size_t k = 0; k < sizeof cases / sizeof *cases; k++) {
            VkFormat format = cases[k].format;
            VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = format, .extent = {W, H, 1},
                                     .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                     .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
            VkImage im = NULL; h_vkCreateImage(dev, &ici, NULL, &im);
            if (!im) { printf("image not created\n"); abort(); }
            VkImageSubresourceRange sr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = format,
                                         .subresourceRange = sr};
            VkImageView view = NULL; h_vkCreateImageView(dev, &vci, NULL, &view);
            VkAttachmentDescription ad = {0, format, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                          VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
            VkAttachmentReference ar = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
            VkSubpassDescription sd = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &ar};
            VkRenderPassCreateInfo rci = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ad, .subpassCount = 1, .pSubpasses = &sd};
            VkRenderPass rp; h_vkCreateRenderPass(dev, &rci, NULL, &rp);
            VkFramebufferCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = 1, .pAttachments = &view, .width = W,
                                           .height = H, .layers = 1};
            VkFramebuffer fb; h_vkCreateFramebuffer(dev, &fci, NULL, &fb);
            VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                               .commandBufferCount = 1};
            VkCommandBuffer cb; h_vkAllocateCommandBuffers(dev, &cai, &cb);

            VkCommandBufferBeginInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            h_vkBeginCommandBuffer(cb, &cbi);
            if (cases[k].how == BY_RENDER_PASS) {
                VkClearValue clear = {.color = cases[k].value};
                VkRenderPassBeginInfo rbi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb, .renderArea = {{0, 0}, {W, H}},
                                             .clearValueCount = 1, .pClearValues = &clear};
                h_vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
                h_vkCmdEndRenderPass(cb);
            } else {
                h_vkCmdClearColorImage(cb, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cases[k].value, 1, &sr);
            }
            h_vkEndCommandBuffer(cb);

            // The replay as vkQueueSubmit runs it, then the image read back on the GPU
            rs_t rs = {0}; subq_t sq = {0}; rs.cb = [g_q commandBuffer]; rs.sq = &sq; replay(&rs, cb);
            id<MTLBuffer> out = [g_dev newBufferWithLength:W * H * TEXEL_BYTES options:MTLResourceStorageModeShared];
            if (!out) { printf("no readback buffer\n"); abort(); }
            id<MTLBlitCommandEncoder> be = [rs.cb blitCommandEncoder];
            [be copyFromTexture:im->tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(W, H, 1) toBuffer:out destinationOffset:0
                destinationBytesPerRow:W * TEXEL_BYTES destinationBytesPerImage:W * H * TEXEL_BYTES];
            [be endEncoding]; [rs.cb commit]; [rs.cb waitUntilCompleted];
            CHECK(rs.cb.status == MTLCommandBufferStatusCompleted, "the replay completed");
            // The union's first component holds the wanted texel's bits whichever member the case set
            const uint32_t *px = out.contents, want = cases[k].value.uint32[0]; int ok = 0;
            for (int i = 0; i < W * H; i++) ok += px[i] == want;
            CHECK(ok == W * H, cases[k].what);
            if (ok != W * H) fprintf(stderr, "  %d of %d texels as expected; first texel %08x, wanted %08x\n", ok, W * H, px[0], want);
            h_vkDestroyFramebuffer(dev, fb, NULL); h_vkDestroyRenderPass(dev, rp, NULL); h_vkDestroyImageView(dev, view, NULL); h_vkDestroyImage(dev, im, NULL);
        }
        static const struct {
            VkFormat format; VkClearColorValue border, clear; uint32_t img_w, img_h, fb_w, fb_h; VkRect2D area; const char *what;
        } area_cases[] = {
            {VK_FORMAT_R32_UINT, {.uint32 = {123456789u}}, {.uint32 = {UNSIGNED_ABOVE_INT32_MAX}}, W, H, W, H, {{3, 5}, {7, 6}},
             "partial renderArea clears only inside uint32 pixels and preserves border"},
            {VK_FORMAT_R32_SINT, {.int32 = {42}}, {.int32 = {SIGNED_NEGATIVE}}, W, H, W, H, {{3, 5}, {7, 6}},
             "partial renderArea clears only inside int32 pixels and preserves border"},
            {VK_FORMAT_R32_SFLOAT, {.float32 = {0.25f}}, {.float32 = {FLOAT_HALF}}, W, H, W, H, {{3, 5}, {7, 6}},
             "partial renderArea clears only inside float32 pixels and preserves border"},
            {VK_FORMAT_R32_UINT, {.uint32 = {123456789u}}, {.uint32 = {UNSIGNED_ABOVE_INT32_MAX}}, 24, 20, W, H, {{0, 0}, {W, H}},
             "attachment larger than framebuffer clears only the framebuffer renderArea"},
            {VK_FORMAT_R32_UINT, {.uint32 = {123456789u}}, {.uint32 = {UNSIGNED_ABOVE_INT32_MAX}}, W, H, W, H, {{0, 0}, {W, H}},
             "full-area render pass clears entire attachment (control)"},
        };
        for (size_t k = 0; k < sizeof area_cases / sizeof *area_cases; k++) {
            uint32_t iw = area_cases[k].img_w, ih = area_cases[k].img_h;
            VkFormat format = area_cases[k].format;
            VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = format, .extent = {iw, ih, 1},
                                     .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                     .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
            VkImage im = NULL; h_vkCreateImage(dev, &ici, NULL, &im);
            if (!im) { printf("image not created\n"); abort(); }
            VkImageSubresourceRange sr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = format,
                                         .subresourceRange = sr};
            VkImageView view = NULL; h_vkCreateImageView(dev, &vci, NULL, &view);
            VkAttachmentDescription ad = {0, format, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                          VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
            VkAttachmentReference ar = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
            VkSubpassDescription sd = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &ar};
            VkRenderPassCreateInfo rci = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ad, .subpassCount = 1, .pSubpasses = &sd};
            VkRenderPass rp; h_vkCreateRenderPass(dev, &rci, NULL, &rp);
            VkFramebufferCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = 1, .pAttachments = &view,
                                           .width = area_cases[k].fb_w, .height = area_cases[k].fb_h, .layers = 1};
            VkFramebuffer fb; h_vkCreateFramebuffer(dev, &fci, NULL, &fb);
            VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                               .commandBufferCount = 1};
            VkCommandBuffer cb; h_vkAllocateCommandBuffers(dev, &cai, &cb);
            VkCommandBufferBeginInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            h_vkBeginCommandBuffer(cb, &cbi);
            h_vkCmdClearColorImage(cb, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &area_cases[k].border, 1, &sr);
            VkClearValue clear = {.color = area_cases[k].clear};
            VkRenderPassBeginInfo rbi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb,
                                         .renderArea = area_cases[k].area, .clearValueCount = 1, .pClearValues = &clear};
            h_vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
            h_vkCmdEndRenderPass(cb);
            h_vkEndCommandBuffer(cb);

            rs_t rs = {0}; subq_t sq = {0}; rs.cb = [g_q commandBuffer]; rs.sq = &sq; replay(&rs, cb);
            id<MTLBuffer> out = [g_dev newBufferWithLength:iw * ih * TEXEL_BYTES options:MTLResourceStorageModeShared];
            if (!out) { printf("no readback buffer\n"); abort(); }
            id<MTLBlitCommandEncoder> be = [rs.cb blitCommandEncoder];
            [be copyFromTexture:im->tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(iw, ih, 1) toBuffer:out destinationOffset:0
                destinationBytesPerRow:iw * TEXEL_BYTES destinationBytesPerImage:iw * ih * TEXEL_BYTES];
            [be endEncoding]; [rs.cb commit]; [rs.cb waitUntilCompleted];
            CHECK(rs.cb.status == MTLCommandBufferStatusCompleted, "the partial-area replay completed");
            const uint32_t *px = out.contents, want_in = area_cases[k].clear.uint32[0], want_out = area_cases[k].border.uint32[0];
            VkRect2D a = area_cases[k].area; uint32_t ok = 0;
            for (uint32_t y = 0; y < ih; y++) for (uint32_t x = 0; x < iw; x++) {
                int in = (int32_t)x >= a.offset.x && (int32_t)x < a.offset.x + (int32_t)a.extent.width &&
                         (int32_t)y >= a.offset.y && (int32_t)y < a.offset.y + (int32_t)a.extent.height;
                ok += px[y * iw + x] == (in ? want_in : want_out);
            }
            CHECK(ok == iw * ih, area_cases[k].what);
            if (ok != iw * ih) fprintf(stderr, "  %u of %u texels matched\n", ok, iw * ih);
            h_vkDestroyFramebuffer(dev, fb, NULL); h_vkDestroyRenderPass(dev, rp, NULL); h_vkDestroyImageView(dev, view, NULL); h_vkDestroyImage(dev, im, NULL);
        }
    }
    printf("Vulkan integer clears: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
