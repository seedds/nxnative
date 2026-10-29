// The descriptor microbenchmark of docs/design/vulkan-backend.md 2.3: the CPU cost of a frame of 4,900 draws (a busy frame's count) under the backend's model, descriptor sets
// cached by content per frame with the per-draw banks (c0, c2) as dynamic uniform buffers whose offsets change per draw, against push descriptors (VK_KHR_push_descriptor), which
// write every binding of every draw into the command buffer. Each draw is a point reading its position from its c0 bank, a factor from its c2 bank, a material's constant buffer
// (one of 49, changing every 100 draws) and two textures (one of 7 pairs, changing every 700), as a frame of sorted materials does. Per strategy: 3 frames of warm-up, then 20
// measured; the table gives the median CPU time to record a frame, per draw, the descriptor writes and set allocations of a frame, and a frame's time to record, submit and
// complete (on lavapipe the GPU's work is the CPU's too: compare the record column across drivers). Both strategies draw the same image (checked). Run without the validation
// layer, whose cost would swamp what is measured; the GPU tests run the backend's own descriptor path under validation. A device without push descriptors prints that row empty
#include "t_vk_common.h"
#include "plat/plat.h"
#include "../../tests/vk/shaders/gen/descbench_vert.h"
#include "../../tests/vk/shaders/gen/descbench_frag.h"
#include "../../tests/vk/shaders/gen/descbench_push_frag.h"

enum { DRAWS = 4900, MATS = 49, PAIRS = 7, W = 128, WARM = 3, FRAMES = 20, SLOT = 256 };
static hostvk_dev g_d;
static VkDevice D;

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < g_d.mem.memoryTypeCount; i++) if ((bits >> i & 1) && (g_d.mem.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}
static VkDeviceMemory alloc(VkMemoryRequirements r, VkMemoryPropertyFlags want) {
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, r.size, mem_type(r.memoryTypeBits, want)};
    VkDeviceMemory m = VK_NULL_HANDLE; if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(D, &ai, NULL, &m)) return VK_NULL_HANDLE;
    return m;
}
static VkBuffer buffer(VkDeviceSize n, VkBufferUsageFlags u, VkDeviceMemory *m, void **map) {
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, n, u, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    VkBuffer b; vkCreateBuffer(D, &bi, NULL, &b);
    VkMemoryRequirements r; vkGetBufferMemoryRequirements(D, b, &r);
    *m = alloc(r, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkBindBufferMemory(D, b, *m, 0); vkMapMemory(D, *m, 0, VK_WHOLE_SIZE, 0, map);
    return b;
}
static VkImage image(uint32_t w, VkImageUsageFlags u, VkDeviceMemory *m, VkImageView *view) {
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {w, w, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT,
                            VK_IMAGE_TILING_OPTIMAL, u, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage img; vkCreateImage(D, &ci, NULL, &img);
    VkMemoryRequirements r; vkGetImageMemoryRequirements(D, img, &r);
    *m = alloc(r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT); vkBindImageMemory(D, img, *m, 0);
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, img, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCreateImageView(D, &vi, NULL, view);
    return img;
}
static void barrier(VkCommandBuffer cb) {   // everything before, everything after (the backend's full barrier)
    VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 1, &mb, 0, NULL, 0, NULL}; vkCmdPipelineBarrier2(cb, &di);
}
static VkShaderModule module(const uint32_t *c, size_t n) { VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, n, c}; VkShaderModule m; vkCreateShaderModule(D, &ci, NULL, &m); return m; }
static VkPipeline pipeline(VkPipelineLayout l, const uint32_t *fs, size_t fsn) {
    VkShaderModule v = module(descbench_vert_spv, sizeof descbench_vert_spv), f = module(fs, fsn);
    VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, v, "main", NULL},
                                             {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, f, "main", NULL}};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_FALSE};
    VkViewport vp = {0, 0, W, W, 0, 1}; VkRect2D sc = {{0, 0}, {W, W}};
    VkPipelineViewportStateCreateInfo vs = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, &vp, 1, &sc};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, NULL, 0, VK_FALSE, VK_FALSE, VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE,
                                                 VK_FRONT_FACE_COUNTER_CLOCKWISE, VK_FALSE, 0, 0, 0, 1};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState ba = {VK_FALSE, 0, 0, 0, 0, 0, 0, 0xf};
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, VK_FALSE, VK_LOGIC_OP_COPY, 1, &ba, {0}};
    VkFormat cf = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, NULL, 0, 1, &cf, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED};
    VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri, 0, 2, st, &vi, &ia, NULL, &vs, &rs, &ms, NULL, &cbs, NULL, l, VK_NULL_HANDLE, 0, VK_NULL_HANDLE, -1};
    VkPipeline p = VK_NULL_HANDLE; vkCreateGraphicsPipelines(D, VK_NULL_HANDLE, 1, &ci, NULL, &p);
    vkDestroyShaderModule(D, v, NULL); vkDestroyShaderModule(D, f, NULL);
    return p;
}

// The frame's resources
static VkBuffer g_ring, g_mat; static VkDeviceMemory g_ring_m, g_mat_m; static uint8_t *g_ring_p, *g_mat_p;
static VkImageView g_tv[PAIRS][2]; static VkSampler g_samp;
static VkImage g_target; static VkImageView g_tview;
static VkCommandPool g_pool; static VkCommandBuffer g_cb; static VkFence g_fence;
static uint64_t g_writes, g_allocs;

static void per_draw_data(void) {   // c0: the position, c2: the factor, one 256-byte slot each per draw
    for (int i = 0; i < DRAWS; i++) {
        float pos[4] = {((float)(i % 70) + 0.5f) / W * 2 - 1, ((float)(i / 70) + 0.5f) / W * 2 - 1, 0, 1}, k[4] = {0.25f, 0.5f, 0.75f, 1.f / (float)(1 + i % 5)};
        memcpy(g_ring_p + (size_t)(2 * i) * SLOT, pos, 16); memcpy(g_ring_p + (size_t)(2 * i + 1) * SLOT, k, 16);
    }
}
static void begin(VkCommandBuffer cb) {
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkResetCommandPool(D, g_pool, 0); vkBeginCommandBuffer(cb, &bi); barrier(cb);
    VkRenderingAttachmentInfo a = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, g_tview, VK_IMAGE_LAYOUT_GENERAL, 0, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED,
                                   VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, {.color = {{0, 0, 0, 0}}}};
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, 0, {{0, 0}, {W, W}}, 1, 0, 1, &a, NULL, NULL};
    vkCmdBeginRendering(cb, &ri);
}
static double end_submit(VkCommandBuffer cb) {   // the frame submitted and waited for: its time in ms
    vkCmdEndRendering(cb); barrier(cb); vkEndCommandBuffer(cb);
    uint64_t t0 = plat_now_ns();
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
    vkResetFences(D, 1, &g_fence); vkQueueSubmit(g_d.queue, 1, &si, g_fence); vkWaitForFences(D, 1, &g_fence, VK_TRUE, UINT64_MAX);
    return (double)(plat_now_ns() - t0) / 1e6;
}

// Cached sets: a small open-addressing table per frame, keyed by the set's content hash (the backend's set_key, vk_pipeline.c)
typedef struct { uint64_t key; VkDescriptorSet set; } ent;
static ent g_tab[256];
static uint64_t fnv(uint64_t h, const void *p, size_t n) { const uint8_t *b = p; for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ull; } return h; }
static VkDescriptorSet cached(VkDescriptorPool dp, VkDescriptorSetLayout l, uint64_t key, const VkWriteDescriptorSet *w, uint32_t nw) {
    uint32_t i = (uint32_t)(key * 0x9e3779b97f4a7c15ull >> 56);
    while (g_tab[i].key && g_tab[i].key != key) i = (i + 1) & 255;
    if (g_tab[i].key == key) return g_tab[i].set;
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, dp, 1, &l};
    VkDescriptorSet s; if (vkAllocateDescriptorSets(D, &ai, &s)) return VK_NULL_HANDLE;
    VkWriteDescriptorSet ww[8]; for (uint32_t k = 0; k < nw; k++) { ww[k] = w[k]; ww[k].dstSet = s; }
    vkUpdateDescriptorSets(D, nw, ww, 0, NULL);
    g_tab[i].key = key; g_tab[i].set = s; g_writes += nw; g_allocs++;
    return s;
}
static void frame_cached(VkCommandBuffer cb, VkPipeline p, VkPipelineLayout pl, VkDescriptorSetLayout l0, VkDescriptorSetLayout l1, VkDescriptorPool dp) {
    vkResetDescriptorPool(D, dp, 0); memset(g_tab, 0, sizeof g_tab);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    int cur_m = -1, cur_p = -1; VkDescriptorSet s0 = VK_NULL_HANDLE;
    for (int i = 0; i < DRAWS; i++) {
        int m = i / 100, pr = i / 700;
        if (m != cur_m) {   // the material's set 0: the two banks (dynamic) and its constant buffer
            VkDescriptorBufferInfo bi[3] = {{g_ring, 0, 16}, {g_ring, 0, 16}, {g_mat, (VkDeviceSize)m * SLOT, 16}};
            VkWriteDescriptorSet w[3] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, 0, 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, NULL, &bi[0], NULL},
                                         {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, 0, 1, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, NULL, &bi[1], NULL},
                                         {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, 0, 2, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, NULL, &bi[2], NULL}};
            s0 = cached(dp, l0, fnv(0xcbf29ce484222325ull ^ 0, bi, sizeof bi), w, 3); cur_m = m;
        }
        if (pr != cur_p) {
            VkDescriptorImageInfo ii[2] = {{g_samp, g_tv[pr][0], VK_IMAGE_LAYOUT_GENERAL}, {g_samp, g_tv[pr][1], VK_IMAGE_LAYOUT_GENERAL}};
            VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, 0, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, ii, NULL, NULL},
                                         {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, 0, 1, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii[1], NULL, NULL}};
            VkDescriptorSet s1 = cached(dp, l1, fnv(0xcbf29ce484222325ull ^ 1, ii, sizeof ii), w, 2); cur_p = pr;
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 1, 1, &s1, 0, NULL);
        }
        uint32_t dyn[2] = {(uint32_t)(2 * i) * SLOT, (uint32_t)(2 * i + 1) * SLOT};
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &s0, 2, dyn);
        vkCmdDraw(cb, 1, 1, 0, 0);
    }
}
static void frame_push(VkCommandBuffer cb, VkPipeline p, VkPipelineLayout pl) {
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    for (int i = 0; i < DRAWS; i++) {
        int m = i / 100, pr = i / 700;
        VkDescriptorBufferInfo bi[3] = {{g_ring, (VkDeviceSize)(2 * i) * SLOT, 16}, {g_ring, (VkDeviceSize)(2 * i + 1) * SLOT, 16}, {g_mat, (VkDeviceSize)m * SLOT, 16}};
        VkDescriptorImageInfo ii[2] = {{g_samp, g_tv[pr][0], VK_IMAGE_LAYOUT_GENERAL}, {g_samp, g_tv[pr][1], VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet w[5] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, 0, 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, NULL, &bi[0], NULL},
                                     {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, 0, 1, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, NULL, &bi[1], NULL},
                                     {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, 0, 2, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, NULL, &bi[2], NULL},
                                     {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, 0, 3, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii[0], NULL, NULL},
                                     {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, 0, 4, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii[1], NULL, NULL}};
        vkCmdPushDescriptorSetKHR(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 5, w);
        g_writes += 5;
        vkCmdDraw(cb, 1, 1, 0, 0);
    }
}
static int cmp_d(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static void readback(uint8_t *out) {   // the target's pixels
    VkDeviceMemory m; void *map; VkBuffer b = buffer((VkDeviceSize)W * W * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &m, &map);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkResetCommandPool(D, g_pool, 0); vkBeginCommandBuffer(g_cb, &bi); barrier(g_cb);
    VkBufferImageCopy r = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {W, W, 1}};
    vkCmdCopyImageToBuffer(g_cb, g_target, VK_IMAGE_LAYOUT_GENERAL, b, 1, &r); barrier(g_cb); vkEndCommandBuffer(g_cb);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &g_cb, 0, NULL};
    vkResetFences(D, 1, &g_fence); vkQueueSubmit(g_d.queue, 1, &si, g_fence); vkWaitForFences(D, 1, &g_fence, VK_TRUE, UINT64_MAX);
    memcpy(out, map, (size_t)W * W * 4);
    vkDestroyBuffer(D, b, NULL); vkFreeMemory(D, m, NULL);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    hostvk_opts o = {-1, 0, 0}; char err[512];
    if (init_or_skip("t_vk_descbench", &o, &g_d, err, sizeof err)) { t("device up", 0); printf("%s\n", err); return finish("t_vk_descbench"); }
    D = g_d.device;
    printf("device '%s' (%s %s), validation off (a benchmark)\n", g_d.props.deviceName, g_d.driver_name, g_d.driver_info);
    if (g_d.props.limits.minUniformBufferOffsetAlignment > SLOT) { printf("note: uniform offset alignment %llu above %d: skipped\n", (unsigned long long)g_d.props.limits.minUniformBufferOffsetAlignment, SLOT); return finish("t_vk_descbench"); }
    g_ring = buffer((VkDeviceSize)2 * DRAWS * SLOT, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &g_ring_m, (void **)&g_ring_p);
    g_mat = buffer((VkDeviceSize)MATS * SLOT, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &g_mat_m, (void **)&g_mat_p);
    for (int m = 0; m < MATS; m++) { float tint[4] = {(float)(m % 7) / 6.f, (float)(m % 5) / 4.f, (float)(m % 3) / 2.f, 1}; memcpy(g_mat_p + (size_t)m * SLOT, tint, 16); }
    per_draw_data();
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, g_d.queue_family};
    vkCreateCommandPool(D, &pci, NULL, &g_pool);
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, g_pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1}; vkAllocateCommandBuffers(D, &cai, &g_cb);
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; vkCreateFence(D, &fci, NULL, &g_fence);
    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; sci.magFilter = sci.minFilter = VK_FILTER_NEAREST; sci.maxLod = 0; vkCreateSampler(D, &sci, NULL, &g_samp);
    // textures: 7 pairs of 4×4 images, each cleared to its own colour; the target
    VkImage timg[PAIRS][2]; VkDeviceMemory tmem[PAIRS][2], target_m;
    g_target = image(W, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &target_m, &g_tview);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(g_cb, &bi);
    VkImageMemoryBarrier2 ib[2 * PAIRS + 1]; int nib = 0;
    for (int p = 0; p < PAIRS; p++) for (int k = 0; k < 2; k++) {
        timg[p][k] = image(4, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &tmem[p][k], &g_tv[p][k]);
        ib[nib++] = (VkImageMemoryBarrier2){VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                            VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED,
                                            VK_QUEUE_FAMILY_IGNORED, timg[p][k], {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    }
    ib[nib++] = (VkImageMemoryBarrier2){VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                        VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED,
                                        VK_QUEUE_FAMILY_IGNORED, g_target, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 0, NULL, 0, NULL, (uint32_t)nib, ib}; vkCmdPipelineBarrier2(g_cb, &di);
    for (int p = 0; p < PAIRS; p++) for (int k = 0; k < 2; k++) {
        VkClearColorValue cv = {{(float)p / 6.f, (float)k, 0.5f, 1}}; VkImageSubresourceRange rr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(g_cb, timg[p][k], VK_IMAGE_LAYOUT_GENERAL, &cv, 1, &rr);
    }
    barrier(g_cb); vkEndCommandBuffer(g_cb);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &g_cb, 0, NULL};
    vkQueueSubmit(g_d.queue, 1, &si, g_fence); vkWaitForFences(D, 1, &g_fence, VK_TRUE, UINT64_MAX);
    // the cached-set layouts (the universal layout's split: buffers in set 0 with the banks dynamic, textures in set 1) and the push layout
    VkShaderStageFlags vf = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutBinding b0[3] = {{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, vf, NULL}, {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, vf, NULL}, {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, vf, NULL}};
    VkDescriptorSetLayoutBinding b1[2] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, vf, NULL}, {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, vf, NULL}};
    VkDescriptorSetLayoutBinding bp[5] = {{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, vf, NULL}, {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, vf, NULL}, {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, vf, NULL},
                                          {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, vf, NULL}, {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, vf, NULL}};
    VkDescriptorSetLayoutCreateInfo l0i = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 3, b0}, l1i = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 2, b1};
    VkDescriptorSetLayoutCreateInfo lpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR, 5, bp};
    VkDescriptorSetLayout l0, l1, lp = VK_NULL_HANDLE; vkCreateDescriptorSetLayout(D, &l0i, NULL, &l0); vkCreateDescriptorSetLayout(D, &l1i, NULL, &l1);
    VkDescriptorSetLayout ls[2] = {l0, l1};
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 2, ls, 0, NULL};
    VkPipelineLayout plc, plp = VK_NULL_HANDLE; vkCreatePipelineLayout(D, &pli, NULL, &plc);
    VkPipeline pc = pipeline(plc, descbench_frag_spv, sizeof descbench_frag_spv), pp = VK_NULL_HANDLE;
    int push = g_d.x.push_descriptor;
    if (push) {
        vkCreateDescriptorSetLayout(D, &lpi, NULL, &lp);
        VkPipelineLayoutCreateInfo ppi = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &lp, 0, NULL}; vkCreatePipelineLayout(D, &ppi, NULL, &plp);
        pp = pipeline(plp, descbench_push_frag_spv, sizeof descbench_push_frag_spv);
    }
    t("pipelines made", pc && (!push || pp));
    VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2 * MATS}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, MATS}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * PAIRS}};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, MATS + PAIRS, 3, ps};
    VkDescriptorPool dp; vkCreateDescriptorPool(D, &dpi, NULL, &dp);
    static uint8_t img[2][W * W * 4];
    double rec[2][FRAMES], tot[2][FRAMES]; uint64_t writes[2] = {0, 0}, allocs[2] = {0, 0};
    for (int s = 0; s < (push ? 2 : 1); s++) {
        for (int f = 0; f < WARM + FRAMES; f++) {
            g_writes = g_allocs = 0;
            uint64_t t0 = plat_now_ns();
            begin(g_cb);
            if (s == 0) frame_cached(g_cb, pc, plc, l0, l1, dp); else frame_push(g_cb, pp, plp);
            double r = (double)(plat_now_ns() - t0) / 1e6, e = end_submit(g_cb);
            if (f >= WARM) { rec[s][f - WARM] = r; tot[s][f - WARM] = r + e; writes[s] = g_writes; allocs[s] = g_allocs; }
        }
        readback(img[s]);
    }
    printf("\n%-44s %8s %14s %10s %18s %16s %14s\n", "strategy (4,900 draws a frame)", "draws", "record ms", "ns/draw", "descriptor writes", "set allocations", "frame ms");
    static const char *const name[2] = {"cached sets + dynamic offsets (the backend)", "push descriptors (VK_KHR_push_descriptor)"};
    for (int s = 0; s < 2; s++) {
        if (s == 1 && !push) { printf("%-44s %8s %14s %10s %18s %16s %14s\n", name[s], "-", "-", "-", "-", "-", "(not supported)"); continue; }
        qsort(rec[s], FRAMES, sizeof(double), cmp_d); qsort(tot[s], FRAMES, sizeof(double), cmp_d);
        double r = rec[s][FRAMES / 2];
        printf("%-44s %8d %14.3f %10.1f %18llu %16llu %14.3f\n", name[s], DRAWS, r, r * 1e6 / DRAWS, (unsigned long long)writes[s], (unsigned long long)allocs[s], tot[s][FRAMES / 2]);
    }
    printf("(medians of %d frames after %d of warm-up; record = CPU time to record a frame's draws and bindings)\n\n", FRAMES, WARM);
    int lit = 0; for (int i = 0; i < W * W; i++) lit += img[0][4 * i + 3] != 0;
    t("the cached-set frame drew its 4,900 points", lit == DRAWS);
    if (push) t("both strategies draw the same image", !memcmp(img[0], img[1], sizeof img[0]));
    vkDeviceWaitIdle(D);
    vkDestroyDescriptorPool(D, dp, NULL); vkDestroyPipeline(D, pc, NULL); vkDestroyPipelineLayout(D, plc, NULL);
    if (push) { vkDestroyPipeline(D, pp, NULL); vkDestroyPipelineLayout(D, plp, NULL); vkDestroyDescriptorSetLayout(D, lp, NULL); }
    vkDestroyDescriptorSetLayout(D, l0, NULL); vkDestroyDescriptorSetLayout(D, l1, NULL);
    for (int p = 0; p < PAIRS; p++) for (int k = 0; k < 2; k++) { vkDestroyImageView(D, g_tv[p][k], NULL); vkDestroyImage(D, timg[p][k], NULL); vkFreeMemory(D, tmem[p][k], NULL); }
    vkDestroyImageView(D, g_tview, NULL); vkDestroyImage(D, g_target, NULL); vkFreeMemory(D, target_m, NULL);
    vkDestroySampler(D, g_samp, NULL); vkDestroyFence(D, g_fence, NULL); vkDestroyCommandPool(D, g_pool, NULL);
    vkDestroyBuffer(D, g_ring, NULL); vkFreeMemory(D, g_ring_m, NULL); vkDestroyBuffer(D, g_mat, NULL); vkFreeMemory(D, g_mat_m, NULL);
    hostvk_destroy(&g_d);
    return finish("t_vk_descbench");
}
