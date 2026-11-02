// The Vulkan backend's device memory allocator (decision D2, taken at step D2: our own, in C; docs/design/vulkan-backend.md 4.2 and 4.3). Drivers limit the live
// VkDeviceMemory objects (maxMemoryAllocationCount: 4,096 on NVIDIA and AMD under Windows) and a game makes thousands of textures and buffers, so the backend's own images and
// buffers are ranges of larger allocations, blocks, made per memory type:
// - a block is 64 MiB in a DEVICE_LOCAL type and 16 MiB in any other (host memory), at most an eighth of its heap (a 256 MiB BAR heap gets 32 MiB blocks), the largest power of two
//   below that, at least 1 MiB; a host-visible block is mapped once, when it is made, and a range's pointer is the block's plus its offset;
// - a resource larger than half a block, one whose driver prefers or requires an allocation of its own (VkMemoryDedicatedRequirements), and one asked for with HVK_MEM_OWN gets a
//   VkDeviceMemory of its own (with VkMemoryDedicatedAllocateInfo): render targets and large textures stay dedicated as drivers prefer;
// - within a block, best fit over the free ranges: each range keeps its kind (free, linear: buffers; optimal: images with optimal tiling), the offset respects the resource's
//   alignment, and a linear range and an optimal one never share a page of bufferImageGranularity (the neighbours of the free range a resource is cut from are checked); in a
//   host-visible type without HOST_COHERENT every range starts and ends on a multiple of nonCoherentAtomSize, so a flush of one never covers a neighbour;
// - a released range joins the free ranges beside it; a block left empty goes back to the driver once HVK_ALLOC_GRACE more submissions have completed (a block emptied and filled
//   again within that time is kept), or at once when the allocation count is at the device's limit;
// - the count: every block and every allocation of its own takes one of caps.max_allocations (hostvk_dev.allocations, which the pool imports, the guest layers and the remaining
//   direct allocations share); a refusal for the count logs a [RED] line with the counts.
// Thread-safe: one lock, a leaf (no other lock of the backend is taken under it). The deferred release of what the GPU may still use is vk_memory.c's (hvk_defer), which calls
// hvk_mem_free when the fence allows. DM_VK_MAX_ALLOCATIONS=<n> lowers caps.max_allocations (vk_backend.c; a test switch, so lavapipe can stand for a limited driver).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "vk_device.h"

typedef struct hvk_mem hvk_mem;   // one allocation: a range of a block, or a VkDeviceMemory of its own

enum {
    HVK_MEM_HOST = 1,   // host-visible and coherent, mapped (hvk_mem_map); without it, device-local where the resource allows it, else any type it allows
    HVK_MEM_OWN = 2,    // a VkDeviceMemory of its own whatever the size
};
enum { HVK_ALLOC_GRACE = 8 };   // submissions an empty block waits before it goes back to the driver

// A buffer with memory, bound: n bytes (16 when 0) with usage (SHADER_DEVICE_ADDRESS dropped without buffer device address); with HVK_MEM_HOST *map is its CPU pointer.
// VK_SUCCESS, or the failure with nothing left made (*buf VK_NULL_HANDLE, *mem NULL)
VkResult hvk_mem_buffer(VkDeviceSize n, VkBufferUsageFlags usage, unsigned flags, VkBuffer *buf, hvk_mem **mem, void **map);
void hvk_mem_buffer_free(VkBuffer buf, hvk_mem *mem);    // now: the GPU is done with it
// Memory for an image the caller made, bound to it: linear 1 for VK_IMAGE_TILING_LINEAR (a linear range), 0 for optimal tiling. VK_SUCCESS, or the failure with the image unbound
VkResult hvk_mem_image(VkImage img, int linear, unsigned flags, hvk_mem **mem);
void hvk_mem_free(hvk_mem *m);                            // the range or the allocation given back (NULL: nothing); its resource is destroyed already, the GPU done with it
void *hvk_mem_map(const hvk_mem *m);                      // the CPU pointer of a host-visible allocation's first byte, NULL otherwise
VkDeviceMemory hvk_mem_handle(const hvk_mem *m);          // the VkDeviceMemory it is in (a block's, or its own)
VkDeviceSize hvk_mem_offset(const hvk_mem *m);            // its offset there
VkDeviceSize hvk_mem_size(const hvk_mem *m);              // its size (the resource's VkMemoryRequirements size)
int hvk_mem_is_own(const hvk_mem *m);                     // 1: a VkDeviceMemory of its own
// A CPU write to a host-visible allocation made visible to the GPU, a GPU write (after its fence) to the CPU: nothing in a HOST_COHERENT type; otherwise the range [off, off + n)
// of the allocation (VK_WHOLE_SIZE: to its end) widened to nonCoherentAtomSize and clamped to the allocation's VkDeviceMemory, flushed or invalidated
void hvk_mem_flush(const hvk_mem *m, VkDeviceSize off, VkDeviceSize n);
void hvk_mem_invalidate(const hvk_mem *m, VkDeviceSize off, VkDeviceSize n);
// The range those calls give Vulkan (offset in the VkDeviceMemory, size), whether the type is coherent or not: what a test checks
void hvk_mem_atom_range(const hvk_mem *m, VkDeviceSize off, VkDeviceSize n, VkDeviceSize *mem_off, VkDeviceSize *mem_n);

// The allocation count against caps.max_allocations, for allocations made outside the allocator (vk_memory.c's pool imports and hvk_buffer_make): take one slot (at the limit,
// empty blocks go back to the driver first); 0 when none is left, with a [RED] line naming what (bytes) and the counts. hvk_alloc_count_give returns it
int hvk_alloc_count_take(const char *what, VkDeviceSize bytes);
void hvk_alloc_count_give(void);
void hvk_alloc_note_live(void);                           // after an allocation counted elsewhere (vk_memory.c's imports): the live count's peak (the statistics)
int hvk_alloc_reclaim(void);                              // every empty block back to the driver now: how many

// The backend's calls: after the device (block sizes per type, one log line); after each completed fence (vk_memory.c hvk_deferred_run: empty blocks past their grace go);
// at shutdown (the statistics line, every block freed: a range still held is a leak, said in red)
void hvk_alloc_init(void);
void hvk_alloc_trim(uint64_t done);
void hvk_alloc_shutdown(void);

// Statistics: blocks and own allocations live and at their peak, bytes in blocks and in use (the resources' sizes) now and at the peak of block bytes, the waste then (bytes in
// blocks not in use: free ranges and the padding of alignment and granularity), own allocations' bytes, ranges and own allocations made and given back, refusals for the count
typedef struct {
    uint32_t blocks, blocks_peak, own, own_peak, empty, live_peak;   // live_peak: hostvk_dev.allocations' peak (every kind) as the allocator, hvk_buffer_make and the pool
                                                                     // imports saw it (the guest layers' own allocations are counted there, not sampled)
    uint64_t block_bytes, block_bytes_peak, used_bytes, used_at_peak, own_bytes, own_bytes_peak;
    uint64_t allocs, frees, refused;
    VkDeviceSize granularity;   // the bufferImageGranularity in use (the device's, or a test's larger one)
} hvk_alloc_stats;
void hvk_alloc_get_stats(hvk_alloc_stats *s);
void hvk_alloc_line(char *out, size_t n);                 // the statistics as the shutdown line gives them
VkDeviceSize hvk_alloc_block_size(uint32_t type);         // a type's block size (0 before hvk_alloc_init)

// Tests (dm/gfx/hostvk/tests/t_vk_alloc.c), before the device comes up: a bufferImageGranularity larger than the device's (0: the device's), so lavapipe's small one does not
// hide the separation of linear and optimal ranges
void hvk_alloc_test_granularity(VkDeviceSize g);
