// t_vk_guest: the guest Vulkan layer (dm/gfx/hostvk/guest_vk.c; docs/design/vulkan-backend.md 8.1, step P9a) driven as a game drives it: vkGetInstanceProcAddr found by name in the
// import table (as the loader binds a game's import), every other function from it and from vkGetDeviceProcAddr, none from volk. On the backend's device (lavapipe under the
// validation layer with synchronization validation in CTest): the Tegra X1 shape (API version, IDs, one queue family, three memory types on one heap, limits the games read as
// the stricter of the shape and the host, features where the shared device has them); memory of the guest types (vertex data and a uniform array written through the mapping,
// a readback buffer of the cached type, an optimal image on device-local memory of its own, a linear image whose CPU-written texels are read back); a render pass, three
// pipelines through the backend's disk cache (a plain one; Super Mario Galaxy's two shader forms: its second blend source at Location 1 without an Index, and its ArrayStride on a
// struct member), their draws read back pixel for pixel; occlusion and timestamp queries; DM_VK_CAPTURE's files; a swapchain over a VK_NN_vi_surface surface, acquired and
// presented four times, with a layer crop and without, read back through the backend's headless presenter. Every validation message fails the test. Without a Vulkan device it
// skips as the other GPU tests do (t_vk_harness.h).
#include "t_vk_harness.h"
#include "../../../host/hle.h"
#include "../../hostvk/guest_vk.h"
#include "../../hostvk/vk_pso.h"
#include <vulkan/vulkan_vi.h>
#include "shaders/gen/guest_vert.h"
#include "shaders/gen/guest_ubo_vert.h"
#include "shaders/gen/guest_dual_frag.h"
#include "shaders/gen/col_frag.h"

PLAT_TABLE_DEFINE(swhle)   // the import table (dm/host/hle.c defines it in a host binary)
int dm_opmode(void) { return 0; }   // handheld: the surface's extent is 1280x720
Result h_vi_SetCrop(void *layer, int x, int y, int w, int h);   // dm/gfx/hle_vi.c (linked): the game's vi::SetLayerCrop, whose state the present reads

// ---------- The game's function pointers ----------
#define GLOBAL_FNS(X) X(CreateInstance) X(EnumerateInstanceExtensionProperties) X(EnumerateInstanceVersion)
#define INST_FNS(X) X(EnumeratePhysicalDevices) X(GetPhysicalDeviceProperties) X(GetPhysicalDeviceFeatures) X(GetPhysicalDeviceMemoryProperties) \
    X(GetPhysicalDeviceQueueFamilyProperties) X(EnumerateDeviceExtensionProperties) X(CreateDevice) X(GetDeviceProcAddr) X(CreateViSurfaceNN) \
    X(GetPhysicalDeviceSurfaceCapabilitiesKHR) X(GetPhysicalDeviceSurfaceFormatsKHR) X(GetPhysicalDeviceSurfaceSupportKHR) X(DestroySurfaceKHR) X(DestroyInstance) \
    X(GetPhysicalDeviceFormatProperties)
#define DEV_FNS(X) X(GetDeviceQueue) X(AllocateMemory) X(FreeMemory) X(MapMemory) X(UnmapMemory) X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) \
    X(BindBufferMemory) X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements) X(BindImageMemory) X(GetImageSubresourceLayout) X(CreateImageView) X(DestroyImageView) \
    X(CreateRenderPass) X(DestroyRenderPass) X(CreateFramebuffer) X(DestroyFramebuffer) X(CreateShaderModule) X(DestroyShaderModule) X(CreatePipelineCache) \
    X(DestroyPipelineCache) X(GetPipelineCacheData) X(CreatePipelineLayout) X(DestroyPipelineLayout) X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) \
    X(CreateDescriptorPool) X(DestroyDescriptorPool) X(AllocateDescriptorSets) X(UpdateDescriptorSets) X(CreateGraphicsPipelines) X(DestroyPipeline) X(CreateCommandPool) \
    X(DestroyCommandPool) X(AllocateCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer) X(ResetCommandBuffer) X(CmdBeginRenderPass) X(CmdEndRenderPass) \
    X(CmdBindPipeline) X(CmdBindVertexBuffers) X(CmdBindDescriptorSets) X(CmdSetViewport) X(CmdSetScissor) X(CmdDraw) X(CmdCopyImageToBuffer) X(CmdPipelineBarrier) \
    X(CmdResetQueryPool) X(CmdBeginQuery) X(CmdEndQuery) X(CmdWriteTimestamp) X(CreateQueryPool) X(DestroyQueryPool) X(GetQueryPoolResults) X(CreateFence) X(DestroyFence) \
    X(WaitForFences) X(ResetFences) X(CreateSemaphore) X(DestroySemaphore) X(QueueSubmit) X(DeviceWaitIdle) X(CreateSwapchainKHR) X(DestroySwapchainKHR) \
    X(GetSwapchainImagesKHR) X(AcquireNextImageKHR) X(QueuePresentKHR) X(DestroyDevice)
static struct {
#define DECL(n) PFN_vk##n n;
    GLOBAL_FNS(DECL) INST_FNS(DECL) DEV_FNS(DECL)
#undef DECL
} G;
static void *import(const char *name) {   // a game import, bound by name through the import table as the loader binds it
    size_t sz = 0; const hle_ent *e = plat_table_swhle(&sz);
    for (size_t i = 0; e && i < sz / sizeof *e; i++) if (e[i].name && !strcmp(e[i].name, name)) return e[i].fn;
    return NULL;
}
static int g_missing;
#define LOADG(n) if (!(G.n = (PFN_vk##n)gipa(NULL, "vk" #n))) { g_missing++; printf("missing vk%s\n", #n); }
#define LOADI(n) if (!(G.n = (PFN_vk##n)gipa(inst, "vk" #n))) { g_missing++; printf("missing vk%s\n", #n); }
#define LOADD(n) if (!(G.n = (PFN_vk##n)G.GetDeviceProcAddr(dev, "vk" #n))) { g_missing++; printf("missing vk%s\n", #n); }

static void ck(const char *what, VkResult r) { if (r) { char n[200]; snprintf(n, sizeof n, "%s: VkResult %d", what, r); t(n, 0); } }
static uint32_t umax32(uint32_t a, uint32_t b) { return a > b ? a : b; }
static uint32_t umin32(uint32_t a, uint32_t b) { return a < b ? a : b; }
static VkDeviceSize up(VkDeviceSize v, VkDeviceSize a) { return (v + a - 1) / a * a; }
static int near(int a, int b) { return a - b <= 1 && b - a <= 1; }

// Super Mario Galaxy's form of a module: each array type's ArrayStride, where the array is a struct's member, moved onto that member (OpMemberDecorate)
static size_t galaxy_form(const uint32_t *in, size_t n, uint32_t *out) {
    size_t k = 0;
    for (size_t i = 0; i < 5; i++) out[k++] = in[i];
    for (size_t i = 5; i < n; i += in[i] >> 16) {
        uint32_t wc = in[i] >> 16;
        if ((in[i] & 0xffff) == 71 && wc == 4 && in[i + 2] == 6) {   // OpDecorate ArrayStride
            uint32_t st = 0, mem = 0;
            for (size_t j = 5; j < n && !st; j += in[j] >> 16)
                if ((in[j] & 0xffff) == 30) for (uint32_t q = 2; q < (in[j] >> 16); q++) if (in[j + q] == in[i + 1]) { st = in[j + 1]; mem = q - 2; break; }   // OpTypeStruct
            if (st) { out[k++] = 5u << 16 | 72; out[k++] = st; out[k++] = mem; out[k++] = 6; out[k++] = in[i + 3]; continue; }
        }
        for (uint32_t q = 0; q < wc; q++) out[k++] = in[i + q];
    }
    return k;
}
static int file_is(const char *path, const void *data, size_t n) {   // the file holds exactly these bytes
    FILE *f = fopen(path, "rb"); if (!f) return 0;
    uint8_t *b = malloc(n + 1); size_t got = b ? fread(b, 1, n + 1, f) : 0; fclose(f);
    int ok = b && got == n && !memcmp(b, data, n); free(b); return ok;
}
static void key_of(const void *code, size_t n, char key[17]) { uint8_t h[32]; plat_sha256(code, n, h); for (int i = 0; i < 8; i++) snprintf(key + 2 * i, 3, "%02x", h[i]); }

// A pipeline of the test: vs + fs, a vertex buffer of position (vec2) and colour (vec4) per vertex (colour: location 1 fed), triangles, dynamic viewport and scissor; dual: blends
// the first source with the second (SRC1_COLOR, ZERO)
static VkPipeline make_pipe(VkDevice dev, VkPipelineCache pc, VkShaderModule vs, VkShaderModule fs, VkPipelineLayout lay, VkRenderPass rp, int colour, int dual) {
    VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL},
                                             {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", NULL}};
    VkVertexInputBindingDescription vb = {0, 24, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription va[2] = {{0, 0, VK_FORMAT_R32G32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 8}};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, NULL, 0, 1, &vb, colour ? 2u : 1u, va};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, NULL, 0, VK_FALSE, VK_FALSE, VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE,
                                                 VK_FRONT_FACE_COUNTER_CLOCKWISE, VK_FALSE, 0, 0, 0, 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState ba = {VK_FALSE, 0, 0, 0, 0, 0, 0, 0xf};
    if (dual) ba = (VkPipelineColorBlendAttachmentState){VK_TRUE, VK_BLEND_FACTOR_SRC1_COLOR, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, 0xf};
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, VK_FALSE, VK_LOGIC_OP_COPY, 1, &ba, {0, 0, 0, 0}};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 2, dyn};
    VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, NULL, 0, 2, st, &vi, &ia, NULL, &vp, &rs, &ms, NULL, &cb, &dy, lay, rp, 0, VK_NULL_HANDLE, -1};
    VkPipeline p = VK_NULL_HANDLE; ck("vkCreateGraphicsPipelines", G.CreateGraphicsPipelines(dev, pc, 1, &ci, NULL, &p));
    return p;
}
// One colour attachment, cleared and stored; final: its layout after the pass (an external dependency orders a transfer read after it, or a present)
static VkRenderPass pass(VkDevice dev, VkFormat fmt, VkImageLayout final) {
    VkAttachmentDescription at = {0, fmt, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                  VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, final};
    VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sp = {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, 1, &ref, NULL, NULL, 0, NULL};
    VkSubpassDependency dep[2] = {
        {VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
         VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0},
        {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, final == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, final == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ? VK_ACCESS_TRANSFER_READ_BIT : 0, 0}};
    VkRenderPassCreateInfo ci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, 1, &at, 1, &sp, 2, dep};
    VkRenderPass rp = VK_NULL_HANDLE; ck("vkCreateRenderPass", G.CreateRenderPass(dev, &ci, NULL, &rp));
    return rp;
}
static VkShaderModule module(VkDevice dev, const uint32_t *code, size_t bytes) {
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, bytes, code};
    VkShaderModule m = VK_NULL_HANDLE; ck("vkCreateShaderModule", G.CreateShaderModule(dev, &ci, NULL, &m));
    return m;
}
static void quad(float *v, int px0, int py0, int px1, int py1, int w, int h, const float c[4]) {   // two triangles over pixels [px0, px1) x [py0, py1) of a w x h target
    float x0 = -1 + 2.0f * px0 / w, x1 = -1 + 2.0f * px1 / w, y0 = -1 + 2.0f * py0 / h, y1 = -1 + 2.0f * py1 / h;
    const float p[6][2] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y0}, {x1, y1}, {x0, y1}};
    for (int i = 0; i < 6; i++) { v[6 * i] = p[i][0]; v[6 * i + 1] = p[i][1]; memcpy(v + 6 * i + 2, c, 16); }
}

int main(void) {
    const hostvk_dev *hd = hv_up("t_vk_guest");
    const VkPhysicalDeviceLimits *hl = &hd->props.limits;
    char cap[600], name[300];
    snprintf(cap, sizeof cap, "%s/capture", g_lib); mkdir(cap, 0755); setenv("DM_VK_CAPTURE", cap, 1);

    // ---------- The import and the instance ----------
    // A reference to the layer's object links it, with its entries, from its static library, as the host's thunk table (gen/hle_thunks.c) links it into a host binary
    t("the test's modules pass the layer's SPIR-V checks", !guest_spirv_check(guest_vert_spv, sizeof guest_vert_spv / 4) &&
      !guest_spirv_check(guest_ubo_vert_spv, sizeof guest_ubo_vert_spv / 4) && !guest_spirv_check(guest_dual_frag_spv, sizeof guest_dual_frag_spv / 4));
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)import("vkGetInstanceProcAddr");
    t("vkGetInstanceProcAddr is an entry of the import table", gipa != NULL);
    if (!gipa) return hv_finish();
    GLOBAL_FNS(LOADG)
    uint32_t ver = 0; G.EnumerateInstanceVersion(&ver);
    t("vkEnumerateInstanceVersion: Vulkan 1.1", ver == VK_API_VERSION_1_1);
    VkExtensionProperties ie[8]; uint32_t nie = 8; G.EnumerateInstanceExtensionProperties(NULL, &nie, ie);
    int vi_ext = 0; for (uint32_t i = 0; i < nie; i++) vi_ext += !strcmp(ie[i].extensionName, "VK_NN_vi_surface") || !strcmp(ie[i].extensionName, "VK_KHR_surface");
    t("instance extensions: VK_KHR_surface and VK_NN_vi_surface", vi_ext == 2);
    const char *iext[2] = {"VK_KHR_surface", "VK_NN_vi_surface"};
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "t_vk_guest", 1, "t_vk_guest", 1, VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 2, iext};
    VkInstance inst = VK_NULL_HANDLE; ck("vkCreateInstance", G.CreateInstance(&ici, NULL, &inst));
    INST_FNS(LOADI)
    t("the KHR name of a promoted function finds the core one", gipa(inst, "vkGetPhysicalDeviceProperties2KHR") == gipa(inst, "vkGetPhysicalDeviceProperties2") &&
      gipa(inst, "vkGetPhysicalDeviceProperties2") != NULL && gipa(inst, "vkBindBufferMemory2KHR") != NULL);
    t("a function the layer does not implement is null", gipa(inst, "vkCreateDebugUtilsMessengerEXT") == NULL && gipa(inst, "notvk") == NULL);

    // ---------- The Tegra shape ----------
    uint32_t npd = 0; G.EnumeratePhysicalDevices(inst, &npd, NULL);
    VkPhysicalDevice pd = VK_NULL_HANDLE; npd = 1; G.EnumeratePhysicalDevices(inst, &npd, &pd);
    t("one physical device", npd == 1 && pd);
    VkPhysicalDeviceProperties pp; G.GetPhysicalDeviceProperties(pd, &pp);
    const VkPhysicalDeviceLimits *l = &pp.limits;
    snprintf(name, sizeof name, "properties: API %u.%u, vendor %#x device %#x '%s'", VK_API_VERSION_MAJOR(pp.apiVersion), VK_API_VERSION_MINOR(pp.apiVersion), pp.vendorID, pp.deviceID, pp.deviceName);
    t(name, pp.apiVersion == VK_API_VERSION_1_1 && pp.vendorID == 0x10de && pp.deviceID == 0x92ba && !strcmp(pp.deviceName, "NVIDIA Tegra X1 (nvgpu)"));
    snprintf(name, sizeof name, "limits: uniform offset alignment %llu (Tegra 256, host %llu), push constants %u, sets %u, viewports %u, colour attachments %u",
             (unsigned long long)l->minUniformBufferOffsetAlignment, (unsigned long long)hl->minUniformBufferOffsetAlignment, l->maxPushConstantsSize, l->maxBoundDescriptorSets,
             l->maxViewports, l->maxColorAttachments);
    t(name, l->minUniformBufferOffsetAlignment == (umax32(256, (uint32_t)hl->minUniformBufferOffsetAlignment)) && l->minUniformBufferOffsetAlignment % hl->minUniformBufferOffsetAlignment == 0 &&
            l->maxPushConstantsSize == umin32(256, hl->maxPushConstantsSize) && l->maxBoundDescriptorSets == umin32(8, hl->maxBoundDescriptorSets) && l->maxViewports == 1 &&
            l->maxColorAttachments == umin32(8, hl->maxColorAttachments));
    t("limits: the host's timestamp period, the shape's dual-source attachments, the host's where the shape gives none (geometry output vertices)",
      l->timestampPeriod == hl->timestampPeriod && l->maxFragmentDualSrcAttachments == umin32(1, hl->maxFragmentDualSrcAttachments) &&
      l->maxGeometryOutputVertices == hl->maxGeometryOutputVertices && l->maxVertexInputBindings == umin32(14, hl->maxVertexInputBindings));
    VkPhysicalDeviceFeatures pf; G.GetPhysicalDeviceFeatures(pd, &pf);
    snprintf(name, sizeof name, "features: dualSrcBlend %u geometryShader %u textureCompressionBC %u, robustBufferAccess %u (not on the shared device), ASTC %u (host %d)",
             pf.dualSrcBlend, pf.geometryShader, pf.textureCompressionBC, pf.robustBufferAccess, pf.textureCompressionASTC_LDR, hd->astc_ldr);
    t(name, pf.dualSrcBlend && pf.geometryShader && pf.textureCompressionBC && !pf.robustBufferAccess && pf.textureCompressionASTC_LDR == (VkBool32)hd->astc_ldr && !pf.tessellationShader);
    VkPhysicalDeviceMemoryProperties pm; G.GetPhysicalDeviceMemoryProperties(pd, &pm);
    const VkMemoryPropertyFlags DL = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, HV = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    t("memory: three types on one heap (device-local; host-visible coherent; and cached)", pm.memoryTypeCount == 3 && pm.memoryHeapCount == 1 && pm.memoryTypes[0].propertyFlags == DL &&
      pm.memoryTypes[1].propertyFlags == (DL | HV) && pm.memoryTypes[2].propertyFlags == (DL | HV | VK_MEMORY_PROPERTY_HOST_CACHED_BIT));
    uint32_t nqf = 0; G.GetPhysicalDeviceQueueFamilyProperties(pd, &nqf, NULL);
    VkQueueFamilyProperties qfp; nqf = 1; G.GetPhysicalDeviceQueueFamilyProperties(pd, &nqf, &qfp);
    t("one queue family: graphics, compute and transfer", nqf == 1 && qfp.queueFlags == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT) && qfp.timestampValidBits > 0);
    VkExtensionProperties de[8]; uint32_t nde = 8; G.EnumerateDeviceExtensionProperties(pd, NULL, &nde, de);
    int swx = 0; for (uint32_t i = 0; i < nde; i++) swx |= !strcmp(de[i].extensionName, "VK_KHR_swapchain");
    t("device extensions: VK_KHR_swapchain", swx);
    VkFormatProperties fp; G.GetPhysicalDeviceFormatProperties(pd, VK_FORMAT_R8G8B8A8_UNORM, &fp);
    t("formats: the host's answer (RGBA8 renders)", (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0);

    // ---------- The device ----------
    float prio = 1;
    VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio};
    VkPhysicalDeviceFeatures want = {0}; want.dualSrcBlend = want.geometryShader = want.occlusionQueryPrecise = VK_TRUE;
    const char *dext[1] = {"VK_KHR_swapchain"};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qi, 0, NULL, 1, dext, &want};
    VkDevice dev = VK_NULL_HANDLE; ck("vkCreateDevice", G.CreateDevice(pd, &dci, NULL, &dev));
    DEV_FNS(LOADD)
    snprintf(name, sizeof name, "every function the test calls comes from the layer's vkGet*ProcAddr (%d missing)", g_missing);
    t(name, g_missing == 0);
    if (g_missing) return hv_finish();
    VkQueue q = VK_NULL_HANDLE; G.GetDeviceQueue(dev, 0, 0, &q);
    hvk_cache_stats cs0; hvk_cache_stats_get(&cs0);

    // ---------- Memory, buffers, images ----------
    enum { W = 64, H = 64 };
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 4096, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    VkBuffer vbuf, ubuf, rbuf; ck("vkCreateBuffer", G.CreateBuffer(dev, &bci, NULL, &vbuf));
    bci.size = 256; bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT; ck("vkCreateBuffer", G.CreateBuffer(dev, &bci, NULL, &ubuf));
    bci.size = W * H * 4 + 64; bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT; ck("vkCreateBuffer", G.CreateBuffer(dev, &bci, NULL, &rbuf));
    VkMemoryRequirements vr, ur, rr; G.GetBufferMemoryRequirements(dev, vbuf, &vr); G.GetBufferMemoryRequirements(dev, ubuf, &ur); G.GetBufferMemoryRequirements(dev, rbuf, &rr);
    t("buffer requirements: every guest type", vr.memoryTypeBits == 7 && ur.memoryTypeBits == 7 && rr.memoryTypeBits == 7);
    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {W, H, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage img, lin; ck("vkCreateImage", G.CreateImage(dev, &ii, NULL, &img));
    ii.extent = (VkExtent3D){4, 4, 1}; ii.tiling = VK_IMAGE_TILING_LINEAR; ii.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT; ii.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
    ck("vkCreateImage (linear)", G.CreateImage(dev, &ii, NULL, &lin));
    VkMemoryRequirements ir, lr; G.GetImageMemoryRequirements(dev, img, &ir); G.GetImageMemoryRequirements(dev, lin, &lr);
    t("image requirements: an optimal image takes any guest type, a linear one host-visible types", ir.memoryTypeBits == 7 && (lr.memoryTypeBits & 2));
    VkDeviceSize uoff = up(vr.size, umax32((uint32_t)ur.alignment, (uint32_t)l->minUniformBufferOffsetAlignment)), loff = up(uoff + ur.size, lr.alignment);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, loff + lr.size, 1};
    VkDeviceMemory m1, m2, m0; ck("vkAllocateMemory (type 1)", G.AllocateMemory(dev, &mai, NULL, &m1));
    mai.allocationSize = rr.size; mai.memoryTypeIndex = 2; ck("vkAllocateMemory (type 2)", G.AllocateMemory(dev, &mai, NULL, &m2));
    mai.allocationSize = ir.size; mai.memoryTypeIndex = 0; ck("vkAllocateMemory (type 0)", G.AllocateMemory(dev, &mai, NULL, &m0));
    ck("vkBindBufferMemory", G.BindBufferMemory(dev, vbuf, m1, 0)); ck("vkBindBufferMemory", G.BindBufferMemory(dev, ubuf, m1, uoff));
    ck("vkBindBufferMemory", G.BindBufferMemory(dev, rbuf, m2, 0));
    ck("vkBindImageMemory", G.BindImageMemory(dev, img, m0, 0)); ck("vkBindImageMemory (linear)", G.BindImageMemory(dev, lin, m1, loff));
    uint8_t *p1 = NULL, *p2 = NULL; ck("vkMapMemory", G.MapMemory(dev, m1, 0, VK_WHOLE_SIZE, 0, (void **)&p1)); ck("vkMapMemory", G.MapMemory(dev, m2, 0, VK_WHOLE_SIZE, 0, (void **)&p2));
    void *p0 = (void *)1; VkResult r0 = G.MapMemory(dev, m0, 0, VK_WHOLE_SIZE, 0, &p0);
    t("type 0 is not host-visible: mapping it is refused", r0 == VK_ERROR_MEMORY_MAP_FAILED && p0 == NULL);
    t("the host-visible types map", p1 && p2);
    if (!p1 || !p2) return hv_finish();
    static const float red[4] = {1, 0, 0, 1}, white[4] = {1, 1, 1, 1}, none[4] = {0, 0, 0, 0};
    float *v = (float *)p1;   // vertex data written through the mapping: three quads of the 64x64 target, the swapchain's quad of a 320x180 one
    quad(v, 0, 0, 16, 16, W, H, red); quad(v + 36, 16, 0, 32, 16, W, H, white); quad(v + 72, 32, 0, 48, 16, W, H, none); quad(v + 108, 40, 20, 80, 50, 320, 180, red);
    const float ubo[16] = {9, 9, 9, 9, 8, 8, 8, 8, 0.0f, 0.5f, 1.0f, 1.0f, 7, 7, 7, 7};   // element 2: the colour the third quad gets
    memcpy(p1 + uoff, ubo, sizeof ubo);
    VkImageSubresource sr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0}; VkSubresourceLayout sl; G.GetImageSubresourceLayout(dev, lin, &sr, &sl);
    for (uint32_t y = 0; y < 4; y++) for (uint32_t x = 0; x < 4; x++) { uint8_t *px = p1 + loff + sl.offset + y * sl.rowPitch + 4 * x; px[0] = (uint8_t)(16 * x); px[1] = (uint8_t)(16 * y); px[2] = 7; px[3] = 255; }
    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, img, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {0, 0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView view; ck("vkCreateImageView", G.CreateImageView(dev, &vci, NULL, &view));

    // ---------- Shaders, layouts, pipelines ----------
    static uint32_t gal[4096]; size_t ngal = galaxy_form(guest_ubo_vert_spv, sizeof guest_ubo_vert_spv / 4, gal);
    VkShaderModule vs = module(dev, guest_vert_spv, sizeof guest_vert_spv), fs = module(dev, col_frag_spv, sizeof col_frag_spv);
    VkShaderModule fsd = module(dev, guest_dual_frag_spv, sizeof guest_dual_frag_spv), vsu = module(dev, gal, ngal * 4);
    char key[17], path[800]; key_of(gal, ngal * 4, key); snprintf(path, sizeof path, "%s/%s.spv", cap, key);
    t("DM_VK_CAPTURE: the module in Galaxy's form is written as the game gave it", file_is(path, gal, ngal * 4));
    key_of(guest_dual_frag_spv, sizeof guest_dual_frag_spv, key); snprintf(path, sizeof path, "%s/%s.spv", cap, key);
    t("DM_VK_CAPTURE: the dual-source fragment shader is written as the game gave it", file_is(path, guest_dual_frag_spv, sizeof guest_dual_frag_spv));
    uint8_t junk[64]; memset(junk, 0x5a, sizeof junk);
    VkPipelineCacheCreateInfo pci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, NULL, 0, sizeof junk, junk};   // as NVIDIA's Switch binary cache: not the host's
    VkPipelineCache gpc; ck("vkCreatePipelineCache", G.CreatePipelineCache(dev, &pci, NULL, &gpc));
    VkDescriptorSetLayoutBinding lb = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &lb};
    VkDescriptorSetLayout dsl; ck("vkCreateDescriptorSetLayout", G.CreateDescriptorSetLayout(dev, &lci, NULL, &dsl));
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 0, NULL, 0, NULL};
    VkPipelineLayout pl0, pl1; ck("vkCreatePipelineLayout", G.CreatePipelineLayout(dev, &plci, NULL, &pl0));
    plci.setLayoutCount = 1; plci.pSetLayouts = &dsl; ck("vkCreatePipelineLayout", G.CreatePipelineLayout(dev, &plci, NULL, &pl1));
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1};
    VkDescriptorPoolCreateInfo dpci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 1, &ps};
    VkDescriptorPool dp; ck("vkCreateDescriptorPool", G.CreateDescriptorPool(dev, &dpci, NULL, &dp));
    VkDescriptorSetAllocateInfo dsai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, dp, 1, &dsl};
    VkDescriptorSet ds; ck("vkAllocateDescriptorSets", G.AllocateDescriptorSets(dev, &dsai, &ds));
    VkDescriptorBufferInfo dbi = {ubuf, 0, 64};
    VkWriteDescriptorSet wds = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, ds, 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, NULL, &dbi, NULL};
    G.UpdateDescriptorSets(dev, 1, &wds, 0, NULL);
    VkRenderPass rp = pass(dev, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkFramebufferCreateInfo fci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, rp, 1, &view, W, H, 1};
    VkFramebuffer fb; ck("vkCreateFramebuffer", G.CreateFramebuffer(dev, &fci, NULL, &fb));
    VkPipeline pa = make_pipe(dev, gpc, vs, fs, pl0, rp, 1, 0), pb = make_pipe(dev, gpc, vs, fsd, pl0, rp, 1, 1), pc = make_pipe(dev, gpc, vsu, fs, pl1, rp, 0, 0);
    t("three pipelines made (plain, dual-source from Location 1, uniform array in Galaxy's form)", pa && pb && pc);
    hvk_cache_stats cs1; hvk_cache_stats_get(&cs1);
    snprintf(name, sizeof name, "pipelines through the backend's disk cache: %llu creations counted, cache %s", (unsigned long long)(cs1.created - cs0.created), cs1.on ? "on" : "off");
    t(name, cs1.created - cs0.created == 3 && cs1.on);
    size_t pcn = 99; G.GetPipelineCacheData(dev, gpc, &pcn, NULL);
    t("the game's pipeline cache holds nothing", pcn == 0);

    // ---------- Commands: draws, queries, copies ----------
    VkQueryPoolCreateInfo qpci = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_OCCLUSION, 1, 0};
    VkQueryPool occ, ts; ck("vkCreateQueryPool", G.CreateQueryPool(dev, &qpci, NULL, &occ));
    qpci.queryType = VK_QUERY_TYPE_TIMESTAMP; qpci.queryCount = 2; ck("vkCreateQueryPool", G.CreateQueryPool(dev, &qpci, NULL, &ts));
    VkCommandPoolCreateInfo cpci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, 0};
    VkCommandPool cp; ck("vkCreateCommandPool", G.CreateCommandPool(dev, &cpci, NULL, &cp));
    VkCommandBufferAllocateInfo cbai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, cp, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cb; ck("vkAllocateCommandBuffers", G.AllocateCommandBuffers(dev, &cbai, &cb));
    VkCommandBufferBeginInfo cbbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    G.BeginCommandBuffer(cb, &cbbi);
    G.CmdResetQueryPool(cb, occ, 0, 1); G.CmdResetQueryPool(cb, ts, 0, 2);
    G.CmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, ts, 0);
    VkImageMemoryBarrier lb_ = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_PREINITIALIZED,
                                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, lin, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    G.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &lb_);
    VkBufferImageCopy lc = {W * H * 4, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {4, 4, 1}};
    G.CmdCopyImageToBuffer(cb, lin, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbuf, 1, &lc);
    VkClearValue clear = {{{0, 0, 0, 1}}};
    VkRenderPassBeginInfo rpbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, rp, fb, {{0, 0}, {W, H}}, 1, &clear};
    G.CmdBeginRenderPass(cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp = {0, 0, W, H, 0, 1}; VkRect2D sc = {{0, 0}, {W, H}}; VkDeviceSize zero = 0;
    G.CmdSetViewport(cb, 0, 1, &vp); G.CmdSetScissor(cb, 0, 1, &sc); G.CmdBindVertexBuffers(cb, 0, 1, &vbuf, &zero);
    G.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pa);
    G.CmdBeginQuery(cb, occ, 0, VK_QUERY_CONTROL_PRECISE_BIT); G.CmdDraw(cb, 6, 1, 0, 0); G.CmdEndQuery(cb, occ, 0);
    G.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pb); G.CmdDraw(cb, 6, 1, 6, 0);
    G.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pc); G.CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl1, 0, 1, &ds, 0, NULL); G.CmdDraw(cb, 6, 1, 12, 0);
    G.CmdEndRenderPass(cb);
    G.CmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, ts, 1);
    VkBufferImageCopy ic = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {W, H, 1}};
    G.CmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbuf, 1, &ic);
    VkMemoryBarrier hb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    G.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, NULL, 0, NULL);
    ck("vkEndCommandBuffer", G.EndCommandBuffer(cb));
    VkFenceCreateInfo fci2 = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0};
    VkFence fence; ck("vkCreateFence", G.CreateFence(dev, &fci2, NULL, &fence));
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
    ck("vkQueueSubmit", G.QueueSubmit(q, 1, &si, fence));
    ck("vkWaitForFences", G.WaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
    const uint8_t *px = p2;
#define PX(x, y) (px + ((size_t)(y) * W + (size_t)(x)) * 4)
    snprintf(name, sizeof name, "plain draw: red at (8, 8) %u %u %u %u", PX(8, 8)[0], PX(8, 8)[1], PX(8, 8)[2], PX(8, 8)[3]);
    t(name, PX(8, 8)[0] == 255 && PX(8, 8)[1] == 0 && PX(8, 8)[2] == 0 && PX(8, 8)[3] == 255);
    snprintf(name, sizeof name, "dual source: the second source (written at Location 1) multiplies the first at (24, 8): %u %u %u %u (64 128 191 255)", PX(24, 8)[0], PX(24, 8)[1], PX(24, 8)[2], PX(24, 8)[3]);
    t(name, near(PX(24, 8)[0], 64) && near(PX(24, 8)[1], 128) && near(PX(24, 8)[2], 191) && PX(24, 8)[3] == 255);
    snprintf(name, sizeof name, "member ArrayStride moved: element 2 of the uniform array at (40, 8): %u %u %u %u (0 128 255 255)", PX(40, 8)[0], PX(40, 8)[1], PX(40, 8)[2], PX(40, 8)[3]);
    t(name, PX(40, 8)[0] == 0 && near(PX(40, 8)[1], 128) && PX(40, 8)[2] == 255 && PX(40, 8)[3] == 255);
    int edges = 1; for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { int in = y < 16 && x < 48; if (!in && (PX(x, y)[0] || PX(x, y)[1] || PX(x, y)[2] || PX(x, y)[3] != 255)) edges = 0; }
    t("every pixel outside the three quads is the clear colour", edges);
    int lok = 1; const uint8_t *lp = p2 + W * H * 4;
    for (uint32_t y = 0; y < 4; y++) for (uint32_t x = 0; x < 4; x++) { const uint8_t *c = lp + (y * 4 + x) * 4; lok &= c[0] == 16 * x && c[1] == 16 * y && c[2] == 7 && c[3] == 255; }
    t("linear image: the texels the CPU wrote through the mapping are its texels (it aliases the game's memory)", lok);
    uint64_t occv = 0; VkResult qr = G.GetQueryPoolResults(dev, occ, 0, 1, 8, &occv, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    snprintf(name, sizeof name, "occlusion query: %llu samples passed (a 16x16 quad: 256)", (unsigned long long)occv);
    t(name, qr == VK_SUCCESS && occv == 256);
    uint64_t tsv[4] = {0}; qr = G.GetQueryPoolResults(dev, ts, 0, 2, sizeof tsv, tsv, 16, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT | VK_QUERY_RESULT_WAIT_BIT);
    snprintf(name, sizeof name, "timestamp queries: available, %llu ticks apart", (unsigned long long)(tsv[2] - tsv[0]));
    t(name, qr == VK_SUCCESS && tsv[1] && tsv[3] && tsv[2] >= tsv[0]);

    // ---------- Swapchain and present ----------
    enum { SW = 320, SH = 180 };
    VkViSurfaceCreateInfoNN vsci = {VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN, NULL, 0, (void *)(uintptr_t)0x1000};
    VkSurfaceKHR surf; ck("vkCreateViSurfaceNN", G.CreateViSurfaceNN(inst, &vsci, NULL, &surf));
    VkSurfaceCapabilitiesKHR caps; G.GetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surf, &caps);
    VkBool32 sup = 0; G.GetPhysicalDeviceSurfaceSupportKHR(pd, 0, surf, &sup);
    t("surface: 1280x720 in handheld mode, INHERIT among its transforms, family 0 presents", caps.currentExtent.width == 1280 && caps.currentExtent.height == 720 &&
      (caps.supportedTransforms & VK_SURFACE_TRANSFORM_INHERIT_BIT_KHR) && sup);
    uint32_t nsf = 0; G.GetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &nsf, NULL);
    VkSurfaceFormatKHR sfs[8]; nsf = nsf > 8 ? 8 : nsf; G.GetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &nsf, sfs);
    t("surface formats: BGRA8 first", nsf >= 1 && sfs[0].format == VK_FORMAT_B8G8R8A8_UNORM);
    VkSwapchainCreateInfoKHR scci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, NULL, 0, surf, 3, VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, {SW, SH}, 1,
                                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_SURFACE_TRANSFORM_INHERIT_BIT_KHR,
                                     VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_PRESENT_MODE_FIFO_KHR, VK_TRUE, VK_NULL_HANDLE};
    VkSwapchainKHR swc; ck("vkCreateSwapchainKHR", G.CreateSwapchainKHR(dev, &scci, NULL, &swc));
    uint32_t nsi = 0; G.GetSwapchainImagesKHR(dev, swc, &nsi, NULL);
    VkImage simg[4]; nsi = nsi > 4 ? 4 : nsi; G.GetSwapchainImagesKHR(dev, swc, &nsi, simg);
    t("swapchain: three images", nsi == 3);
    VkRenderPass rps = pass(dev, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    VkImageView sview[4]; VkFramebuffer sfb[4];
    for (uint32_t i = 0; i < nsi; i++) {
        vci.image = simg[i]; vci.format = VK_FORMAT_B8G8R8A8_UNORM; ck("vkCreateImageView", G.CreateImageView(dev, &vci, NULL, &sview[i]));
        VkFramebufferCreateInfo sfci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, rps, 1, &sview[i], SW, SH, 1};
        ck("vkCreateFramebuffer", G.CreateFramebuffer(dev, &sfci, NULL, &sfb[i]));
    }
    VkPipeline ps_ = make_pipe(dev, gpc, vs, fs, pl0, rps, 1, 0);
    VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, NULL, 0};
    VkSemaphore sem_a, sem_r; ck("vkCreateSemaphore", G.CreateSemaphore(dev, &sci, NULL, &sem_a)); ck("vkCreateSemaphore", G.CreateSemaphore(dev, &sci, NULL, &sem_r));
    ck("vkResetFences", G.ResetFences(dev, 1, &fence));
    G.ResetCommandBuffer(cb, 0);
    int presented_ok = 1;
    for (int k = 0; k < 4; k++) {
        static const int crops[4][4] = {{32, 16, 160, 90}, {32, 16, 160, 90}, {32, 16, 160, 90}, {0, 0, 0, 0}};
        h_vi_SetCrop(NULL, crops[k][0], crops[k][1], crops[k][2], crops[k][3]);   // through hle_vi.c, as the game crops its layer (0 x 0: none)
        uint32_t idx = 99; ck("vkAcquireNextImageKHR", G.AcquireNextImageKHR(dev, swc, UINT64_MAX, sem_a, VK_NULL_HANDLE, &idx));
        if (k) { ck("vkWaitForFences", G.WaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX)); ck("vkResetFences", G.ResetFences(dev, 1, &fence)); G.ResetCommandBuffer(cb, 0); }
        presented_ok &= idx == (uint32_t)k % nsi;
        G.BeginCommandBuffer(cb, &cbbi);
        VkClearValue sclear = {{{0, 0.2f * (float)(k + 1), 0, 1}}};
        VkRenderPassBeginInfo srpbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, rps, sfb[idx], {{0, 0}, {SW, SH}}, 1, &sclear};
        G.CmdBeginRenderPass(cb, &srpbi, VK_SUBPASS_CONTENTS_INLINE);
        VkViewport svp = {0, 0, SW, SH, 0, 1}; VkRect2D ssc = {{0, 0}, {SW, SH}};
        G.CmdSetViewport(cb, 0, 1, &svp); G.CmdSetScissor(cb, 0, 1, &ssc); G.CmdBindVertexBuffers(cb, 0, 1, &vbuf, &zero);
        G.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, ps_); G.CmdDraw(cb, 6, 1, 18, 0);
        G.CmdEndRenderPass(cb);
        G.EndCommandBuffer(cb);
        VkPipelineStageFlags wst = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo ssi = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 1, &sem_a, &wst, 1, &cb, 1, &sem_r};
        ck("vkQueueSubmit", G.QueueSubmit(q, 1, &ssi, fence));
        VkResult pres = VK_ERROR_UNKNOWN;
        VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, NULL, 1, &sem_r, 1, &swc, &idx, &pres};
        ck("vkQueuePresentKHR", G.QueuePresentKHR(q, &pi));
        presented_ok &= pres == VK_SUCCESS;
        if (k == 2 || k == 3) {   // what the presenter would show: the crop of frame 2, the whole image of frame 3
            static uint8_t shot[SW * SH * 4]; hvk_present_info info; memset(shot, 0, sizeof shot);
            int got = hvk_present_last(shot, sizeof shot, &info);
            uint32_t cw = k == 2 ? 160 : SW, chh = k == 2 ? 90 : SH, ox = k == 2 ? 32 : 0, oy = k == 2 ? 16 : 0;
            snprintf(name, sizeof name, "present %d: the presenter shows %ux%u at (%u, %u), format %d, after %llu presents", k, info.width, info.height, info.x, info.y, info.format,
                     (unsigned long long)info.frames);
            t(name, got && info.width == cw && info.height == chh && info.x == ox && info.y == oy && info.format == VK_FORMAT_B8G8R8A8_UNORM && info.frames == (uint64_t)k + 1);
            const uint8_t *c0 = shot, *c1 = shot + ((size_t)(30 - oy) * cw + (50 - ox)) * 4;   // (ox, oy): the clear colour; (50, 30): the red quad
            uint8_t g = (uint8_t)(0.2f * (float)(k + 1) * 255.0f + 0.5f);
            snprintf(name, sizeof name, "present %d: clear colour (B G R A %u %u %u %u, G %u) and the red quad (%u %u %u %u)", k, c0[0], c0[1], c0[2], c0[3], g, c1[0], c1[1], c1[2], c1[3]);
            t(name, c0[0] == 0 && near(c0[1], g) && c0[2] == 0 && c0[3] == 255 && c1[0] == 0 && c1[1] == 0 && c1[2] == 255 && c1[3] == 255);
        }
    }
    t("four acquires in turn over the three images, four presents that succeed", presented_ok);

    // ---------- Teardown, through the layer ----------
    ck("vkDeviceWaitIdle", G.DeviceWaitIdle(dev));
    G.DestroySemaphore(dev, sem_a, NULL); G.DestroySemaphore(dev, sem_r, NULL);
    G.DestroyPipeline(dev, ps_, NULL);
    for (uint32_t i = 0; i < nsi; i++) { G.DestroyFramebuffer(dev, sfb[i], NULL); G.DestroyImageView(dev, sview[i], NULL); }
    G.DestroyRenderPass(dev, rps, NULL);
    G.DestroySwapchainKHR(dev, swc, NULL); G.DestroySurfaceKHR(inst, surf, NULL);
    G.DestroyFence(dev, fence, NULL); G.DestroyCommandPool(dev, cp, NULL); G.DestroyQueryPool(dev, occ, NULL); G.DestroyQueryPool(dev, ts, NULL);
    G.DestroyPipeline(dev, pa, NULL); G.DestroyPipeline(dev, pb, NULL); G.DestroyPipeline(dev, pc, NULL); G.DestroyPipelineCache(dev, gpc, NULL);
    G.DestroyFramebuffer(dev, fb, NULL); G.DestroyRenderPass(dev, rp, NULL);
    G.DestroyDescriptorPool(dev, dp, NULL); G.DestroyPipelineLayout(dev, pl0, NULL); G.DestroyPipelineLayout(dev, pl1, NULL); G.DestroyDescriptorSetLayout(dev, dsl, NULL);
    G.DestroyShaderModule(dev, vs, NULL); G.DestroyShaderModule(dev, fs, NULL); G.DestroyShaderModule(dev, fsd, NULL); G.DestroyShaderModule(dev, vsu, NULL);
    G.DestroyImageView(dev, view, NULL); G.DestroyImage(dev, img, NULL); G.DestroyImage(dev, lin, NULL);
    G.DestroyBuffer(dev, vbuf, NULL); G.DestroyBuffer(dev, ubuf, NULL); G.DestroyBuffer(dev, rbuf, NULL);
    G.UnmapMemory(dev, m1); G.UnmapMemory(dev, m2);
    G.FreeMemory(dev, m0, NULL); G.FreeMemory(dev, m1, NULL); G.FreeMemory(dev, m2, NULL);
    G.DestroyDevice(dev, NULL); G.DestroyInstance(inst, NULL);
    rm_dir(cap);
    return hv_finish();
}
