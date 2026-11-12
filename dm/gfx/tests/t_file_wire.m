// A shared buffer over file-backed pages joins the residency set with its pages already resident, wired through the CPU's fault path (mlock) before the commit, and that wiring
// is given back once the commit has made the pages resident for the GPU. With the guest's memory in files (DM_GUEST_FILE) the commit itself paged the file in from the GPU
// driver's wiring path, and on the M1 iPad that page-in waited for good on an APFS lock held by a launchd thread, itself waiting: the game froze with sound and no picture
// (docs/reference/memory.md, "The residency commit froze over the guest's files"). Anonymous memory is not touched
#include "../nvn/nvn_mtl.m"
#include <mach/mach_vm.h>
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(label, c) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s (line %d)\n", label, __LINE__); } } while (0)
// The smallest and largest user wiring count over [p, p + n)
static void wired(const void *p, size_t n, unsigned *lo, unsigned *hi) {
    mach_vm_address_t a = (mach_vm_address_t)p, e = a + n; *lo = ~0u; *hi = 0;
    while (a < e) {
        mach_vm_size_t sz = 0; natural_t depth = 0; vm_region_submap_info_data_64_t info; mach_msg_type_number_t c = VM_REGION_SUBMAP_INFO_COUNT_64;
        if (mach_vm_region_recurse(mach_task_self(), &a, &sz, &depth, (vm_region_recurse_info_t)&info, &c) || a >= e) break;
        if (info.user_wired_count < *lo) *lo = info.user_wired_count;
        if (info.user_wired_count > *hi) *hi = info.user_wired_count;
        a += sz;
    }
}
static int all_resident(const void *p, size_t n) {
    size_t pg = getpagesize(), np = n / pg; char *v = malloc(np); int ok = !mincore(p, n, v);
    for (size_t i = 0; ok && i < np; i++) ok = v[i] & MINCORE_INCORE;
    free(v); return ok;
}
static int in_set(id<MTLAllocation> b) { pthread_mutex_lock(&g_res_mu); int r = [g_res_objects containsObject:b]; pthread_mutex_unlock(&g_res_mu); return r; }
int main(void) { @autoreleasepool {
    mtl_init();
    size_t pg = getpagesize(), n = 256 * pg;
    char path[] = "/tmp/t_file_wire.XXXXXX"; int fd = mkstemp(path); if (fd < 0) abort(); unlink(path);
    if (ftruncate(fd, (off_t)n)) abort();
    uint8_t *m = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0); if (m == MAP_FAILED) abort();
    memset(m, 0x5a, n / 2);   // half written, half holes
    msync(m, n, MS_SYNC); msync(m, n, MS_INVALIDATE);   // written out and dropped: the next touch is a page-in
    mprotect(m + 16 * pg, 16 * pg, PROT_READ);   // as the write tracking leaves a texture's pages
    unsigned lo, hi;

    id<MTLBuffer> b = [g_dev newBufferWithBytesNoCopy:m length:n options:MTLResourceStorageModeShared deallocator:nil];
    res_add(b);
    wired(m, n, &lo, &hi);
    CHECK("the file pages are user-wired once res_add returns, before the commit", lo == 1 && hi == 1);
    CHECK("and resident: the commit has nothing to page in", all_resident(m, n));
    CHECK("the buffer is in the set", in_set(b));
    res_commit();
    wired(m, n, &lo, &hi);
    CHECK("the commit gives the user wiring back", lo == 0 && hi == 0);
    CHECK("the bytes are the file's", m[0] == 0x5a && m[n - 1] == 0);

    // Wiring someone else holds over the range is left as it was (mlock counts)
    mlock(m, 8 * pg);
    id<MTLBuffer> b2 = [g_dev newBufferWithBytesNoCopy:m length:n options:MTLResourceStorageModeShared deallocator:nil];
    res_add(b2); res_commit();
    wired(m, 8 * pg, &lo, &hi);
    CHECK("an mlock taken outside stays", lo == 1 && hi == 1);
    munlock(m, 8 * pg);

    // Added twice before one commit: wired once, given back once
    id<MTLBuffer> b3 = [g_dev newBufferWithBytesNoCopy:m + 128 * pg length:64 * pg options:MTLResourceStorageModeShared deallocator:nil];
    res_add(b3); res_add(b3);
    wired(m + 128 * pg, 64 * pg, &lo, &hi);
    CHECK("a second res_add of a buffer in the set wires nothing more", lo == 1 && hi == 1);
    res_commit();
    wired(m + 128 * pg, 64 * pg, &lo, &hi);
    CHECK("and the commit leaves nothing wired", lo == 0 && hi == 0);

    // Anonymous memory: no wiring at all
    uint8_t *a = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0); if (a == MAP_FAILED) abort();
    id<MTLBuffer> ab = [g_dev newBufferWithBytesNoCopy:a length:n options:MTLResourceStorageModeShared deallocator:nil];
    res_add(ab);
    wired(a, n, &lo, &hi);
    CHECK("an anonymous range is not wired by res_add", hi == 0);
    res_commit();
    id<MTLBuffer> own = [g_dev newBufferWithLength:n options:MTLResourceStorageModeShared];
    res_add(own); res_commit();
    CHECK("a buffer Metal allocated goes in as before", in_set(own));

    res_remove(b); res_remove(b2); res_remove(b3); res_remove(ab); res_remove(own); res_commit();
    printf("t_file_wire: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
