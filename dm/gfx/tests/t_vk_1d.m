// A Vulkan 1D image is a one-row 2D (array) texture (hle_vk.m vkCreateImage; docs/reference/rendering.md, "1D images drawn into"): Metal's 1D texture cannot be a render
// target, has one mip level and no depth format, and its API validation layer aborts on each. Run under that layer (MTL_DEBUG_LAYER=1), this check makes a mipmapped,
// layered 1D colour image, clears it to red, then draws into level 1 of layer 1 through a 1D view in a pass that clears it to green, and reads every level and layer back;
// then a 1D depth image, cleared to 0.25 and kept through a pass that loads it. Views of 1D and 1D-array type must be 2D and 2D-array textures, as vk_shaders.py
// translates 1D images (spirv-cross --msl-texture-1d-as-2d)
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

static VkCommandBuffer new_cb(VkDevice dev, VkCommandPool cp) {
    VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer cb; h_vkAllocateCommandBuffers(dev, &cai, &cb);
    VkCommandBufferBeginInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    h_vkBeginCommandBuffer(cb, &cbi);
    return cb;
}
static VkImageView new_view(VkDevice dev, VkImage im, VkImageViewType t, VkFormat f, VkImageAspectFlags asp, uint32_t level, uint32_t layer, uint32_t layers) {
    VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im, .viewType = t, .format = f, .subresourceRange = {asp, level, 1, layer, layers}};
    VkImageView v = NULL; h_vkCreateImageView(dev, &vci, NULL, &v);
    return v;
}

int main(void) {
    VkDevice dev = &g_device;
    @autoreleasepool {
        dev_init();
        enum { W = 512, MIPS = 3, LAYERS = 2 };
        VkCommandPoolCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        VkCommandPool cp; h_vkCreateCommandPool(dev, &cpi, NULL, &cp);

        // Colour: a full-screen triangle writing transparent black, blended over the attachment (ONE, ONE_MINUS_SRC_ALPHA), so the pass's clear colour is what stays
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
        {
            VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_1D, .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {W, 1, 1},
                                     .mipLevels = MIPS, .arrayLayers = LAYERS, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                     .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
            VkImage im = NULL; h_vkCreateImage(dev, &ici, NULL, &im);
            if (!im) { printf("1D colour image not created\n"); abort(); }
            CHECK(im->tex.textureType == MTLTextureType2DArray && im->tex.height == 1 && im->tex.mipmapLevelCount == MIPS && im->tex.arrayLength == LAYERS,
                  "a layered, mipmapped 1D image is a one-row 2D array texture with all its levels");
            VkImageView all = new_view(dev, im, VK_IMAGE_VIEW_TYPE_1D_ARRAY, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, LAYERS);
            CHECK(all && all->tex.textureType == MTLTextureType2DArray, "a 1D-array view is a 2D-array texture");
            VkImageView view = new_view(dev, im, VK_IMAGE_VIEW_TYPE_1D, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, 1);
            if (!view || !view->tex) { printf("1D view not created\n"); abort(); }
            CHECK(view->tex.textureType == MTLTextureType2D && view->tex.width == W / 2 && view->tex.height == 1, "a 1D view of level 1 is a one-row 2D texture half as wide");

            VkAttachmentDescription ad = {0, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                          VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
            VkAttachmentReference ar = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
            VkSubpassDescription sd = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &ar};
            VkRenderPassCreateInfo rci = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ad, .subpassCount = 1, .pSubpasses = &sd};
            VkRenderPass rp; h_vkCreateRenderPass(dev, &rci, NULL, &rp);
            VkFramebufferCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = 1, .pAttachments = &view, .width = W / 2,
                                           .height = 1, .layers = 1};
            VkFramebuffer fb; h_vkCreateFramebuffer(dev, &fci, NULL, &fb);

            VkCommandBuffer cb = new_cb(dev, cp);
            VkClearColorValue red = {.float32 = {1, 0, 0, 1}}; VkImageSubresourceRange sr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, MIPS, 0, LAYERS};
            h_vkCmdClearColorImage(cb, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &red, 1, &sr);
            VkClearValue green = {.color.float32 = {0, 1, 0, 1}};
            VkRenderPassBeginInfo rbi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb, .renderArea = {{0, 0}, {W / 2, 1}},
                                         .clearValueCount = 1, .pClearValues = &green};
            h_vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
            h_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            h_vkCmdDraw(cb, 3, 1, 0, 0);
            h_vkCmdEndRenderPass(cb);
            h_vkEndCommandBuffer(cb);

            // The replay as vkQueueSubmit runs it, then every level and layer read back on the GPU
            rs_t rs = {0}; subq_t sq = {0}; rs.cb = [g_q commandBuffer]; rs.sq = &sq; replay(&rs, cb);
            end_blit(&rs);
            id<MTLBuffer> out = [g_dev newBufferWithLength:W * 4 * MIPS * LAYERS options:MTLResourceStorageModeShared];
            if (!out) { printf("no readback buffer\n"); abort(); }
            id<MTLBlitCommandEncoder> be = [rs.cb blitCommandEncoder];
            for (uint32_t l = 0; l < LAYERS; l++)
                for (uint32_t m = 0; m < MIPS; m++)
                    [be copyFromTexture:im->tex sourceSlice:l sourceLevel:m sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(W >> m, 1, 1) toBuffer:out
                      destinationOffset:(l * MIPS + m) * W * 4 destinationBytesPerRow:(W >> m) * 4 destinationBytesPerImage:(W >> m) * 4];
            [be endEncoding]; [rs.cb commit]; [rs.cb waitUntilCompleted];
            CHECK(rs.cb.status == MTLCommandBufferStatusCompleted, "the colour replay completed");
            static const uint8_t r8[4] = {255, 0, 0, 255}, g8[4] = {0, 255, 0, 255};
            for (uint32_t l = 0; l < LAYERS; l++)
                for (uint32_t m = 0; m < MIPS; m++) {
                    const uint8_t *px = (const uint8_t *)out.contents + (l * MIPS + m) * W * 4, *want = l == 1 && m == 1 ? g8 : r8; int ok = 0, n = W >> m;
                    for (int i = 0; i < n; i++) ok += !memcmp(px + 4 * i, want, 4);
                    char what[128]; snprintf(what, sizeof what, "1D layer %u level %u is %s", l, m, want == g8 ? "green (drawn through the 1D view)" : "red (cleared, not drawn)");
                    CHECK(ok == n, what);
                    if (ok != n) fprintf(stderr, "  %d of %d texels as expected; first texel %02x %02x %02x %02x\n", ok, n, px[0], px[1], px[2], px[3]);
                }
            h_vkDestroyFramebuffer(dev, fb, NULL); h_vkDestroyRenderPass(dev, rp, NULL); h_vkDestroyImageView(dev, view, NULL); h_vkDestroyImageView(dev, all, NULL);
            h_vkDestroyImage(dev, im, NULL);
        }

        // Depth: a 1D depth image (Metal has no 1D depth texture), cleared to 0.25 and loaded and stored by a pass that draws nothing
        {
            enum { DW = 64 };
            VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_1D, .format = VK_FORMAT_D32_SFLOAT, .extent = {DW, 1, 1},
                                     .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                     .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
            VkImage im = NULL; h_vkCreateImage(dev, &ici, NULL, &im);
            if (!im) { printf("1D depth image not created\n"); abort(); }
            CHECK(im->tex.textureType == MTLTextureType2D && im->tex.height == 1, "a 1D depth image is a one-row 2D depth texture");
            VkImageView view = new_view(dev, im, VK_IMAGE_VIEW_TYPE_1D, VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1);
            if (!view || !view->tex) { printf("1D depth view not created\n"); abort(); }
            VkAttachmentDescription ad = {0, VK_FORMAT_D32_SFLOAT, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                          VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
            VkAttachmentReference ar = {0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
            VkSubpassDescription sd = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .pDepthStencilAttachment = &ar};
            VkRenderPassCreateInfo rci = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ad, .subpassCount = 1, .pSubpasses = &sd};
            VkRenderPass rp; h_vkCreateRenderPass(dev, &rci, NULL, &rp);
            VkFramebufferCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = 1, .pAttachments = &view, .width = DW,
                                           .height = 1, .layers = 1};
            VkFramebuffer fb; h_vkCreateFramebuffer(dev, &fci, NULL, &fb);
            VkCommandBuffer cb = new_cb(dev, cp);
            VkClearDepthStencilValue v = {0.25f, 0}; VkImageSubresourceRange sr = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
            h_vkCmdClearDepthStencilImage(cb, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &sr);
            VkRenderPassBeginInfo rbi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb, .renderArea = {{0, 0}, {DW, 1}}};
            h_vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
            h_vkCmdEndRenderPass(cb);
            h_vkEndCommandBuffer(cb);

            rs_t rs = {0}; subq_t sq = {0}; rs.cb = [g_q commandBuffer]; rs.sq = &sq; replay(&rs, cb);
            end_blit(&rs);
            id<MTLBuffer> out = [g_dev newBufferWithLength:DW * 4 options:MTLResourceStorageModeShared];
            if (!out) { printf("no readback buffer\n"); abort(); }
            id<MTLBlitCommandEncoder> be = [rs.cb blitCommandEncoder];
            [be copyFromTexture:im->tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(DW, 1, 1) toBuffer:out destinationOffset:0
                destinationBytesPerRow:DW * 4 destinationBytesPerImage:DW * 4];
            [be endEncoding]; [rs.cb commit]; [rs.cb waitUntilCompleted];
            CHECK(rs.cb.status == MTLCommandBufferStatusCompleted, "the depth replay completed");
            const float *dp = out.contents; int ok = 0;
            for (int i = 0; i < DW; i++) ok += dp[i] == 0.25f;
            CHECK(ok == DW, "a 1D depth attachment keeps its cleared depth through a pass that loads it");
            if (ok != DW) fprintf(stderr, "  %d of %d texels as expected; first texel %g\n", ok, DW, dp[0]);
            h_vkDestroyFramebuffer(dev, fb, NULL); h_vkDestroyRenderPass(dev, rp, NULL); h_vkDestroyImageView(dev, view, NULL); h_vkDestroyImage(dev, im, NULL);
        }
    }
    printf("Vulkan 1D images: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
