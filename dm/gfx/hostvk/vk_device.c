// The Vulkan backend's device layer: instance, physical device, baseline, level, logical device (vk_device.h has the rules)
#include "plat/plat.h"   // before front.h: the POSIX functions mingw-w64 lacks (front.h's inline helper calls setenv)
#include "front.h"       // front_vk_instance_extensions
#include "vk_device.h"
#include "vk_import.h"
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__has_include)
#if __has_include("game_profile.h")
#include "game_profile.h"   // DM_GAME_NAME, the application name drivers see (some keep per-application profiles by it)
#endif
#endif
#ifdef DM_GAME_NAME
#define APP_NAME DM_GAME_NAME
#else
#define APP_NAME "nxnative"
#endif

// ---------- Log ----------
static void (*g_log)(const char *line);
void hostvk_set_log(void (*fn)(const char *line)) { g_log = fn; }
void hostvk_log(const char *fmt, ...) {
    char b[4096];
    va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    if (g_log) g_log(b); else fprintf(stderr, "[dm] %s\n", b);
}
static int fail(char *err, size_t n, int code, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static int fail(char *err, size_t n, int code, const char *fmt, ...) {
    if (err && n) { va_list a; va_start(a, fmt); vsnprintf(err, n, fmt, a); va_end(a); }
    return code;
}
static void append(char *s, size_t n, const char *item) {   // a comma-separated list
    size_t k = strlen(s);
    snprintf(s + k, n - k, "%s%s", k ? ", " : "", item);
}

// ---------- Names ----------
const char *const hostvk_opt_names[HOSTVK_OPT_N] = {
    [HOSTVK_OPT_GPL] = VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME,
    [HOSTVK_OPT_EDS3] = VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME,
    [HOSTVK_OPT_VERTEX_INPUT] = VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME,
    [HOSTVK_OPT_DESCRIPTOR_BUFFER] = VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME,
    [HOSTVK_OPT_PRESENT_ID] = VK_KHR_PRESENT_ID_EXTENSION_NAME,
    [HOSTVK_OPT_PRESENT_WAIT] = VK_KHR_PRESENT_WAIT_EXTENSION_NAME,
    [HOSTVK_OPT_COND_RENDER] = VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME,
    [HOSTVK_OPT_DEPTH_CLIP_CONTROL] = VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME,
    [HOSTVK_OPT_FEEDBACK_LOOP] = VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME,
    [HOSTVK_OPT_CUSTOM_BORDER] = VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME,
    [HOSTVK_OPT_PROVOKING_VERTEX] = VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME,
    [HOSTVK_OPT_CONSERVATIVE] = VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME,
    [HOSTVK_OPT_SAMPLE_LOCATIONS] = VK_EXT_SAMPLE_LOCATIONS_EXTENSION_NAME,
    [HOSTVK_OPT_BLEND_ADVANCED] = VK_EXT_BLEND_OPERATION_ADVANCED_EXTENSION_NAME,
    [HOSTVK_OPT_DEPTH_BIAS_CONTROL] = VK_EXT_DEPTH_BIAS_CONTROL_EXTENSION_NAME,
    [HOSTVK_OPT_MEMORY_BUDGET] = VK_EXT_MEMORY_BUDGET_EXTENSION_NAME,
    [HOSTVK_OPT_CALIBRATED_TIMESTAMPS] = VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME,   // or VK_EXT_calibrated_timestamps, the same functions before the promotion
    [HOSTVK_OPT_HOST_IMAGE_COPY] = VK_EXT_HOST_IMAGE_COPY_EXTENSION_NAME,
    [HOSTVK_OPT_PIPELINE_BINARY] = VK_KHR_PIPELINE_BINARY_EXTENSION_NAME,
    [HOSTVK_OPT_SHADER_OBJECT] = VK_EXT_SHADER_OBJECT_EXTENSION_NAME,
};
const char *const hostvk_eds_names[HOSTVK_EDS_N] = {
    [HOSTVK_EDS_LOGIC_OP] = "extendedDynamicState2LogicOp",
    [HOSTVK_EDS_LOGIC_OP_ENABLE] = "extendedDynamicState3LogicOpEnable",
    [HOSTVK_EDS_BLEND_ENABLE] = "extendedDynamicState3ColorBlendEnable",
    [HOSTVK_EDS_BLEND_EQUATION] = "extendedDynamicState3ColorBlendEquation",
    [HOSTVK_EDS_WRITE_MASK] = "extendedDynamicState3ColorWriteMask",
    [HOSTVK_EDS_PATCH_POINTS] = "extendedDynamicState2PatchControlPoints",
    [HOSTVK_EDS_TESS_ORIGIN] = "extendedDynamicState3TessellationDomainOrigin",
    [HOSTVK_EDS_DEPTH_CLAMP] = "extendedDynamicState3DepthClampEnable",
    [HOSTVK_EDS_POLYGON_MODE] = "extendedDynamicState3PolygonMode",
    [HOSTVK_EDS_SAMPLES] = "extendedDynamicState3RasterizationSamples",
    [HOSTVK_EDS_SAMPLE_MASK] = "extendedDynamicState3SampleMask",
    [HOSTVK_EDS_ALPHA_TO_COVERAGE] = "extendedDynamicState3AlphaToCoverageEnable",
};

// The core features of the baseline (design 9.1)
#define CORE(name) { #name, offsetof(VkPhysicalDeviceFeatures, name) }
static const struct { const char *name; size_t off; } core_req[] = {
    CORE(geometryShader), CORE(tessellationShader), CORE(logicOp), CORE(depthClamp), CORE(depthBiasClamp), CORE(fillModeNonSolid), CORE(wideLines), CORE(largePoints),
    CORE(samplerAnisotropy), CORE(textureCompressionBC), CORE(multiDrawIndirect), CORE(drawIndirectFirstInstance), CORE(independentBlend), CORE(dualSrcBlend),
    CORE(imageCubeArray), CORE(sampleRateShading), CORE(shaderClipDistance), CORE(shaderCullDistance), CORE(occlusionQueryPrecise), CORE(pipelineStatisticsQuery),
    CORE(vertexPipelineStoresAndAtomics), CORE(fragmentStoresAndAtomics), CORE(shaderStorageImageExtendedFormats),
};
#define CORE_BIT(f, i) (*(const VkBool32 *)((const char *)(f) + core_req[i].off))

// ---------- What one physical device offers ----------
typedef struct {
    VkExtensionProperties *ext; uint32_t n_ext;
    int host_mem, xfb, swapchain, gpl, eds2, eds3, vid, cr, calibrated_ext;   // the extension is listed (gpl: with VK_KHR_pipeline_library)
    VkPhysicalDeviceFeatures2 f;
    VkPhysicalDeviceVulkan12Features f12;
    VkPhysicalDeviceVulkan13Features f13;
    VkPhysicalDeviceTransformFeedbackFeaturesEXT fxfb;
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT fgpl;
    VkPhysicalDeviceExtendedDynamicState2FeaturesEXT feds2;
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT feds3;
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT fvid;
    VkPhysicalDeviceConditionalRenderingFeaturesEXT fcr;
    VkPhysicalDeviceProperties2 p;
    VkPhysicalDeviceVulkan12Properties p12;
    VkPhysicalDeviceVulkan13Properties p13;
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT phost;
} devq;

static int has_ext(const devq *q, const char *name) {
    for (uint32_t i = 0; i < q->n_ext; i++) if (!strcmp(q->ext[i].extensionName, name)) return 1;
    return 0;
}
static void devq_free(devq *q) { free(q->ext); q->ext = NULL; q->n_ext = 0; }
// Fill q for p: extensions, then (for a Vulkan 1.3 device) the features and properties this layer reads, chaining only the structures of extensions the device lists
static void devq_query(VkPhysicalDevice p, devq *q) {
    memset(q, 0, sizeof *q);
    vkEnumerateDeviceExtensionProperties(p, NULL, &q->n_ext, NULL);
    q->ext = calloc(q->n_ext ? q->n_ext : 1, sizeof *q->ext);
    if (q->ext && vkEnumerateDeviceExtensionProperties(p, NULL, &q->n_ext, q->ext) < 0) q->n_ext = 0;
    if (!q->ext) q->n_ext = 0;
    q->host_mem = has_ext(q, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    q->xfb = has_ext(q, VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME);
    q->swapchain = has_ext(q, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    q->gpl = has_ext(q, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME) && has_ext(q, VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME);
    q->eds2 = has_ext(q, VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME);
    q->eds3 = has_ext(q, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
    q->vid = has_ext(q, VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME);
    q->cr = has_ext(q, VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME);
    q->calibrated_ext = has_ext(q, VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
    vkGetPhysicalDeviceProperties(p, &q->p.properties);
    if (q->p.properties.apiVersion < VK_API_VERSION_1_3) return;   // below the baseline: the 1.2 / 1.3 structures may not be chained
    void **next = &q->f.pNext;
    q->f.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    q->f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES; *next = &q->f12; next = &q->f12.pNext;
    q->f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES; *next = &q->f13; next = &q->f13.pNext;
    if (q->xfb) { q->fxfb.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT; *next = &q->fxfb; next = &q->fxfb.pNext; }
    if (q->gpl) { q->fgpl.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT; *next = &q->fgpl; next = &q->fgpl.pNext; }
    if (q->eds2) { q->feds2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT; *next = &q->feds2; next = &q->feds2.pNext; }
    if (q->eds3) { q->feds3.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT; *next = &q->feds3; next = &q->feds3.pNext; }
    if (q->vid) { q->fvid.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT; *next = &q->fvid; next = &q->fvid.pNext; }
    if (q->cr) { q->fcr.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT; *next = &q->fcr; next = &q->fcr.pNext; }
    vkGetPhysicalDeviceFeatures2(p, &q->f);
    next = &q->p.pNext;
    q->p.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    q->p12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES; *next = &q->p12; next = &q->p12.pNext;
    q->p13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES; *next = &q->p13; next = &q->p13.pNext;
    if (q->host_mem) { q->phost.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT; *next = &q->phost; next = &q->phost.pNext; }
    vkGetPhysicalDeviceProperties2(p, &q->p);
}

static uint32_t eds_bits(const devq *q) {
    uint32_t b = 0;
#define EDS(bit, cond) if (cond) b |= 1u << (bit)
    if (q->eds2) {
        EDS(HOSTVK_EDS_LOGIC_OP, q->feds2.extendedDynamicState2LogicOp);
        EDS(HOSTVK_EDS_PATCH_POINTS, q->feds2.extendedDynamicState2PatchControlPoints);
    }
    if (q->eds3) {
        const VkPhysicalDeviceExtendedDynamicState3FeaturesEXT *e = &q->feds3;
        EDS(HOSTVK_EDS_LOGIC_OP_ENABLE, e->extendedDynamicState3LogicOpEnable);
        EDS(HOSTVK_EDS_BLEND_ENABLE, e->extendedDynamicState3ColorBlendEnable);
        EDS(HOSTVK_EDS_BLEND_EQUATION, e->extendedDynamicState3ColorBlendEquation);
        EDS(HOSTVK_EDS_WRITE_MASK, e->extendedDynamicState3ColorWriteMask);
        EDS(HOSTVK_EDS_TESS_ORIGIN, e->extendedDynamicState3TessellationDomainOrigin);
        EDS(HOSTVK_EDS_DEPTH_CLAMP, e->extendedDynamicState3DepthClampEnable);
        EDS(HOSTVK_EDS_POLYGON_MODE, e->extendedDynamicState3PolygonMode);
        EDS(HOSTVK_EDS_SAMPLES, e->extendedDynamicState3RasterizationSamples);
        EDS(HOSTVK_EDS_SAMPLE_MASK, e->extendedDynamicState3SampleMask);
        EDS(HOSTVK_EDS_ALPHA_TO_COVERAGE, e->extendedDynamicState3AlphaToCoverageEnable);
    }
#undef EDS
    return b;
}

// Fill d's description of p (properties, capabilities, optional extensions, level) from q; return 1 when p meets the baseline, else 0 with what it lacks in why
static int assess(VkPhysicalDevice p, const devq *q, const hostvk_opts *o, uint32_t instance_version, hostvk_dev *d, char *why, size_t wn) {
    why[0] = 0;
    d->props = q->p.properties;
    vkGetPhysicalDeviceMemoryProperties(p, &d->mem);
    snprintf(d->driver_name, sizeof d->driver_name, "%s", q->p12.driverName);
    snprintf(d->driver_info, sizeof d->driver_info, "%s", q->p12.driverInfo);
    if (d->props.apiVersion < VK_API_VERSION_1_3) {
        char v[48]; snprintf(v, sizeof v, "Vulkan 1.3 (the device has %u.%u.%u)", VK_API_VERSION_MAJOR(d->props.apiVersion), VK_API_VERSION_MINOR(d->props.apiVersion),
                             VK_API_VERSION_PATCH(d->props.apiVersion));
        append(why, wn, v);
        return 0;
    }
    // queue families
    uint32_t nf = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(p, &nf, NULL);
    VkQueueFamilyProperties *fam = calloc(nf ? nf : 1, sizeof *fam);
    if (fam) vkGetPhysicalDeviceQueueFamilyProperties(p, &nf, fam); else nf = 0;
    d->queue_family = d->transfer_family = UINT32_MAX;
    for (uint32_t i = 0; i < nf; i++) {
        VkQueueFlags fl = fam[i].queueFlags;
        if (d->queue_family == UINT32_MAX && (fl & VK_QUEUE_GRAPHICS_BIT) && (fl & VK_QUEUE_COMPUTE_BIT) && fam[i].queueCount) d->queue_family = i;
        if (d->transfer_family == UINT32_MAX && (fl & VK_QUEUE_TRANSFER_BIT) && !(fl & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) && fam[i].queueCount) d->transfer_family = i;
    }
    free(fam);
    if (d->queue_family == UINT32_MAX) append(why, wn, "a queue family with graphics and compute");
    // extensions
    if (!q->host_mem) append(why, wn, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    if (!q->xfb) append(why, wn, VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME);
    if (o->want_surface && !q->swapchain) append(why, wn, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    // features
    for (size_t i = 0; i < sizeof core_req / sizeof core_req[0]; i++) if (!CORE_BIT(&q->f.features, i)) append(why, wn, core_req[i].name);
    if (!q->f12.drawIndirectCount) append(why, wn, "drawIndirectCount");
    if (!q->f12.timelineSemaphore) append(why, wn, "timelineSemaphore");
    if (!q->f13.dynamicRendering) append(why, wn, "dynamicRendering");
    if (!q->f13.synchronization2) append(why, wn, "synchronization2");
    if (!q->f13.subgroupSizeControl) append(why, wn, "subgroupSizeControl");
    if (q->xfb && !q->fxfb.transformFeedback) append(why, wn, "transformFeedback");
    // the optional list
    d->opt_present = 0;
    d->missing[0] = 0;
    d->vk14 = d->props.apiVersion >= VK_API_VERSION_1_4 && instance_version >= VK_API_VERSION_1_4;
    if (!d->vk14) {
        char v[96];
        if (d->props.apiVersion >= VK_API_VERSION_1_4)
            snprintf(v, sizeof v, "Vulkan 1.4 (the loader is %u.%u.%u)", VK_API_VERSION_MAJOR(instance_version), VK_API_VERSION_MINOR(instance_version), VK_API_VERSION_PATCH(instance_version));
        else snprintf(v, sizeof v, "Vulkan 1.4");
        append(d->missing, sizeof d->missing, v);
    }
    for (int i = 0; i < HOSTVK_OPT_N; i++) {
        int has = i == HOSTVK_OPT_GPL ? q->gpl : i == HOSTVK_OPT_CALIBRATED_TIMESTAMPS ? has_ext(q, hostvk_opt_names[i]) || q->calibrated_ext : has_ext(q, hostvk_opt_names[i]);
        if (has) d->opt_present |= 1u << i; else append(d->missing, sizeof d->missing, hostvk_opt_names[i]);
    }
    d->astc_ldr = q->f.features.textureCompressionASTC_LDR != 0;
    if (!d->astc_ldr) append(d->missing, sizeof d->missing, "textureCompressionASTC_LDR");
    d->eds_present = eds_bits(q);
    d->eds_missing[0] = 0;
    for (int i = 0; i < HOSTVK_EDS_N; i++) if (!(d->eds_present & (1u << i))) append(d->eds_missing, sizeof d->eds_missing, hostvk_eds_names[i]);
    // capabilities
    hostvk_caps *c = &d->caps;
    memset(c, 0, sizeof *c);
    const VkPhysicalDeviceLimits *l = &d->props.limits;
    c->unified_memory = d->props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU || d->props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
    c->gpl = q->gpl && q->fgpl.graphicsPipelineLibrary;
    c->eds3_blend = (d->eds_present & HOSTVK_EDS_BLEND) == HOSTVK_EDS_BLEND;
    c->eds3_raster = (d->eds_present & HOSTVK_EDS_RASTER) == HOSTVK_EDS_RASTER;
    c->vertex_input_dynamic = q->vid && q->fvid.vertexInputDynamicState;
    c->cond_render = q->cr && q->fcr.conditionalRendering;
    c->xfb = q->xfb && q->fxfb.transformFeedback;
    c->geometry = q->f.features.geometryShader != 0;
    c->tessellation = q->f.features.tessellationShader != 0;
    c->astc_ldr = d->astc_ldr;
    c->logic_op = q->f.features.logicOp != 0;
    VkSampleCountFlags sc = l->framebufferColorSampleCounts & l->framebufferDepthSampleCounts & l->framebufferStencilSampleCounts;
    for (c->max_samples = 64; c->max_samples > 1 && !(sc & c->max_samples); c->max_samples >>= 1) {}
    c->max_point_size = (uint32_t)l->pointSizeRange[1];
    c->max_vertex_attribs = l->maxVertexInputAttributes;
    c->timeline = q->f12.timelineSemaphore != 0;
    c->buffer_device_address = q->f12.bufferDeviceAddress != 0;
    c->min_import_alignment = q->host_mem ? q->phost.minImportedHostPointerAlignment : 0;
    c->max_buffer_size = q->p13.maxBufferSize;
    c->max_allocations = l->maxMemoryAllocationCount;
    int some_dynamic = c->vertex_input_dynamic || d->eds_present != 0;
    c->level = c->gpl ? (c->vertex_input_dynamic && c->eds3_blend && c->eds3_raster ? 'A' : 'B') : some_dynamic ? 'C' : 'D';
    // host allocations importable as pool buffers (design 4.2): the buffer usage the pools need, with the handle type host pointers are imported with
    if (q->host_mem) {
        VkPhysicalDeviceExternalBufferInfo bi = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO, NULL, 0, hostvk_pool_usage(d), VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
        VkExternalBufferProperties bp = {VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
        vkGetPhysicalDeviceExternalBufferProperties(p, &bi, &bp);
        const VkExternalMemoryProperties *m = &bp.externalMemoryProperties;
        if (!(m->externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) || (m->externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT) ||
            !(m->compatibleHandleTypes & VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT))
            append(why, wn, "host allocations importable as pool buffers");
        if (!c->min_import_alignment || (c->min_import_alignment & (c->min_import_alignment - 1))) append(why, wn, "a power-of-two minImportedHostPointerAlignment");
    }
    return why[0] == 0;
}

static void driver_version(const hostvk_dev *d, char *out, size_t n) {
    uint32_t v = d->props.driverVersion;
    if (d->props.vendorID == 0x10DE) snprintf(out, n, "%u.%u.%u.%u", v >> 22, (v >> 14) & 0xff, (v >> 6) & 0xff, v & 0x3f);   // NVIDIA's own packing
#ifdef _WIN32
    else if (d->props.vendorID == 0x8086) snprintf(out, n, "%u.%u", v >> 14, v & 0x3fff);   // Intel's Windows driver: the last two fields of its build number
#endif
    else snprintf(out, n, "%u.%u.%u", VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v));   // Mesa and AMD use VK_MAKE_API_VERSION
}

// ---------- Validation messages ----------
static VKAPI_ATTR VkBool32 VKAPI_CALL on_message(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT type,
                                                 const VkDebugUtilsMessengerCallbackDataEXT *m, void *user) {
    hostvk_dev *d = user;
    const char *id = m && m->pMessageIdName ? m->pMessageIdName : "-";
    const char *text = m && m->pMessage ? m->pMessage : "";
    int err = (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0;
    if (!strcmp(id, "Loader Message")) {   // the loader's own (a driver it could not load): not the validation layer's
        hostvk_log("%s[hostvk] loader: %s", err ? "[RED] " : "", text);
        return VK_FALSE;
    }
    if (err) d->val_errors++; else d->val_warnings++;
    hostvk_log("[RED] [hostvk] validation %s (%s): %s", err ? "error" : "warning", id, text);
    (void)type;
    return VK_FALSE;
}

// ---------- Instance ----------
static int g_volk;   // volkInitialize succeeded once (the loader stays loaded for the process)
static int find_name(const VkExtensionProperties *e, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return 1;
    return 0;
}
static VkExtensionProperties *instance_exts(const char *layer, uint32_t *n) {
    *n = 0;
    if (vkEnumerateInstanceExtensionProperties(layer, n, NULL) < 0) { *n = 0; return NULL; }
    VkExtensionProperties *e = calloc(*n ? *n : 1, sizeof *e);
    if (!e || vkEnumerateInstanceExtensionProperties(layer, n, e) < 0) { free(e); *n = 0; return NULL; }
    return e;
}
static int make_instance(const hostvk_opts *o, hostvk_dev *d, char *err, size_t n) {
    static const char *const layer = "VK_LAYER_KHRONOS_validation";
    d->validation = 0;
    if (o->validation) {
        uint32_t nl = 0;
        vkEnumerateInstanceLayerProperties(&nl, NULL);
        VkLayerProperties *lp = calloc(nl ? nl : 1, sizeof *lp);
        if (lp && vkEnumerateInstanceLayerProperties(&nl, lp) >= 0)
            for (uint32_t i = 0; i < nl; i++) if (!strcmp(lp[i].layerName, layer)) d->validation = 1;
        free(lp);
        if (!d->validation) hostvk_log("[RED] [hostvk] validation was asked for, but %s is not installed: running without it", layer);
    }
    uint32_t ng = 0, nv = 0;
    VkExtensionProperties *ge = instance_exts(NULL, &ng), *ve = d->validation ? instance_exts(layer, &nv) : NULL;
    const char *want[24]; uint32_t nw = 0;
    if (o->want_surface) {
        unsigned k = 16;
        int total = front_vk_instance_extensions(want, &k);
        if (total <= 0) { free(ge); free(ve); return fail(err, n, HOSTVK_E_VULKAN, "a surface was asked for, but the front end has no Vulkan window (front_vk_instance_extensions gave no extension)"); }
        if (total > 16) { free(ge); free(ve); return fail(err, n, HOSTVK_E_VULKAN, "the front end asks for %d instance extensions, more than 16", total); }
        nw = k;
    }
    int dbg = 0, feats = 0;
    if (d->validation) {
        dbg = find_name(ge, ng, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) || find_name(ve, nv, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        feats = find_name(ve, nv, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
        if (dbg) want[nw++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        else hostvk_log("[RED] [hostvk] no %s: validation messages go to the layer's own output and are not counted", VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        if (feats) want[nw++] = VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME;
        else hostvk_log("[RED] [hostvk] no %s: synchronization validation is off", VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
    }
    for (uint32_t i = 0; i < nw; i++)
        if (!find_name(ge, ng, want[i]) && !find_name(ve, nv, want[i])) {
            free(ge); free(ve);
            return fail(err, n, HOSTVK_E_VULKAN, "the instance extension %s is not present", want[i]);
        }
    free(ge); free(ve);
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, APP_NAME, 1, "nxnative", 1,
                             d->instance_version >= VK_API_VERSION_1_4 ? VK_API_VERSION_1_4 : VK_API_VERSION_1_3};
    VkDebugUtilsMessengerCreateInfoEXT mi = {VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT, NULL, 0,
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT, on_message, d};
    static const VkValidationFeatureEnableEXT enables[] = {VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT};
    VkValidationFeaturesEXT vf = {VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT, NULL, 1, enables, 0, NULL};
    const void *chain = NULL;
    if (feats) { vf.pNext = chain; chain = &vf; }
    if (dbg) { mi.pNext = chain; chain = &mi; }   // messages of vkCreateInstance and vkDestroyInstance themselves
    VkInstanceCreateInfo ci = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, chain, 0, &app, d->validation ? 1u : 0u, d->validation ? &layer : NULL, nw, want};
    VkResult r = vkCreateInstance(&ci, NULL, &d->instance);
    if (r == VK_ERROR_INCOMPATIBLE_DRIVER) {   // what the loader answers when it finds no driver at all (a Windows PC or runner with vulkan-1.dll but no Vulkan GPU driver)
        d->instance = VK_NULL_HANDLE;
        return fail(err, n, HOSTVK_E_NO_DEVICE, "no Vulkan driver (vkCreateInstance: VK_ERROR_INCOMPATIBLE_DRIVER)");
    }
    if (r != VK_SUCCESS) { d->instance = VK_NULL_HANDLE; return fail(err, n, HOSTVK_E_VULKAN, "vkCreateInstance failed (VkResult %d)", r); }
    volkLoadInstance(d->instance);
    if (dbg) {
        mi.pNext = NULL;
        r = vkCreateDebugUtilsMessengerEXT(d->instance, &mi, NULL, &d->messenger);
        if (r != VK_SUCCESS) { d->messenger = VK_NULL_HANDLE; return fail(err, n, HOSTVK_E_VULKAN, "vkCreateDebugUtilsMessengerEXT failed (VkResult %d)", r); }
    }
    return 0;
}

// ---------- Logical device ----------
static int make_device(const hostvk_opts *o, const devq *q, hostvk_dev *d, char *err, size_t n) {
    const char *ext[16]; uint32_t ne = 0;
    ext[ne++] = VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME;
    ext[ne++] = VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME;
    d->swapchain = o->want_surface != 0;
    if (d->swapchain) ext[ne++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    // The optional extensions be_caps reports are enabled; the others of 9.1's list are recorded (opt_present) and enabled by the step that first uses them
    if (d->caps.gpl) { ext[ne++] = VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME; ext[ne++] = VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME; }
    uint32_t eds = d->eds_present;
    int eds2 = q->eds2 && (eds & (1u << HOSTVK_EDS_LOGIC_OP | 1u << HOSTVK_EDS_PATCH_POINTS)), eds3 = q->eds3 && (eds & ~(1u << HOSTVK_EDS_LOGIC_OP | 1u << HOSTVK_EDS_PATCH_POINTS));
    if (eds2) ext[ne++] = VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME;
    if (eds3) ext[ne++] = VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME;
    if (d->caps.vertex_input_dynamic) ext[ne++] = VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME;
    if (d->caps.cond_render) ext[ne++] = VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME;
    // Features: the baseline's and the enabled extensions' (not every supported core feature: robustBufferAccess and the like cost performance)
    VkPhysicalDeviceFeatures2 f = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    for (size_t i = 0; i < sizeof core_req / sizeof core_req[0]; i++) *(VkBool32 *)((char *)&f.features + core_req[i].off) = VK_TRUE;
    f.features.textureCompressionASTC_LDR = d->astc_ldr ? VK_TRUE : VK_FALSE;
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.drawIndirectCount = VK_TRUE; f12.timelineSemaphore = VK_TRUE; f12.bufferDeviceAddress = d->caps.buffer_device_address ? VK_TRUE : VK_FALSE;
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE; f13.synchronization2 = VK_TRUE; f13.subgroupSizeControl = VK_TRUE; f13.maintenance4 = q->f13.maintenance4;
    VkPhysicalDeviceTransformFeedbackFeaturesEXT fxfb = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT, NULL, VK_TRUE, q->fxfb.geometryStreams};
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT fgpl = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT, NULL, VK_TRUE};
    VkPhysicalDeviceExtendedDynamicState2FeaturesEXT feds2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT};
    feds2.extendedDynamicState2LogicOp = (eds >> HOSTVK_EDS_LOGIC_OP) & 1;
    feds2.extendedDynamicState2PatchControlPoints = (eds >> HOSTVK_EDS_PATCH_POINTS) & 1;
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT feds3 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
    feds3.extendedDynamicState3LogicOpEnable = (eds >> HOSTVK_EDS_LOGIC_OP_ENABLE) & 1;
    feds3.extendedDynamicState3ColorBlendEnable = (eds >> HOSTVK_EDS_BLEND_ENABLE) & 1;
    feds3.extendedDynamicState3ColorBlendEquation = (eds >> HOSTVK_EDS_BLEND_EQUATION) & 1;
    feds3.extendedDynamicState3ColorWriteMask = (eds >> HOSTVK_EDS_WRITE_MASK) & 1;
    feds3.extendedDynamicState3TessellationDomainOrigin = (eds >> HOSTVK_EDS_TESS_ORIGIN) & 1;
    feds3.extendedDynamicState3DepthClampEnable = (eds >> HOSTVK_EDS_DEPTH_CLAMP) & 1;
    feds3.extendedDynamicState3PolygonMode = (eds >> HOSTVK_EDS_POLYGON_MODE) & 1;
    feds3.extendedDynamicState3RasterizationSamples = (eds >> HOSTVK_EDS_SAMPLES) & 1;
    feds3.extendedDynamicState3SampleMask = (eds >> HOSTVK_EDS_SAMPLE_MASK) & 1;
    feds3.extendedDynamicState3AlphaToCoverageEnable = (eds >> HOSTVK_EDS_ALPHA_TO_COVERAGE) & 1;
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT fvid = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT, NULL, VK_TRUE};
    VkPhysicalDeviceConditionalRenderingFeaturesEXT fcr = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT, NULL, VK_TRUE, VK_FALSE};
    void **next = &f.pNext;
    *next = &f12; next = &f12.pNext;
    *next = &f13; next = &f13.pNext;
    *next = &fxfb; next = &fxfb.pNext;
    if (d->caps.gpl) { *next = &fgpl; next = &fgpl.pNext; }
    if (eds2) { *next = &feds2; next = &feds2.pNext; }
    if (eds3) { *next = &feds3; next = &feds3.pNext; }
    if (d->caps.vertex_input_dynamic) { *next = &fvid; next = &fvid.pNext; }
    if (d->caps.cond_render) { *next = &fcr; next = &fcr.pNext; }
    // One queue for graphics, compute and copies; the transfer-only family (transfer_family) gets a queue when uploads move to it
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, d->queue_family, 1, &prio};
    VkDeviceCreateInfo ci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f, 0, 1, &qi, 0, NULL, ne, ext, NULL};
    VkResult r = vkCreateDevice(d->phys, &ci, NULL, &d->device);
    if (r != VK_SUCCESS) { d->device = VK_NULL_HANDLE; return fail(err, n, HOSTVK_E_VULKAN, "vkCreateDevice failed (VkResult %d)", r); }
    volkLoadDevice(d->device);   // device functions straight from the driver: no loader trampoline per command (decision D9)
    vkGetDeviceQueue(d->device, d->queue_family, 0, &d->queue);
    return 0;
}

static void teardown(hostvk_dev *d) {
    if (d->device) { vkDeviceWaitIdle(d->device); vkDestroyDevice(d->device, NULL); }
    if (d->messenger) vkDestroyDebugUtilsMessengerEXT(d->instance, d->messenger, NULL);
    if (d->instance) vkDestroyInstance(d->instance, NULL);
    d->device = VK_NULL_HANDLE; d->messenger = VK_NULL_HANDLE; d->instance = VK_NULL_HANDLE; d->phys = VK_NULL_HANDLE; d->queue = VK_NULL_HANDLE;
}

int hostvk_init(const hostvk_opts *o, hostvk_dev *d, char *err, size_t n) {
    static const hostvk_opts defaults = {-1, 0, 0};
    if (!o) o = &defaults;
    memset(d, 0, sizeof *d);
    if (err && n) err[0] = 0;
    if (!g_volk) {
#ifdef _WIN32
        const char *loader = "vulkan-1.dll";
#else
        const char *loader = "libvulkan.so.1";
#endif
        if (volkInitialize() != VK_SUCCESS) return fail(err, n, HOSTVK_E_NO_VULKAN, "no Vulkan loader: %s could not be loaded", loader);
        g_volk = 1;
    }
    d->instance_version = volkGetInstanceVersion();
    if (d->instance_version < VK_API_VERSION_1_3)
        return fail(err, n, HOSTVK_E_NO_VULKAN, "the Vulkan loader is %u.%u.%u: 1.3 is needed", VK_API_VERSION_MAJOR(d->instance_version),
                    VK_API_VERSION_MINOR(d->instance_version), VK_API_VERSION_PATCH(d->instance_version));
    int rc = make_instance(o, d, err, n);
    if (rc) { teardown(d); return rc; }
    uint32_t np = 0;
    vkEnumeratePhysicalDevices(d->instance, &np, NULL);
    VkPhysicalDevice *pd = calloc(np ? np : 1, sizeof *pd);
    if (!pd || (np && vkEnumeratePhysicalDevices(d->instance, &np, pd) < 0)) np = 0;
    if (!np) { free(pd); teardown(d); return fail(err, n, HOSTVK_E_NO_DEVICE, "the Vulkan loader lists no physical device"); }
    if (o->device_index >= 0 && (uint32_t)o->device_index >= np) {
        free(pd); teardown(d);
        return fail(err, n, HOSTVK_E_INDEX, "device_index %d is out of range: the loader lists %u device%s", o->device_index, np, np == 1 ? "" : "s");
    }
    char why[1024], first_why[1024] = "";
    devq q = {0};
    int chosen = -1;
    for (uint32_t i = 0; i < np && chosen < 0; i++) {
        if (o->device_index >= 0 && i != (uint32_t)o->device_index) continue;
        devq_query(pd[i], &q);
        int ok = assess(pd[i], &q, o, d->instance_version, d, why, sizeof why);
        if (ok) { chosen = (int)i; d->phys = pd[i]; break; }   // q stays filled for make_device
        devq_free(&q);
        hostvk_log("[RED] [hostvk] device %u '%s' does not meet the baseline (design 9.1): it lacks %s", i, d->props.deviceName, why);
        if (!first_why[0]) snprintf(first_why, sizeof first_why, "device %u '%s' lacks %s", i, d->props.deviceName, why);
    }
    free(pd);
    if (chosen < 0) { teardown(d); return fail(err, n, HOSTVK_E_BASELINE, "no device meets the baseline (design 9.1): %s", first_why); }
    d->device_index = (uint32_t)chosen;
    uint32_t asked = d->instance_version >= VK_API_VERSION_1_4 ? VK_API_VERSION_1_4 : VK_API_VERSION_1_3;   // VkApplicationInfo.apiVersion (make_instance)
    d->api_version = d->props.apiVersion < asked ? d->props.apiVersion : asked;
    rc = make_device(o, &q, d, err, n);
    devq_free(&q);
    if (rc) { teardown(d); return rc; }
    VkResult r = hostvk_import_probe(d);
    if (r != VK_SUCCESS) { teardown(d); return fail(err, n, HOSTVK_E_VULKAN, "the host-memory import probe failed (VkResult %d)", r); }
    char ver[48];
    driver_version(d, ver, sizeof ver);
    uint32_t v = d->props.apiVersion;
    hostvk_log("[hostvk] device '%s' driver %s (%s: %s) Vulkan %u.%u.%u level %c: %s", d->props.deviceName, ver, d->driver_name, d->driver_info,
               VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v), d->caps.level, d->missing[0] ? d->missing : "no optional extension missing");
    if (d->caps.level != 'A')
        hostvk_log("[hostvk] level %c: %s; not dynamic: %s", d->caps.level,
                   d->caps.gpl ? "pipeline libraries" : "no pipeline libraries", d->eds_missing[0] ? d->eds_missing : "-");
    return 0;
}

void hostvk_destroy(hostvk_dev *d) {
    if (d->allocations) hostvk_log("[RED] [hostvk] destroying the device with %u allocations still live", (unsigned)d->allocations);
    teardown(d);
}
