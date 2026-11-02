// The Vulkan backend's device memory allocator (dm/gfx/hostvk/vk_alloc.c, decision D2) and the pools' shared imports (vk_memory.c; docs/design/vulkan-backend.md 4.2, step D2),
// on a real device (lavapipe in CTest, under the validation layer: t_vk_harness.h). The test lowers the allocation limit to 64 (DM_VK_MAX_ALLOCATIONS, set before the device comes
// up; lavapipe's own maxMemoryAllocationCount has no practical limit) and raises bufferImageGranularity to 64 KiB (hvk_alloc_test_granularity: lavapipe's is small, which would hide
// the separation of linear and optimal ranges). Checks:
// - 300 shaders loaded through the shader path (hvk_shader_by_key, from the test's library and shader map) each get their 64 KiB constant buffer (vk_shader.c c1_make) as a
//   range of a block, not an allocation of their own: the live allocations stay under the limit, the constants are where the reflection put them;
// - 400 buffers (host-visible and device-local, 256 B to 64 KiB) and 200 optimal-tiling images made under the limit, where one allocation each would have stopped at 64: none
//   refused, the live allocations never above the limit, ranges inside their VkDeviceMemory without overlap, each offset a multiple of the resource's alignment, and no linear
//   range and optimal range sharing a page of the granularity;
// - a host-visible range written through its pointer, copied by the GPU into an optimal image and back into another range, read back equal; the flush range of a range widened
//   to nonCoherentAtomSize;
// - release and reuse: a range given back between two others is what the next resource of its size and kind gets, the same bytes;
// - an empty block stays for HVK_ALLOC_GRACE - 1 submissions and goes back to the driver at the next;
// - allocations of their own: a buffer larger than half a block, an image asked for with HVK_MEM_OWN; own allocations up to the limit, the next refused (a [RED] line with the
//   counts), and given back;
// - 300 memory pools in committed guest memory (be_guest_map), made through the frontend: once the live allocations reach half the limit, the pools of a 64 MiB granule share an
//   import; every pool addressed by the GPU (a fill through its buffer, read in guest memory); a decommit between pools moves the pools of that shared import to new imports, which
//   the GPU addresses; finalized, every import is released;
// - the statistics, and validation.
#include "t_vk_harness.h"
#include "vk_backend.h"
#include "shaders/gen/q_val0.h"   // a module of the backend's own (its counter kernel): the synthetic shaders' code

void hvk_sub_begin(void);
uint64_t hvk_sub_commit(void);
void hvk_finish(void);

static VkDeviceSize page_of(VkDeviceSize off, VkDeviceSize g) { return off / g; }
typedef struct { VkDeviceMemory mem; VkDeviceSize off, size, align; int optimal; } rng;
static int rng_cmp(const void *a, const void *b) {
    const rng *x = a, *y = b;
    if (x->mem != y->mem) return (uintptr_t)x->mem < (uintptr_t)y->mem ? -1 : 1;
    return x->off < y->off ? -1 : x->off > y->off;
}
static uint32_t live(void) { return hvk_device()->allocations; }
static uint32_t g_live_max;
static void note_live(void) { if (live() > g_live_max) g_live_max = live(); }
static void submissions(int n) { for (int i = 0; i < n; i++) { hvk_sub_begin(); hvk_sub_commit(); } hvk_finish(); }
static void to_general(VkCommandBuffer cb, VkImage img) {
    VkImageMemoryBarrier2 ib = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED,
                                VK_QUEUE_FAMILY_IGNORED, img, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 0, NULL, 0, NULL, 1, &ib};
    vkCmdPipelineBarrier2(cb, &di);
}
static VkResult image_new(uint32_t side, unsigned flags, VkImage *img, hvk_mem **mem) {
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {side, side, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT,
                            VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE,
                            0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    *mem = NULL;
    VkResult r = vkCreateImage(hvk.d, &ci, NULL, img);
    if (r) { *img = VK_NULL_HANDLE; return r; }
    r = hvk_mem_image(*img, 0, flags, mem);
    if (r) { vkDestroyImage(hvk.d, *img, NULL); *img = VK_NULL_HANDLE; }
    return r;
}

enum { NBUF = 400, NIMG = 200, NPOOL = 300 };
static VkBuffer g_b[NBUF]; static hvk_mem *g_bm[NBUF];
static VkImage g_i[NIMG]; static hvk_mem *g_im[NIMG];
static NVNmemoryPool g_pools[NPOOL];

int main(void) {
    setenv("DM_VK_MAX_ALLOCATIONS", "64", 0);   // before the device comes up (vk_backend.c reads it there)
    const VkDeviceSize G = 64 << 10;
    hvk_alloc_test_granularity(G);
    hv_up("t_vk_alloc");
    const hostvk_dev *d = hvk_device();
    const uint32_t L = d->caps.max_allocations;
    char name[300];
    snprintf(name, sizeof name, "the allocation limit lowered to %u (the device's maxMemoryAllocationCount %u)", L, d->caps.device_max_allocations);
    t(name, L == 64 && d->caps.device_max_allocations > L);
    hvk_alloc_stats st0; hvk_alloc_get_stats(&st0);
    printf("bufferImageGranularity %llu (the allocator's %llu), nonCoherentAtomSize %llu; %u memory types, type 0 block %llu MiB; %u allocations live after bring-up\n",
           (unsigned long long)d->props.limits.bufferImageGranularity, (unsigned long long)st0.granularity, (unsigned long long)d->props.limits.nonCoherentAtomSize,
           d->mem.memoryTypeCount, (unsigned long long)(hvk_alloc_block_size(0) >> 20), live());
    t("the allocator keeps linear and optimal ranges 64 KiB apart (the test's granularity)", st0.granularity == G);
    {   // what the driver says of allocations of their own for a render target and a large buffer (printed: the README's table)
        VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {1920, 1080, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT,
                                VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 64u << 20, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
        VkImage img; VkBuffer buf;
        if (!vkCreateImage(hvk.d, &ci, NULL, &img) && !vkCreateBuffer(hvk.d, &bi, NULL, &buf)) {
            VkMemoryDedicatedRequirements di = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS}, db = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
            VkMemoryRequirements2 ri = {VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, &di}, rb = {VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, &db};
            VkImageMemoryRequirementsInfo2 ii = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, NULL, img};
            VkBufferMemoryRequirementsInfo2 bb = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2, NULL, buf};
            vkGetImageMemoryRequirements2(hvk.d, &ii, &ri); vkGetBufferMemoryRequirements2(hvk.d, &bb, &rb);
            printf("driver: a 1920x1080 RGBA8 render target needs %llu bytes aligned to %llu, prefers / requires its own allocation %u / %u; a 64 MiB storage buffer %u / %u\n",
                   (unsigned long long)ri.memoryRequirements.size, (unsigned long long)ri.memoryRequirements.alignment, di.prefersDedicatedAllocation, di.requiresDedicatedAllocation,
                   db.prefersDedicatedAllocation, db.requiresDedicatedAllocation);
            vkDestroyImage(hvk.d, img, NULL); vkDestroyBuffer(hvk.d, buf, NULL);
        }
    }
    g_live_max = live();

    // ---- 0. Shaders' constant buffers (vk_shader.c c1_make: 64 KiB each, host-visible) through the shader path: NSH shaders written into the test's library and shader map
    // (t_vk_harness.h's folder; the module is the backend's own counter kernel q_val0, the reflection a compute shader's with a c1 segment of 768 bytes, as the largest of the
    // libraries so far), each loaded by its key as a prewarm row loads one (hvk_shader_by_key) ----
    {
        enum { NSH = 300, C1 = 768 };
        int written = 0;
        FILE *map = fopen(g_map, "a");
        static char json[2 * C1 + 400];
        for (int i = 0; i < NSH && map; i++) {
            char key[17], p[900]; snprintf(key, sizeof key, "a110c%011x", (unsigned)i);
            snprintf(p, sizeof p, "%s/%s.spv", g_libv, key);
            FILE *f = fopen(p, "wb"); if (!f) break;
            fwrite(q_val0_spv, 1, sizeof q_val0_spv, f); fclose(f);
            int k = snprintf(json, sizeof json, "{\"key\":\"%s\",\"target\":\"vk\",\"chain\":\"%s\",\"stage\":\"cs\",\"entry\":\"main\",\"threadgroup_size\":[32,1,1],\"bindings\":[],\"c1\":\"",
                             key, DMSH_CHAIN_VK);
            for (int b = 0; b < C1; b++) k += snprintf(json + k, sizeof json - (size_t)k, "%02x", (unsigned)((i + b) & 0xff));
            snprintf(json + k, sizeof json - (size_t)k, "\"}");
            snprintf(p, sizeof p, "%s/%s.json", g_libv, key);
            if (!(f = fopen(p, "w"))) break;
            fputs(json, f); fclose(f);
            fprintf(map, "alloc\t%d\tcompute\t%s\tc1 test %d\n", i, key, i);
            written++;
        }
        if (map) fclose(map);
        uint32_t live0 = live(); int loaded = 0, with_c1 = 0, own_c1 = 0, exact = 0;
        VkDeviceMemory mems[8]; int nmem = 0;
        for (int i = 0; i < written; i++) {
            char key[17]; snprintf(key, sizeof key, "a110c%011x", (unsigned)i);
            int stale = 0; hvk_shader *sh = hvk_shader_by_key(key, &stale);
            note_live();
            if (!sh) continue;
            loaded++;
            if (!sh->c1buf || !sh->c1mem) continue;
            with_c1++; own_c1 += hvk_mem_is_own(sh->c1mem);
            const uint8_t *m = hvk_mem_map(sh->c1mem);
            int ok = m && hvk_mem_size(sh->c1mem) >= 65536 && m[C1] == 0 && m[65535] == 0;
            for (int b = 0; ok && b < C1; b++) ok = m[b] == (uint8_t)((i + b) & 0xff);
            exact += ok;
            VkDeviceMemory h = hvk_mem_handle(sh->c1mem); int seen = 0;
            for (int j = 0; j < nmem; j++) seen |= mems[j] == h;
            if (!seen && nmem < 8) mems[nmem++] = h;
        }
        snprintf(name, sizeof name, "%d shaders loaded by key with the limit at %u: %d with a 64 KiB constant buffer, %d of them an allocation of its own", loaded, L, with_c1, own_c1);
        t(name, written == NSH && loaded == NSH && with_c1 == NSH && !own_c1);
        snprintf(name, sizeof name, "their constant buffers in %d blocks; %u allocations live (%u before them); the constants where the reflection put them, zeros after: %d of %d",
                 nmem, live(), live0, exact, with_c1);
        t(name, nmem >= 1 && nmem <= 2 && live() <= live0 + 1 && g_live_max <= L && exact == with_c1);
    }

    // ---- 1. Many small buffers and images under the limit ----
    static const VkDeviceSize sizes[] = {256, 1000, 4096, 12345, 65536};
    int bad = 0, own = 0;
    for (int i = 0; i < NBUF; i++) {
        VkResult r = hvk_mem_buffer(sizes[i % 5], VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                    i & 1 ? HVK_MEM_HOST : 0, &g_b[i], &g_bm[i], NULL);
        bad += r != VK_SUCCESS; own += r == VK_SUCCESS && hvk_mem_is_own(g_bm[i]); note_live();
    }
    for (int i = 0; i < NIMG; i++) { VkResult r = image_new(16u << (i % 4), 0, &g_i[i], &g_im[i]); bad += r != VK_SUCCESS; own += r == VK_SUCCESS && hvk_mem_is_own(g_im[i]); note_live(); }
    snprintf(name, sizeof name, "%d buffers and %d images made with the limit at %u: %d refused, %d with an allocation of their own", NBUF, NIMG, L, bad, own);
    t(name, !bad && !own);
    snprintf(name, sizeof name, "the live allocations never above the limit (at most %u of %u)", g_live_max, L);
    t(name, g_live_max <= L);
    hvk_alloc_stats st1; hvk_alloc_get_stats(&st1);
    snprintf(name, sizeof name, "%d resources in %u blocks", NBUF + NIMG, st1.blocks);
    t(name, st1.blocks >= 1 && st1.blocks <= 8);
    // every range inside its memory, no two overlapping, each offset aligned; no linear and optimal range on one page of the granularity
    static rng r[NBUF + NIMG]; int n = 0, misaligned = 0;
    for (int i = 0; i < NBUF; i++) {
        VkMemoryRequirements q; vkGetBufferMemoryRequirements(hvk.d, g_b[i], &q);
        r[n++] = (rng){hvk_mem_handle(g_bm[i]), hvk_mem_offset(g_bm[i]), hvk_mem_size(g_bm[i]), q.alignment, 0};
    }
    for (int i = 0; i < NIMG; i++) {
        VkMemoryRequirements q; vkGetImageMemoryRequirements(hvk.d, g_i[i], &q);
        r[n++] = (rng){hvk_mem_handle(g_im[i]), hvk_mem_offset(g_im[i]), hvk_mem_size(g_im[i]), q.alignment, 1};
    }
    for (int i = 0; i < n; i++) misaligned += r[i].align && r[i].off % r[i].align;
    qsort(r, (size_t)n, sizeof *r, rng_cmp);
    int overlap = 0, shared = 0, pairs = 0;
    for (int i = 0; i + 1 < n; i++) overlap += r[i].mem == r[i + 1].mem && r[i].off + r[i].size > r[i + 1].off;
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n && r[j].mem == r[i].mem; j++) {
            if (r[i].optimal == r[j].optimal) continue;
            pairs++;
            shared += page_of(r[i].off + r[i].size - 1, G) >= page_of(r[j].off, G);   // sorted: i's last page must lie before j's first
        }
    snprintf(name, sizeof name, "%d ranges: none overlapping, %d offsets not a multiple of their alignment", n, misaligned);
    t(name, !overlap && !misaligned);
    snprintf(name, sizeof name, "%d pairs of a buffer and an optimal image in one block: %d on a shared 64 KiB page", pairs, shared);
    t(name, pairs > 0 && !shared);

    // ---- 2. A host-visible range written through its pointer, read back through GPU copies ----
    {
        VkBuffer src, dst; hvk_mem *sm, *dm; void *sp, *dp; VkImage img; hvk_mem *imm;
        const uint32_t side = 64; const VkDeviceSize bytes = side * side * 4;
        VkResult a = hvk_mem_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, HVK_MEM_HOST, &src, &sm, &sp);
        VkResult b = hvk_mem_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, HVK_MEM_HOST, &dst, &dm, &dp);
        VkResult c = image_new(side, 0, &img, &imm);
        t("a host-visible source and destination and an optimal image made, ranges of blocks", !a && !b && !c && !hvk_mem_is_own(sm) && !hvk_mem_is_own(dm) && !hvk_mem_is_own(imm));
        if (!a && !b && !c) {
            uint32_t *w = sp; for (uint32_t i = 0; i < side * side; i++) w[i] = i * 2654435761u;
            memset(dp, 0, bytes);
            hvk_mem_flush(sm, 0, VK_WHOLE_SIZE);
            VkCommandBuffer cb; int ok = 0;
            if (hvk_oneoff_begin(&cb)) {
                to_general(cb, img);
                VkBufferImageCopy rg = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {side, side, 1}};
                vkCmdCopyBufferToImage(cb, src, img, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
                hvk_full_barrier(cb);
                vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_GENERAL, dst, 1, &rg);
                hvk_full_barrier(cb);
                ok = hvk_oneoff_end(cb);
            }
            hvk_mem_invalidate(dm, 0, VK_WHOLE_SIZE);
            t("written through the range's pointer, copied into an image and back by the GPU: the same bytes", ok && !memcmp(sp, dp, bytes));
            snprintf(name, sizeof name, "the source's pointer is its block's at offset %llu (a range, not offset 0 of its own memory)", (unsigned long long)hvk_mem_offset(sm));
            t(name, hvk_mem_offset(sm) > 0 || hvk_mem_offset(dm) > 0);
            VkDeviceSize atom = d->props.limits.nonCoherentAtomSize ? d->props.limits.nonCoherentAtomSize : 1, mo, mn;
            hvk_mem_atom_range(sm, 3, 5, &mo, &mn);
            VkDeviceSize lo = hvk_mem_offset(sm) + 3, hi = lo + 5;
            snprintf(name, sizeof name, "a flush of bytes 3 to 8 of the range covers [%llu, %llu) of its memory: multiples of nonCoherentAtomSize %llu around them",
                     (unsigned long long)mo, (unsigned long long)(mo + mn), (unsigned long long)atom);
            t(name, mo % atom == 0 && mo <= lo && mo + mn >= hi && (mn % atom == 0) && mo + atom > lo);
        }
        hvk_mem_buffer_free(src, sm); hvk_mem_buffer_free(dst, dm); if (img) vkDestroyImage(hvk.d, img, NULL); hvk_mem_free(imm);
    }

    // ---- 3. Release and reuse ----
    {
        VkBuffer b[3], b2; hvk_mem *m[3], *m2; void *p[3], *p2; int ok = 1;
        for (int i = 0; i < 3; i++) ok &= !hvk_mem_buffer(200000, VK_BUFFER_USAGE_TRANSFER_DST_BIT, HVK_MEM_HOST, &b[i], &m[i], &p[i]);
        if (ok) {
            VkDeviceMemory was = hvk_mem_handle(m[1]); VkDeviceSize at = hvk_mem_offset(m[1]); void *wp = p[1];
            memset(p[1], 0x5A, 200000);
            hvk_mem_buffer_free(b[1], m[1]);
            ok = !hvk_mem_buffer(200000, VK_BUFFER_USAGE_TRANSFER_DST_BIT, HVK_MEM_HOST, &b2, &m2, &p2);
            snprintf(name, sizeof name, "a range given back between two others (memory %p offset %llu) is the next one of its size: the same bytes", (void *)was, (unsigned long long)at);
            t(name, ok && hvk_mem_handle(m2) == was && hvk_mem_offset(m2) == at && p2 == wp && ((uint8_t *)p2)[199999] == 0x5A);
            if (ok) hvk_mem_buffer_free(b2, m2);
            hvk_mem_buffer_free(b[0], m[0]); hvk_mem_buffer_free(b[2], m[2]);
        } else t("three buffers of 200,000 bytes", 0);
    }

    // ---- 4. An empty block goes back to the driver after its grace ----
    {
        enum { NBIG = 24 };
        VkBuffer b[NBIG]; hvk_mem *m[NBIG]; int nb = 0;
        hvk_alloc_stats s; hvk_alloc_get_stats(&s);
        uint32_t blocks0 = s.blocks, live0 = live();
        while (nb < NBIG) {   // 8 MiB device-local buffers until one lands in a new block
            if (hvk_mem_buffer(8u << 20, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 0, &b[nb], &m[nb], NULL)) break;
            nb++; hvk_alloc_get_stats(&s);
            if (s.blocks > blocks0) break;
        }
        note_live();
        snprintf(name, sizeof name, "%d buffers of 8 MiB: the last in a new block (%u blocks, %u before)", nb, s.blocks, blocks0);
        t(name, s.blocks == blocks0 + 1 && live() == live0 + 1);
        for (int i = 0; i < nb; i++) hvk_mem_buffer_free(b[i], m[i]);
        hvk_alloc_get_stats(&s);
        snprintf(name, sizeof name, "given back, the new block is empty and kept (%u blocks, %u empty)", s.blocks, s.empty);
        t(name, s.blocks == blocks0 + 1 && s.empty == 1);
        submissions(HVK_ALLOC_GRACE - 1);
        hvk_alloc_get_stats(&s);
        snprintf(name, sizeof name, "after %d submissions it is still there (%u blocks)", HVK_ALLOC_GRACE - 1, s.blocks);
        t(name, s.blocks == blocks0 + 1);
        submissions(1);
        hvk_alloc_get_stats(&s);
        snprintf(name, sizeof name, "after %d it went back to the driver (%u blocks, %u allocations live, %u before)", HVK_ALLOC_GRACE, s.blocks, live(), live0);
        t(name, s.blocks == blocks0 && s.empty == 0 && live() == live0);
    }

    // ---- 5. Allocations of their own ----
    {
        uint32_t live0 = live();
        VkBuffer big; hvk_mem *bm; VkImage img; hvk_mem *im;
        VkDeviceSize half = hvk_alloc_block_size(0) / 2;
        VkResult a = hvk_mem_buffer(half + 4096, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 0, &big, &bm, NULL);
        snprintf(name, sizeof name, "a buffer of half a block and 4 KiB (%llu bytes) gets an allocation of its own", (unsigned long long)(half + 4096));
        t(name, !a && hvk_mem_is_own(bm) && hvk_mem_offset(bm) == 0 && live() == live0 + 1);
        VkResult c = image_new(256, HVK_MEM_OWN, &img, &im);
        t("an image asked for with HVK_MEM_OWN gets one too", !c && hvk_mem_is_own(im) && live() == live0 + 2);
        note_live();
        if (!a) hvk_mem_buffer_free(big, bm);
        if (!c) { vkDestroyImage(hvk.d, img, NULL); hvk_mem_free(im); }
        t("both given back: the live allocations as before", live() == live0);
        // own allocations up to the limit: the next is refused for the count, with a red line
        enum { NOWN = 80 };
        VkBuffer ob[NOWN]; hvk_mem *om[NOWN]; int made = 0; VkResult last = VK_SUCCESS;
        hvk_alloc_stats s0; hvk_alloc_get_stats(&s0);
        while (made < NOWN && !(last = hvk_mem_buffer(4096, VK_BUFFER_USAGE_TRANSFER_DST_BIT, HVK_MEM_OWN, &ob[made], &om[made], NULL))) { made++; note_live(); }
        hvk_alloc_stats s1; hvk_alloc_get_stats(&s1);
        snprintf(name, sizeof name, "own allocations up to the limit: %d made, the next refused for the count (VkResult %d), %u live of %u", made, last, live(), L);
        t(name, last == VK_ERROR_TOO_MANY_OBJECTS && live() == L && made == (int)(L - live0) && s1.refused == s0.refused + 1);
        for (int i = 0; i < made; i++) hvk_mem_buffer_free(ob[i], om[i]);
        t("given back: the live allocations as before", live() == live0);
    }

    // the resources of part 1 given back
    for (int i = 0; i < NBUF; i++) hvk_mem_buffer_free(g_b[i], g_bm[i]);
    for (int i = 0; i < NIMG; i++) { vkDestroyImage(hvk.d, g_i[i], NULL); hvk_mem_free(g_im[i]); }
    submissions(HVK_ALLOC_GRACE);
    hvk_alloc_stats s2; hvk_alloc_get_stats(&s2);
    snprintf(name, sizeof name, "everything of the test given back: %u blocks left (%.1f MiB in use: the backend's own)", s2.blocks, (double)s2.used_bytes / 1048576.0);
    t(name, s2.empty == 0 && s2.own == 0);

    // ---- 6. Memory pools in committed guest memory: shared imports ----
    {
        const size_t REGION = 192u << 20, STRIDE = 600u << 10, POOL = 64u << 10;   // 300 pools of 64 KiB, 600 KiB apart: 176 MiB over three or four 64 MiB granules
        uint8_t *base = plat_vm_alloc(REGION);
        t("192 MiB of guest memory committed", base != NULL);
        if (base) {
            be_guest_map(base, REGION);
            hv_device(1);
            uint32_t live0 = live(); int okp = 0;
            for (int i = 0; i < NPOOL; i++) {
                NVNmemoryPoolBuilder b; h_nvnMemoryPoolBuilderSetDefaults(&b); h_nvnMemoryPoolBuilderSetDevice(&b, &g_dev);
                h_nvnMemoryPoolBuilderSetStorage(&b, base + (size_t)i * STRIDE, POOL);
                h_nvnMemoryPoolBuilderSetFlags(&b, NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
                okp += h_nvnMemoryPoolInitialize(&g_pools[i], &b) != 0;
                note_live();
            }
            uint32_t io, is, ip; hvk_import_counts(&io, &is, &ip);
            snprintf(name, sizeof name, "%d pools made (%u allocations live before them, %u after): %u own imports, %u shared by %u pools", okp, live0, live(), io, is, ip);
            t(name, okp == NPOOL && live() <= L && io + is == live() - live0 && ip + io == NPOOL && is >= 3 && is <= 5 && ip >= 250);
            snprintf(name, sizeof name, "the live allocations never above the limit (at most %u of %u)", g_live_max, L);
            t(name, g_live_max <= L);
            // every pool addressed by the GPU: a fill through its import's buffer, read in guest memory
            int miss = 0, wrong = 0;
            for (int pass = 0; pass < 2; pass++) {
                if (pass) {   // a decommit between two pools of a shared import: its pools move to new imports
                    int k = NPOOL - 20;   // a pool late enough to be on a shared import
                    uint64_t before = hvk_buf_import((uint64_t)(uintptr_t)(base + (size_t)k * STRIDE), 4), stay = hvk_buf_import((uint64_t)(uintptr_t)base, 4);
                    uint8_t *gap = base + (size_t)k * STRIDE + POOL;
                    hvk_import_counts(&io, &is, &ip);
                    uint32_t ip0 = ip;
                    be_guest_unmap(gap, STRIDE - POOL);
                    uint64_t after = hvk_buf_import((uint64_t)(uintptr_t)(base + (size_t)k * STRIDE), 4);
                    hvk_import_counts(&io, &is, &ip);
                    snprintf(name, sizeof name, "%zu KiB between pools %d and %d decommitted: their shared import replaced (serial %llu, then %llu), the first pool's own kept; %u of %u "
                             "pools still shared, %u allocations live", (STRIDE - POOL) >> 10, k, k + 1, (unsigned long long)before, (unsigned long long)after, ip, ip0, live());
                    t(name, before && after && after != before && hvk_buf_import((uint64_t)(uintptr_t)base, 4) == stay && ip == ip0 && live() <= L);
                }
                VkCommandBuffer cb; miss = 0; wrong = 0;
                for (int i = 0; i < NPOOL; i++) memset(base + (size_t)i * STRIDE + 16, 0, 4);
                if (!hvk_oneoff_begin(&cb)) { t("a one-off command buffer", 0); break; }
                hvk_full_barrier(cb);
                for (int i = 0; i < NPOOL; i++) {
                    VkBuffer buf; VkDeviceSize off;
                    if (!hvk_buf((uint64_t)(uintptr_t)(base + (size_t)i * STRIDE + 16), 4, &buf, &off, NULL)) { miss++; continue; }
                    vkCmdFillBuffer(cb, buf, off, 4, 0x1000u * (uint32_t)pass + (uint32_t)i);
                }
                VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT};
                VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 1, &mb, 0, NULL, 0, NULL};
                vkCmdPipelineBarrier2(cb, &di);
                int ok = hvk_oneoff_end(cb);
                for (int i = 0; i < NPOOL; i++) { uint32_t v; memcpy(&v, base + (size_t)i * STRIDE + 16, 4); wrong += v != 0x1000u * (uint32_t)pass + (uint32_t)i; }
                snprintf(name, sizeof name, "%s: the GPU fills a word in each of the %d pools through its import: %d without a buffer, %d words wrong", pass ? "after the decommit" : "shared imports",
                         NPOOL, miss, wrong);
                t(name, ok && !miss && !wrong);
            }
            be_guest_map(base + (size_t)(NPOOL - 20) * STRIDE + POOL, STRIDE - POOL);
            for (int i = 0; i < NPOOL; i++) h_nvnMemoryPoolFinalize(&g_pools[i]);
            submissions(1);
            hvk_import_counts(&io, &is, &ip);
            snprintf(name, sizeof name, "every pool finalized: %u own imports, %u shared, %u pools on them; %u allocations live (%u before the pools)", io, is, ip, live(), live0);
            t(name, !io && !is && !ip && live() == live0);
            be_guest_unmap(base, REGION);
            plat_vm_release(base, REGION);
        }
    }

    char line[600]; hvk_alloc_line(line, sizeof line);
    printf("allocator: %s\n", line);
    hvk_alloc_stats s; hvk_alloc_get_stats(&s);
    snprintf(name, sizeof name, "the statistics: peak %u blocks, %llu ranges and own allocations made, %llu given back, %llu refused for the count", s.blocks_peak,
             (unsigned long long)s.allocs, (unsigned long long)s.frees, (unsigned long long)s.refused);
    t(name, s.blocks_peak >= 2 && s.allocs >= NBUF + NIMG && s.refused >= 1 && s.live_peak <= L);
    return hv_finish();
}
