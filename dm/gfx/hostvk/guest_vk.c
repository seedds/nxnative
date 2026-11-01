// The guest Vulkan layer on the Vulkan backend (Windows and Linux; docs/design/vulkan-backend.md 8.1, step P9a): for games that render with Vulkan (Hades, Super Mario Galaxy and
// Super Mario Galaxy 2), a pass-through shim. The game's vk* imports bind to the functions here through the host's import table (hle.h's swhle table, the VK macro below), as they
// bind to dm/gfx/vk/hle_vk.m on Apple, which is this layer's specification of the device's shape and of the game-facing behaviour (its Metal replay, offline shader library and
// vk_shaders.py hashes stay Apple-only). vkGet{Instance,Device}ProcAddr look names up in the same table (a name ending in KHR is also tried without it, for extensions promoted
// to core 1.1); every name the game asks for is logged once.
// Model:
//   · Device. The guest's instance, physical device, device and queue are this layer's own handles over the backend's device (be_native_device: the instance, physical device,
//     device and queue the NVN backend uses, vk_device.c). What the game reads is shaped like the Tegra X1 under NVIDIA's Switch driver as hle_vk.m answers it (Vulkan 1.1, vendor
//     0x10de device 0x92ba, one queue family, three memory types on one heap, its limits and features), so it takes the paths it takes there; each limit is the stricter of that
//     shape and the host device's, each feature is offered only where the host device has it enabled (vk_device.c enables the cheap ones a game's SPIR-V may declare), formats
//     are the host's answers, and timestamps tick at the host's period. Command buffers, pipelines, descriptors, buffers, views, samplers, fences, semaphores, events and queries
//     are the driver's own objects, handed through; allocation callbacks are never passed on (they are guest code).
//   · Memory. The guest sees the Switch's types: 0 device-local, 1 and 2 host-visible and coherent (2 cached). A VkDeviceMemory is this layer's record; the driver allocation is
//     made at its first buffer bind, map or linear-image bind (memory that only backs optimal images never gets one, as in hle_vk.m): type 0 becomes device-local memory, types 1
//     and 2 host-visible coherent memory (device-local too where that heap is large: resizable BAR or unified memory; cached for type 2), mapped once and handed to the game, so
//     its CPU writes are the GPU's data and linear images alias their memory exactly as on the Switch (Super Mario Galaxy fills staging images and one sampled 4x4 image from the
//     CPU: rendering.md, "Linear images alias their memory"). Without the x86-64 engine (the GPU tests) the driver's own mapping is the direct form: the game reads and writes
//     this memory only through vkMapMemory's pointer, never at an address it chose. With it the pointer must lie in the engine's arena below 2^39 (design aot-recompiler.md 5.2):
//     the host-visible types are then arena pages imported with VK_EXT_external_memory_host, as vk_memory.c imports NVN pools (design 4.2), every buffer and linear image made
//     with the external-memory handle type it needs (gmem_import below; a driver that refuses imports for a linear image's format gives that image memory of its own).
//     Optimal images are device-local whatever memory the game binds (as hle_vk.m ignores image bindings): vkBindImageMemory gives each one an allocation of its own (dedicated
//     where the driver prefers it). They count against maxMemoryAllocationCount with the backend's allocations (hostvk_dev.allocations).
//   · Shaders. The game's SPIR-V goes to the driver unchanged, after structural checks (guest_spirv_check) and the two documented fix-ups, as SPIR-V binary rewrites (no
//     SPIRV-Tools at run time): ArrayStride on struct members moved onto the array types (Super Mario Galaxy's run-time SPIR-V; rendering.md, "Vulkan shaders generated at run
//     time"), and the second blend source written at output Location 1 without an Index, which a pipeline that blends with a SRC1 factor gets as Location 0, Index 1 (rendering.md,
//     "Dual-source blending from output location 1": NVIDIA's hardware feeds output 1 to source 1); both key on the decorations, not on the game. DM_VK_CAPTURE=<dir> writes every
//     module the game creates to <dir>/<key>.spv (the game's own words; key: the first 8 bytes of their SHA-256, as on Apple).
//   · Pipelines are the game's own, made through the backend's disk cache (vk_cache.c, design 3.4: each thread's VkPipelineCache, written 10 s after a burst of new pipelines; the
//     presents tick it), so later starts are fast. The game's own pipeline cache objects hold nothing (their initial data is NVIDIA's Switch binary cache).
//   · Present. A swapchain on the VK_NN_vi_surface surface is a set of host images registered with the backend's present path (be_ext_tex, nvn_be_ext.h; guest_vk.h). The game's
//     PRESENT_SRC_KHR (a layout of presentable images, and of an extension the shared device need not enable) becomes HVK_EXT_LAYOUT in its render passes and barriers. The
//     acquire paces on the backend's vsync source when it has one and on a clock of its own otherwise, as hle_vk.m's (frame-rate.md, "Vulkan acquire and vsync event follow the
//     display link"), waits until the image's previous present has stopped reading it, and signals the game's semaphore and fence with an empty submission after it. The present
//     waits for the game's semaphores in a submission that signals this layer's timeline, then hands the image to be_present_ext with that value and the vi layer crop
//     (dm_vi_layer_crop, hle_vi.c; rendering.md, "Swapchain pre-transform and layer crop"); the frame tick (dm_guest_frame_tick) follows. The backend's presenter is headless until
//     phase P4 (vk_backend.c): it copies the shown region, which hvk_present_last reads back.
// Declared with a red line: sparse binding; features the game enables that the shared device does not have (robustBufferAccess is not enabled: it costs NVN games performance);
// vertex formats the host cannot fetch; the game destroying a swapchain image. vkGet*ProcAddr return the host function (PFN_vkVoidFunction); the x86-64 engine's thunk of the
// two entries hands the guest its guest-visible address (dm/host/tools/gen_hle_thunks.py: a function pointer return; dm/host/aot.h dm_guest_fnptr_named), as the loader binds
// a direct import to it.
// Environment: DM_VK_LOG=1 logs object creation; DM_VK_CAPTURE=<dir> (above).
#include "../../host/hle.h"
#include "vk_backend.h"
#include "vk_pso.h"
#include "guest_vk.h"
#include "../nvn/nvn_be_ext.h"   // the backend's present path and its pacing: be_ext_tex, be_present_ext, be_present_wait, be_fps_target, be_vsync_active, be_vsync_wait
#include "../nvn/nvn_hmap.h"
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_vi.h>
#include <stdatomic.h>
#include <stddef.h>
#if defined(_WIN32)
#include <process.h>   // getpid
#else
#include <unistd.h>
#endif

void dm_vi_layer_crop(int c[4]);                         // dm/gfx/hle_vi.c (part of nxhost): x, y, w, h the game last cropped its layer to (all 0 = none)
const void *plat_table_swhle(size_t *bytes);              // dm/host/hle.c (PLAT_TABLE_DEFINE): the swhle entries of every object file

#define VLOG(...) LOG("[vk] " __VA_ARGS__)
#define RED1(...) do { static _Atomic int once_; if (!atomic_exchange(&once_, 1)) LOG("[vk] [RED] " __VA_ARGS__); } while (0)
static int g_log;
#define OLOG(...) do { if (g_log) VLOG(__VA_ARGS__); } while (0)

// VK: one Vulkan function, checked against its PFN_ type and registered in the host's import table under its own name (PLAT_TABLE_ENTRY, plat.h). Its C function h_<name> has
// HLE_LINKAGE (hle.h): external here, so the host-call thunks generated from these entries can call it (dm/host/tools/gen_hle_thunks.py reads VK( and VKB( as entries named
// <name>, C function h_<name>, as it reads hle_nvn.c's NVN()
#define VK(name, ret, ...)                                                                        \
    HLE_LINKAGE ret h_##name(__VA_ARGS__);                                                        \
    static const PFN_##name chk_##name __attribute__((unused)) = h_##name;                        \
    PLAT_TABLE_ENTRY(swhle) static const hle_ent e_##name = {#name, (void *)h_##name};            \
    HLE_LINKAGE ret h_##name(__VA_ARGS__)
// VKB: hle_vk.m's entries that run on the thread's big host stack. Without native execution the execution engine calls host code on the host thread's own stack (hle.h,
// HLE_FORCE_BIG), so it is VK; it marks the same functions as on Apple, those that call deep into the driver
#define VKB VK

static uint32_t umin(uint32_t a, uint32_t b) { return a < b ? a : b; }
static uint32_t umax(uint32_t a, uint32_t b) { return a > b ? a : b; }
static void *zalloc(size_t n) { void *p = calloc(1, n ? n : 1); if (!p) die("[vk] out of memory %zu", n); return p; }

// ---------- SPIR-V: structural checks and the fix-ups (guest_vk.h) ----------
enum {
    SPV_MAGIC = 0x07230203u,
    OP_SOURCE_CONTINUED = 2, OP_SOURCE = 3, OP_SOURCE_EXTENSION = 4, OP_NAME = 5, OP_MEMBER_NAME = 6, OP_STRING = 7, OP_LINE = 8, OP_EXTENSION = 10, OP_EXT_INST_IMPORT = 11,
    OP_MEMORY_MODEL = 14, OP_ENTRY_POINT = 15, OP_EXECUTION_MODE = 16, OP_CAPABILITY = 17, OP_TYPE_ARRAY = 28, OP_TYPE_RUNTIME_ARRAY = 29, OP_TYPE_STRUCT = 30,
    OP_FUNCTION = 54, OP_VARIABLE = 59, OP_DECORATE = 71, OP_MEMBER_DECORATE = 72, OP_DECORATION_GROUP = 73, OP_GROUP_DECORATE = 74, OP_GROUP_MEMBER_DECORATE = 75,
    OP_NO_LINE = 317, OP_MODULE_PROCESSED = 330, OP_EXECUTION_MODE_ID = 331, OP_DECORATE_ID = 332, OP_DECORATE_STRING = 5632, OP_MEMBER_DECORATE_STRING = 5633,
    DEC_ARRAY_STRIDE = 6, DEC_LOCATION = 30, DEC_INDEX = 32, SC_OUTPUT = 3, EM_FRAGMENT = 4, CAP_SHADER = 1,
};
// The section of SPIR-V's logical layout (2.4) an instruction belongs to: 0 capabilities, 1 extensions, 2 extended instruction imports, 3 the memory model, 4 entry points,
// 5 execution modes, 6 debug, 7 annotations, 8 types, constants and global variables, 9 functions; -1 for OpLine / OpNoLine, which may stand among types and in functions
static int spv_section(uint32_t op) {
    switch (op) {
        case OP_CAPABILITY: return 0;
        case OP_EXTENSION: return 1;
        case OP_EXT_INST_IMPORT: return 2;
        case OP_MEMORY_MODEL: return 3;
        case OP_ENTRY_POINT: return 4;
        case OP_EXECUTION_MODE: case OP_EXECUTION_MODE_ID: return 5;
        case OP_SOURCE_CONTINUED: case OP_SOURCE: case OP_SOURCE_EXTENSION: case OP_NAME: case OP_MEMBER_NAME: case OP_STRING: case OP_MODULE_PROCESSED: return 6;
        case OP_DECORATE: case OP_MEMBER_DECORATE: case OP_DECORATION_GROUP: case OP_GROUP_DECORATE: case OP_GROUP_MEMBER_DECORATE: case OP_DECORATE_ID: case OP_DECORATE_STRING:
        case OP_MEMBER_DECORATE_STRING: return 7;
        case OP_LINE: case OP_NO_LINE: return -1;
        case OP_FUNCTION: return 9;
        default: return 8;
    }
}
const char *guest_spirv_check(const uint32_t *w, size_t n) {
    if (!w || n < 5) return "shorter than the SPIR-V header";
    if (w[0] != SPV_MAGIC) return w[0] == 0x03022307u ? "big-endian SPIR-V, which the fix-ups do not read" : "not SPIR-V (its magic number)";
    if ((w[1] & 0xff0000ffu) || (w[1] >> 16 & 0xff) != 1 || (w[1] >> 8 & 0xff) > 6) return "a SPIR-V version other than 1.0 to 1.6";
    uint32_t bound = w[3];
    if (!bound || bound > 0x400000u) return "an id bound of 0 or above 4,194,304";
    if (w[4]) return "a header schema other than 0";
    int sec = 0, mm = 0, ep = 0, shader = 0;
    for (size_t i = 5; i < n;) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
        if (!wc) return "an instruction of word count 0";
        if (wc > n - i) return "an instruction that runs past the end of the module";
        if (sec < 9) {
            int s = spv_section(op);
            if (s >= 0) { if (s < sec) return "instructions out of the logical layout's order"; sec = s; }
        }
        switch (op) {
            case OP_CAPABILITY: if (wc != 2) return "a malformed OpCapability"; shader |= w[i + 1] == CAP_SHADER; break;
            case OP_MEMORY_MODEL: if (wc != 3) return "a malformed OpMemoryModel"; mm++; break;
            case OP_ENTRY_POINT: if (wc < 4 || w[i + 2] >= bound) return "a malformed OpEntryPoint"; ep++; break;
            case OP_DECORATE: if (wc < 3 || w[i + 1] >= bound) return "a malformed OpDecorate"; break;
            case OP_MEMBER_DECORATE: if (wc < 4 || w[i + 1] >= bound) return "a malformed OpMemberDecorate"; break;
            case OP_TYPE_ARRAY: if (wc != 4 || w[i + 1] >= bound || w[i + 2] >= bound) return "a malformed OpTypeArray"; break;
            case OP_TYPE_RUNTIME_ARRAY: if (wc != 3 || w[i + 1] >= bound || w[i + 2] >= bound) return "a malformed OpTypeRuntimeArray"; break;
            case OP_TYPE_STRUCT:
                if (wc < 2) return "a malformed OpTypeStruct";
                for (uint32_t k = 1; k < wc; k++) if (w[i + k] >= bound) return "an OpTypeStruct id at or above the bound";
                break;
            case OP_VARIABLE: if (wc < 4 || w[i + 1] >= bound || w[i + 2] >= bound) return "a malformed OpVariable"; break;
        }
        i += wc;
    }
    if (mm != 1) return mm ? "more than one OpMemoryModel" : "no OpMemoryModel";
    if (!ep) return "no entry point";
    if (!shader) return "no Shader capability";
    return NULL;
}
// What the fix-ups read of each id (indexed by id, below the bound)
typedef struct {
    uint32_t st;          // the word index of its OpTypeStruct (a struct type), 0 otherwise
    uint32_t stride;      // the ArrayStride it has or gets (an array type), 0 for none
    uint32_t loc_at;      // the word index of its OpDecorate Location (a variable), 0 for none
    uint8_t arr, added;   // an array type; its stride comes from a member decoration
    uint8_t out, frag;    // an Output variable; in a fragment entry point's interface
    uint8_t index;        // decorated with Index (1 + its value)
} spv_id;
static uint32_t spv_string_words(const uint32_t *w, uint32_t avail) {   // the words of a literal string (nul-terminated, padded to a word)
    for (uint32_t k = 0; k < avail; k++)
        for (int b = 0; b < 4; b++) if (!(w[k] >> (8 * b) & 0xff)) return k + 1;
    return avail;
}
static spv_id *spv_scan(const uint32_t *w, size_t n) {
    spv_id *id = calloc(w[3], sizeof *id);
    if (!id) return NULL;
    for (size_t i = 5; i < n; i += w[i] >> 16) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
        switch (op) {
            case OP_TYPE_ARRAY: case OP_TYPE_RUNTIME_ARRAY: id[w[i + 1]].arr = 1; break;
            case OP_TYPE_STRUCT: id[w[i + 1]].st = (uint32_t)i; break;
            case OP_VARIABLE: if (w[i + 3] == SC_OUTPUT) id[w[i + 2]].out = 1; break;
            case OP_ENTRY_POINT:
                if (w[i + 1] == EM_FRAGMENT)
                    for (uint32_t k = 3 + spv_string_words(w + i + 3, wc - 3); k < wc; k++) if (w[i + k] < w[3]) id[w[i + k]].frag = 1;
                break;
            case OP_DECORATE:
                if (wc != 4) break;
                if (w[i + 2] == DEC_ARRAY_STRIDE) id[w[i + 1]].stride = w[i + 3];
                else if (w[i + 2] == DEC_LOCATION) id[w[i + 1]].loc_at = (uint32_t)i;
                else if (w[i + 2] == DEC_INDEX) id[w[i + 1]].index = (uint8_t)(1 + (w[i + 3] ? 1 : 0));
                break;
        }
    }
    return id;
}
// The output the dual-source form moves: a fragment output at Location 1 without an Index, when no output has Index 1 already; 0 for none
static uint32_t spv_dual_var(const uint32_t *w, const spv_id *id) {
    uint32_t v = 0;
    for (uint32_t k = 1; k < w[3]; k++) {
        if (!id[k].out) continue;
        if (id[k].index == 2) return 0;
        if (id[k].frag && !id[k].index && id[k].loc_at && w[id[k].loc_at + 3] == 1) v = k;
    }
    return v;
}
int guest_spirv_dual_candidate(const uint32_t *w, size_t n) {
    if (guest_spirv_check(w, n)) return 0;
    spv_id *id = spv_scan(w, n);
    int r = id && spv_dual_var(w, id) != 0;
    free(id);
    return r;
}
uint32_t *guest_spirv_fix(const uint32_t *w, size_t n, unsigned what, size_t *out_n, guest_spirv_report *r) {
    guest_spirv_report rep = {0};
    *out_n = n;
    if (r) *r = rep;
    spv_id *id = spv_scan(w, n);
    if (!id) return NULL;
    uint32_t *added = NULL; size_t nadded = 0, first_drop = 0;   // the array types that get a stride, in the order of their member decorations; where the first one stood
    if (what & GUEST_SPIRV_STRIDES)
        for (size_t i = 5; i < n; i += w[i] >> 16) {
            uint32_t wc = w[i] >> 16;
            if ((w[i] & 0xffff) != OP_MEMBER_DECORATE || wc != 5 || w[i + 3] != DEC_ARRAY_STRIDE) continue;
            if (!rep.strides_dropped++) first_drop = i;
            const spv_id *s = &id[w[i + 1]];
            uint32_t m = w[i + 2], swc = s->st ? w[s->st] >> 16 : 0;
            if (!s->st || 2 + m >= swc) continue;   // not a struct, or no such member: dropped (ArrayStride belongs on an array type)
            uint32_t t = w[s->st + 2 + m];
            if (!id[t].arr) continue;               // a member that is no array: dropped
            if (id[t].stride) { rep.stride_conflicts += id[t].stride != w[i + 4]; continue; }
            id[t].stride = w[i + 4]; id[t].added = 1;
            uint32_t *na = realloc(added, (nadded + 1) * sizeof *na);
            if (!na) { free(added); free(id); return NULL; }
            added = na; added[nadded++] = t; rep.strides_moved++;
        }
    uint32_t dual = what & GUEST_SPIRV_DUAL ? spv_dual_var(w, id) : 0;
    rep.dual_rewritten = dual != 0;
    if (!rep.strides_dropped && !dual) { free(id); if (r) *r = rep; return (uint32_t *)w; }
    size_t m = n - 5 * (size_t)rep.strides_dropped + 4 * nadded + (dual ? 4 : 0);
    uint32_t *o = malloc(m * sizeof *o);
    if (!o) { free(added); free(id); return NULL; }
    memcpy(o, w, 5 * sizeof *o);
    size_t k = 5;
    for (size_t i = 5; i < n; i += w[i] >> 16) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
        if ((what & GUEST_SPIRV_STRIDES) && op == OP_MEMBER_DECORATE && wc == 5 && w[i + 3] == DEC_ARRAY_STRIDE) {
            if (i == first_drop)
                for (size_t a = 0; a < nadded; a++) { o[k++] = 4u << 16 | OP_DECORATE; o[k++] = added[a]; o[k++] = DEC_ARRAY_STRIDE; o[k++] = id[added[a]].stride; }
            continue;
        }
        memcpy(o + k, w + i, wc * sizeof *o);
        if (dual && i == id[dual].loc_at) {   // Location 1 → Location 0, then Index 1
            o[k + 3] = 0;
            o[k + 4] = 4u << 16 | OP_DECORATE; o[k + 5] = dual; o[k + 6] = DEC_INDEX; o[k + 7] = 1;
            k += 4;
        }
        k += wc;
    }
    free(added); free(id);
    *out_n = k;
    if (r) *r = rep;
    return o;
}

// ---------- State ----------
// The guest's dispatchable handles: this layer's own objects (their first word is where a loader keeps its dispatch table). Command buffers are the driver's
static struct { void *loader; } g_inst_o, g_pdev_o, g_dev_o, g_queue_o;
#define G_INST ((VkInstance)(void *)&g_inst_o)
#define G_PDEV ((VkPhysicalDevice)(void *)&g_pdev_o)
#define G_DEV ((VkDevice)(void *)&g_dev_o)
#define G_QUEUE ((VkQueue)(void *)&g_queue_o)
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;        // lazy driver allocations and the device's objects
static pthread_mutex_t g_pres_mu = PTHREAD_MUTEX_INITIALIZER;   // presents in order: one timeline value each
static VkSemaphore g_tl;                                        // this layer's timeline: signalled by each present's submission (be_present_ext waits for it)
static uint64_t g_tl_val;
static int g_bar;                                               // host-visible device-local memory is plentiful (resizable BAR, or unified memory)
static _Atomic uint64_t g_n_draw, g_n_pass, g_n_submit, g_n_pso, g_n_shader, g_mem_bytes;

// A host-visible memory type's heap above 256 MiB, or unified memory (design 4.2, the usual sign of resizable BAR)
static void bar_init(void) {
    const VkPhysicalDeviceMemoryProperties *m = &hvk.dev.mem;
    g_bar = hvk.dev.caps.unified_memory;
    for (uint32_t i = 0; i < m->memoryTypeCount && !g_bar; i++) {
        VkMemoryPropertyFlags f = m->memoryTypes[i].propertyFlags;
        if ((f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) && (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && m->memoryHeaps[m->memoryTypes[i].heapIndex].size > (256ull << 20)) g_bar = 1;
    }
}
// The host memory type for guest type g among bits: type 0 device-local (not host-visible first: the game never maps it); types 1 and 2 host-visible and coherent, device-local
// when g_bar and not otherwise (a small BAR heap is not taken), cached first for type 2. UINT32_MAX for none
static uint32_t pick_type(uint32_t g, uint32_t bits) {
    const VkPhysicalDeviceMemoryProperties *m = &hvk.dev.mem;
    const VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t best = UINT32_MAX; int bs = -1;
    for (uint32_t i = 0; i < m->memoryTypeCount; i++) {
        if (!(bits >> i & 1)) continue;
        VkMemoryPropertyFlags f = m->memoryTypes[i].propertyFlags;
        int dl = (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0, vis = (f & hv) == hv, cached = (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0, s;
        if (g == 0) s = dl ? 2 + !vis : 0;
        else { if (!vis) continue; s = 1 + 2 * (dl == g_bar) + 4 * (g == 2 && cached); }
        if (s > bs) { bs = s; best = i; }
    }
    return best;
}
static uint32_t guest_bits(uint32_t host_bits) { uint32_t b = 0; for (uint32_t g = 0; g < 3; g++) if (pick_type(g, host_bits) != UINT32_MAX) b |= 1u << g; return b; }
// One driver allocation of this layer, counted against maxMemoryAllocationCount with the backend's (dedicated: the image it is made for, when the driver prefers that)
static VkResult host_alloc(VkDeviceSize size, uint32_t type, VkImage dedicated, VkDeviceMemory *out) {
    if (type == UINT32_MAX) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    if (++hvk.dev.allocations > hvk.dev.caps.max_allocations) { hvk.dev.allocations--; RED1("over maxMemoryAllocationCount (%u) allocations: refused", hvk.dev.caps.max_allocations); return VK_ERROR_TOO_MANY_OBJECTS; }
    VkMemoryDedicatedAllocateInfo di = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, NULL, dedicated, VK_NULL_HANDLE};
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, dedicated ? &di : NULL, size ? size : 16, type};
    VkResult r = vkAllocateMemory(hvk.d, &ai, NULL, out);
    if (r) { hvk.dev.allocations--; *out = VK_NULL_HANDLE; }
    else atomic_fetch_add(&g_mem_bytes, ai.allocationSize);
    return r;
}
static void host_free(VkDeviceMemory m, VkDeviceSize size) { if (m) { vkFreeMemory(hvk.d, m, NULL); hvk.dev.allocations--; atomic_fetch_sub(&g_mem_bytes, size ? size : 16); } }

// ---------- Instance and physical device ----------
static VkResult fill_names(const char *const *names, uint32_t n, uint32_t *count, VkExtensionProperties *p) {
    if (!p) { *count = n; return VK_SUCCESS; }
    uint32_t k = umin(*count, n);
    for (uint32_t i = 0; i < k; i++) { memset(&p[i], 0, sizeof p[i]); snprintf(p[i].extensionName, sizeof p[i].extensionName, "%s", names[i]); p[i].specVersion = 1; }
    *count = k; return k < n ? VK_INCOMPLETE : VK_SUCCESS;
}
// hle_vk.m's lists: the ones the shim implements itself (surface, vi surface, swapchain) and Vulkan 1.1's promoted extensions, which every Vulkan 1.3 host has as core functions
static const char *const g_inst_ext[] = {"VK_KHR_surface", "VK_NN_vi_surface", "VK_KHR_get_physical_device_properties2"};
static const char *const g_dev_ext[] = {"VK_KHR_swapchain", "VK_KHR_descriptor_update_template", "VK_KHR_maintenance1", "VK_KHR_get_memory_requirements2",
                                        "VK_KHR_dedicated_allocation", "VK_KHR_bind_memory2"};
static void join_names(char *out, size_t n, const char *const *names, uint32_t count) {
    out[0] = 0; size_t o = 0;
    for (uint32_t i = 0; i < count && o + 2 < n; i++) o += (size_t)snprintf(out + o, n - o, "%s%s", i ? " " : "", names[i]);
    if (!count) snprintf(out, n, "none");
}
VK(vkEnumerateInstanceVersion, VkResult, uint32_t *v) { *v = VK_API_VERSION_1_1; return VK_SUCCESS; }
VK(vkEnumerateInstanceLayerProperties, VkResult, uint32_t *n, VkLayerProperties *p) { (void)p; *n = 0; return VK_SUCCESS; }
VK(vkEnumerateInstanceExtensionProperties, VkResult, const char *layer, uint32_t *n, VkExtensionProperties *p) {
    if (layer) { *n = 0; return VK_ERROR_LAYER_NOT_PRESENT; }
    return fill_names(g_inst_ext, sizeof g_inst_ext / sizeof *g_inst_ext, n, p);
}
VKB(vkCreateInstance, VkResult, const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *a, VkInstance *out) {
    (void)a; g_log = getenv("DM_VK_LOG") != NULL;
    const VkApplicationInfo *ai = ci->pApplicationInfo; char ext[512];
    join_names(ext, sizeof ext, ci->ppEnabledExtensionNames, ci->enabledExtensionCount);
    VLOG("CreateInstance: application %s, engine %s, API %u.%u; extensions %s; %u layers", ai && ai->pApplicationName ? ai->pApplicationName : "-",
         ai && ai->pEngineName ? ai->pEngineName : "-", ai ? VK_API_VERSION_MAJOR(ai->apiVersion) : 0, ai ? VK_API_VERSION_MINOR(ai->apiVersion) : 0, ext, ci->enabledLayerCount);
    const hostvk_dev *d = be_native_device();
    if (!d) { LOG("[vk] [RED] no Vulkan device for the guest Vulkan layer (the backend has none: see its red line): the game cannot render"); return VK_ERROR_INCOMPATIBLE_DRIVER; }
    VLOG("guest Vulkan on the backend's device '%s' (%s), Vulkan %u.%u, level %c: the game's calls go to its driver", d->props.deviceName, d->driver_name,
         VK_API_VERSION_MAJOR(d->props.apiVersion), VK_API_VERSION_MINOR(d->props.apiVersion), d->caps.level);
    *out = G_INST; return VK_SUCCESS;
}
VK(vkDestroyInstance, void, VkInstance i, const VkAllocationCallbacks *a) { (void)i; (void)a; }
VK(vkEnumeratePhysicalDevices, VkResult, VkInstance i, uint32_t *n, VkPhysicalDevice *p) {
    (void)i; if (!p) { *n = 1; return VK_SUCCESS; }
    if (!*n) return VK_INCOMPLETE;
    *n = 1; p[0] = G_PDEV; return VK_SUCCESS;
}
// The Tegra X1 under NVIDIA's Switch driver as hle_vk.m shapes it (its values, which are Metal's where Metal is stricter); 0 leaves a limit to the host
static void tegra_limits(VkPhysicalDeviceLimits *l) {
    memset(l, 0, sizeof *l);
    l->maxImageDimension1D = l->maxImageDimension2D = l->maxImageDimensionCube = 16384; l->maxImageDimension3D = 2048; l->maxImageArrayLayers = 2048;
    l->maxTexelBufferElements = 1u << 27; l->maxUniformBufferRange = 65536; l->maxStorageBufferRange = 1u << 30; l->maxPushConstantsSize = 256;
    l->maxMemoryAllocationCount = 4096; l->maxSamplerAllocationCount = 4000; l->bufferImageGranularity = 1024; l->maxBoundDescriptorSets = 8;
    l->maxPerStageDescriptorSamplers = 16; l->maxPerStageDescriptorUniformBuffers = 15; l->maxPerStageDescriptorStorageBuffers = 16;
    l->maxPerStageDescriptorSampledImages = 31; l->maxPerStageDescriptorStorageImages = 8; l->maxPerStageDescriptorInputAttachments = 8; l->maxPerStageResources = 128;
    l->maxDescriptorSetSamplers = 4000; l->maxDescriptorSetUniformBuffers = 90; l->maxDescriptorSetUniformBuffersDynamic = 15;
    l->maxDescriptorSetStorageBuffers = 96; l->maxDescriptorSetStorageBuffersDynamic = 16; l->maxDescriptorSetSampledImages = 4096;
    l->maxDescriptorSetStorageImages = 4096; l->maxDescriptorSetInputAttachments = 8;
    l->maxVertexInputAttributes = 31; l->maxVertexInputBindings = 14; l->maxVertexInputAttributeOffset = 2047; l->maxVertexInputBindingStride = 2048; l->maxVertexOutputComponents = 128;
    l->maxFragmentInputComponents = 128; l->maxFragmentOutputAttachments = 8; l->maxFragmentDualSrcAttachments = 1; l->maxFragmentCombinedOutputResources = 16;
    l->maxComputeSharedMemorySize = 32768; l->maxComputeWorkGroupCount[0] = l->maxComputeWorkGroupCount[1] = l->maxComputeWorkGroupCount[2] = 65535;
    l->maxComputeWorkGroupInvocations = 1024; l->maxComputeWorkGroupSize[0] = l->maxComputeWorkGroupSize[1] = 1024; l->maxComputeWorkGroupSize[2] = 64;
    l->subPixelPrecisionBits = 8; l->subTexelPrecisionBits = 8; l->mipmapPrecisionBits = 8; l->maxDrawIndexedIndexValue = 0xffffffffu; l->maxDrawIndirectCount = 0xffffffffu;
    l->maxSamplerLodBias = 15; l->maxSamplerAnisotropy = 16; l->maxViewports = 1; l->maxViewportDimensions[0] = l->maxViewportDimensions[1] = 16384;
    l->viewportBoundsRange[0] = -32768; l->viewportBoundsRange[1] = 32767; l->viewportSubPixelBits = 8;
    l->minMemoryMapAlignment = 64; l->minTexelBufferOffsetAlignment = 256; l->minUniformBufferOffsetAlignment = 256; l->minStorageBufferOffsetAlignment = 256;
    l->minTexelOffset = -8; l->maxTexelOffset = 7; l->minTexelGatherOffset = -32; l->maxTexelGatherOffset = 31;
    l->minInterpolationOffset = -0.5f; l->maxInterpolationOffset = 0.4375f; l->subPixelInterpolationOffsetBits = 4;
    l->maxFramebufferWidth = l->maxFramebufferHeight = 16384; l->maxFramebufferLayers = 2048;
    l->framebufferColorSampleCounts = l->framebufferDepthSampleCounts = l->framebufferStencilSampleCounts = l->framebufferNoAttachmentsSampleCounts = 0x7;
    l->maxColorAttachments = 8; l->sampledImageColorSampleCounts = l->sampledImageIntegerSampleCounts = l->sampledImageDepthSampleCounts = l->sampledImageStencilSampleCounts = 0x7;
    l->storageImageSampleCounts = 1; l->maxSampleMaskWords = 1; l->timestampComputeAndGraphics = 1; l->timestampPeriod = 1;
    l->maxClipDistances = 8; l->maxCullDistances = 0; l->maxCombinedClipAndCullDistances = 8; l->discreteQueuePriorities = 2;
    l->pointSizeRange[0] = 1; l->pointSizeRange[1] = 511; l->lineWidthRange[0] = l->lineWidthRange[1] = 1; l->pointSizeGranularity = 0.0625f; l->lineWidthGranularity = 0;
    l->strictLines = 1; l->standardSampleLocations = 1; l->optimalBufferCopyOffsetAlignment = 256; l->optimalBufferCopyRowPitchAlignment = 256; l->nonCoherentAtomSize = 64;
}
// Each limit the stricter of the Tegra shape's and the host's: a maximum the lower (the host's where the shape leaves it 0), an alignment or granularity the larger (a multiple of
// both: they are powers of two), a range the intersection, sample counts both; timestamps tick at the host's period and the queue priorities and line rules are the host's
static void shape_limits(VkPhysicalDeviceLimits *l, const VkPhysicalDeviceLimits *h) {
    VkPhysicalDeviceLimits t; tegra_limits(&t);
#define MAXL(f) (l->f = t.f && t.f < h->f ? t.f : h->f)
#define MINL(f) (l->f = t.f > h->f ? t.f : h->f)
    MAXL(maxImageDimension1D); MAXL(maxImageDimension2D); MAXL(maxImageDimension3D); MAXL(maxImageDimensionCube); MAXL(maxImageArrayLayers); MAXL(maxTexelBufferElements);
    MAXL(maxUniformBufferRange); MAXL(maxStorageBufferRange); MAXL(maxPushConstantsSize); MAXL(maxMemoryAllocationCount); MAXL(maxSamplerAllocationCount);
    MINL(bufferImageGranularity); l->sparseAddressSpaceSize = h->sparseAddressSpaceSize; MAXL(maxBoundDescriptorSets);
    MAXL(maxPerStageDescriptorSamplers); MAXL(maxPerStageDescriptorUniformBuffers); MAXL(maxPerStageDescriptorStorageBuffers); MAXL(maxPerStageDescriptorSampledImages);
    MAXL(maxPerStageDescriptorStorageImages); MAXL(maxPerStageDescriptorInputAttachments); MAXL(maxPerStageResources); MAXL(maxDescriptorSetSamplers);
    MAXL(maxDescriptorSetUniformBuffers); MAXL(maxDescriptorSetUniformBuffersDynamic); MAXL(maxDescriptorSetStorageBuffers); MAXL(maxDescriptorSetStorageBuffersDynamic);
    MAXL(maxDescriptorSetSampledImages); MAXL(maxDescriptorSetStorageImages); MAXL(maxDescriptorSetInputAttachments);
    MAXL(maxVertexInputAttributes); MAXL(maxVertexInputBindings); MAXL(maxVertexInputAttributeOffset); MAXL(maxVertexInputBindingStride); MAXL(maxVertexOutputComponents);
    MAXL(maxTessellationGenerationLevel); MAXL(maxTessellationPatchSize); MAXL(maxTessellationControlPerVertexInputComponents);
    MAXL(maxTessellationControlPerVertexOutputComponents); MAXL(maxTessellationControlPerPatchOutputComponents); MAXL(maxTessellationControlTotalOutputComponents);
    MAXL(maxTessellationEvaluationInputComponents); MAXL(maxTessellationEvaluationOutputComponents); MAXL(maxGeometryShaderInvocations); MAXL(maxGeometryInputComponents);
    MAXL(maxGeometryOutputComponents); MAXL(maxGeometryOutputVertices); MAXL(maxGeometryTotalOutputComponents);
    MAXL(maxFragmentInputComponents); MAXL(maxFragmentOutputAttachments); MAXL(maxFragmentDualSrcAttachments); MAXL(maxFragmentCombinedOutputResources);
    MAXL(maxComputeSharedMemorySize); MAXL(maxComputeWorkGroupInvocations);
    for (int i = 0; i < 3; i++) { MAXL(maxComputeWorkGroupCount[i]); MAXL(maxComputeWorkGroupSize[i]); }
    MAXL(subPixelPrecisionBits); MAXL(subTexelPrecisionBits); MAXL(mipmapPrecisionBits); MAXL(maxDrawIndexedIndexValue); MAXL(maxDrawIndirectCount);
    MAXL(maxSamplerLodBias); MAXL(maxSamplerAnisotropy); MAXL(maxViewports); MAXL(maxViewportDimensions[0]); MAXL(maxViewportDimensions[1]);
    l->viewportBoundsRange[0] = t.viewportBoundsRange[0] > h->viewportBoundsRange[0] ? t.viewportBoundsRange[0] : h->viewportBoundsRange[0];
    l->viewportBoundsRange[1] = t.viewportBoundsRange[1] < h->viewportBoundsRange[1] ? t.viewportBoundsRange[1] : h->viewportBoundsRange[1];
    MAXL(viewportSubPixelBits); MINL(minMemoryMapAlignment); MINL(minTexelBufferOffsetAlignment); MINL(minUniformBufferOffsetAlignment); MINL(minStorageBufferOffsetAlignment);
    l->minTexelOffset = t.minTexelOffset > h->minTexelOffset ? t.minTexelOffset : h->minTexelOffset; MAXL(maxTexelOffset);
    l->minTexelGatherOffset = t.minTexelGatherOffset > h->minTexelGatherOffset ? t.minTexelGatherOffset : h->minTexelGatherOffset; MAXL(maxTexelGatherOffset);
    l->minInterpolationOffset = t.minInterpolationOffset > h->minInterpolationOffset ? t.minInterpolationOffset : h->minInterpolationOffset; MAXL(maxInterpolationOffset);
    MAXL(subPixelInterpolationOffsetBits); MAXL(maxFramebufferWidth); MAXL(maxFramebufferHeight); MAXL(maxFramebufferLayers);
    l->framebufferColorSampleCounts = t.framebufferColorSampleCounts & h->framebufferColorSampleCounts;
    l->framebufferDepthSampleCounts = t.framebufferDepthSampleCounts & h->framebufferDepthSampleCounts;
    l->framebufferStencilSampleCounts = t.framebufferStencilSampleCounts & h->framebufferStencilSampleCounts;
    l->framebufferNoAttachmentsSampleCounts = t.framebufferNoAttachmentsSampleCounts & h->framebufferNoAttachmentsSampleCounts;
    MAXL(maxColorAttachments);
    l->sampledImageColorSampleCounts = t.sampledImageColorSampleCounts & h->sampledImageColorSampleCounts;
    l->sampledImageIntegerSampleCounts = t.sampledImageIntegerSampleCounts & h->sampledImageIntegerSampleCounts;
    l->sampledImageDepthSampleCounts = t.sampledImageDepthSampleCounts & h->sampledImageDepthSampleCounts;
    l->sampledImageStencilSampleCounts = t.sampledImageStencilSampleCounts & h->sampledImageStencilSampleCounts;
    l->storageImageSampleCounts = t.storageImageSampleCounts & h->storageImageSampleCounts;
    MAXL(maxSampleMaskWords); l->timestampComputeAndGraphics = t.timestampComputeAndGraphics && h->timestampComputeAndGraphics; l->timestampPeriod = h->timestampPeriod;
    MAXL(maxClipDistances); MAXL(maxCullDistances); MAXL(maxCombinedClipAndCullDistances); l->discreteQueuePriorities = h->discreteQueuePriorities;
    l->pointSizeRange[0] = t.pointSizeRange[0] > h->pointSizeRange[0] ? t.pointSizeRange[0] : h->pointSizeRange[0];
    l->pointSizeRange[1] = t.pointSizeRange[1] < h->pointSizeRange[1] ? t.pointSizeRange[1] : h->pointSizeRange[1];
    l->lineWidthRange[0] = t.lineWidthRange[0] > h->lineWidthRange[0] ? t.lineWidthRange[0] : h->lineWidthRange[0];
    l->lineWidthRange[1] = t.lineWidthRange[1] < h->lineWidthRange[1] ? t.lineWidthRange[1] : h->lineWidthRange[1];
    MINL(pointSizeGranularity); MINL(lineWidthGranularity); l->strictLines = h->strictLines; l->standardSampleLocations = t.standardSampleLocations && h->standardSampleLocations;
    MINL(optimalBufferCopyOffsetAlignment); MINL(optimalBufferCopyRowPitchAlignment); MINL(nonCoherentAtomSize);
#undef MAXL
#undef MINL
}
static void pdev_props(VkPhysicalDeviceProperties *p) {
    memset(p, 0, sizeof *p);
    p->apiVersion = VK_API_VERSION_1_1; p->driverVersion = VK_MAKE_API_VERSION(0, 1, 0, 0); p->vendorID = 0x10de; p->deviceID = 0x92ba;
    p->deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU; snprintf(p->deviceName, sizeof p->deviceName, "NVIDIA Tegra X1 (nvgpu)");
    shape_limits(&p->limits, &hvk.dev.props.limits);
}
// The features of the Tegra shape: hle_vk.m's, and geometryShader, which Metal lacks and the Switch has (Super Mario Galaxy's GX point and line geometry shaders run there)
static const VkPhysicalDeviceFeatures g_tegra_feats = {
    .robustBufferAccess = 1, .fullDrawIndexUint32 = 1, .imageCubeArray = 1, .independentBlend = 1, .geometryShader = 1, .sampleRateShading = 1, .dualSrcBlend = 1,
    .drawIndirectFirstInstance = 1, .depthClamp = 1, .depthBiasClamp = 1, .fillModeNonSolid = 1, .largePoints = 1, .samplerAnisotropy = 1, .textureCompressionBC = 1,
    .textureCompressionASTC_LDR = 1, .occlusionQueryPrecise = 1, .fragmentStoresAndAtomics = 1, .shaderImageGatherExtended = 1, .shaderClipDistance = 1, .shaderResourceMinLod = 1,
};
#define FEAT(name) {#name, offsetof(VkPhysicalDeviceFeatures, name)}
static const struct { const char *name; size_t off; } g_feat_names[] = {
    FEAT(robustBufferAccess), FEAT(fullDrawIndexUint32), FEAT(imageCubeArray), FEAT(independentBlend), FEAT(geometryShader), FEAT(tessellationShader), FEAT(sampleRateShading),
    FEAT(dualSrcBlend), FEAT(logicOp), FEAT(multiDrawIndirect), FEAT(drawIndirectFirstInstance), FEAT(depthClamp), FEAT(depthBiasClamp), FEAT(fillModeNonSolid), FEAT(depthBounds),
    FEAT(wideLines), FEAT(largePoints), FEAT(alphaToOne), FEAT(multiViewport), FEAT(samplerAnisotropy), FEAT(textureCompressionETC2), FEAT(textureCompressionASTC_LDR),
    FEAT(textureCompressionBC), FEAT(occlusionQueryPrecise), FEAT(pipelineStatisticsQuery), FEAT(vertexPipelineStoresAndAtomics), FEAT(fragmentStoresAndAtomics),
    FEAT(shaderTessellationAndGeometryPointSize), FEAT(shaderImageGatherExtended), FEAT(shaderStorageImageExtendedFormats), FEAT(shaderStorageImageMultisample),
    FEAT(shaderStorageImageReadWithoutFormat), FEAT(shaderStorageImageWriteWithoutFormat), FEAT(shaderUniformBufferArrayDynamicIndexing),
    FEAT(shaderSampledImageArrayDynamicIndexing), FEAT(shaderStorageBufferArrayDynamicIndexing), FEAT(shaderStorageImageArrayDynamicIndexing), FEAT(shaderClipDistance),
    FEAT(shaderCullDistance), FEAT(shaderFloat64), FEAT(shaderInt64), FEAT(shaderInt16), FEAT(shaderResourceResidency), FEAT(shaderResourceMinLod), FEAT(sparseBinding),
    FEAT(sparseResidencyBuffer), FEAT(sparseResidencyImage2D), FEAT(sparseResidencyImage3D), FEAT(sparseResidency2Samples), FEAT(sparseResidency4Samples),
    FEAT(sparseResidency8Samples), FEAT(sparseResidency16Samples), FEAT(sparseResidencyAliased), FEAT(variableMultisampleRate), FEAT(inheritedQueries),
};
#undef FEAT
#define FEAT_BIT(f, i) (*(const VkBool32 *)((const char *)(f) + g_feat_names[i].off))
_Static_assert(sizeof g_feat_names / sizeof g_feat_names[0] == sizeof(VkPhysicalDeviceFeatures) / sizeof(VkBool32), "every core feature is named");
// The features of a set that the shared device was not made with, comma-separated ("" when none)
static void feats_missing(const VkPhysicalDeviceFeatures *want, char *out, size_t n) {
    out[0] = 0; size_t o = 0;
    for (size_t i = 0; i < sizeof g_feat_names / sizeof g_feat_names[0]; i++)
        if (FEAT_BIT(want, i) && !FEAT_BIT(&hvk.dev.enabled, i) && o + 2 < n) o += (size_t)snprintf(out + o, n - o, "%s%s", o ? ", " : "", g_feat_names[i].name);
}
static void pdev_feats(VkPhysicalDeviceFeatures *f) {
    char miss[512]; feats_missing(&g_tegra_feats, miss, sizeof miss);
    if (miss[0]) RED1("features of the Tegra shape the shared device does not have, offered as absent: %s", miss);
    for (size_t i = 0; i < sizeof g_feat_names / sizeof g_feat_names[0]; i++)
        *(VkBool32 *)((char *)f + g_feat_names[i].off) = FEAT_BIT(&g_tegra_feats, i) && FEAT_BIT(&hvk.dev.enabled, i);
}
static void pdev_mem(VkPhysicalDeviceMemoryProperties *m) {   // the Switch's three types on one heap, as hle_vk.m
    memset(m, 0, sizeof *m);
    const VkMemoryPropertyFlags dl = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    m->memoryTypeCount = 3;
    m->memoryTypes[0] = (VkMemoryType){dl, 0}; m->memoryTypes[1] = (VkMemoryType){dl | hv, 0}; m->memoryTypes[2] = (VkMemoryType){dl | hv | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0};
    m->memoryHeapCount = 1; m->memoryHeaps[0] = (VkMemoryHeap){3ull << 30, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT};
}
static void pdev_family(VkQueueFamilyProperties *p) {   // one family; every queue the game gets is the shared device's queue
    uint32_t n = 0; vkGetPhysicalDeviceQueueFamilyProperties(hvk.dev.phys, &n, NULL);
    VkQueueFamilyProperties *h = zalloc(umax(n, 1) * sizeof *h); vkGetPhysicalDeviceQueueFamilyProperties(hvk.dev.phys, &n, h);
    uint32_t bits = hvk.dev.queue_family < n ? h[hvk.dev.queue_family].timestampValidBits : 64;
    free(h);
    *p = (VkQueueFamilyProperties){VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT, 16, bits, {1, 1, 1}};
}
static void dedicated_req(void *pn) {   // hle_vk.m's answer: no allocation is preferred or required to be dedicated (this layer makes images' own allocations itself)
    for (VkBaseOutStructure *s = pn; s; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS) { VkMemoryDedicatedRequirements *d = (void *)s; d->prefersDedicatedAllocation = d->requiresDedicatedAllocation = VK_FALSE; }
}
// Properties2 / Features2 / MemoryProperties2 / QueueFamilyProperties2: the core part, as hle_vk.m answers them (their chains are left as the game filled them)
VK(vkGetPhysicalDeviceProperties, void, VkPhysicalDevice pd, VkPhysicalDeviceProperties *p) { (void)pd; pdev_props(p); }
VK(vkGetPhysicalDeviceProperties2, void, VkPhysicalDevice pd, VkPhysicalDeviceProperties2 *p) { (void)pd; pdev_props(&p->properties); }
VK(vkGetPhysicalDeviceFeatures, void, VkPhysicalDevice pd, VkPhysicalDeviceFeatures *f) { (void)pd; pdev_feats(f); }
VK(vkGetPhysicalDeviceFeatures2, void, VkPhysicalDevice pd, VkPhysicalDeviceFeatures2 *f) { (void)pd; pdev_feats(&f->features); }
VK(vkGetPhysicalDeviceMemoryProperties, void, VkPhysicalDevice pd, VkPhysicalDeviceMemoryProperties *m) { (void)pd; pdev_mem(m); }
VK(vkGetPhysicalDeviceMemoryProperties2, void, VkPhysicalDevice pd, VkPhysicalDeviceMemoryProperties2 *m) { (void)pd; pdev_mem(&m->memoryProperties); }
VK(vkGetPhysicalDeviceQueueFamilyProperties, void, VkPhysicalDevice pd, uint32_t *n, VkQueueFamilyProperties *p) {
    (void)pd; if (!p) { *n = 1; return; }
    if (!*n) return;
    *n = 1; pdev_family(p);
}
VK(vkGetPhysicalDeviceQueueFamilyProperties2, void, VkPhysicalDevice pd, uint32_t *n, VkQueueFamilyProperties2 *p) {
    (void)pd; if (!p) { *n = 1; return; }
    if (!*n) return;
    *n = 1; pdev_family(&p->queueFamilyProperties);
}
// Formats are the host's answers: what its driver can do with them
VK(vkGetPhysicalDeviceFormatProperties, void, VkPhysicalDevice pd, VkFormat format, VkFormatProperties *p) { (void)pd; vkGetPhysicalDeviceFormatProperties(hvk.dev.phys, format, p); }
VK(vkGetPhysicalDeviceFormatProperties2, void, VkPhysicalDevice pd, VkFormat format, VkFormatProperties2 *p) { (void)pd; vkGetPhysicalDeviceFormatProperties2(hvk.dev.phys, format, p); }
VK(vkGetPhysicalDeviceImageFormatProperties, VkResult, VkPhysicalDevice pd, VkFormat format, VkImageType type, VkImageTiling tiling, VkImageUsageFlags usage,
   VkImageCreateFlags flags, VkImageFormatProperties *p) {
    (void)pd; return vkGetPhysicalDeviceImageFormatProperties(hvk.dev.phys, format, type, tiling, usage, flags, p);
}
VK(vkGetPhysicalDeviceImageFormatProperties2, VkResult, VkPhysicalDevice pd, const VkPhysicalDeviceImageFormatInfo2 *i, VkImageFormatProperties2 *p) {
    (void)pd; return vkGetPhysicalDeviceImageFormatProperties2(hvk.dev.phys, i, p);
}
VK(vkGetPhysicalDeviceSparseImageFormatProperties, void, VkPhysicalDevice pd, VkFormat f, VkImageType t, VkSampleCountFlagBits s, VkImageUsageFlags u, VkImageTiling tl,
   uint32_t *n, VkSparseImageFormatProperties *p) {
    (void)pd; (void)f; (void)t; (void)s; (void)u; (void)tl; (void)p; *n = 0;   // no sparse features are offered
}

// ---------- Surface ----------
typedef struct { void *window; } gsurf;
static VkExtent2D surf_ext(void) { return dm_opmode() ? (VkExtent2D){1920, 1080} : (VkExtent2D){1280, 720}; }
VK(vkCreateViSurfaceNN, VkResult, VkInstance i, const VkViSurfaceCreateInfoNN *ci, const VkAllocationCallbacks *a, VkSurfaceKHR *out) {
    (void)i; (void)a; gsurf *s = zalloc(sizeof *s); s->window = ci->window; *out = (VkSurfaceKHR)(void *)s;
    VLOG("CreateViSurfaceNN window %p", ci->window); return VK_SUCCESS;
}
VK(vkDestroySurfaceKHR, void, VkInstance i, VkSurfaceKHR s, const VkAllocationCallbacks *a) { (void)i; (void)a; free((void *)s); }
VK(vkGetPhysicalDeviceSurfaceSupportKHR, VkResult, VkPhysicalDevice pd, uint32_t fam, VkSurfaceKHR s, VkBool32 *ok) { (void)pd; (void)s; *ok = fam == 0; return VK_SUCCESS; }
VK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR, VkResult, VkPhysicalDevice pd, VkSurfaceKHR s, VkSurfaceCapabilitiesKHR *c) {
    (void)pd; (void)s; memset(c, 0, sizeof *c);
    c->minImageCount = 2; c->maxImageCount = 4; c->currentExtent = surf_ext(); c->minImageExtent = (VkExtent2D){1, 1}; c->maxImageExtent = (VkExtent2D){1920, 1080};
    // INHERIT as well: Super Mario Galaxy creates its swapchain with preTransform INHERIT and asserts when the surface does not list it (transforms are ignored here)
    c->maxImageArrayLayers = 1; c->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR; c->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    c->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR | VK_SURFACE_TRANSFORM_INHERIT_BIT_KHR;
    c->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    return VK_SUCCESS;
}
VK(vkGetPhysicalDeviceSurfaceFormatsKHR, VkResult, VkPhysicalDevice pd, VkSurfaceKHR s, uint32_t *n, VkSurfaceFormatKHR *f) {
    (void)pd; (void)s;
    static const VkSurfaceFormatKHR fs[] = {{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}, {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
                                            {VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}, {VK_FORMAT_R8G8B8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}};
    const uint32_t k = sizeof fs / sizeof *fs;
    if (!f) { *n = k; return VK_SUCCESS; }
    uint32_t m = umin(*n, k); memcpy(f, fs, m * sizeof *fs); *n = m; return m < k ? VK_INCOMPLETE : VK_SUCCESS;
}
VK(vkGetPhysicalDeviceSurfacePresentModesKHR, VkResult, VkPhysicalDevice pd, VkSurfaceKHR s, uint32_t *n, VkPresentModeKHR *m) {
    (void)pd; (void)s; if (!m) { *n = 1; return VK_SUCCESS; }
    if (!*n) return VK_INCOMPLETE;
    *n = 1; m[0] = VK_PRESENT_MODE_FIFO_KHR; return VK_SUCCESS;
}

// ---------- Device and queue ----------
VK(vkEnumerateDeviceLayerProperties, VkResult, VkPhysicalDevice pd, uint32_t *n, VkLayerProperties *p) { (void)pd; (void)p; *n = 0; return VK_SUCCESS; }
VK(vkEnumerateDeviceExtensionProperties, VkResult, VkPhysicalDevice pd, const char *layer, uint32_t *n, VkExtensionProperties *p) {
    (void)pd; if (layer) { *n = 0; return VK_ERROR_LAYER_NOT_PRESENT; }
    return fill_names(g_dev_ext, sizeof g_dev_ext / sizeof *g_dev_ext, n, p);
}
static void img_table_init(void);
VKB(vkCreateDevice, VkResult, VkPhysicalDevice pd, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *a, VkDevice *out) {
    (void)pd; (void)a;
    if (!hvk.on) return VK_ERROR_INITIALIZATION_FAILED;
    char ext[512]; join_names(ext, sizeof ext, ci->ppEnabledExtensionNames, ci->enabledExtensionCount);
    uint32_t nq = 0; for (uint32_t i = 0; i < ci->queueCreateInfoCount; i++) nq += ci->pQueueCreateInfos[i].queueCount;
    VLOG("CreateDevice: %u queues; extensions %s", nq, ext);
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) {
        int known = 0;
        for (size_t k = 0; k < sizeof g_dev_ext / sizeof *g_dev_ext; k++) known |= !strcmp(ci->ppEnabledExtensionNames[i], g_dev_ext[k]);
        if (!known) LOG("[vk] [RED] the game enables device extension %s, which this layer does not offer: its functions are not looked up", ci->ppEnabledExtensionNames[i]);
    }
    const VkPhysicalDeviceFeatures *want = ci->pEnabledFeatures;
    for (const VkBaseInStructure *s = ci->pNext; s && !want; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) want = &((const VkPhysicalDeviceFeatures2 *)(const void *)s)->features;
    if (want) {
        char miss[512]; feats_missing(want, miss, sizeof miss);
        if (miss[0]) LOG("[vk] [RED] the game enables features the shared device was not made with: %s (what depends on them is undefined on this host)", miss);
    }
    pthread_mutex_lock(&g_mu);
    bar_init(); img_table_init();
    if (!g_tl) {
        VkSemaphoreTypeCreateInfo ti = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, NULL, VK_SEMAPHORE_TYPE_TIMELINE, 0};
        VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &ti, 0};
        if (vkCreateSemaphore(hvk.d, &si, NULL, &g_tl)) { pthread_mutex_unlock(&g_mu); g_tl = VK_NULL_HANDLE; LOG("[vk] [RED] the layer's timeline semaphore could not be made"); return VK_ERROR_INITIALIZATION_FAILED; }
        g_tl_val = 0;
    }
    pthread_mutex_unlock(&g_mu);
    *out = G_DEV; return VK_SUCCESS;
}
static void queue_idle(void) { pthread_mutex_lock(&hvk.queue_mu); vkQueueWaitIdle(hvk.dev.queue); pthread_mutex_unlock(&hvk.queue_mu); }
VKB(vkDestroyDevice, void, VkDevice d, const VkAllocationCallbacks *a) {   // the shared device stays: the backend's
    (void)d; (void)a;
    if (!hvk.on) return;
    queue_idle();
    pthread_mutex_lock(&g_mu);
    if (g_tl) vkDestroySemaphore(hvk.d, g_tl, NULL);
    g_tl = VK_NULL_HANDLE; g_tl_val = 0;
    pthread_mutex_unlock(&g_mu);
}
VK(vkGetDeviceQueue, void, VkDevice d, uint32_t fam, uint32_t idx, VkQueue *q) { (void)d; (void)fam; (void)idx; *q = G_QUEUE; }
VK(vkGetDeviceQueue2, void, VkDevice d, const VkDeviceQueueInfo2 *i, VkQueue *q) { (void)d; (void)i; *q = G_QUEUE; }
void dm_vk_drain(void) { if (hvk.on) queue_idle(); }   // dm_gfx_suspend (hle_nvn.c): the queue's work done before the app is suspended

// ---------- Memory and buffers ----------
typedef struct {
    VkDeviceSize size;
    uint32_t gtype;            // the guest's memory type, 0 to 2
    _Atomic(VkDeviceMemory) mem;   // the driver allocation, made at the first buffer bind, map or linear-image bind
    uint32_t htype;            // its host memory type
    void *map;                 // its mapping (host-visible types): mapped once, for good
    void *block; size_t block_n;   // the arena block an imported allocation is (gmem_import), given back to the arena after vkFreeMemory; NULL for the driver's own memory
} gmem;
static gmem *gm_of(VkDeviceMemory m) { return (gmem *)(void *)m; }
// The memory the game maps is guest memory: on the x86-64 engine it lies in the arena below 2^39 (docs/design/aot-recompiler.md 5.2; dm/host/aot.h, "Host memory the guest
// sees"), whose allocator the host program hands this layer (main_x86.c: dm_guest_alloc, dm_guest_free); a binary without the engine (the GPU tests but t_vk_guest) hands none
// and keeps the driver's mappings. With it (the import mode) the host-visible types are arena blocks imported with VK_EXT_external_memory_host (design 4.2, the baseline's
// extension: vk_device.c requires it), so the game's pointer is the arena's and the GPU reads and writes the same pages; type 0 stays a device-local allocation. An import
// can be bound only by a buffer or image made with the host-allocation handle type, so every buffer the game makes carries it, and every linear image whose format the
// device imports for (lavapipe: all; drivers that refuse images, design 4.2: such an image gets memory of its own, with a red line). Declared, not done:
// VK_EXT_map_memory_placed, which would place the driver's own mapping in the arena instead (device-local host-visible memory with resizable BAR, any linear image), is not
// offered by lavapipe 25.2.8, so no device here could run it
static void *(*g_arena_alloc)(size_t n);
static void (*g_arena_release)(void *p, size_t n);
void guest_vk_set_arena(void *(*alloc)(size_t n), void (*release)(void *p, size_t n));
void guest_vk_set_arena(void *(*alloc)(size_t n), void (*release)(void *p, size_t n)) { g_arena_alloc = alloc; g_arena_release = release; }
static int import_mode(void) { return g_arena_alloc != NULL; }
// gm as an import of a fresh arena block (under g_mu): *out the allocation, *type its host type among bits; a VkResult (the block given back) when the driver refuses it
static VkResult gmem_import(gmem *gm, uint32_t bits, VkDeviceMemory *out, uint32_t *type) {
    const size_t pg = plat_page_size(), al = hvk.dev.caps.min_import_alignment > pg ? (size_t)hvk.dev.caps.min_import_alignment : pg;
    const size_t n = ((size_t)(gm->size ? gm->size : 1) + al - 1) / al * al, bn = n + (al > pg ? al : 0);   // room to align a block that is only page-aligned
    uint8_t *block = g_arena_alloc(bn), *at = (uint8_t *)(((uintptr_t)block + al - 1) & ~(uintptr_t)(al - 1));
    VkMemoryHostPointerPropertiesEXT hp = {VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    VkResult r = vkGetMemoryHostPointerPropertiesEXT(hvk.d, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, at, &hp);
    const uint32_t t = r ? UINT32_MAX : pick_type(gm->gtype, hp.memoryTypeBits & bits);
    if (!r && (t == UINT32_MAX || !(hvk.dev.mem.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))) r = VK_ERROR_INVALID_EXTERNAL_HANDLE;
    if (!r && ++hvk.dev.allocations > hvk.dev.caps.max_allocations) { hvk.dev.allocations--; r = VK_ERROR_TOO_MANY_OBJECTS; }
    else if (!r) {
        VkImportMemoryHostPointerInfoEXT im = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT, NULL, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, at};
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &im, n, t};
        r = vkAllocateMemory(hvk.d, &ai, NULL, out);
        if (r) hvk.dev.allocations--;
    }
    if (r) {
        g_arena_release(block, bn); *out = VK_NULL_HANDLE;
        RED1("arena memory of %llu bytes for guest memory type %u could not be imported (VkResult %d, importable host types %#x, the object's %#x): the game gets the driver's own "
             "mapping, outside the arena below 2^39", (unsigned long long)gm->size, gm->gtype, r, hp.memoryTypeBits, bits);
        return r;
    }
    atomic_fetch_add(&g_mem_bytes, gm->size ? gm->size : 16);   // as host_alloc counts, host_free uncounts
    gm->map = at; gm->block = block; gm->block_n = bn; *type = t;
    return VK_SUCCESS;
}
// The driver allocation of gm, made now if it has none, of a type among bits; VK_ERROR_* when it cannot serve those bits
static VkResult gmem_ensure(gmem *gm, uint32_t bits) {
    if (atomic_load_explicit(&gm->mem, memory_order_acquire)) {
        if (bits >> gm->htype & 1) return VK_SUCCESS;
        RED1("memory of host type %u bound to an object that takes types %#x only", gm->htype, bits);
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    pthread_mutex_lock(&g_mu);
    VkResult r = VK_SUCCESS;
    if (!gm->mem) {
        uint32_t t = pick_type(gm->gtype, bits);
        VkDeviceMemory m = VK_NULL_HANDLE;
        if (import_mode() && gm->gtype != 0 && !gmem_import(gm, bits, &m, &t)) r = VK_SUCCESS;   // the arena's pages, imported
        else {
            r = host_alloc(gm->size, t, VK_NULL_HANDLE, &m);
            if (!r && (hvk.dev.mem.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
                r = vkMapMemory(hvk.d, m, 0, VK_WHOLE_SIZE, 0, &gm->map);
                if (r) { host_free(m, gm->size); m = VK_NULL_HANDLE; gm->map = NULL; }
            }
        }
        if (!r) { gm->htype = t; atomic_store_explicit(&gm->mem, m, memory_order_release); OLOG("memory %p: %llu bytes of host type %u (guest type %u)%s", (void *)gm, (unsigned long long)gm->size, t, gm->gtype, gm->block ? ", arena pages imported" : ""); }
        else LOG("[vk] [RED] no driver allocation of %llu bytes for guest memory type %u (host types %#x, VkResult %d)", (unsigned long long)gm->size, gm->gtype, bits, r);
    } else if (!(bits >> gm->htype & 1)) { RED1("memory of host type %u bound to an object that takes types %#x only", gm->htype, bits); r = VK_ERROR_OUT_OF_DEVICE_MEMORY; }
    pthread_mutex_unlock(&g_mu);
    return r;
}
VK(vkAllocateMemory, VkResult, VkDevice d, const VkMemoryAllocateInfo *ai, const VkAllocationCallbacks *a, VkDeviceMemory *out) {
    (void)d; (void)a;
    if (ai->memoryTypeIndex >= 3) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    gmem *gm = zalloc(sizeof *gm); gm->size = ai->allocationSize; gm->gtype = ai->memoryTypeIndex;
    OLOG("AllocateMemory %p %llu bytes type %u", (void *)gm, (unsigned long long)gm->size, gm->gtype);
    *out = (VkDeviceMemory)(void *)gm; return VK_SUCCESS;
}
VKB(vkFreeMemory, void, VkDevice d, VkDeviceMemory m, const VkAllocationCallbacks *a) {
    (void)d; (void)a; gmem *gm = gm_of(m); if (!gm) return;
    host_free(gm->mem, gm->size);
    if (gm->block) g_arena_release(gm->block, gm->block_n);   // an import's pages, back to the arena once the allocation over them is gone
    free(gm);
}
VKB(vkMapMemory, VkResult, VkDevice d, VkDeviceMemory m, VkDeviceSize off, VkDeviceSize size, VkMemoryMapFlags fl, void **pp) {
    (void)d; (void)size; (void)fl; gmem *gm = gm_of(m);
    if (gm->gtype == 0) RED1("the game maps memory of type 0, which is not host-visible: refused");
    uint32_t all = hvk.dev.mem.memoryTypeCount >= 32 ? ~0u : (1u << hvk.dev.mem.memoryTypeCount) - 1;
    if (gm->gtype == 0 || gmem_ensure(gm, all) || !gm->map) { *pp = NULL; return VK_ERROR_MEMORY_MAP_FAILED; }
    *pp = (uint8_t *)gm->map + off; return VK_SUCCESS;
}
VK(vkUnmapMemory, void, VkDevice d, VkDeviceMemory m) { (void)d; (void)m; }   // mapped for good
VK(vkFlushMappedMemoryRanges, VkResult, VkDevice d, uint32_t n, const VkMappedMemoryRange *r) { (void)d; (void)n; (void)r; return VK_SUCCESS; }   // coherent types only
VK(vkInvalidateMappedMemoryRanges, VkResult, VkDevice d, uint32_t n, const VkMappedMemoryRange *r) { (void)d; (void)n; (void)r; return VK_SUCCESS; }
VK(vkGetDeviceMemoryCommitment, void, VkDevice d, VkDeviceMemory m, VkDeviceSize *bytes) { (void)d; *bytes = gm_of(m)->mem ? gm_of(m)->size : 0; }
// The game's create info with the host-allocation handle type in front of its chain (the import mode: any buffer may be bound to an imported allocation), unless its chain
// already names external memory
static int chain_has(const void *next, VkStructureType t) { for (const VkBaseInStructure *s = next; s; s = s->pNext) if (s->sType == t) return 1; return 0; }
VK(vkCreateBuffer, VkResult, VkDevice d, const VkBufferCreateInfo *ci, const VkAllocationCallbacks *a, VkBuffer *out) {
    (void)d; (void)a;
    if (!import_mode() || chain_has(ci->pNext, VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO)) return vkCreateBuffer(hvk.d, ci, NULL, out);
    VkExternalMemoryBufferCreateInfo ex = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, ci->pNext, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo c = *ci; c.pNext = &ex;
    return vkCreateBuffer(hvk.d, &c, NULL, out);
}
VK(vkDestroyBuffer, void, VkDevice d, VkBuffer b, const VkAllocationCallbacks *a) { (void)d; (void)a; vkDestroyBuffer(hvk.d, b, NULL); }
VK(vkGetBufferMemoryRequirements, void, VkDevice d, VkBuffer b, VkMemoryRequirements *r) {
    (void)d; vkGetBufferMemoryRequirements(hvk.d, b, r); r->memoryTypeBits = guest_bits(r->memoryTypeBits);
}
VK(vkGetBufferMemoryRequirements2, void, VkDevice d, const VkBufferMemoryRequirementsInfo2 *i, VkMemoryRequirements2 *r) {
    (void)d; vkGetBufferMemoryRequirements2(hvk.d, i, r); r->memoryRequirements.memoryTypeBits = guest_bits(r->memoryRequirements.memoryTypeBits); dedicated_req(r->pNext);
}
VKB(vkBindBufferMemory, VkResult, VkDevice d, VkBuffer b, VkDeviceMemory m, VkDeviceSize off) {
    (void)d; gmem *gm = gm_of(m); VkMemoryRequirements req; vkGetBufferMemoryRequirements(hvk.d, b, &req);
    VkResult r = gmem_ensure(gm, req.memoryTypeBits);
    return r ? r : vkBindBufferMemory(hvk.d, b, gm->mem, off);
}
VKB(vkBindBufferMemory2, VkResult, VkDevice d, uint32_t n, const VkBindBufferMemoryInfo *bi) {
    for (uint32_t i = 0; i < n; i++) { VkResult r = h_vkBindBufferMemory(d, bi[i].buffer, bi[i].memory, bi[i].memoryOffset); if (r) return r; }
    return VK_SUCCESS;
}
VK(vkCreateBufferView, VkResult, VkDevice d, const VkBufferViewCreateInfo *ci, const VkAllocationCallbacks *a, VkBufferView *out) { (void)d; (void)a; return vkCreateBufferView(hvk.d, ci, NULL, out); }
VK(vkDestroyBufferView, void, VkDevice d, VkBufferView v, const VkAllocationCallbacks *a) { (void)d; (void)a; vkDestroyBufferView(hvk.d, v, NULL); }

// ---------- Images, views, samplers ----------
// What this layer keeps per image (the driver's handle is the game's): its own allocation (optimal images, swapchain images), linear tiling, made for a swapchain
typedef struct { VkDeviceMemory own; VkDeviceSize own_size; uint8_t linear, swapchain, noimport; } gimg;   // noimport: a linear image the device cannot bind to an import
static nvn_hmap g_img; static pthread_mutex_t g_img_mu = PTHREAD_MUTEX_INITIALIZER;
static void img_table_init(void) { pthread_mutex_lock(&g_img_mu); if (!g_img.klen) nvn_hm_init(&g_img, 8); pthread_mutex_unlock(&g_img_mu); }
static void img_put(VkImage im, gimg *g) { pthread_mutex_lock(&g_img_mu); if (!g_img.klen) nvn_hm_init(&g_img, 8); nvn_hm_put(&g_img, &im, g); pthread_mutex_unlock(&g_img_mu); }
static gimg *img_get(VkImage im) { pthread_mutex_lock(&g_img_mu); gimg *g = g_img.klen ? nvn_hm_get(&g_img, &im) : NULL; pthread_mutex_unlock(&g_img_mu); return g; }
static gimg *img_del(VkImage im) { pthread_mutex_lock(&g_img_mu); gimg *g = g_img.klen ? nvn_hm_del(&g_img, &im) : NULL; pthread_mutex_unlock(&g_img_mu); return g; }
// An allocation of the image's own: device-local, dedicated where the driver prefers it
static VkResult img_own(VkImage im, gimg *g) {
    VkImageMemoryRequirementsInfo2 ri = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, NULL, im};
    VkMemoryDedicatedRequirements dr = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 req = {VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, &dr};
    vkGetImageMemoryRequirements2(hvk.d, &ri, &req);
    VkDeviceMemory m;
    VkResult r = host_alloc(req.memoryRequirements.size, pick_type(0, req.memoryRequirements.memoryTypeBits), dr.prefersDedicatedAllocation || dr.requiresDedicatedAllocation ? im : VK_NULL_HANDLE, &m);
    if (!r) r = vkBindImageMemory(hvk.d, im, m, 0);
    if (r) { LOG("[vk] [RED] no device-local memory of %llu bytes for an image (VkResult %d)", (unsigned long long)req.memoryRequirements.size, r); return r; }
    g->own = m; g->own_size = req.memoryRequirements.size;
    return VK_SUCCESS;
}
VKB(vkCreateImage, VkResult, VkDevice d, const VkImageCreateInfo *ci, const VkAllocationCallbacks *a, VkImage *out) {
    (void)d; (void)a;
    // The import mode: a linear image may be bound to an imported allocation, so it carries the host-allocation handle type where the device imports for its format and use
    int linear = ci->tiling == VK_IMAGE_TILING_LINEAR, ext = 0;
    VkExternalMemoryImageCreateInfo ex = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, ci->pNext, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkImageCreateInfo c = *ci;
    if (linear && import_mode() && !chain_has(ci->pNext, VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO)) {
        VkPhysicalDeviceExternalImageFormatInfo ei = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO, NULL, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
        VkPhysicalDeviceImageFormatInfo2 fi = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, &ei, ci->format, ci->imageType, ci->tiling, ci->usage, ci->flags};
        VkExternalImageFormatProperties ep = {VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
        VkImageFormatProperties2 fp = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &ep};
        ext = !vkGetPhysicalDeviceImageFormatProperties2(hvk.dev.phys, &fi, &fp) && (ep.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT);
        if (ext) c.pNext = &ex;
    }
    // An image with external memory must start UNDEFINED (VUID-VkImageCreateInfo-pNext-01443), and a game makes its CPU-filled linear images PREINITIALIZED: in the import mode
    // every image starts UNDEFINED and every barrier from PREINITIALIZED leaves UNDEFINED (lay below), one rule for all images. Vulkan does not promise that a transition out of
    // UNDEFINED keeps a linear image's memory; drivers do not touch linear memory in a layout transition (lavapipe: t_vk_guest's linear texels), declared here as the assumption
    if (import_mode() && c.initialLayout == VK_IMAGE_LAYOUT_PREINITIALIZED) c.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult r = vkCreateImage(hvk.d, &c, NULL, out);
    if (r) { LOG("[vk] [RED] the driver refuses image %ux%ux%u format %d levels %u layers %u tiling %d usage %#x (VkResult %d)", ci->extent.width, ci->extent.height,
                 ci->extent.depth, ci->format, ci->mipLevels, ci->arrayLayers, ci->tiling, ci->usage, r); return r; }
    gimg *g = zalloc(sizeof *g); g->linear = (uint8_t)linear; g->noimport = (uint8_t)(linear && import_mode() && !ext);
    img_put(*out, g);
    OLOG("CreateImage %p %ux%ux%u format %d levels %u layers %u usage %#x%s", (void *)*out, ci->extent.width, ci->extent.height, ci->extent.depth, ci->format, ci->mipLevels,
         ci->arrayLayers, ci->usage, g->linear ? " linear" : "");
    return VK_SUCCESS;
}
VKB(vkDestroyImage, void, VkDevice d, VkImage im, const VkAllocationCallbacks *a) {
    (void)d; (void)a; if (!im) return;
    gimg *g = img_del(im);
    if (g && g->swapchain) { img_put(im, g); RED1("the game destroys a swapchain image: ignored (vkDestroySwapchainKHR releases it)"); return; }
    vkDestroyImage(hvk.d, im, NULL);
    if (g) { host_free(g->own, g->own_size); free(g); }
}
VK(vkGetImageMemoryRequirements, void, VkDevice d, VkImage im, VkMemoryRequirements *r) {
    (void)d; vkGetImageMemoryRequirements(hvk.d, im, r);
    gimg *g = img_get(im); r->memoryTypeBits = g && g->linear ? guest_bits(r->memoryTypeBits) : 7;   // an optimal image takes any type: its binding is not used
}
VK(vkGetImageMemoryRequirements2, void, VkDevice d, const VkImageMemoryRequirementsInfo2 *i, VkMemoryRequirements2 *r) {
    (void)d; vkGetImageMemoryRequirements2(hvk.d, i, r);
    gimg *g = img_get(i->image); r->memoryRequirements.memoryTypeBits = g && g->linear ? guest_bits(r->memoryRequirements.memoryTypeBits) : 7;
    dedicated_req(r->pNext);
}
VK(vkGetImageSparseMemoryRequirements, void, VkDevice d, VkImage i, uint32_t *n, VkSparseImageMemoryRequirements *r) { (void)d; (void)i; (void)r; *n = 0; }
VK(vkGetImageSparseMemoryRequirements2, void, VkDevice d, const VkImageSparseMemoryRequirementsInfo2 *i, uint32_t *n, VkSparseImageMemoryRequirements2 *r) { (void)d; (void)i; (void)r; *n = 0; }
VK(vkGetImageSubresourceLayout, void, VkDevice d, VkImage im, const VkImageSubresource *sr, VkSubresourceLayout *l) { (void)d; vkGetImageSubresourceLayout(hvk.d, im, sr, l); }
// A linear image is bound where the game says, so the CPU's writes through the mapping are its texels; an optimal one gets device-local memory of its own
VKB(vkBindImageMemory, VkResult, VkDevice d, VkImage im, VkDeviceMemory m, VkDeviceSize off) {
    (void)d; gimg *g = img_get(im);
    if (!g) { RED1("memory bound to an image this layer did not make: refused"); return VK_ERROR_OUT_OF_DEVICE_MEMORY; }
    if (g->swapchain || g->own) return VK_SUCCESS;
    if (g->linear) {
        VkMemoryRequirements req; vkGetImageMemoryRequirements(hvk.d, im, &req);
        gmem *gm = gm_of(m);
        if (!gmem_ensure(gm, req.memoryTypeBits) && !(gm->block && g->noimport)) return vkBindImageMemory(hvk.d, im, gm->mem, off);
        if (gm->block && g->noimport)
            RED1("a linear image whose format the device does not import host memory for is bound to the game's memory, an import of arena pages: it gets memory of its own, "
                 "which the game's CPU writes do not reach (VK_EXT_map_memory_placed would keep the driver's memory and its mapping in the arena: not offered here)");
        else RED1("a linear image cannot be bound to the game's memory: it gets memory of its own, which the game's CPU writes do not reach");
    }
    return img_own(im, g);
}
VKB(vkBindImageMemory2, VkResult, VkDevice d, uint32_t n, const VkBindImageMemoryInfo *bi) {
    for (uint32_t i = 0; i < n; i++) { VkResult r = h_vkBindImageMemory(d, bi[i].image, bi[i].memory, bi[i].memoryOffset); if (r) return r; }
    return VK_SUCCESS;
}
VKB(vkCreateImageView, VkResult, VkDevice d, const VkImageViewCreateInfo *ci, const VkAllocationCallbacks *a, VkImageView *out) { (void)d; (void)a; return vkCreateImageView(hvk.d, ci, NULL, out); }
VKB(vkDestroyImageView, void, VkDevice d, VkImageView v, const VkAllocationCallbacks *a) { (void)d; (void)a; vkDestroyImageView(hvk.d, v, NULL); }
VKB(vkCreateSampler, VkResult, VkDevice d, const VkSamplerCreateInfo *ci, const VkAllocationCallbacks *a, VkSampler *out) { (void)d; (void)a; return vkCreateSampler(hvk.d, ci, NULL, out); }
VKB(vkDestroySampler, void, VkDevice d, VkSampler s, const VkAllocationCallbacks *a) { (void)d; (void)a; vkDestroySampler(hvk.d, s, NULL); }

// ---------- Descriptors and pipeline layouts: the driver's ----------
VK(vkCreateDescriptorSetLayout, VkResult, VkDevice d, const VkDescriptorSetLayoutCreateInfo *ci, const VkAllocationCallbacks *a, VkDescriptorSetLayout *out) {
    (void)d; (void)a; return vkCreateDescriptorSetLayout(hvk.d, ci, NULL, out);
}
VK(vkDestroyDescriptorSetLayout, void, VkDevice d, VkDescriptorSetLayout L, const VkAllocationCallbacks *a) { (void)d; (void)a; vkDestroyDescriptorSetLayout(hvk.d, L, NULL); }
VK(vkCreateDescriptorPool, VkResult, VkDevice d, const VkDescriptorPoolCreateInfo *ci, const VkAllocationCallbacks *a, VkDescriptorPool *out) {
    (void)d; (void)a; return vkCreateDescriptorPool(hvk.d, ci, NULL, out);
}
VK(vkResetDescriptorPool, VkResult, VkDevice d, VkDescriptorPool p, VkDescriptorPoolResetFlags f) { (void)d; return vkResetDescriptorPool(hvk.d, p, f); }
VK(vkDestroyDescriptorPool, void, VkDevice d, VkDescriptorPool p, const VkAllocationCallbacks *a) { (void)d; (void)a; vkDestroyDescriptorPool(hvk.d, p, NULL); }
VK(vkAllocateDescriptorSets, VkResult, VkDevice d, const VkDescriptorSetAllocateInfo *ai, VkDescriptorSet *out) { (void)d; return vkAllocateDescriptorSets(hvk.d, ai, out); }
VK(vkFreeDescriptorSets, VkResult, VkDevice d, VkDescriptorPool p, uint32_t n, const VkDescriptorSet *s) { (void)d; return vkFreeDescriptorSets(hvk.d, p, n, s); }
VK(vkUpdateDescriptorSets, void, VkDevice d, uint32_t nw, const VkWriteDescriptorSet *w, uint32_t nc, const VkCopyDescriptorSet *c) { (void)d; vkUpdateDescriptorSets(hvk.d, nw, w, nc, c); }
VK(vkCreateDescriptorUpdateTemplate, VkResult, VkDevice d, const VkDescriptorUpdateTemplateCreateInfo *ci, const VkAllocationCallbacks *a, VkDescriptorUpdateTemplate *out) {
    (void)d; (void)a; return vkCreateDescriptorUpdateTemplate(hvk.d, ci, NULL, out);
}
VK(vkDestroyDescriptorUpdateTemplate, void, VkDevice d, VkDescriptorUpdateTemplate t, const VkAllocationCallbacks *a) { (void)d; (void)a; vkDestroyDescriptorUpdateTemplate(hvk.d, t, NULL); }
VK(vkUpdateDescriptorSetWithTemplate, void, VkDevice d, VkDescriptorSet s, VkDescriptorUpdateTemplate t, const void *data) { (void)d; vkUpdateDescriptorSetWithTemplate(hvk.d, s, t, data); }
VK(vkCreatePipelineLayout, VkResult, VkDevice d, const VkPipelineLayoutCreateInfo *ci, const VkAllocationCallbacks *a, VkPipelineLayout *out) {
    (void)d; (void)a; return vkCreatePipelineLayout(hvk.d, ci, NULL, out);
}
VK(vkDestroyPipelineLayout, void, VkDevice d, VkPipelineLayout l, const VkAllocationCallbacks *a) { (void)d; (void)a; vkDestroyPipelineLayout(hvk.d, l, NULL); }

// ---------- Render passes, framebuffers ----------
// PRESENT_SRC_KHR names the layout of a real swapchain's presentable images; the game's swapchain images are ordinary images the present path copies from, in HVK_EXT_LAYOUT
// In the import mode PREINITIALIZED is UNDEFINED (every image starts UNDEFINED there: vkCreateImage)
static VkImageLayout lay(VkImageLayout l) {
    return l == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR ? HVK_EXT_LAYOUT : l == VK_IMAGE_LAYOUT_PREINITIALIZED && import_mode() ? VK_IMAGE_LAYOUT_UNDEFINED : l;
}
VK(vkCreateRenderPass, VkResult, VkDevice d, const VkRenderPassCreateInfo *ci, const VkAllocationCallbacks *a, VkRenderPass *out) {
    (void)d; (void)a;
    VkRenderPassCreateInfo c = *ci; VkAttachmentDescription at[16], *heap = NULL;
    VkAttachmentDescription *p = ci->attachmentCount <= 16 ? at : (heap = zalloc(ci->attachmentCount * sizeof *heap));
    for (uint32_t i = 0; i < ci->attachmentCount; i++) { p[i] = ci->pAttachments[i]; p[i].initialLayout = lay(p[i].initialLayout); p[i].finalLayout = lay(p[i].finalLayout); }
    c.pAttachments = p;
    VkResult r = vkCreateRenderPass(hvk.d, &c, NULL, out);
    free(heap);
    return r;
}
VK(vkDestroyRenderPass, void, VkDevice d, VkRenderPass rp, const VkAllocationCallbacks *a) { (void)d; (void)a; vkDestroyRenderPass(hvk.d, rp, NULL); }
VK(vkGetRenderAreaGranularity, void, VkDevice d, VkRenderPass rp, VkExtent2D *g) { (void)d; vkGetRenderAreaGranularity(hvk.d, rp, g); }
VK(vkCreateFramebuffer, VkResult, VkDevice d, const VkFramebufferCreateInfo *ci, const VkAllocationCallbacks *a, VkFramebuffer *out) { (void)d; (void)a; return vkCreateFramebuffer(hvk.d, ci, NULL, out); }
VK(vkDestroyFramebuffer, void, VkDevice d, VkFramebuffer fb, const VkAllocationCallbacks *a) { (void)d; (void)a; vkDestroyFramebuffer(hvk.d, fb, NULL); }

// ---------- Shaders ----------
// The game's module: the driver's module of its words (fixed), and for a fragment shader with an output at Location 1 without an Index, those words, from which its dual-source
// form is made at the first pipeline that blends with the second source
typedef struct { VkShaderModule mod, mod_d; uint32_t *dual; size_t ndual; int tried_d; char key[17]; } gshader;
static gshader *gs_of(VkShaderModule m) { return (gshader *)(void *)m; }
// DM_VK_CAPTURE=<dir>: every SPIR-V module the game creates is written to <dir>/<key>.spv, as on Apple (Super Mario Galaxy generates its GX shaders as SPIR-V at run time, so no
// game file holds them; vk_shaders.py translates a capture folder like a RomFS). Whole files only: a capture run may be killed at any time
static void sh_capture(const char *key, const void *code, size_t n) {
    static const char *dir; static _Atomic int init;
    if (!atomic_exchange(&init, 1)) { dir = getenv("DM_VK_CAPTURE"); if (dir && !*dir) dir = NULL; if (dir) VLOG("shader capture → %s", dir); }
    if (!dir) return;
    char p[1024]; snprintf(p, sizeof p, "%s/%s.spv", dir, key);
    FILE *e = fopen(p, "rb"); if (e) { fclose(e); return; }
    char t[1100]; snprintf(t, sizeof t, "%s.%d.tmp", p, (int)getpid());
    FILE *f = fopen(t, "wb"); if (!f) { RED1("shader capture cannot write to %s", dir); return; }
    int ok = fwrite(code, 1, n, f) == n; ok &= fclose(f) == 0;
    if (ok && rename(t, p)) { remove(t); }   // another thread wrote it first (Windows refuses to rename over a file)
    else if (!ok) remove(t);
}
static VkResult module_make(const uint32_t *w, size_t nw, const void *next, VkShaderModule *out) {
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, next, 0, nw * 4, w};
    return vkCreateShaderModule(hvk.d, &ci, NULL, out);
}
VKB(vkCreateShaderModule, VkResult, VkDevice d, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *a, VkShaderModule *out) {
    (void)d; (void)a;
    const uint32_t *w = ci->pCode; size_t n = ci->codeSize / 4;
    uint8_t h[32]; plat_sha256(ci->pCode, ci->codeSize, h);
    gshader *s = zalloc(sizeof *s);
    for (int i = 0; i < 8; i++) snprintf(s->key + 2 * i, 3, "%02x", h[i]);
    sh_capture(s->key, ci->pCode, ci->codeSize);
    const char *bad = ci->codeSize % 4 ? "a size that is not whole words" : guest_spirv_check(w, n);
    if (bad) { LOG("[vk] [RED] shader %s (%zu bytes) is not handed to the driver: %s", s->key, ci->codeSize, bad); free(s); return VK_ERROR_INVALID_SHADER_NV; }
    guest_spirv_report rep; size_t nf;
    uint32_t *f = guest_spirv_fix(w, n, GUEST_SPIRV_STRIDES, &nf, &rep);
    if (!f) { free(s); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    if (rep.strides_dropped) {
        static _Atomic int k; if (atomic_fetch_add(&k, 1) < 3) VLOG("shader %s: %d member ArrayStride decorations moved onto %d array types", s->key, rep.strides_dropped, rep.strides_moved);
        if (rep.stride_conflicts) LOG("[vk] [RED] shader %s: %d member ArrayStride decorations differ from their array type's (the first is kept)", s->key, rep.stride_conflicts);
    }
    if (guest_spirv_dual_candidate(f, nf)) { s->dual = malloc(nf * 4); if (s->dual) { memcpy(s->dual, f, nf * 4); s->ndual = nf; } }
    VkResult r = module_make(f, nf, ci->pNext, &s->mod);
    if (f != w) free(f);
    if (r) { LOG("[vk] [RED] the driver refuses shader %s (VkResult %d)", s->key, r); free(s->dual); free(s); return r; }
    atomic_fetch_add(&g_n_shader, 1);
    OLOG("CreateShaderModule %s%s", s->key, s->dual ? " (second output at location 1)" : "");
    *out = (VkShaderModule)(void *)s; return VK_SUCCESS;
}
VKB(vkDestroyShaderModule, void, VkDevice d, VkShaderModule m, const VkAllocationCallbacks *a) {
    (void)d; (void)a; gshader *s = gs_of(m); if (!s) return;
    vkDestroyShaderModule(hvk.d, s->mod, NULL); if (s->mod_d) vkDestroyShaderModule(hvk.d, s->mod_d, NULL);
    free(s->dual); free(s);
}
// The dual-source form of s (Location 1 → Location 0, Index 1), made once; s's own module when it has none
static VkShaderModule dual_module(gshader *s) {
    if (!s->dual) return s->mod;
    pthread_mutex_lock(&g_mu);   // pipelines may be made on several threads
    if (!s->tried_d) {
        s->tried_d = 1;
        guest_spirv_report rep; size_t nf;
        uint32_t *f = guest_spirv_fix(s->dual, s->ndual, GUEST_SPIRV_DUAL, &nf, &rep);
        if (f && rep.dual_rewritten && module_make(f, nf, NULL, &s->mod_d)) s->mod_d = VK_NULL_HANDLE;
        if (f && f != s->dual) free(f);
        if (!s->mod_d) LOG("[vk] [RED] shader %s: its dual-source form could not be made: the second blend source is undefined", s->key);
        else OLOG("shader %s: dual-source form (output at Location 0, Index 1)", s->key);
    }
    pthread_mutex_unlock(&g_mu);
    return s->mod_d ? s->mod_d : s->mod;
}

// ---------- Pipeline caches and pipelines ----------
// The game's pipeline cache objects hold nothing: their initial data is NVIDIA's Switch binary cache. Its pipelines go through the backend's disk cache instead (vk_cache.c)
VK(vkCreatePipelineCache, VkResult, VkDevice d, const VkPipelineCacheCreateInfo *ci, const VkAllocationCallbacks *a, VkPipelineCache *out) {
    (void)d; (void)a; OLOG("CreatePipelineCache with %zu bytes of initial data: not used (the backend's disk cache serves)", ci->initialDataSize);
    *out = (VkPipelineCache)zalloc(16); return VK_SUCCESS;
}
VK(vkDestroyPipelineCache, void, VkDevice d, VkPipelineCache c, const VkAllocationCallbacks *a) { (void)d; (void)a; free((void *)c); }
VK(vkGetPipelineCacheData, VkResult, VkDevice d, VkPipelineCache c, size_t *n, void *data) { (void)d; (void)c; (void)data; *n = 0; return VK_SUCCESS; }
VK(vkMergePipelineCaches, VkResult, VkDevice d, VkPipelineCache dst, uint32_t n, const VkPipelineCache *src) { (void)d; (void)dst; (void)n; (void)src; return VK_SUCCESS; }
static int src1_factor(VkBlendFactor f) { return f >= VK_BLEND_FACTOR_SRC1_COLOR && f <= VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA; }
static int dual_source(const VkPipelineColorBlendStateCreateInfo *cb) {   // a blend factor reads the second source
    for (uint32_t i = 0; cb && i < cb->attachmentCount; i++) {
        const VkPipelineColorBlendAttachmentState *b = &cb->pAttachments[i];
        if (b->blendEnable && (src1_factor(b->srcColorBlendFactor) || src1_factor(b->dstColorBlendFactor) || src1_factor(b->srcAlphaBlendFactor) || src1_factor(b->dstAlphaBlendFactor))) return 1;
    }
    return 0;
}
static void vertex_formats_check(const VkPipelineVertexInputStateCreateInfo *vi) {   // a vertex format the host cannot fetch (3-component 16-bit formats on some GPUs): said once each
    static VkFormat said[64]; static int nsaid; static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    for (uint32_t i = 0; vi && i < vi->vertexAttributeDescriptionCount; i++) {
        VkFormat f = vi->pVertexAttributeDescriptions[i].format; VkFormatProperties p;
        vkGetPhysicalDeviceFormatProperties(hvk.dev.phys, f, &p);
        if (p.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) continue;
        pthread_mutex_lock(&mu);
        int known = 0; for (int k = 0; k < nsaid; k++) known |= said[k] == f;
        if (!known && nsaid < 64) { said[nsaid++] = f; LOG("[vk] [RED] vertex format %d: the host's driver cannot fetch it (a pipeline reads it: its draws are undefined here)", f); }
        pthread_mutex_unlock(&mu);
    }
}
// A creation through the calling thread's disk cache, with its feedback (vk_gpl.c's create)
static VkResult make_graphics(const VkGraphicsPipelineCreateInfo *ci0, VkPipeline *out) {
    VkGraphicsPipelineCreateInfo ci = *ci0;
    VkPipelineShaderStageCreateInfo st[8];
    if (ci.stageCount > 8) { LOG("[vk] [RED] a pipeline of %u stages", ci.stageCount); *out = VK_NULL_HANDLE; return VK_ERROR_OUT_OF_HOST_MEMORY; }
    int dual = dual_source(ci.pColorBlendState);
    for (uint32_t s = 0; s < ci.stageCount; s++) {
        st[s] = ci0->pStages[s];
        gshader *g = gs_of(st[s].module);
        st[s].module = g ? (st[s].stage == VK_SHADER_STAGE_FRAGMENT_BIT && dual ? dual_module(g) : g->mod) : VK_NULL_HANDLE;
    }
    ci.pStages = st;
    vertex_formats_check(ci.pVertexInputState);
    VkPipelineCreationFeedback fb = {0};
    VkPipelineCreationFeedbackCreateInfo fci = {VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO, (void *)ci.pNext, &fb, 0, NULL};
    ci.pNext = &fci;
    VkPipelineCache pc = hvk_cache_begin();
    VkResult r = vkCreateGraphicsPipelines(hvk.d, pc, 1, &ci, NULL, out);
    hvk_cache_end(&fb, r == VK_SUCCESS);
    if (r) *out = VK_NULL_HANDLE; else atomic_fetch_add(&g_n_pso, 1);
    return r;
}
VKB(vkCreateGraphicsPipelines, VkResult, VkDevice d, VkPipelineCache c, uint32_t n, const VkGraphicsPipelineCreateInfo *ci, const VkAllocationCallbacks *a, VkPipeline *out) {
    (void)d; (void)c; (void)a; VkResult res = VK_SUCCESS;
    for (uint32_t i = 0; i < n; i++) {
        VkResult r = make_graphics(&ci[i], &out[i]);
        if (r) { static _Atomic int k; if (atomic_fetch_add(&k, 1) < 20) LOG("[vk] [RED] the driver refuses graphics pipeline %u of %u (VkResult %d)", i, n, r); if (!res) res = r; }
    }
    return res;
}
VKB(vkCreateComputePipelines, VkResult, VkDevice d, VkPipelineCache c, uint32_t n, const VkComputePipelineCreateInfo *ci, const VkAllocationCallbacks *a, VkPipeline *out) {
    (void)d; (void)c; (void)a; VkResult res = VK_SUCCESS;
    for (uint32_t i = 0; i < n; i++) {
        VkComputePipelineCreateInfo x = ci[i]; gshader *g = gs_of(x.stage.module); x.stage.module = g ? g->mod : VK_NULL_HANDLE;
        VkPipelineCreationFeedback fb = {0};
        VkPipelineCreationFeedbackCreateInfo fci = {VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO, (void *)x.pNext, &fb, 0, NULL};
        x.pNext = &fci;
        VkPipelineCache pc = hvk_cache_begin();
        VkResult r = vkCreateComputePipelines(hvk.d, pc, 1, &x, NULL, &out[i]);
        hvk_cache_end(&fb, r == VK_SUCCESS);
        if (r) { out[i] = VK_NULL_HANDLE; LOG("[vk] [RED] the driver refuses compute pipeline %u of %u (VkResult %d)", i, n, r); if (!res) res = r; }
        else atomic_fetch_add(&g_n_pso, 1);
    }
    return res;
}
VKB(vkDestroyPipeline, void, VkDevice d, VkPipeline p, const VkAllocationCallbacks *a) { (void)d; (void)a; vkDestroyPipeline(hvk.d, p, NULL); }

// ---------- Command pools and buffers: the driver's (their queue family is the shared device's) ----------
VK(vkCreateCommandPool, VkResult, VkDevice d, const VkCommandPoolCreateInfo *ci, const VkAllocationCallbacks *a, VkCommandPool *out) {
    (void)d; (void)a; VkCommandPoolCreateInfo c = *ci; c.queueFamilyIndex = hvk.dev.queue_family;
    return vkCreateCommandPool(hvk.d, &c, NULL, out);
}
VK(vkDestroyCommandPool, void, VkDevice d, VkCommandPool p, const VkAllocationCallbacks *a) { (void)d; (void)a; vkDestroyCommandPool(hvk.d, p, NULL); }
VK(vkResetCommandPool, VkResult, VkDevice d, VkCommandPool p, VkCommandPoolResetFlags f) { (void)d; return vkResetCommandPool(hvk.d, p, f); }
VK(vkTrimCommandPool, void, VkDevice d, VkCommandPool p, VkCommandPoolTrimFlags f) { (void)d; vkTrimCommandPool(hvk.d, p, f); }
VK(vkAllocateCommandBuffers, VkResult, VkDevice d, const VkCommandBufferAllocateInfo *ai, VkCommandBuffer *out) { (void)d; return vkAllocateCommandBuffers(hvk.d, ai, out); }
VK(vkFreeCommandBuffers, void, VkDevice d, VkCommandPool p, uint32_t n, const VkCommandBuffer *cbs) { (void)d; vkFreeCommandBuffers(hvk.d, p, n, cbs); }
VK(vkBeginCommandBuffer, VkResult, VkCommandBuffer cb, const VkCommandBufferBeginInfo *bi) { return vkBeginCommandBuffer(cb, bi); }
VK(vkEndCommandBuffer, VkResult, VkCommandBuffer cb) { return vkEndCommandBuffer(cb); }
VK(vkResetCommandBuffer, VkResult, VkCommandBuffer cb, VkCommandBufferResetFlags f) { return vkResetCommandBuffer(cb, f); }
VK(vkCmdBeginRenderPass, void, VkCommandBuffer cb, const VkRenderPassBeginInfo *bi, VkSubpassContents sc) { atomic_fetch_add_explicit(&g_n_pass, 1, memory_order_relaxed); vkCmdBeginRenderPass(cb, bi, sc); }
VK(vkCmdNextSubpass, void, VkCommandBuffer cb, VkSubpassContents sc) { vkCmdNextSubpass(cb, sc); }
VK(vkCmdEndRenderPass, void, VkCommandBuffer cb) { vkCmdEndRenderPass(cb); }
VK(vkCmdExecuteCommands, void, VkCommandBuffer cb, uint32_t n, const VkCommandBuffer *cbs) { vkCmdExecuteCommands(cb, n, cbs); }
VK(vkCmdBindPipeline, void, VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p) { vkCmdBindPipeline(cb, bp, p); }
VK(vkCmdBindDescriptorSets, void, VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout l, uint32_t first, uint32_t n, const VkDescriptorSet *s, uint32_t ndyn, const uint32_t *dyn) {
    vkCmdBindDescriptorSets(cb, bp, l, first, n, s, ndyn, dyn);
}
VK(vkCmdBindVertexBuffers, void, VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer *b, const VkDeviceSize *o) { vkCmdBindVertexBuffers(cb, first, n, b, o); }
VK(vkCmdBindIndexBuffer, void, VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkIndexType t) { vkCmdBindIndexBuffer(cb, b, o, t); }
VK(vkCmdSetViewport, void, VkCommandBuffer cb, uint32_t first, uint32_t n, const VkViewport *v) { vkCmdSetViewport(cb, first, n, v); }
VK(vkCmdSetScissor, void, VkCommandBuffer cb, uint32_t first, uint32_t n, const VkRect2D *r) { vkCmdSetScissor(cb, first, n, r); }
VK(vkCmdSetLineWidth, void, VkCommandBuffer cb, float w) { vkCmdSetLineWidth(cb, w); }
VK(vkCmdSetDepthBias, void, VkCommandBuffer cb, float c, float cl, float s) { vkCmdSetDepthBias(cb, c, cl, s); }
VK(vkCmdSetBlendConstants, void, VkCommandBuffer cb, const float c[4]) { vkCmdSetBlendConstants(cb, c); }
VK(vkCmdSetDepthBounds, void, VkCommandBuffer cb, float lo, float hi) { vkCmdSetDepthBounds(cb, lo, hi); }
VK(vkCmdSetStencilCompareMask, void, VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t m) { vkCmdSetStencilCompareMask(cb, f, m); }
VK(vkCmdSetStencilWriteMask, void, VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t m) { vkCmdSetStencilWriteMask(cb, f, m); }
VK(vkCmdSetStencilReference, void, VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t r) { vkCmdSetStencilReference(cb, f, r); }
VK(vkCmdPushConstants, void, VkCommandBuffer cb, VkPipelineLayout l, VkShaderStageFlags st, uint32_t off, uint32_t size, const void *v) { vkCmdPushConstants(cb, l, st, off, size, v); }
VK(vkCmdDraw, void, VkCommandBuffer cb, uint32_t count, uint32_t inst, uint32_t first, uint32_t first_inst) {
    atomic_fetch_add_explicit(&g_n_draw, 1, memory_order_relaxed); vkCmdDraw(cb, count, inst, first, first_inst);
}
VK(vkCmdDrawIndexed, void, VkCommandBuffer cb, uint32_t count, uint32_t inst, uint32_t first, int32_t voff, uint32_t first_inst) {
    atomic_fetch_add_explicit(&g_n_draw, 1, memory_order_relaxed); vkCmdDrawIndexed(cb, count, inst, first, voff, first_inst);
}
VK(vkCmdDrawIndirect, void, VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t stride) {
    atomic_fetch_add_explicit(&g_n_draw, 1, memory_order_relaxed); vkCmdDrawIndirect(cb, b, o, n, stride);
}
VK(vkCmdDrawIndexedIndirect, void, VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t stride) {
    atomic_fetch_add_explicit(&g_n_draw, 1, memory_order_relaxed); vkCmdDrawIndexedIndirect(cb, b, o, n, stride);
}
VK(vkCmdDispatch, void, VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z) { vkCmdDispatch(cb, x, y, z); }
VK(vkCmdDispatchIndirect, void, VkCommandBuffer cb, VkBuffer b, VkDeviceSize o) { vkCmdDispatchIndirect(cb, b, o); }
VK(vkCmdCopyBuffer, void, VkCommandBuffer cb, VkBuffer src, VkBuffer dst, uint32_t n, const VkBufferCopy *r) { vkCmdCopyBuffer(cb, src, dst, n, r); }
VK(vkCmdCopyImage, void, VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n, const VkImageCopy *r) { vkCmdCopyImage(cb, src, sl, dst, dl, n, r); }
VK(vkCmdBlitImage, void, VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n, const VkImageBlit *r, VkFilter f) {
    vkCmdBlitImage(cb, src, sl, dst, dl, n, r, f);
}
VK(vkCmdCopyBufferToImage, void, VkCommandBuffer cb, VkBuffer src, VkImage dst, VkImageLayout l, uint32_t n, const VkBufferImageCopy *r) { vkCmdCopyBufferToImage(cb, src, dst, l, n, r); }
VK(vkCmdCopyImageToBuffer, void, VkCommandBuffer cb, VkImage src, VkImageLayout l, VkBuffer dst, uint32_t n, const VkBufferImageCopy *r) { vkCmdCopyImageToBuffer(cb, src, l, dst, n, r); }
VK(vkCmdUpdateBuffer, void, VkCommandBuffer cb, VkBuffer dst, VkDeviceSize o, VkDeviceSize n, const void *data) { vkCmdUpdateBuffer(cb, dst, o, n, data); }
VK(vkCmdFillBuffer, void, VkCommandBuffer cb, VkBuffer dst, VkDeviceSize o, VkDeviceSize n, uint32_t v) { vkCmdFillBuffer(cb, dst, o, n, v); }
VK(vkCmdClearColorImage, void, VkCommandBuffer cb, VkImage im, VkImageLayout l, const VkClearColorValue *c, uint32_t n, const VkImageSubresourceRange *r) {
    vkCmdClearColorImage(cb, im, l, c, n, r);
}
VK(vkCmdClearDepthStencilImage, void, VkCommandBuffer cb, VkImage im, VkImageLayout l, const VkClearDepthStencilValue *c, uint32_t n, const VkImageSubresourceRange *r) {
    vkCmdClearDepthStencilImage(cb, im, l, c, n, r);
}
VK(vkCmdClearAttachments, void, VkCommandBuffer cb, uint32_t na, const VkClearAttachment *a, uint32_t nr, const VkClearRect *r) { vkCmdClearAttachments(cb, na, a, nr, r); }
VK(vkCmdResolveImage, void, VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n, const VkImageResolve *r) { vkCmdResolveImage(cb, src, sl, dst, dl, n, r); }
VK(vkCmdSetEvent, void, VkCommandBuffer cb, VkEvent e, VkPipelineStageFlags st) { vkCmdSetEvent(cb, e, st); }
VK(vkCmdResetEvent, void, VkCommandBuffer cb, VkEvent e, VkPipelineStageFlags st) { vkCmdResetEvent(cb, e, st); }
// Image barriers in the shared device's terms: PRESENT_SRC_KHR as HVK_EXT_LAYOUT (lay above), the game's queue family 0 as the shared queue's family. A copy is made only when
// a barrier changes
static uint32_t qf(uint32_t f) { return f == VK_QUEUE_FAMILY_IGNORED || f == VK_QUEUE_FAMILY_EXTERNAL || f == VK_QUEUE_FAMILY_FOREIGN_EXT ? f : hvk.dev.queue_family; }
static const VkImageMemoryBarrier *img_barriers(uint32_t n, const VkImageMemoryBarrier *b, VkImageMemoryBarrier *local, VkImageMemoryBarrier **heap) {
    *heap = NULL;
    uint32_t i = 0;
    while (i < n && lay(b[i].oldLayout) == b[i].oldLayout && lay(b[i].newLayout) == b[i].newLayout && qf(b[i].srcQueueFamilyIndex) == b[i].srcQueueFamilyIndex &&
           qf(b[i].dstQueueFamilyIndex) == b[i].dstQueueFamilyIndex) i++;
    if (i == n) return b;
    VkImageMemoryBarrier *o = n <= 16 ? local : (*heap = zalloc(n * sizeof *o));
    for (i = 0; i < n; i++) {
        o[i] = b[i]; o[i].oldLayout = lay(o[i].oldLayout); o[i].newLayout = lay(o[i].newLayout);
        o[i].srcQueueFamilyIndex = qf(o[i].srcQueueFamilyIndex); o[i].dstQueueFamilyIndex = qf(o[i].dstQueueFamilyIndex);
    }
    return o;
}
static const VkBufferMemoryBarrier *buf_barriers(uint32_t n, const VkBufferMemoryBarrier *b, VkBufferMemoryBarrier *local, VkBufferMemoryBarrier **heap) {
    *heap = NULL;
    uint32_t i = 0;
    while (i < n && qf(b[i].srcQueueFamilyIndex) == b[i].srcQueueFamilyIndex && qf(b[i].dstQueueFamilyIndex) == b[i].dstQueueFamilyIndex) i++;
    if (i == n) return b;
    VkBufferMemoryBarrier *o = n <= 16 ? local : (*heap = zalloc(n * sizeof *o));
    for (i = 0; i < n; i++) { o[i] = b[i]; o[i].srcQueueFamilyIndex = qf(o[i].srcQueueFamilyIndex); o[i].dstQueueFamilyIndex = qf(o[i].dstQueueFamilyIndex); }
    return o;
}
VK(vkCmdPipelineBarrier, void, VkCommandBuffer cb, VkPipelineStageFlags s, VkPipelineStageFlags d, VkDependencyFlags f, uint32_t nm, const VkMemoryBarrier *m,
   uint32_t nb, const VkBufferMemoryBarrier *b, uint32_t ni, const VkImageMemoryBarrier *i) {
    VkImageMemoryBarrier il[16], *ih; VkBufferMemoryBarrier bl[16], *bh;
    const VkImageMemoryBarrier *ii = img_barriers(ni, i, il, &ih); const VkBufferMemoryBarrier *bb = buf_barriers(nb, b, bl, &bh);
    vkCmdPipelineBarrier(cb, s, d, f, nm, m, nb, bb, ni, ii);
    free(ih); free(bh);
}
VK(vkCmdWaitEvents, void, VkCommandBuffer cb, uint32_t ne, const VkEvent *e, VkPipelineStageFlags s, VkPipelineStageFlags d, uint32_t nm, const VkMemoryBarrier *m,
   uint32_t nb, const VkBufferMemoryBarrier *b, uint32_t ni, const VkImageMemoryBarrier *i) {
    VkImageMemoryBarrier il[16], *ih; VkBufferMemoryBarrier bl[16], *bh;
    const VkImageMemoryBarrier *ii = img_barriers(ni, i, il, &ih); const VkBufferMemoryBarrier *bb = buf_barriers(nb, b, bl, &bh);
    vkCmdWaitEvents(cb, ne, e, s, d, nm, m, nb, bb, ni, ii);
    free(ih); free(bh);
}
VK(vkCmdResetQueryPool, void, VkCommandBuffer cb, VkQueryPool q, uint32_t first, uint32_t n) { vkCmdResetQueryPool(cb, q, first, n); }
VK(vkCmdBeginQuery, void, VkCommandBuffer cb, VkQueryPool q, uint32_t i, VkQueryControlFlags f) { vkCmdBeginQuery(cb, q, i, f); }
VK(vkCmdEndQuery, void, VkCommandBuffer cb, VkQueryPool q, uint32_t i) { vkCmdEndQuery(cb, q, i); }
VK(vkCmdWriteTimestamp, void, VkCommandBuffer cb, VkPipelineStageFlagBits st, VkQueryPool q, uint32_t i) { vkCmdWriteTimestamp(cb, st, q, i); }
VK(vkCmdCopyQueryPoolResults, void, VkCommandBuffer cb, VkQueryPool q, uint32_t first, uint32_t n, VkBuffer dst, VkDeviceSize o, VkDeviceSize stride, VkQueryResultFlags f) {
    vkCmdCopyQueryPoolResults(cb, q, first, n, dst, o, stride, f);
}

// ---------- Queue, fences, semaphores, events, queries ----------
VKB(vkQueueSubmit, VkResult, VkQueue q, uint32_t n, const VkSubmitInfo *si, VkFence f) {
    (void)q; dm_gfx_enter();   // the gate dm_gfx_suspend closes (hle_nvn.c)
    pthread_mutex_lock(&hvk.queue_mu); VkResult r = vkQueueSubmit(hvk.dev.queue, n, si, f); pthread_mutex_unlock(&hvk.queue_mu);
    dm_gfx_leave();
    atomic_fetch_add(&g_n_submit, 1);
    if (r) LOG("[vk] [RED] vkQueueSubmit failed (VkResult %d)", r);
    return r;
}
VK(vkQueueBindSparse, VkResult, VkQueue q, uint32_t n, const VkBindSparseInfo *bi, VkFence f) {
    (void)q; (void)bi; (void)f; if (n) RED1("sparse binding: no sparse feature is offered, refused");
    return n ? VK_ERROR_FEATURE_NOT_PRESENT : VK_SUCCESS;
}
VKB(vkQueueWaitIdle, VkResult, VkQueue q) { (void)q; pthread_mutex_lock(&hvk.queue_mu); VkResult r = vkQueueWaitIdle(hvk.dev.queue); pthread_mutex_unlock(&hvk.queue_mu); return r; }
VKB(vkDeviceWaitIdle, VkResult, VkDevice d) { (void)d; return h_vkQueueWaitIdle(G_QUEUE); }   // the shared device's one queue
VK(vkCreateFence, VkResult, VkDevice d, const VkFenceCreateInfo *ci, const VkAllocationCallbacks *a, VkFence *out) { (void)d; (void)a; return vkCreateFence(hvk.d, ci, NULL, out); }
VK(vkDestroyFence, void, VkDevice d, VkFence f, const VkAllocationCallbacks *a) { (void)d; (void)a; vkDestroyFence(hvk.d, f, NULL); }
VK(vkResetFences, VkResult, VkDevice d, uint32_t n, const VkFence *f) { (void)d; return vkResetFences(hvk.d, n, f); }
VK(vkGetFenceStatus, VkResult, VkDevice d, VkFence f) { (void)d; return vkGetFenceStatus(hvk.d, f); }
VKB(vkWaitForFences, VkResult, VkDevice d, uint32_t n, const VkFence *fs, VkBool32 all, uint64_t timeout) { (void)d; return vkWaitForFences(hvk.d, n, fs, all, timeout); }
VK(vkCreateSemaphore, VkResult, VkDevice d, const VkSemaphoreCreateInfo *ci, const VkAllocationCallbacks *a, VkSemaphore *out) { (void)d; (void)a; return vkCreateSemaphore(hvk.d, ci, NULL, out); }
VK(vkDestroySemaphore, void, VkDevice d, VkSemaphore s, const VkAllocationCallbacks *a) { (void)d; (void)a; vkDestroySemaphore(hvk.d, s, NULL); }
VK(vkCreateEvent, VkResult, VkDevice d, const VkEventCreateInfo *ci, const VkAllocationCallbacks *a, VkEvent *out) { (void)d; (void)a; return vkCreateEvent(hvk.d, ci, NULL, out); }
VK(vkDestroyEvent, void, VkDevice d, VkEvent e, const VkAllocationCallbacks *a) { (void)d; (void)a; vkDestroyEvent(hvk.d, e, NULL); }
VK(vkGetEventStatus, VkResult, VkDevice d, VkEvent e) { (void)d; return vkGetEventStatus(hvk.d, e); }
VK(vkSetEvent, VkResult, VkDevice d, VkEvent e) { (void)d; return vkSetEvent(hvk.d, e); }
VK(vkResetEvent, VkResult, VkDevice d, VkEvent e) { (void)d; return vkResetEvent(hvk.d, e); }
VK(vkCreateQueryPool, VkResult, VkDevice d, const VkQueryPoolCreateInfo *ci, const VkAllocationCallbacks *a, VkQueryPool *out) {
    (void)d; (void)a; VkResult r = vkCreateQueryPool(hvk.d, ci, NULL, out);
    VLOG("query pool %p: type %d, %u queries", (void *)*out, ci->queryType, ci->queryCount);
    return r;
}
VK(vkDestroyQueryPool, void, VkDevice d, VkQueryPool q, const VkAllocationCallbacks *a) { (void)d; (void)a; vkDestroyQueryPool(hvk.d, q, NULL); }
VKB(vkGetQueryPoolResults, VkResult, VkDevice d, VkQueryPool q, uint32_t first, uint32_t n, size_t size, void *data, VkDeviceSize stride, VkQueryResultFlags fl) {
    (void)d; return vkGetQueryPoolResults(hvk.d, q, first, n, size, data, stride, fl);
}

// ---------- Swapchain and present ----------
// A swapchain: host images of their own device-local memory, registered with the present path; ticket[i] is image i's last present
typedef struct { uint32_t n, next, w, h; VkFormat fmt; VkImage img[4]; uint64_t serial[4], ticket[4]; } gswap;
static gswap *sw_of(VkSwapchainKHR s) { return (gswap *)(void *)s; }
static void sw_free(gswap *sc) {
    for (uint32_t i = 0; i < sc->n; i++) {
        if (!sc->img[i]) continue;
        be_present_wait(sc->ticket[i]); hvk_ext_tex_drop(sc->serial[i]);
        gimg *g = img_del(sc->img[i]);
        vkDestroyImage(hvk.d, sc->img[i], NULL);
        if (g) { host_free(g->own, g->own_size); free(g); }
    }
    free(sc);
}
VKB(vkCreateSwapchainKHR, VkResult, VkDevice d, const VkSwapchainCreateInfoKHR *ci, const VkAllocationCallbacks *a, VkSwapchainKHR *out) {
    (void)d; (void)a;
    gswap *sc = zalloc(sizeof *sc); sc->n = umin(umax(ci->minImageCount, 2), 4); sc->w = ci->imageExtent.width; sc->h = ci->imageExtent.height; sc->fmt = ci->imageFormat;
    for (uint32_t i = 0; i < sc->n; i++) {
        VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, ci->imageFormat, {sc->w, sc->h, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT,
                                VK_IMAGE_TILING_OPTIMAL, ci->imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
        gimg *g = zalloc(sizeof *g); g->swapchain = 1;
        VkResult r = vkCreateImage(hvk.d, &ii, NULL, &sc->img[i]);
        if (!r) r = img_own(sc->img[i], g);
        if (r) {
            LOG("[vk] [RED] swapchain image %u (%ux%u format %d usage %#x): the driver refuses it (VkResult %d)", i, sc->w, sc->h, ci->imageFormat, ii.usage, r);
            if (sc->img[i]) vkDestroyImage(hvk.d, sc->img[i], NULL);
            sc->img[i] = VK_NULL_HANDLE; free(g); sw_free(sc); return r == VK_ERROR_OUT_OF_DEVICE_MEMORY ? r : VK_ERROR_INITIALIZATION_FAILED;
        }
        img_put(sc->img[i], g);
        hvk_ext_image e = {sc->img[i], ci->imageFormat, sc->w, sc->h};
        sc->serial[i] = be_ext_tex(&e);
    }
    VLOG("swap chain %ux%u format %d, %u images (at least %u requested), present mode %d", sc->w, sc->h, ci->imageFormat, sc->n, ci->minImageCount, ci->presentMode);
    *out = (VkSwapchainKHR)(void *)sc; return VK_SUCCESS;
}
VKB(vkDestroySwapchainKHR, void, VkDevice d, VkSwapchainKHR s, const VkAllocationCallbacks *a) { (void)d; (void)a; if (s) sw_free(sw_of(s)); }
VK(vkGetSwapchainImagesKHR, VkResult, VkDevice d, VkSwapchainKHR s, uint32_t *n, VkImage *out) {
    (void)d; gswap *sc = sw_of(s);
    if (!out) { *n = sc->n; return VK_SUCCESS; }
    uint32_t k = umin(*n, sc->n); memcpy(out, sc->img, k * sizeof *out); *n = k; return k < sc->n ? VK_INCOMPLETE : VK_SUCCESS;
}
static uint64_t g_next_acq;   // pacing without a vsync source: the earliest time of the next acquire (one frame period after the previous one)
VKB(vkAcquireNextImageKHR, VkResult, VkDevice d, VkSwapchainKHR s, uint64_t timeout, VkSemaphore sem, VkFence f, uint32_t *idx) {
    (void)d; (void)timeout; gswap *sc = sw_of(s);
    uint32_t i = sc->next++ % sc->n;
    uint64_t period = 1000000000ull / (uint64_t)be_fps_target(), now = now_ns();
    // Paced by the vsync source when the backend has one, as NVN's acquire (hle_nvn.c nvnWindowAcquireTexture): FIFO presents one frame per vsync. A clock of our own drifts
    // against the screen (Super Mario Galaxy 2 on an iPhone: frame-rate.md, "Vulkan acquire and vsync event follow the display link"); headless, the clock is all there is
    if (be_vsync_active() && be_vsync_wait((int)(60 / be_fps_target()), 2 * period)) g_next_acq = now_ns() + period;
    else {
        if (g_next_acq > now) plat_sleep_ns(g_next_acq - now);
        now = now_ns(); g_next_acq = g_next_acq && now < g_next_acq + period ? g_next_acq + period : now + period;
    }
    be_present_wait(sc->ticket[i]);   // its previous present has stopped reading it, so drawing into it again is safe
    if (sem || f) {   // signalled after everything submitted so far, the present's copy of this image among it
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 0, NULL, sem ? 1u : 0u, &sem};
        pthread_mutex_lock(&hvk.queue_mu); VkResult r = vkQueueSubmit(hvk.dev.queue, 1, &si, f); pthread_mutex_unlock(&hvk.queue_mu);
        if (r) { LOG("[vk] [RED] the acquire's signal submission failed (VkResult %d)", r); return r; }
    }
    *idx = i; return VK_SUCCESS;
}
static _Atomic uint64_t g_frames;
static void frame_end(void) {
    static uint64_t pd, pp, ps;
    uint64_t f = atomic_fetch_add(&g_frames, 1) + 1, dr = g_n_draw, pa = g_n_pass, su = g_n_submit;
    if (f <= 3 || f % 300 == 0) {
        VLOG("frame %llu: draws %llu, passes %llu, submits %llu (since the last record, %llu frames); pipelines so far %llu, shaders %llu, memory %.1f MB", (unsigned long long)f,
             (unsigned long long)(dr - pd), (unsigned long long)(pa - pp), (unsigned long long)(su - ps), (unsigned long long)(f <= 3 ? 1 : 300), (unsigned long long)g_n_pso,
             (unsigned long long)g_n_shader, g_mem_bytes / 1048576.0);
        pd = dr; pp = pa; ps = su;
    }
    hvk_cache_tick(plat_now_ns());   // the disk cache is written 10 s after the last of a burst of new pipelines
    dm_guest_frame_tick();
}
VKB(vkQueuePresentKHR, VkResult, VkQueue q, const VkPresentInfoKHR *pi) {
    (void)q; dm_gfx_enter();
    int crop[4] = {0, 0, 0, 0};   // the vi layer crop: Super Mario Galaxy shows the top-left 1280x720 of its 1920x1080 swapchain in handheld mode
    dm_vi_layer_crop(crop);
    pthread_mutex_lock(&g_pres_mu);
    // The game's semaphores waited for in a submission that signals this layer's timeline: the present path waits for that value
    uint64_t v = ++g_tl_val;
    VkPipelineStageFlags stl[16], *st = pi->waitSemaphoreCount <= 16 ? stl : zalloc(pi->waitSemaphoreCount * sizeof *st);
    for (uint32_t i = 0; i < pi->waitSemaphoreCount; i++) st[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkTimelineSemaphoreSubmitInfo ts = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, NULL, 0, NULL, 1, &v};
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, &ts, pi->waitSemaphoreCount, pi->pWaitSemaphores, st, 0, NULL, 1, &g_tl};
    pthread_mutex_lock(&hvk.queue_mu); VkResult r = vkQueueSubmit(hvk.dev.queue, 1, &si, VK_NULL_HANDLE); pthread_mutex_unlock(&hvk.queue_mu);
    if (st != stl) free(st);
    if (r) {   // the value is still reached, so nothing waits for it forever
        LOG("[vk] [RED] the present's submission failed (VkResult %d): the frame is shown without waiting for the game's semaphores", r);
        VkSemaphoreSignalInfo ss = {VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, NULL, g_tl, v}; vkSignalSemaphore(hvk.d, &ss);
    }
    for (uint32_t i = 0; i < pi->swapchainCount; i++) {
        gswap *sc = sw_of(pi->pSwapchains[i]); uint32_t k = pi->pImageIndices[i];
        int fit = crop[2] > 0 && crop[3] > 0 && k < sc->n && (uint32_t)(crop[0] + crop[2]) <= sc->w && (uint32_t)(crop[1] + crop[3]) <= sc->h;
        if (k < sc->n) sc->ticket[k] = be_present_ext(sc->serial[k], 1.0 / be_fps_target(), (void *)g_tl, v, fit ? crop : NULL);
        if (pi->pResults) pi->pResults[i] = k < sc->n ? VK_SUCCESS : VK_ERROR_OUT_OF_DATE_KHR;
    }
    pthread_mutex_unlock(&g_pres_mu);
    dm_gfx_leave();
    frame_end();
    return VK_SUCCESS;
}

// ---------- Function lookup ----------
static void *vk_lookup(const char *name) {
    if (!name || strncmp(name, "vk", 2)) return NULL;
    size_t sz = 0; const hle_ent *t = plat_table_swhle(&sz); size_t ne = t ? sz / sizeof *t : 0;
    void *fn = NULL; size_t n = strlen(name);
    for (int pass = 0; pass < 2 && !fn; pass++) {
        size_t len = pass ? n - 3 : n;
        if (pass && (n < 4 || strcmp(name + n - 3, "KHR"))) break;   // second pass: an extension function promoted to core under the name without KHR
        for (size_t i = 0; i < ne && !fn; i++) if (t[i].name && !strncmp(t[i].name, name, len) && !t[i].name[len]) fn = t[i].fn;
    }
    static const char *seen[512]; static int nseen; static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    int known = 0; for (int i = 0; i < nseen; i++) if (!strcmp(seen[i], name)) { known = 1; break; }
    if (!known && nseen < 512) { seen[nseen++] = strdup(name); VLOG("lookup %s → %s", name, fn ? "host implementation" : "none (returns null)"); }
    pthread_mutex_unlock(&mu);
    return fn;
}
VK(vkGetInstanceProcAddr, PFN_vkVoidFunction, VkInstance i, const char *name) { (void)i; return (PFN_vkVoidFunction)vk_lookup(name); }
VK(vkGetDeviceProcAddr, PFN_vkVoidFunction, VkDevice d, const char *name) { (void)d; return (PFN_vkVoidFunction)vk_lookup(name); }
