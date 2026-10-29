// Tests of host-memory import (dm/gfx/hostvk/vk_import.c; docs/design/vulkan-backend.md 4.1-4.2, 10 item 2 "t_vk_import", risk 6 of 11.2), headless, under the validation
// layer with synchronization validation. What it establishes on the device it runs on, printed and summed up in dm/gfx/hostvk/README.md for lavapipe:
//   1. the round trip: 64 KiB of plat_vm_alloc memory imported, written by the CPU after the import, a compute shader (t_vk_import_add1.comp) adds 1 to every dword, the CPU
//      reads the result after the fence (design 4.1 (1)-(3));
//   2. the alignment: an import at the device's minImportedHostPointerAlignment works; a misaligned address or size, a zero size, are refused by hostvk_import before Vulkan is
//      called (VUID-VkImportMemoryHostPointerInfoEXT-pHostPointer-01749, VUID-VkMemoryAllocateInfo-allocationSize-01745: the driver cannot be asked);
//   3. overlapping imports, which design 4.2 needs when adjacent pools share a page. The spec (chapter "Host External Memory", VkImportMemoryHostPointerInfoEXT) says:
//      "Applications can import the same payload into multiple instances of Vulkan and multiple times into a given Vulkan instance. However, implementations may fail to import
//      the same payload multiple times into a given physical device due to platform constraints." and "Importing memory from a particular host pointer may not be possible due to
//      additional platform-specific restrictions beyond the scope of this specification in which case the implementation must fail the memory import operation with the error
//      code VK_ERROR_INVALID_EXTERNAL_HANDLE_KHR." So a second import of the same or an overlapping range may fail, and must then fail with that code: the test accepts either
//      and prints which; when both live, a GPU write through one is read through the other;
//   4. the release order: the first of two overlapping imports released while the second lives, which keeps working;
//   5. the allocation count: 1,000 small adjacent imports against maxMemoryAllocationCount (printed), the cost of an import and a release, and hostvk_import's own refusal at
//      the limit (VUID-vkAllocateMemory-maxMemoryAllocationCount-04101; the spec's note: exceeding it is undefined behaviour, VK_ERROR_TOO_MANY_OBJECTS is not guaranteed);
//   6. coherence: the memory type of an import, and a CPU write after the import and after the command buffer was recorded, before the submit, seen by a GPU copy without
//      vkFlushMappedMemoryRanges when the type is HOST_COHERENT (the spec's "Host Write Ordering Guarantees": a queue submission's first access scope "includes all host writes
//      to mappable device memory that are available to the host memory domain", which coherent memory's writes are); with vkMapMemory and a flush otherwise;
//   7. on Linux, a probe for the recompiler's guest arena (docs/design/migration-status.md, "Decision-changing assumptions"): a MAP_SHARED mapping of a memfd, and two views of
//      one memfd imported separately (aliases of the same pages), printed, not required.
// With no Vulkan at all it skips (t_vk_common.h)
#include "plat/plat.h"
#include "t_vk_common.h"
#include "vk_import.h"
#include <stdint.h>
#include "t_vk_import_add1.h"
#if defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif

static hostvk_dev D;
static char name[512];

// ---------- Host memory: plat_vm_alloc, aligned inside to the device's demand ----------
typedef struct { uint8_t *raw; size_t raw_n; uint8_t *p; size_t n; } hmem;
static int hmem_get(hmem *m, size_t bytes) {
    size_t a = (size_t)D.caps.min_import_alignment;
    m->raw_n = bytes + a; m->n = bytes;
    m->raw = plat_vm_alloc(m->raw_n);
    if (!m->raw) return 0;
    m->p = (uint8_t *)(((uintptr_t)m->raw + a - 1) & ~(uintptr_t)(a - 1));
    return 1;
}
static void hmem_put(hmem *m) { if (m->raw) plat_vm_release(m->raw, m->raw_n); memset(m, 0, sizeof *m); }

// ---------- The GPU side: one command buffer, the add-1 pipeline, a copy ----------
static VkCommandPool g_pool; static VkCommandBuffer g_cb; static VkFence g_fence;
static VkDescriptorSetLayout g_dsl; static VkPipelineLayout g_pl; static VkPipeline g_pipe; static VkDescriptorPool g_dp; static VkDescriptorSet g_ds;
static VkResult gpu_init(void) {
    VkDevice dv = D.device; VkResult r;
    VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, D.queue_family};
    if ((r = vkCreateCommandPool(dv, &pi, NULL, &g_pool))) return r;
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, g_pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    if ((r = vkAllocateCommandBuffers(dv, &ai, &g_cb))) return r;
    VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if ((r = vkCreateFence(dv, &fi, NULL, &g_fence))) return r;
    VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &b};
    if ((r = vkCreateDescriptorSetLayout(dv, &li, NULL, &g_dsl))) return r;
    VkPushConstantRange pc = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 4};
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &g_dsl, 1, &pc};
    if ((r = vkCreatePipelineLayout(dv, &pli, NULL, &g_pl))) return r;
    VkShaderModuleCreateInfo si = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof t_vk_import_add1_spv, t_vk_import_add1_spv};
    VkShaderModule sm;
    if ((r = vkCreateShaderModule(dv, &si, NULL, &sm))) return r;
    VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0,
                                      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main", NULL}, g_pl, VK_NULL_HANDLE, -1};
    r = vkCreateComputePipelines(dv, VK_NULL_HANDLE, 1, &ci, NULL, &g_pipe);
    vkDestroyShaderModule(dv, sm, NULL);
    if (r) return r;
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 1, &ps};
    if ((r = vkCreateDescriptorPool(dv, &dpi, NULL, &g_dp))) return r;
    VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, g_dp, 1, &g_dsl};
    return vkAllocateDescriptorSets(dv, &dai, &g_ds);
}
static void gpu_free(void) {
    VkDevice dv = D.device;
    vkDestroyDescriptorPool(dv, g_dp, NULL); vkDestroyPipeline(dv, g_pipe, NULL); vkDestroyPipelineLayout(dv, g_pl, NULL);
    vkDestroyDescriptorSetLayout(dv, g_dsl, NULL); vkDestroyFence(dv, g_fence, NULL); vkDestroyCommandPool(dv, g_pool, NULL);
}
static void barrier(VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da) {
    VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, NULL, ss, sa, ds, da};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 1, &mb, 0, NULL, 0, NULL};
    vkCmdPipelineBarrier2(g_cb, &di);
}
// A command buffer starts after everything before it (the previous submissions' writes made available and visible to every command)
static void begin(void) {
    vkResetCommandBuffer(g_cb, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    vkBeginCommandBuffer(g_cb, &bi);
    barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
}
// End with the device's writes made visible to the host (design 4.1 (3): the CPU reads them once the fence has completed), submit, wait
static VkResult end_submit_wait(void) {
    barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    vkEndCommandBuffer(g_cb);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &g_cb, 0, NULL};
    VkResult r = vkQueueSubmit(D.queue, 1, &si, g_fence);
    if (r) return r;
    r = vkWaitForFences(D.device, 1, &g_fence, VK_TRUE, 30ull * 1000 * 1000 * 1000);
    vkResetFences(D.device, 1, &g_fence);
    return r;
}
static void cmd_add1(const hostvk_import_t *im, uint32_t n) {
    VkDescriptorBufferInfo bi = {im->buf, 0, (VkDeviceSize)n * 4};
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, g_ds, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &bi, NULL};
    vkUpdateDescriptorSets(D.device, 1, &w, 0, NULL);   // the set is not in use: the previous submission was waited for
    vkCmdBindPipeline(g_cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_pipe);
    vkCmdBindDescriptorSets(g_cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_pl, 0, 1, &g_ds, 0, NULL);
    vkCmdPushConstants(g_cb, g_pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &n);
    vkCmdDispatch(g_cb, (n + 63) / 64, 1, 1);
}
static VkResult add1(const hostvk_import_t *im, uint32_t n) { begin(); cmd_add1(im, n); return end_submit_wait(); }
static void cmd_copy(VkBuffer src, VkDeviceSize so, VkBuffer dst, VkDeviceSize dof, VkDeviceSize n) {
    VkBufferCopy c = {so, dof, n};
    vkCmdCopyBuffer(g_cb, src, dst, 1, &c);
}
static VkResult copy(VkBuffer src, VkDeviceSize so, VkBuffer dst, VkDeviceSize dof, VkDeviceSize n) { begin(); cmd_copy(src, so, dst, dof, n); return end_submit_wait(); }

static void fill(uint8_t *p, size_t bytes, uint32_t mul, uint32_t add) { uint32_t *v = (uint32_t *)p; for (size_t i = 0; i < bytes / 4; i++) v[i] = (uint32_t)i * mul + add; }
static size_t first_bad(const uint8_t *p, size_t bytes, uint32_t mul, uint32_t add) {   // the first dword that is not i * mul + add, or bytes / 4
    const uint32_t *v = (const uint32_t *)p; size_t i = 0;
    for (; i < bytes / 4; i++) if (v[i] != (uint32_t)i * mul + add) break;
    return i;
}
static const char *res(VkResult r) {
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_ERROR_INVALID_EXTERNAL_HANDLE: return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
    default: { static char b[32]; snprintf(b, sizeof b, "VkResult %d", r); return b; }
    }
}
static int clean_refusal(VkResult r) { return r == VK_ERROR_INVALID_EXTERNAL_HANDLE || r == VK_ERROR_OUT_OF_DEVICE_MEMORY || r == VK_ERROR_OUT_OF_HOST_MEMORY; }
static uint32_t val_msgs(void) { return D.val_errors + D.val_warnings; }

// A CPU write reaches the GPU: nothing for coherent memory, a flush of the whole import otherwise (the import must be mapped for it)
static void cpu_to_gpu(const hostvk_import_t *im) {
    if (im->flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) return;
    void *m = NULL;
    vkMapMemory(D.device, im->mem, 0, VK_WHOLE_SIZE, 0, &m);
    VkMappedMemoryRange mr = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, im->mem, 0, VK_WHOLE_SIZE};
    vkFlushMappedMemoryRanges(D.device, 1, &mr);
    vkUnmapMemory(D.device, im->mem);
}
static void gpu_to_cpu(const hostvk_import_t *im) {
    if (im->flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) return;
    void *m = NULL;
    vkMapMemory(D.device, im->mem, 0, VK_WHOLE_SIZE, 0, &m);
    VkMappedMemoryRange mr = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, im->mem, 0, VK_WHOLE_SIZE};
    vkInvalidateMappedMemoryRanges(D.device, 1, &mr);
    vkUnmapMemory(D.device, im->mem);
}

#if defined(__linux__)
// 7. The recompiler's guest arena may back guest memory with a memfd mapped MAP_SHARED, possibly at two addresses: is such memory importable, and do two views alias on the GPU?
static void probe_memfd(const hostvk_import_t *dst) {
    size_t a = (size_t)D.caps.min_import_alignment, n = 4 * a, pg = (size_t)sysconf(_SC_PAGESIZE);
    if (a > pg || a % pg) { printf("probe: memfd views not tried (the alignment %zu is not the page size %zu)\n", a, pg); return; }
    int fd = memfd_create("t_vk_import", 0);
    if (fd < 0 || ftruncate(fd, (off_t)n)) { printf("probe: memfd_create failed\n"); if (fd >= 0) close(fd); return; }
    uint8_t *v1 = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0), *v2 = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (v1 == MAP_FAILED || v2 == MAP_FAILED) { printf("probe: mmap of the memfd failed\n"); goto out; }
    hostvk_import_t i1, i2;
    uint32_t before = val_msgs();
    VkResult r1 = hostvk_import(&D, v1, n, &i1), r2 = hostvk_import(&D, v2, n, &i2);
    printf("probe: import of a MAP_SHARED memfd view: %s; of a second view of the same memfd: %s\n", res(r1), res(r2));
    snprintf(name, sizeof name, "memfd views: imported, or refused cleanly (%s, %s)", res(r1), res(r2));
    t(name, (r1 == VK_SUCCESS || clean_refusal(r1)) && (r2 == VK_SUCCESS || clean_refusal(r2)) && val_msgs() == before);
    if (r1 == VK_SUCCESS && r2 == VK_SUCCESS) {
        fill(v1, n, 7, 3);   // written through the first view
        cpu_to_gpu(&i1); cpu_to_gpu(&i2);
        VkResult r = add1(&i2, (uint32_t)(n / 4));   // the GPU adds through the second view's import
        gpu_to_cpu(&i1);
        size_t bad = first_bad(v1, n, 7, 4);
        printf("probe: GPU write through the second view's import, read through the first view: %s\n", bad == n / 4 ? "seen" : "not seen");
        t("memfd views alias on the GPU: a write through one import is read through the other view", r == VK_SUCCESS && bad == n / 4);
        r = copy(i1.buf, 0, dst->buf, 0, n);   // and the GPU reads it through the first view's import
        gpu_to_cpu(dst);
        t("a GPU copy through the first view's import reads it", r == VK_SUCCESS && first_bad(dst->base, n, 7, 4) == n / 4);
    }
    if (r1 == VK_SUCCESS) hostvk_import_release(&D, &i1);
    if (r2 == VK_SUCCESS) hostvk_import_release(&D, &i2);
out:
    if (v1 != MAP_FAILED) munmap(v1, n);
    if (v2 != MAP_FAILED) munmap(v2, n);
    close(fd);
}
#endif

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    char err[1024];
    hostvk_opts o = {-1, 1, 0};
    int rc = init_or_skip("t_vk_import", &o, &D, err, sizeof err);
    snprintf(name, sizeof name, "hostvk_init (rc %d%s%s)", rc, err[0] ? ": " : "", err);
    t(name, rc == 0);
    if (rc) { check_validation(&D, "failed init"); return finish("t_vk_import"); }
    const hostvk_caps *c = &D.caps;
    size_t A = (size_t)c->min_import_alignment;
    printf("device '%s', driver '%s'\n", D.props.deviceName, D.driver_info);
    printf("rule: minImportedHostPointerAlignment %zu bytes (the platform layer's page %zu, its allocation granularity %zu)\n", A, plat_page_size(), plat_vm_granularity());
    printf("rule: maxMemoryAllocationCount %u, maxBufferSize %llu, maxStorageBufferRange %u\n", c->max_allocations, (unsigned long long)c->max_buffer_size,
           D.props.limits.maxStorageBufferRange);
    for (uint32_t i = 0; i < D.mem.memoryTypeCount; i++)
        if (c->import_type_bits >> i & 1)
            printf("rule: host memory can be imported as memory type %u (heap %u, flags 0x%x:%s%s%s%s)%s\n", i, D.mem.memoryTypes[i].heapIndex, D.mem.memoryTypes[i].propertyFlags,
                   D.mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ? " DEVICE_LOCAL" : "",
                   D.mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT ? " HOST_VISIBLE" : "",
                   D.mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT ? " HOST_COHERENT" : "",
                   D.mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT ? " HOST_CACHED" : "", i == c->import_type ? ", chosen" : "");
    {
        VkPhysicalDeviceExternalBufferInfo bi = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO, NULL, 0, hostvk_pool_usage(&D), VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
        VkExternalBufferProperties bp = {VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
        vkGetPhysicalDeviceExternalBufferProperties(D.phys, &bi, &bp);
        printf("rule: a pool buffer (usage 0x%x) over a host allocation: external memory features 0x%x (importable %d, dedicated only %d), compatible handle types 0x%x\n",
               bi.usage, bp.externalMemoryProperties.externalMemoryFeatures,
               !!(bp.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT),
               !!(bp.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT), bp.externalMemoryProperties.compatibleHandleTypes);
    }
    VkResult r = gpu_init();
    snprintf(name, sizeof name, "compute pipeline from the committed SPIR-V (%s)", res(r));
    t(name, r == VK_SUCCESS);
    if (r) { hostvk_destroy(&D); return finish("t_vk_import"); }

    // ---- 1. Round trip over 64 KiB ----
    enum { K64 = 64 * 1024 };
    hmem m; t("64 KiB of plat_vm_alloc memory", hmem_get(&m, K64));
    fill(m.p, K64, 3, 7);   // before the import: what a copy-on-import driver would freeze
    hostvk_import_t a;
    r = hostvk_import(&D, m.p, K64, &a);
    snprintf(name, sizeof name, "import 64 KiB at the device's alignment (%s): memory type %u, flags 0x%x, device address 0x%llx", res(r), a.type, a.flags,
             (unsigned long long)a.addr);
    t(name, r == VK_SUCCESS && a.mem && a.buf && (!c->buffer_device_address || a.addr));
    t("the import's type is host-visible", (a.flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0);
    t("one live allocation", D.allocations == 1);
    fill(m.p, K64, 5, 11);   // after the import
    cpu_to_gpu(&a);
    r = add1(&a, K64 / 4);
    gpu_to_cpu(&a);
    size_t bad = first_bad(m.p, K64, 5, 12);
    snprintf(name, sizeof name, "the shader added 1 to the CPU's post-import values, and the CPU sees it after the fence (%s, first wrong dword %zu of %d)", res(r), bad, K64 / 4);
    t(name, r == VK_SUCCESS && bad == K64 / 4);

    if (D.validation) {   // synchronization validation is on: two copies into the same bytes with no barrier between them are a hazard it reports
        uint32_t e0 = D.val_errors, w0 = D.val_warnings;
        printf("expected: a synchronization hazard (write after write) between two copies with no barrier\n");
        begin();
        cmd_copy(a.buf, 0, a.buf, 32768, 16384);
        cmd_copy(a.buf, 0, a.buf, 32768, 16384);
        r = end_submit_wait();
        snprintf(name, sizeof name, "synchronization validation reports the hazard (%u messages)", (unsigned)(D.val_errors + D.val_warnings - e0 - w0));
        t(name, D.val_errors + D.val_warnings > e0 + w0);
        D.val_errors = e0; D.val_warnings = w0;
        gpu_to_cpu(&a);
        t("the copies ran", r == VK_SUCCESS && !memcmp(m.p + 32768, m.p, 16384));
        fill(m.p, K64, 5, 12);   // back to what the checks below expect
    }

    // ---- 2. Alignment ----
    uint32_t before = val_msgs();
    hostvk_import_t x;
    if (A >= 8) {
        r = hostvk_import(&D, m.p + A / 2, A, &x);
        snprintf(name, sizeof name, "misaligned address (alignment / 2) refused cleanly (%s)", res(r));
        t(name, r == VK_ERROR_INVALID_EXTERNAL_HANDLE && !x.mem);
    }
    r = hostvk_import(&D, m.p, A + 64, &x);
    snprintf(name, sizeof name, "misaligned size (alignment + 64) refused cleanly (%s)", res(r));
    t(name, r == VK_ERROR_INVALID_EXTERNAL_HANDLE && !x.mem);
    r = hostvk_import(&D, m.p, 0, &x);
    t("zero size refused cleanly", r == VK_ERROR_INVALID_EXTERNAL_HANDLE && !x.mem);
    t("the refusals reached no Vulkan call: no validation message, no allocation", val_msgs() == before && D.allocations == 1);

    // ---- 3. Overlapping imports while the first lives ----
    hostvk_import_t b, cc;
    before = val_msgs();
    VkResult rb = hostvk_import(&D, m.p + 16 * 1024, 32 * 1024, &b);   // [16 KiB, 48 KiB) of the 64 KiB already imported
    VkResult rc2 = hostvk_import(&D, m.p, K64, &cc);                   // the same 64 KiB again
    printf("rule: an import overlapping a live one: %s; the same range imported twice: %s\n", res(rb), res(rc2));
    snprintf(name, sizeof name, "overlapping imports: made, or refused with the spec's code (%s, %s)", res(rb), res(rc2));
    t(name, (rb == VK_SUCCESS || clean_refusal(rb)) && (rc2 == VK_SUCCESS || clean_refusal(rc2)) && val_msgs() == before);
    hmem e; t("a second 64 KiB region", hmem_get(&e, K64));
    hostvk_import_t ei;
    r = hostvk_import(&D, e.p, K64, &ei);
    t("its import", r == VK_SUCCESS);
    if (rb == VK_SUCCESS && r == VK_SUCCESS) {
        r = add1(&b, 32 * 1024 / 4);   // the GPU writes through the overlapping import: dwords 4096 to 12287 of the region
        gpu_to_cpu(&b);
        const uint32_t *v = (const uint32_t *)m.p;
        int ok = r == VK_SUCCESS;
        for (uint32_t i = 0; i < K64 / 4 && ok; i++) ok = v[i] == i * 5 + 12 + (i >= 4096 && i < 12288);
        t("a GPU write through the overlapping import is seen by the CPU in the shared pages", ok);
        r = copy(a.buf, 0, ei.buf, 0, K64);   // and read on the GPU through the first import
        gpu_to_cpu(&ei);
        t("a GPU copy through the first import reads what the GPU wrote through the overlapping one", r == VK_SUCCESS && !memcmp(e.p, m.p, K64));
    }
    // ---- 4. Release order: the first import goes while the overlapping one lives ----
    hostvk_import_release(&D, &a);
    if (rc2 == VK_SUCCESS) hostvk_import_release(&D, &cc);
    if (rb == VK_SUCCESS) {
        r = add1(&b, 32 * 1024 / 4);
        gpu_to_cpu(&b);
        const uint32_t *v = (const uint32_t *)m.p;
        int ok = r == VK_SUCCESS;
        for (uint32_t i = 4096; i < 12288 && ok; i++) ok = v[i] == i * 5 + 14;
        t("after the overlapping imports are released, the remaining one still works", ok);
        hostvk_import_release(&D, &b);
    }
    t("every import of the region released: the allocation count is back to the second region's", D.allocations == 1);
    snprintf(name, sizeof name, "no validation message from overlapping imports and their release (%u)", val_msgs() - before);
    t(name, val_msgs() == before);

    // ---- 5. 1,000 small imports, and the refusal at the limit ----
    enum { NSMALL = 1000 };
    hmem s; t("1,000 alignment-sized regions in one allocation", hmem_get(&s, (size_t)NSMALL * A));
    static hostvk_import_t small[NSMALL];
    int made = 0;
    uint64_t t0 = plat_now_ns();
    for (int i = 0; i < NSMALL; i++) if (hostvk_import(&D, s.p + (size_t)i * A, A, &small[i]) == VK_SUCCESS) made++;
    uint64_t t1 = plat_now_ns();
    printf("rule: %d of %d imports of %zu bytes made in %.1f us each; maxMemoryAllocationCount %u\n", made, NSMALL, A, (double)(t1 - t0) / 1000.0 / NSMALL, c->max_allocations);
    snprintf(name, sizeof name, "1,000 adjacent imports (%d made, %u allocations live)", made, (unsigned)D.allocations);
    t(name, made == (c->max_allocations >= NSMALL + 1 ? NSMALL : (int)c->max_allocations - 1) && D.allocations == (uint32_t)made + 1);
    if (made == NSMALL) {
        fill(s.p + (size_t)(NSMALL - 1) * A, A, 1, 100);
        cpu_to_gpu(&small[NSMALL - 1]);
        r = add1(&small[NSMALL - 1], (uint32_t)(A / 4));
        gpu_to_cpu(&small[NSMALL - 1]);
        t("the last of them works", r == VK_SUCCESS && first_bad(s.p + (size_t)(NSMALL - 1) * A, A, 1, 101) == A / 4);
    }
    uint32_t saved = D.caps.max_allocations;
    D.caps.max_allocations = D.allocations + 1;   // one more fits, then hostvk_import refuses before Vulkan is asked
    hmem l; t("two more alignment-sized regions", hmem_get(&l, 2 * A));
    hostvk_import_t y, z;
    before = val_msgs();
    VkResult ry = hostvk_import(&D, l.p, A, &y), rz = hostvk_import(&D, l.p + A, A, &z);
    snprintf(name, sizeof name, "at the allocation limit: the last slot taken (%s), the next refused (%s)", res(ry), res(rz));
    t(name, ry == VK_SUCCESS && rz == VK_ERROR_TOO_MANY_OBJECTS && !z.mem && val_msgs() == before);
    if (ry == VK_SUCCESS) hostvk_import_release(&D, &y);
    hmem_put(&l);
    D.caps.max_allocations = saved;
    t0 = plat_now_ns();
    for (int i = 0; i < NSMALL; i++) hostvk_import_release(&D, &small[i]);
    t1 = plat_now_ns();
    printf("rule: %d releases in %.1f us each\n", made, made ? (double)(t1 - t0) / 1000.0 / made : 0.0);
    t("all released: one allocation live", D.allocations == 1);
    hmem_put(&s);

    // ---- 6. Coherence: a CPU write after the import and after recording, before the submit, seen by a GPU copy ----
    hmem src; t("a source region", hmem_get(&src, K64));
    hostvk_import_t si;
    fill(src.p, K64, 9, 1);
    r = hostvk_import(&D, src.p, K64, &si);
    t("its import", r == VK_SUCCESS);
    int coherent = (si.flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    printf("rule: the import's memory type %u is %s\n", si.type, coherent ? "HOST_COHERENT: no flush is needed" : "not HOST_COHERENT: CPU writes need vkFlushMappedMemoryRanges");
    if (r == VK_SUCCESS) {
        memset(e.p, 0, K64);
        fill(src.p, K64, 9, 2);       // after the import
        begin(); cmd_copy(si.buf, 0, ei.buf, 0, K64);
        fill(src.p, K64, 9, 3);       // after recording, before the submit: the queue submission orders it before the copy
        cpu_to_gpu(&si);              // nothing when coherent
        r = end_submit_wait();
        gpu_to_cpu(&ei);
        snprintf(name, sizeof name, "the GPU copy sees the CPU's last write before the submit %s (%s)", coherent ? "with no flush" : "after a flush", res(r));
        t(name, r == VK_SUCCESS && first_bad(e.p, K64, 9, 3) == K64 / 4);
        hostvk_import_release(&D, &si);
    }
    hmem_put(&src);

#if defined(__linux__)
    // ---- 7. The guest arena's memory kinds (Linux): memfd views ----
    probe_memfd(&ei);
#else
    printf("probe: memfd views are Linux only; Windows file-mapping views (MapViewOfFile3) are not probed yet\n");
#endif

    hostvk_import_release(&D, &ei);
    hmem_put(&e);
    hmem_put(&m);
    t("no allocation left", D.allocations == 0);
    gpu_free();
    hostvk_destroy(&D);
    check_validation(&D, "the whole run");
    return finish("t_vk_import");
}
