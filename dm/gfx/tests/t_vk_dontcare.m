// This layer's policy, not a Vulkan rule: an attachment the game opens with LOAD_OP_DONT_CARE keeps what the image held, colour, depth and stencil alike. Vulkan leaves
// those contents undefined; the Switch's driver does no colour discard on the path examined and only drops ZCULL hints for depth, so they survive there and games read
// them. Re:ZERO's pfx pass draws its effects layer over the copy the pass before wrote into the same image, both opened with DONT_CARE, and samples a half-res depth its
// particle pass reopened with DONT_CARE and STORE without drawing.
// On Apple's tile memory a DontCare load starts the tiles empty and the stored image lost the copy (the 3D world black). Here an image is cleared to red, then a pass
// opens it and blends a transparent full-screen triangle over it (ONE, ONE_MINUS_SRC_ALPHA, as pfx's premultiplied layer): with DONT_CARE and LOAD the image must still
// be red, with CLEAR to green it must be green. A depth-stencil image cleared to 0.25 / 0x5a and reopened with DONT_CARE by a pass that draws nothing must store 0.25 / 0x5a.
// The run fills every DontCare load with a pattern (MTL_DEBUG_LAYER_VALIDATE_LOAD_ACTIONS), so no case leans on tile memory.
// These test this layer's policy and its reference switch, not Vulkan: with DM_VK_DONTCARE=none the colour DONT_CARE case must read that fill pattern instead of red, and
// with =colour the depth-stencil case must store the fill pattern instead of 0.25 / 0x5a
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

int main(void) {
    VkDevice dev = &g_device;
    @autoreleasepool {
        dev_init();
        enum { W = 64, H = 64 };
        // A pipeline built here, not through the shader table: a full-screen triangle whose fragment shader writes transparent black, blended over the attachment
        NSString *src = @"#include <metal_stdlib>\nusing namespace metal;\n"
                        "vertex float4 vs_tri(uint v [[vertex_id]]) { float2 p = float2((v << 1) & 2, v & 2); return float4(p * 2 - 1, 0, 1); }\n"
                        "fragment float4 fs_clear() { return float4(0); }\n";
        NSError *err = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:src options:nil error:&err];
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [lib newFunctionWithName:@"vs_tri"]; pd.fragmentFunction = [lib newFunctionWithName:@"fs_clear"];
        MTLRenderPipelineColorAttachmentDescriptor *ca = pd.colorAttachments[0]; ca.pixelFormat = MTLPixelFormatRGBA8Unorm; ca.blendingEnabled = YES;
        ca.sourceRGBBlendFactor = ca.sourceAlphaBlendFactor = MTLBlendFactorOne; ca.destinationRGBBlendFactor = ca.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        VkPipeline pipe = zalloc(sizeof *pipe); pipe->pso = [g_dev newRenderPipelineStateWithDescriptor:pd error:&err]; pipe->prim = MTLPrimitiveTypeTriangle;
        pipe->cull = MTLCullModeNone; pipe->fill = MTLTriangleFillModeFill; pipe->clip = MTLDepthClipModeClip;
        if (!pipe->pso) { printf("test pipeline: %s\n", err.localizedDescription.UTF8String); abort(); }
        VkCommandPoolCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        VkCommandPool cp; h_vkCreateCommandPool(dev, &cpi, NULL, &cp);

        // mode: g_vk_dontcare for the case; fill: the run's DontCare fill pattern, uniform in the image and neither what the pass wrote before it nor the clear value
        static const struct { VkAttachmentLoadOp load; int mode; uint8_t want[4]; int fill; const char *what; } cases[] = {
            {VK_ATTACHMENT_LOAD_OP_DONT_CARE, DC_ALL, {255, 0, 0, 255}, 0, "a DONT_CARE colour attachment keeps the image's contents"},
            {VK_ATTACHMENT_LOAD_OP_LOAD, DC_ALL, {255, 0, 0, 255}, 0, "a LOAD colour attachment keeps the image's contents"},
            {VK_ATTACHMENT_LOAD_OP_CLEAR, DC_ALL, {0, 255, 0, 255}, 0, "a CLEAR colour attachment is cleared to the pass's clear value"},
            {VK_ATTACHMENT_LOAD_OP_DONT_CARE, DC_COLOUR, {255, 0, 0, 255}, 0, "DM_VK_DONTCARE=colour: a DONT_CARE colour attachment keeps the image's contents"},
            {VK_ATTACHMENT_LOAD_OP_DONT_CARE, DC_NONE, {0, 0, 0, 0}, 1, "DM_VK_DONTCARE=none: a DONT_CARE colour attachment reads the fill pattern"},
        };
        for (size_t k = 0; k < sizeof cases / sizeof *cases; k++) {
            g_vk_dontcare = cases[k].mode;
            VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {W, H, 1},
                                     .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                     .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
            VkImage im = NULL; h_vkCreateImage(dev, &ici, NULL, &im);
            if (!im) { printf("image not created\n"); abort(); }
            VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                                         .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
            VkImageView view = NULL; h_vkCreateImageView(dev, &vci, NULL, &view);
            VkAttachmentDescription ad = {0, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, cases[k].load, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
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
            VkClearColorValue red = {.float32 = {1, 0, 0, 1}}; VkImageSubresourceRange sr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            h_vkCmdClearColorImage(cb, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &red, 1, &sr);
            VkClearValue green = {.color.float32 = {0, 1, 0, 1}};
            VkRenderPassBeginInfo rbi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb, .renderArea = {{0, 0}, {W, H}},
                                         .clearValueCount = 1, .pClearValues = &green};
            h_vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
            h_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            h_vkCmdDraw(cb, 3, 1, 0, 0);
            h_vkCmdEndRenderPass(cb);
            h_vkEndCommandBuffer(cb);

            // The replay as vkQueueSubmit runs it, then the image read back on the GPU
            rs_t rs = {0}; subq_t sq = {0}; rs.cb = [g_q commandBuffer]; rs.sq = &sq; replay(&rs, cb);
            id<MTLBuffer> out = [g_dev newBufferWithLength:W * H * 4 options:MTLResourceStorageModeShared];
            if (!out) { printf("no readback buffer\n"); abort(); }
            id<MTLBlitCommandEncoder> be = [rs.cb blitCommandEncoder];
            [be copyFromTexture:im->tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(W, H, 1) toBuffer:out destinationOffset:0
                destinationBytesPerRow:W * 4 destinationBytesPerImage:W * H * 4];
            [be endEncoding]; [rs.cb commit]; [rs.cb waitUntilCompleted];
            CHECK(rs.cb.status == MTLCommandBufferStatusCompleted, "the replay completed");
            const uint8_t *px = out.contents; int pass = 0;
            if (cases[k].fill) {
                static const uint8_t red_px[4] = {255, 0, 0, 255}, green_px[4] = {0, 255, 0, 255}, empty_px[4] = {0, 0, 0, 0};
                pass = 1;
                for (int i = 0; i < W * H; i++)
                    pass &= !memcmp(px + 4 * i, px, 4) && memcmp(px + 4 * i, red_px, 4) && memcmp(px + 4 * i, green_px, 4) && memcmp(px + 4 * i, empty_px, 4);
                if (!pass) fprintf(stderr, "  first texel %02x %02x %02x %02x\n", px[0], px[1], px[2], px[3]);
            } else {
                for (int i = 0; i < W * H; i++) pass += !memcmp(px + 4 * i, cases[k].want, 4);
                if (pass != W * H) fprintf(stderr, "  %d of %d texels as expected; first texel %02x %02x %02x %02x\n", pass, W * H, px[0], px[1], px[2], px[3]);
                pass = pass == W * H;
            }
            CHECK(pass, cases[k].what);
            h_vkDestroyFramebuffer(dev, fb, NULL); h_vkDestroyRenderPass(dev, rp, NULL); h_vkDestroyImageView(dev, view, NULL); h_vkDestroyImage(dev, im, NULL);
        }

        // Depth and stencil: the particle pass's shape, an image with contents reopened with DONT_CARE and STORE by a pass without draws. Unset (all) keeps the contents;
        // DM_VK_DONTCARE=colour stores the fill pattern, uniform in the image and neither 0.25 nor 0x5a
        static const struct { int mode, fill; const char *depth, *stencil; } dcases[] = {
            {DC_ALL, 0, "a DONT_CARE depth attachment keeps the image's contents", "a DONT_CARE stencil attachment keeps the image's contents"},
            {DC_COLOUR, 1, "DM_VK_DONTCARE=colour: a DONT_CARE depth attachment stores the fill pattern",
             "DM_VK_DONTCARE=colour: a DONT_CARE stencil attachment stores the fill pattern"},
        };
        for (size_t k = 0; k < sizeof dcases / sizeof *dcases; k++) {
            VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_D32_SFLOAT_S8_UINT, .extent = {W, H, 1},
                                     .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                     .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
            VkImage im = NULL; h_vkCreateImage(dev, &ici, NULL, &im);
            if (!im) { printf("depth image not created\n"); abort(); }
            VkImageSubresourceRange sr = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
            VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_D32_SFLOAT_S8_UINT,
                                         .subresourceRange = sr};
            VkImageView view = NULL; h_vkCreateImageView(dev, &vci, NULL, &view);
            VkAttachmentDescription ad = {0, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_STORE,
                                          VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_STORE, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                          VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
            VkAttachmentReference ar = {0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
            VkSubpassDescription sd = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .pDepthStencilAttachment = &ar};
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
            VkClearDepthStencilValue old = {0.25f, 0x5a};
            h_vkCmdClearDepthStencilImage(cb, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &old, 1, &sr);
            VkRenderPassBeginInfo rbi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb, .renderArea = {{0, 0}, {W, H}}};
            h_vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
            h_vkCmdEndRenderPass(cb);
            h_vkEndCommandBuffer(cb);

            g_vk_dontcare = dcases[k].mode;
            rs_t rs = {0}; subq_t sq = {0}; rs.cb = [g_q commandBuffer]; rs.sq = &sq; replay(&rs, cb);
            g_vk_dontcare = DC_ALL;
            id<MTLBuffer> dout = [g_dev newBufferWithLength:W * H * 4 options:MTLResourceStorageModeShared], sout = [g_dev newBufferWithLength:W * H options:MTLResourceStorageModeShared];
            if (!dout || !sout) { printf("no readback buffer\n"); abort(); }
            id<MTLBlitCommandEncoder> be = [rs.cb blitCommandEncoder];
            [be copyFromTexture:im->tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(W, H, 1) toBuffer:dout destinationOffset:0
                destinationBytesPerRow:W * 4 destinationBytesPerImage:W * H * 4 options:MTLBlitOptionDepthFromDepthStencil];
            [be copyFromTexture:im->tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(W, H, 1) toBuffer:sout destinationOffset:0
                destinationBytesPerRow:W destinationBytesPerImage:W * H options:MTLBlitOptionStencilFromDepthStencil];
            [be endEncoding]; [rs.cb commit]; [rs.cb waitUntilCompleted];
            CHECK(rs.cb.status == MTLCommandBufferStatusCompleted, "the depth replay completed");
            const float *dp = dout.contents; const uint8_t *sp = sout.contents; int dok = 1, sok = 1;
            for (int i = 0; i < W * H; i++) {
                if (dcases[k].fill) { dok &= dp[i] == dp[0] && dp[i] != 0.25f; sok &= sp[i] == sp[0] && sp[i] != 0x5a; }
                else { dok &= dp[i] == 0.25f; sok &= sp[i] == 0x5a; }
            }
            CHECK(dok, dcases[k].depth);
            CHECK(sok, dcases[k].stencil);
            if (!dok || !sok) fprintf(stderr, "  first texel %g / %02x\n", dp[0], sp[0]);
            h_vkDestroyFramebuffer(dev, fb, NULL); h_vkDestroyRenderPass(dev, rp, NULL); h_vkDestroyImageView(dev, view, NULL); h_vkDestroyImage(dev, im, NULL);
        }
    }
    printf("Vulkan DONT_CARE loads: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
