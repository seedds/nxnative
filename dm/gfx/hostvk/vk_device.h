// The Vulkan backend's device layer (Windows and Linux; docs/design/vulkan-backend.md 1.3, 1.5, 3.6 and 9.1): the instance, the physical device that meets the baseline, the
// logical device with its queue, and the capabilities the rest of the backend and the frontend read (be_get_caps will return the be_caps part of hostvk_caps). Plain C11 over
// volk (dm/third_party/volk), which loads the Vulkan loader at run time: nothing links against libvulkan or vulkan-1.dll, and a machine without Vulkan gets a clean error.
// No SDL here: the window's instance extensions come from front_vk_instance_extensions (front.h) when a surface is wanted, and the caller makes the surface itself later
// (front_vk_create_surface). One device per process: volk's global function pointers are loaded for the device hostvk_init makes.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "volk.h"

// What the backend can do on this device. The first group is design 1.5's be_caps, field for field; the rest is the Vulkan backend's own
typedef struct {
    // ---- be_caps (design 1.5) ----
    uint32_t unified_memory : 1;        // an integrated GPU or a CPU device: device memory is system memory (design 4.2: no mirrors)
    uint32_t gpl : 1;                   // VK_EXT_graphics_pipeline_library (with VK_KHR_pipeline_library), its graphicsPipelineLibrary feature: enabled
    uint32_t eds3_blend : 1;            // every blend state of level A is dynamic (HOSTVK_EDS_BLEND below): enabled
    uint32_t eds3_raster : 1;           // every raster and multisample state of level A is dynamic (HOSTVK_EDS_RASTER below): enabled
    uint32_t vertex_input_dynamic : 1;  // VK_EXT_vertex_input_dynamic_state: enabled
    uint32_t cond_render : 1;           // VK_EXT_conditional_rendering: enabled
    uint32_t xfb : 1;                   // VK_EXT_transform_feedback: required (decision D6), so always 1 on a device hostvk_init accepts
    uint32_t geometry : 1, tessellation : 1;   // geometryShader, tessellationShader: baseline, always 1
    uint32_t astc_ldr : 1;              // textureCompressionASTC_LDR: enabled when present
    uint32_t logic_op : 1;              // logicOp: baseline, always 1
    uint32_t max_samples;               // the highest sample count colour and depth-stencil attachments both support
    uint32_t max_point_size;            // pointSizeRange's upper end, truncated
    uint32_t max_vertex_attribs;        // maxVertexInputAttributes
    // ---- The Vulkan backend's own ----
    uint32_t host_coherent_import : 1;  // the memory type host allocations are imported as (hostvk_import) is HOST_COHERENT: a CPU write needs no flush to reach the GPU
    uint32_t timeline : 1;              // timelineSemaphore (core since 1.2, required by 1.3): enabled
    uint32_t buffer_device_address : 1; // bufferDeviceAddress: enabled; imports then get a VkDeviceAddress
    uint64_t min_import_alignment;      // minImportedHostPointerAlignment: an import's address and size are multiples of it (design 4.2)
    uint64_t max_buffer_size;           // maxBufferSize (Vulkan 1.3): the largest import one VkBuffer covers
    uint32_t max_allocations;           // maxMemoryAllocationCount: imports count against it with every other allocation (design 4.2)
    uint32_t import_type_bits;          // the memory types a page of host memory can be imported as (vkGetMemoryHostPointerPropertiesEXT of a probe page at hostvk_init)
    uint32_t import_type;               // the one hostvk_import prefers among them (HOST_VISIBLE and HOST_COHERENT first)
    char level;                         // 'A' to 'D', design 3.6 (rules in hostvk_init below)
} hostvk_caps;

// The optional extensions of design 9.1's second list (Vulkan 1.4 and ASTC are reported beside them). A bit of hostvk_dev.opt_present per entry
enum {
    HOSTVK_OPT_GPL, HOSTVK_OPT_EDS3, HOSTVK_OPT_VERTEX_INPUT, HOSTVK_OPT_DESCRIPTOR_BUFFER, HOSTVK_OPT_PRESENT_ID, HOSTVK_OPT_PRESENT_WAIT, HOSTVK_OPT_COND_RENDER,
    HOSTVK_OPT_DEPTH_CLIP_CONTROL, HOSTVK_OPT_FEEDBACK_LOOP, HOSTVK_OPT_CUSTOM_BORDER, HOSTVK_OPT_PROVOKING_VERTEX, HOSTVK_OPT_CONSERVATIVE, HOSTVK_OPT_SAMPLE_LOCATIONS,
    HOSTVK_OPT_BLEND_ADVANCED, HOSTVK_OPT_DEPTH_BIAS_CONTROL, HOSTVK_OPT_MEMORY_BUDGET, HOSTVK_OPT_CALIBRATED_TIMESTAMPS, HOSTVK_OPT_HOST_IMAGE_COPY, HOSTVK_OPT_PIPELINE_BINARY,
    HOSTVK_OPT_SHADER_OBJECT, HOSTVK_OPT_N
};
extern const char *const hostvk_opt_names[HOSTVK_OPT_N];   // the extension names, in that order

// The dynamic states level A needs beyond Vulkan 1.3's core (extended dynamic state 1 and the base of 2): the blend group and the raster and multisample group, NVN's state as the
// pipeline description holds it (dm/gfx/nvn/nvn_pipe.h). A bit of hostvk_dev.eds_present per entry; hostvk_eds_names has the feature names
enum {
    // blend (be_caps.eds3_blend)
    HOSTVK_EDS_LOGIC_OP,           // extendedDynamicState2LogicOp (VK_EXT_extended_dynamic_state2): color_st's logic operation
    HOSTVK_EDS_LOGIC_OP_ENABLE,    // extendedDynamicState3LogicOpEnable
    HOSTVK_EDS_BLEND_ENABLE,       // extendedDynamicState3ColorBlendEnable: blend_en
    HOSTVK_EDS_BLEND_EQUATION,     // extendedDynamicState3ColorBlendEquation: blend[]
    HOSTVK_EDS_WRITE_MASK,         // extendedDynamicState3ColorWriteMask: chmask
    // raster and multisample (be_caps.eds3_raster)
    HOSTVK_EDS_PATCH_POINTS,       // extendedDynamicState2PatchControlPoints: the patch size
    HOSTVK_EDS_TESS_ORIGIN,        // extendedDynamicState3TessellationDomainOrigin
    HOSTVK_EDS_DEPTH_CLAMP,        // extendedDynamicState3DepthClampEnable: depth_clamp
    HOSTVK_EDS_POLYGON_MODE,       // extendedDynamicState3PolygonMode: poly_mode
    HOSTVK_EDS_SAMPLES,            // extendedDynamicState3RasterizationSamples: samples
    HOSTVK_EDS_SAMPLE_MASK,        // extendedDynamicState3SampleMask
    HOSTVK_EDS_ALPHA_TO_COVERAGE,  // extendedDynamicState3AlphaToCoverageEnable: a2c
    HOSTVK_EDS_N
};
#define HOSTVK_EDS_BLEND  (((1u << HOSTVK_EDS_PATCH_POINTS) - 1))
#define HOSTVK_EDS_RASTER (((1u << HOSTVK_EDS_N) - 1) & ~HOSTVK_EDS_BLEND)
extern const char *const hostvk_eds_names[HOSTVK_EDS_N];

typedef struct {
    int device_index;   // the physical device at this index of vkEnumeratePhysicalDevices; -1: the first one that meets the baseline
    int validation;     // 1: VK_LAYER_KHRONOS_validation with synchronization validation, and VK_EXT_debug_utils with a messenger that counts and logs what it reports
    int want_surface;   // 1: the instance extensions front_vk_instance_extensions asks for, and VK_KHR_swapchain in the baseline
} hostvk_opts;

// hostvk_init's results: 0, or one of these (err says what in words)
enum {
    HOSTVK_E_NO_VULKAN = -1,   // no Vulkan loader (volkInitialize failed: no libvulkan.so.1 / vulkan-1.dll), or a loader older than 1.3
    HOSTVK_E_NO_DEVICE = -2,   // the loader finds no driver (vkCreateInstance: VK_ERROR_INCOMPATIBLE_DRIVER) or lists no physical device
    HOSTVK_E_BASELINE = -3,    // no device (or not the one asked for) meets the baseline of design 9.1
    HOSTVK_E_INDEX = -4,       // device_index is not a device's index
    HOSTVK_E_VULKAN = -5,      // a Vulkan call failed (instance, device, the import probe)
};

typedef struct hostvk_dev {
    VkInstance instance;
    VkDebugUtilsMessengerEXT messenger;   // VK_NULL_HANDLE without validation
    VkPhysicalDevice phys;
    VkDevice device;
    VkQueue queue;                        // the one queue: graphics, compute and transfer
    uint32_t queue_family;
    uint32_t transfer_family;             // a transfer-only family (a copy engine) for uploads later (design 4.3), UINT32_MAX when the device has none; no queue is made from it yet
    uint32_t device_index;                // its index in vkEnumeratePhysicalDevices
    uint32_t api_version;                 // the version the device is used at: the lower of the device's and the instance's
    uint32_t instance_version;            // the loader's (volkGetInstanceVersion)
    int validation;                       // the validation layer is on (asked for and installed)
    int swapchain;                        // VK_KHR_swapchain is enabled (want_surface)
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceMemoryProperties mem;
    char driver_name[VK_MAX_DRIVER_NAME_SIZE], driver_info[VK_MAX_DRIVER_INFO_SIZE];   // VkPhysicalDeviceDriverProperties
    hostvk_caps caps;
    uint32_t opt_present;                 // bit HOSTVK_OPT_*: the device has that optional extension (enabled only where caps says so)
    uint32_t eds_present;                 // bit HOSTVK_EDS_*: that state can be dynamic on this device
    int vk14, astc_ldr;                   // Vulkan 1.4 (device and loader), textureCompressionASTC_LDR
    char missing[1024];                   // what of design 9.1's optional list the device lacks, comma-separated, as the level line prints it ("" when nothing)
    char eds_missing[512];                // the dynamic states of level A it lacks, comma-separated
    _Atomic uint32_t allocations;         // device memory allocations made through hostvk and live (imports included), against caps.max_allocations
    _Atomic uint32_t val_errors, val_warnings;   // validation-layer messages since hostvk_init; hostvk_destroy keeps them (the tests read them after it)
} hostvk_dev;

// Make the instance, choose the physical device, check the baseline, make the logical device and fill d. Returns 0, or a HOSTVK_E_* code with the reason in err (n bytes)
// and d left with no Vulkan object (nothing to destroy; its validation counters stay readable). Logs one line on success:
//   [hostvk] device '<name>' driver <version> Vulkan <x.y.z> level <A-D>: <the optional extensions of 9.1 the device lacks>
// and, below level A, which dynamic states it lacks. Baseline (design 9.1, decision D6): Vulkan 1.3 on the device and in the loader; VK_EXT_external_memory_host with host
// allocations importable as pool buffers (hostvk_pool_usage); VK_EXT_transform_feedback; VK_KHR_swapchain when a surface is wanted; one queue family with graphics and compute;
// the features geometryShader, tessellationShader, logicOp, depthClamp, depthBiasClamp, fillModeNonSolid, wideLines, largePoints, samplerAnisotropy, textureCompressionBC,
// multiDrawIndirect, drawIndirectFirstInstance, independentBlend, dualSrcBlend, imageCubeArray, sampleRateShading, shaderClipDistance, shaderCullDistance,
// occlusionQueryPrecise, pipelineStatisticsQuery, vertexPipelineStoresAndAtomics, fragmentStoresAndAtomics, shaderStorageImageExtendedFormats, drawIndirectCount,
// timelineSemaphore, dynamicRendering, synchronization2, subgroupSizeControl, transformFeedback.
// Level (design 3.6): A = pipeline libraries, dynamic vertex input and every dynamic state of HOSTVK_EDS_*; B = pipeline libraries without all of those; C = no pipeline
// libraries, but dynamic vertex input or some of HOSTVK_EDS_*; D = none of them.
// Not thread-safe; once per device (volk's function pointers are global)
int hostvk_init(const hostvk_opts *o, hostvk_dev *d, char *err, size_t n);
// Wait for the device to be idle and destroy what hostvk_init made (the caller has released its own objects and imports). d's validation counters stay
void hostvk_destroy(hostvk_dev *d);

// Log lines of the backend: one line without its newline. Default: stderr with the host's "[dm] " prefix (dm/host/dm.h's LOG, which this layer does not include: dm.h needs the
// generated game profile header and declares the whole host). The host may route them elsewhere
void hostvk_set_log(void (*fn)(const char *line));
void hostvk_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
