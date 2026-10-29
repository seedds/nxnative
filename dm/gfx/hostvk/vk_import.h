// Host memory imported into Vulkan (docs/design/vulkan-backend.md 4.1-4.2): the game's pool storage becomes a VkDeviceMemory with VK_EXT_external_memory_host, with one VkBuffer
// over it, so a guest address inside it is (buffer, offset), and a VkDeviceAddress where the device has buffer device address, as on Apple's unified memory. The GPU reads and writes
// the game's own pages: no copy. What the device demands, and what lavapipe answers, is in dm/gfx/hostvk/README.md ("What lavapipe answers"); dm/gfx/hostvk/tests/t_vk_import.c
// establishes it on each device.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "vk_device.h"

typedef struct {
    void *base;                   // the host range imported: base and bytes are multiples of caps.min_import_alignment
    uint64_t bytes;
    VkDeviceMemory mem;
    VkBuffer buf;                 // over the whole range, with hostvk_pool_usage
    VkDeviceAddress addr;         // the buffer's device address, 0 without buffer device address
    uint32_t type;                // the memory type it was imported as
    VkMemoryPropertyFlags flags;  // that type's properties: HOST_COHERENT means a CPU write reaches the GPU without vkFlushMappedMemoryRanges
} hostvk_import_t;

// The usage of a pool buffer: every way an NVN guest address reaches the GPU (vertex, index, uniform, storage, indirect, copy source and destination, uniform and storage texel
// buffers for buffer textures, transform feedback buffers and counters; conditional rendering and device addresses where the device has them enabled)
VkBufferUsageFlags hostvk_pool_usage(const hostvk_dev *d);
// Import [base, base + bytes) of host memory the caller allocated (and keeps mapped and accessible while the import lives). Returns VK_SUCCESS with *out filled, or:
//   VK_ERROR_INVALID_EXTERNAL_HANDLE when base or bytes is not a multiple of caps.min_import_alignment, bytes is 0 or above caps.max_buffer_size (checked here, before Vulkan
//     is called: the Vulkan calls require it, VUID-VkImportMemoryHostPointerInfoEXT-pHostPointer-01749 and VUID-VkMemoryAllocateInfo-allocationSize-01745), or when the driver
//     refuses the range (vkGetMemoryHostPointerPropertiesEXT or vkAllocateMemory);
//   VK_ERROR_TOO_MANY_OBJECTS when caps.max_allocations allocations are live (VUID-vkAllocateMemory-maxMemoryAllocationCount-04101: the backend must not exceed it; design 4.2's
//     coalesced imports are the answer);
//   another VkResult from the driver. A refused import logs a [RED] line and leaves *out zeroed. Thread-safe as far as Vulkan allows (one device; no shared state but the counter)
VkResult hostvk_import(hostvk_dev *d, void *base, size_t bytes, hostvk_import_t *out);
// Destroy the buffer and free the memory: the GPU must be done with them (design 4.2: after the last submission that uses the import completes). The host range stays the caller's
void hostvk_import_release(hostvk_dev *d, hostvk_import_t *im);

// hostvk_init's probe (vk_device.c): the memory types a page-aligned host allocation can be imported as, the preferred one and whether it is coherent. 0 or a VkResult
VkResult hostvk_import_probe(hostvk_dev *d);
