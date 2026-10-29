// Host memory imported into Vulkan with VK_EXT_external_memory_host (vk_import.h)
#include "plat/plat.h"   // plat_vm_alloc for the probe page
#include "vk_import.h"
#include <stdint.h>
#include <string.h>

VkBufferUsageFlags hostvk_pool_usage(const hostvk_dev *d) {
    VkBufferUsageFlags u = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                           VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                           VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;   // NVN buffer textures point into pools
    if (d->caps.xfb) u |= VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT | VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_COUNTER_BUFFER_BIT_EXT;
    if (d->caps.cond_render) u |= VK_BUFFER_USAGE_CONDITIONAL_RENDERING_BIT_EXT;
    if (d->caps.buffer_device_address) u |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    return u;
}

// The type an import is made as: among the allowed ones, host-visible and coherent first (the CPU's writes reach the GPU with no flush, design 4.1 (2)), then host-visible,
// then any (a non-host-visible import forbids CPU access while it lives: the spec's host-memory-import-non-visible-type, never wanted here). UINT32_MAX when none is allowed
static uint32_t pick_type(const hostvk_dev *d, uint32_t bits) {
    static const VkMemoryPropertyFlags pref[] = {VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0};
    for (size_t k = 0; k < sizeof pref / sizeof pref[0]; k++)
        for (uint32_t i = 0; i < d->mem.memoryTypeCount; i++)
            if ((bits >> i & 1) && (d->mem.memoryTypes[i].propertyFlags & pref[k]) == pref[k]) return i;
    return UINT32_MAX;
}

VkResult hostvk_import_probe(hostvk_dev *d) {
    size_t a = (size_t)d->caps.min_import_alignment, raw_n = a * 2;
    uint8_t *raw = plat_vm_alloc(raw_n);   // aligned to the page (Windows: 64 KiB); twice the alignment so an aligned page lies inside when the device demands more
    if (!raw) return VK_ERROR_OUT_OF_HOST_MEMORY;
    void *page = (void *)(((uintptr_t)raw + a - 1) & ~(uintptr_t)(a - 1));
    VkMemoryHostPointerPropertiesEXT hp = {VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    VkResult r = vkGetMemoryHostPointerPropertiesEXT(d->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, page, &hp);
    plat_vm_release(raw, raw_n);
    if (r != VK_SUCCESS) { hostvk_log("[RED] [hostvk] vkGetMemoryHostPointerPropertiesEXT refused a page of plat_vm_alloc memory (VkResult %d)", r); return r; }
    d->caps.import_type_bits = hp.memoryTypeBits;
    d->caps.import_type = pick_type(d, hp.memoryTypeBits);
    if (d->caps.import_type == UINT32_MAX) { hostvk_log("[RED] [hostvk] host memory can be imported as no memory type"); return VK_ERROR_FEATURE_NOT_PRESENT; }
    d->caps.host_coherent_import = (d->mem.memoryTypes[d->caps.import_type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    return VK_SUCCESS;
}

VkResult hostvk_import(hostvk_dev *d, void *base, size_t bytes, hostvk_import_t *out) {
    memset(out, 0, sizeof *out);
    uint64_t a = d->caps.min_import_alignment;
    if (!a || ((uintptr_t)base & (a - 1)) || (bytes & (a - 1)) || !bytes || bytes > d->caps.max_buffer_size) {
        hostvk_log("[RED] [hostvk] import of %p + 0x%zx refused: the address and the size must be nonzero multiples of the device's %llu bytes (minImportedHostPointerAlignment), "
                   "the size at most %llu (maxBufferSize)", base, bytes, (unsigned long long)a, (unsigned long long)d->caps.max_buffer_size);
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }
    // Reserve an allocation slot first, so two threads cannot both take the last one
    uint32_t live = ++d->allocations;
    if (live > d->caps.max_allocations) {
        d->allocations--;
        hostvk_log("[RED] [hostvk] import of %p + 0x%zx refused: %u device memory allocations are live, the device's limit (maxMemoryAllocationCount): pools need coalesced imports",
                   base, bytes, d->caps.max_allocations);
        return VK_ERROR_TOO_MANY_OBJECTS;
    }
    VkMemoryHostPointerPropertiesEXT hp = {VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    VkResult r = vkGetMemoryHostPointerPropertiesEXT(d->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, base, &hp);
    if (r != VK_SUCCESS) { hostvk_log("[RED] [hostvk] import of %p + 0x%zx: vkGetMemoryHostPointerPropertiesEXT refused it (VkResult %d)", base, bytes, r); goto out_slot; }
    VkExternalMemoryBufferCreateInfo eb = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, NULL, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &eb, 0, bytes, hostvk_pool_usage(d), VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    r = vkCreateBuffer(d->device, &bi, NULL, &out->buf);
    if (r != VK_SUCCESS) { out->buf = VK_NULL_HANDLE; hostvk_log("[RED] [hostvk] import of %p + 0x%zx: vkCreateBuffer failed (VkResult %d)", base, bytes, r); goto out_slot; }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(d->device, out->buf, &req);
    uint32_t type = pick_type(d, hp.memoryTypeBits & req.memoryTypeBits);
    if (type == UINT32_MAX || req.size > bytes) {
        hostvk_log("[RED] [hostvk] import of %p + 0x%zx: no memory type both the pointer (0x%x) and the buffer (0x%x) allow, or the buffer needs 0x%llx bytes", base, bytes,
                   hp.memoryTypeBits, req.memoryTypeBits, (unsigned long long)req.size);
        r = VK_ERROR_INVALID_EXTERNAL_HANDLE;
        goto out_buf;
    }
    VkMemoryAllocateFlagsInfo fl = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, NULL, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, 0};
    VkImportMemoryHostPointerInfoEXT im = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT, d->caps.buffer_device_address ? &fl : NULL,
                                           VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, base};
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &im, bytes, type};
    r = vkAllocateMemory(d->device, &ai, NULL, &out->mem);
    if (r != VK_SUCCESS) { out->mem = VK_NULL_HANDLE; hostvk_log("[RED] [hostvk] import of %p + 0x%zx: vkAllocateMemory refused it (VkResult %d)", base, bytes, r); goto out_buf; }
    r = vkBindBufferMemory(d->device, out->buf, out->mem, 0);
    if (r != VK_SUCCESS) { hostvk_log("[RED] [hostvk] import of %p + 0x%zx: vkBindBufferMemory failed (VkResult %d)", base, bytes, r); goto out_mem; }
    if (d->caps.buffer_device_address) {
        VkBufferDeviceAddressInfo ba = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, NULL, out->buf};
        out->addr = vkGetBufferDeviceAddress(d->device, &ba);
    }
    out->base = base; out->bytes = bytes; out->type = type; out->flags = d->mem.memoryTypes[type].propertyFlags;
    return VK_SUCCESS;
out_mem:
    vkFreeMemory(d->device, out->mem, NULL);
out_buf:
    vkDestroyBuffer(d->device, out->buf, NULL);
out_slot:
    d->allocations--;
    memset(out, 0, sizeof *out);
    return r;
}

void hostvk_import_release(hostvk_dev *d, hostvk_import_t *im) {
    if (!im->mem) return;
    vkDestroyBuffer(d->device, im->buf, NULL);
    vkFreeMemory(d->device, im->mem, NULL);
    d->allocations--;
    memset(im, 0, sizeof *im);
}
