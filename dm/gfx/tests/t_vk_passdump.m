// DM_VK_PASSDUMP: the frames it selects (a range, every:n, DM_CTL "passdump n"), and the lines one submit of a selected frame logs (pd_submit, which vkQueueSubmit
// calls before the replay): objects by creation number, passes and submits numbered within the frame, a barrier recorded and skipped by the replay, a descriptor
// set's written elements. Frames are advanced through g_frames, as the presents do; the swap chain is not used
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

// What a block of code writes to stderr (the dump's stream), read back from a temporary file
static int cap_fd, cap_saved;
static void cap_begin(void) {
    char p[] = "/tmp/t_vk_passdump.XXXXXX"; fflush(stderr);
    cap_fd = mkstemp(p); if (cap_fd < 0) { perror("mkstemp"); exit(2); }
    unlink(p); cap_saved = dup(2); if (cap_saved < 0) { perror("dup"); exit(2); }
    dup2(cap_fd, 2);
}
static char *cap_end(void) {
    fflush(stderr); dup2(cap_saved, 2); close(cap_saved);
    off_t n = lseek(cap_fd, 0, SEEK_END); char *s = calloc(1, (size_t)n + 1); if (!s) exit(2);
    pread(cap_fd, s, (size_t)n, 0); close(cap_fd);
    return s;
}
static int has(const char *log, const char *line) { return strstr(log, line) != NULL; }

static VkDevice dev = &g_device;
static VkCommandBuffer cbuf;
static void submit(void) { VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cbuf}; pd_submit(1, &si); }

int main(void) {
    setenv("DM_VK_PASSDUMP", "2-3", 1);
    @autoreleasepool {
        dev_init();
        VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {4, 4, 1}, .mipLevels = 1,
                                 .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                 .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        VkImage rt = NULL, tex = NULL; h_vkCreateImage(dev, &ici, NULL, &rt); h_vkCreateImage(dev, &ici, NULL, &tex);
        if (!rt || !tex) { printf("images not created\n"); abort(); }
        CHECK(rt->num == 1 && tex->num == 2, "images are numbered in creation order from 1");
        VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = rt, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                                     .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VkImageView rtv = NULL, texv = NULL; h_vkCreateImageView(dev, &vci, NULL, &rtv); vci.image = tex; h_vkCreateImageView(dev, &vci, NULL, &texv);
        VkAttachmentDescription ad = {0, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                      VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference ar = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sd = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &ar};
        VkRenderPassCreateInfo rci = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ad, .subpassCount = 1, .pSubpasses = &sd};
        VkRenderPass rp; h_vkCreateRenderPass(dev, &rci, NULL, &rp);
        VkFramebufferCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = 1, .pAttachments = &rtv, .width = 4, .height = 4, .layers = 1};
        VkFramebuffer fb; h_vkCreateFramebuffer(dev, &fci, NULL, &fb);

        VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 256, .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VkBuffer ba, bb; h_vkCreateBuffer(dev, &bci, NULL, &ba); h_vkCreateBuffer(dev, &bci, NULL, &bb);
        CHECK(ba->num == 1 && bb->num == 2, "buffers are numbered in their own sequence");
        VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = 4096, .memoryTypeIndex = 1};
        VkDeviceMemory mem; h_vkAllocateMemory(dev, &mai, NULL, &mem); h_vkBindBufferMemory(dev, ba, mem, 0); h_vkBindBufferMemory(dev, bb, mem, 1024);

        VkDescriptorSetLayoutBinding lb[2] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
                                              {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL}};
        VkDescriptorSetLayoutCreateInfo lci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = lb};
        VkDescriptorSetLayout L; h_vkCreateDescriptorSetLayout(dev, &lci, NULL, &L);
        VkDescriptorPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1};
        VkDescriptorPool pool; h_vkCreateDescriptorPool(dev, &pci, NULL, &pool);
        VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = pool, .descriptorSetCount = 1, .pSetLayouts = &L};
        VkDescriptorSet ds; h_vkAllocateDescriptorSets(dev, &ai, &ds);
        VkSamplerCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR};
        VkSampler smp; h_vkCreateSampler(dev, &sci, NULL, &smp);
        VkDescriptorImageInfo ii = {smp, texv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorBufferInfo bi = {bb, 64, 128};
        VkWriteDescriptorSet w[2] = {{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .dstArrayElement = 3, .descriptorCount = 1,
                                      .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii},
                                     {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 1, .descriptorCount = 1,
                                      .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi}};
        h_vkUpdateDescriptorSets(dev, 2, w, 0, NULL);

        VkCommandPoolCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        VkCommandPool cp; h_vkCreateCommandPool(dev, &cpi, NULL, &cp);
        VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        h_vkAllocateCommandBuffers(dev, &cai, &cbuf);
        VkCommandBufferBeginInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        h_vkBeginCommandBuffer(cbuf, &cbi);
        VkImageMemoryBarrier ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                   .image = rt, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        h_vkCmdPipelineBarrier(cbuf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
        VkClearValue cv = {.color = {.float32 = {0, 0, 0, 1}}};
        VkRenderPassBeginInfo rbi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb, .renderArea = {{0, 0}, {4, 4}}, .clearValueCount = 1, .pClearValues = &cv};
        h_vkCmdBeginRenderPass(cbuf, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        h_vkCmdBindDescriptorSets(cbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, NULL, 0, 1, &ds, 0, NULL);   // the layer reads no pipeline layout here
        VkViewport vp = {0, 0, 4, 4, 0, 1}; h_vkCmdSetViewport(cbuf, 0, 1, &vp);
        h_vkCmdEndRenderPass(cbuf);
        VkBufferCopy bc = {0, 0, 16}; h_vkCmdCopyBuffer(cbuf, ba, bb, 1, &bc);
        VkClearColorValue cc = {.float32 = {1, 0, 0, 1}}; VkImageSubresourceRange sr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        h_vkCmdClearColorImage(cbuf, tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &sr);
        h_vkEndCommandBuffer(cbuf);

        // The replay of the recorded commands, barrier included
        cap_begin();
        rs_t rs = {0}; subq_t sq = {0}; rs.cb = [g_q commandBuffer]; rs.sq = &sq; replay(&rs, cbuf); [rs.cb commit]; [rs.cb waitUntilCompleted];
        char *s = cap_end();
        CHECK(!has(s, "unknown command") && rs.cb.status == MTLCommandBufferStatusCompleted, "the replay skips the recorded barrier");
        free(s);

        // Frame 1 (nothing presented yet) is outside 2-3
        cap_begin(); submit(); s = cap_end();
        CHECK(!has(s, "[vkpass]"), "a frame outside the range logs nothing");
        free(s);

        // Frame 2: two submits
        atomic_store(&g_frames, 1);
        cap_begin(); submit(); submit(); s = cap_end();
        CHECK(has(s, "[vkpass] F2 Q q0 1\n"), "the first submit of the frame is q0, with its command buffer count");
        CHECK(has(s, "[vkpass] F2 Z 0x1>0x400 dep=0 m=0 b=0 i=1 i1:0>2\n"), "the barrier: stage masks, counts and the image's layout change");
        CHECK(has(s, "[vkpass] F2 P p0 4x4 area=0,0,4,4 c0=i1:37:CS\n"), "the pass: size, area, attachment image, format, clear / store");
        CHECK(has(s, "[vkpass] F2 B 0: s1\n"), "the bound set by creation number");
        CHECK(has(s, "[vkpass] F2 S s1 0:t1[8]/1=i2 1:t6[1]/1=b2\n"), "the set's contents: the one written element of the array, the buffer");
        CHECK(has(s, "[vkpass] F2 O vp 0,0,4,4,0,1\n"), "the viewport");
        CHECK(has(s, "[vkpass] F2 E\n"), "the pass end");
        CHECK(has(s, "[vkpass] F2 T b2b b1>b2 1 bytes=16\n"), "the buffer copy");
        CHECK(has(s, "[vkpass] F2 X i2 colour 1\n"), "the image clear");
        CHECK(has(s, "[vkpass] F2 Q q1 1\n") && has(s, "[vkpass] F2 P p1 "), "the second submit is q1 and its pass continues the frame's numbering");
        free(s);

        // Frame 4 is past the range; DM_CTL "passdump 2" asked during frame 4 dumps frames 5 and 6
        atomic_store(&g_frames, 3);
        cap_begin(); submit(); s = cap_end(); CHECK(!has(s, "[vkpass]"), "the frame after the range logs nothing"); free(s);
        dm_vk_passdump_next(2);
        cap_begin(); submit(); s = cap_end(); CHECK(!has(s, "[vkpass]"), "passdump n starts after the frame being recorded"); free(s);
        atomic_store(&g_frames, 4);
        cap_begin(); submit(); s = cap_end(); CHECK(has(s, "[vkpass] F5 Q q0 1\n") && has(s, "[vkpass] F5 P p0 "), "frame 5 is dumped, numbered from q0 / p0 again"); free(s);
        atomic_store(&g_frames, 6);
        cap_begin(); submit(); s = cap_end(); CHECK(!has(s, "[vkpass]"), "frame 7 is past the requested two"); free(s);
        // DM_CTL "passdump 0" ends a requested range, as it does DM_NVN_PASSDUMP's
        dm_vk_passdump_next(5); dm_vk_passdump_next(0); atomic_store(&g_frames, 7);
        cap_begin(); submit(); s = cap_end(); CHECK(!has(s, "[vkpass]"), "passdump 0 ends a requested range"); free(s);
        // A view written to a set after its image was destroyed names the image by the number it was made with (the run scribbles freed memory)
        VkImage gone = NULL; h_vkCreateImage(dev, &ici, NULL, &gone); vci.image = gone;
        VkImageView gonev = NULL; h_vkCreateImageView(dev, &vci, NULL, &gonev); uint64_t gone_num = gone->num; h_vkDestroyImage(dev, gone, NULL);
        VkDescriptorImageInfo gi = {smp, gonev, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet gw = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .dstArrayElement = 5, .descriptorCount = 1,
                                   .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &gi};
        h_vkUpdateDescriptorSets(dev, 1, &gw, 0, NULL);
        CHECK(ds->d[L->b[0].base + 5].num == gone_num, "a view that outlives its image keeps the image's number");
        h_vkDestroyImageView(dev, gonev, NULL);
        h_vkDestroySampler(dev, smp, NULL); h_vkDestroyFramebuffer(dev, fb, NULL); h_vkDestroyImageView(dev, rtv, NULL); h_vkDestroyImageView(dev, texv, NULL);
        h_vkDestroyImage(dev, rt, NULL); h_vkDestroyImage(dev, tex, NULL); h_vkDestroyBuffer(dev, ba, NULL); h_vkDestroyBuffer(dev, bb, NULL); h_vkFreeMemory(dev, mem, NULL);
    }
    // every:n and a single frame, parsed as DM_VK_PASSDUMP's value
    pd_parse("every:3");
    CHECK(pd_frame(3) && pd_frame(6) && !pd_frame(4) && !pd_frame(5), "every:3 selects the multiples of 3");
    pd_parse("7");
    CHECK(pd_frame(7) && !pd_frame(6) && !pd_frame(8), "a single frame");
    printf("Vulkan pass dump: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
