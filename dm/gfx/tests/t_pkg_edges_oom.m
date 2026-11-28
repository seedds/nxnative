// Edge-snapshot allocation failures in pkg_edges_take must not copy into NULL or leave partial edge metadata behind;
// they must drop any partial snapshot, preserve the conservative re-upload fallback, and keep edge-snapshot accounting exact.
#import <Metal/Metal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

static void *edge_test_malloc(size_t n);
static void edge_test_free(void *p);
#define malloc edge_test_malloc
#define free edge_test_free
#include "../nvn/nvn_mtl.m"
#undef malloc
#undef free

void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }

static int checks, failed;
#define EXPECT(name, want, got) do { checks++; uint64_t w_ = (uint64_t)(want), g_ = (uint64_t)(got); if (w_ != g_) { failed++; fprintf(stderr, "%s: expected %llu, got %llu\n", name, (unsigned long long)w_, (unsigned long long)g_); } } while (0)

static int g_arm, g_calls, g_fail_on;
static void *g_live[16];
static int g_live_n;

static void *edge_test_malloc(size_t n) {
    if (g_arm) {
        g_calls++;
        if (g_fail_on && g_calls == g_fail_on) return NULL;
        void *p = malloc(n);
        if (p && g_live_n < 16) g_live[g_live_n++] = p;
        return p;
    }
    return malloc(n);
}

static void edge_test_free(void *p) {
    if (p) {
        for (int i = 0; i < g_live_n; i++) {
            if (g_live[i] == p) { g_live[i] = g_live[--g_live_n]; break; }
        }
    }
    free(p);
}

@interface FakeEdgeBuf : NSObject { @public void *buf; NSUInteger len; } @end
@implementation FakeEdgeBuf
- (void *)contents { return buf; }
- (NSUInteger)length { return len; }
- (NSUInteger)allocatedSize { return len; }
- (id)heap { return nil; }
@end

@interface FakeEdgeDev : NSObject { @public FakeEdgeBuf *next; } @end
@implementation FakeEdgeDev
- (id)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options {
    (void)options;
    FakeEdgeBuf *b = next;
    next = nil;
    if (b && !b->len) b->len = length;
    return b;
}
@end

void crash_install(void);

int main(void) { @autoreleasepool {
    crash_install();
    size_t pg = (size_t)getpagesize();
    size_t pool_len = pg * 6;
    uint8_t *pool = mmap(NULL, pool_len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    EXPECT("pool mapped", 1, pool != MAP_FAILED);
    nvn_dirty_add_range(pool, pool_len);

    // Match #3240 Run 2's geometry on 16 KB pages: start 12 KB into page 0 (edge0 = 4 KB), span 64 KB (edge1 = 12 KB).
    size_t edge0_want = pg / 4;
    size_t edge1_want = pg - edge0_want;
    size_t tex_off = pg - edge0_want;
    size_t tex_size = pg * 4;
    for (size_t i = 0; i < pool_len; i++) pool[i] = (uint8_t)((i * 37 + 11) & 0xff);

    tex_t t;
    memset(&t, 0, sizeof t);
    t.serial = 1916923;
    t.mem = pool + tex_off;
    t.L.size = tex_size;
    t.d.width = 256; t.d.height = 256; t.d.depth = 1; t.d.levels = 1; t.d.format = 0x87;

    uint64_t a0, b0, p0, f0, fb0;
    mtl_pkg_edge_stats(&a0, &b0, &p0, &f0, &fb0);
    EXPECT("initial live edge allocs", 0, a0);
    EXPECT("initial live edge bytes", 0, b0);
    EXPECT("initial edge fail count", 0, f0);

    // 1. Happy path: both edges snapshot, writes beside stay clean, writes inside trigger re-upload.
    {
        NvPkg *pk = [NvPkg new];
        pk->span = nvn_dirty_span(t.mem, t.L.size);
        EXPECT("tracked span", 1, pk->span != NULL);
        pk->stamp = nvn_dirty_now();
        nvn_dirty_protect(t.mem, t.L.size);
        g_arm = 1; g_calls = 0; g_fail_on = 0;
        pkg_edges_take(pk, &t);
        g_arm = 0;
        EXPECT("happy: two mallocs", 2, g_calls);
        EXPECT("happy: two tracked live heap buffers", 2, g_live_n);
        EXPECT("happy: edge0 length", edge0_want, pk->edge_n[0]);
        EXPECT("happy: edge1 length", edge1_want, pk->edge_n[1]);
        EXPECT("happy: edge0 non-null", 1, pk->edge[0] != NULL);
        EXPECT("happy: edge1 non-null", 1, pk->edge[1] != NULL);
        mtl_pkg_edge_stats(&a0, &b0, &p0, &f0, &fb0);
        EXPECT("happy: live allocs", 2, a0);
        EXPECT("happy: live bytes", edge0_want + edge1_want, b0);
        EXPECT("happy: peak bytes", edge0_want + edge1_want, p0);
        EXPECT("happy: zero failures", 0, f0);

        pool[tex_off - 16] ^= 0x55;   // neighbor write in page 0
        EXPECT("happy: write beside edge0 is clean", 1, pkg_edges_clean(pk, &t));
        pool[tex_off + tex_size + 16] ^= 0x55;   // neighbor write in last page
        EXPECT("happy: write beside edge1 is clean", 1, pkg_edges_clean(pk, &t));
        pool[tex_off + tex_size - 16] ^= 0xAA;   // write inside edge1's own bytes
        EXPECT("happy: write inside edge1 is dirty", 0, pkg_edges_clean(pk, &t));

        // 2. Exact #3240 crash shape: edge0 succeeds, edge1 malloc returns NULL.
        pk->stamp = nvn_dirty_now();
        nvn_dirty_protect(t.mem, t.L.size);
        g_arm = 1; g_calls = 0; g_fail_on = 2;
        pkg_edges_take(pk, &t);
        g_arm = 0;
        EXPECT("edge1 OOM: attempted both allocations", 2, g_calls);
        EXPECT("edge1 OOM: partial edge0 freed", 0, g_live_n);
        EXPECT("edge1 OOM: edge0 cleared", 1, pk->edge[0] == NULL && pk->edge_n[0] == 0);
        EXPECT("edge1 OOM: edge1 cleared", 1, pk->edge[1] == NULL && pk->edge_n[1] == 0);
        mtl_pkg_edge_stats(&a0, &b0, &p0, &f0, &fb0);
        EXPECT("edge1 OOM: live allocs 0", 0, a0);
        EXPECT("edge1 OOM: live bytes 0", 0, b0);
        EXPECT("edge1 OOM: fail count 1", 1, f0);
        EXPECT("edge1 OOM: failed bytes recorded", edge1_want, fb0);
        pool[tex_off - 16] ^= 0x11;
        EXPECT("edge1 OOM: conservative fallback (not clean)", 0, pkg_edges_clean(pk, &t));

        // 3. Edge0 malloc returns NULL immediately.
        pk->stamp = nvn_dirty_now();
        nvn_dirty_protect(t.mem, t.L.size);
        g_arm = 1; g_calls = 0; g_fail_on = 1;
        pkg_edges_take(pk, &t);
        g_arm = 0;
        EXPECT("edge0 OOM: stopped after first failure", 1, g_calls);
        EXPECT("edge0 OOM: no live heap buffers", 0, g_live_n);
        EXPECT("edge0 OOM: edges cleared", 1, pk->edge[0] == NULL && pk->edge[1] == NULL && pk->edge_n[0] == 0 && pk->edge_n[1] == 0);
        mtl_pkg_edge_stats(&a0, &b0, &p0, &f0, &fb0);
        EXPECT("edge0 OOM: fail count 2", 2, f0);
        EXPECT("edge0 OOM: failed bytes accumulated", edge0_want + edge1_want, fb0);
        EXPECT("edge0 OOM: conservative fallback", 0, pkg_edges_clean(pk, &t));

        // 4. Undersized / null-contents pk->eb falls back conservatively without copying.
        FakeEdgeBuf *bad_eb = [FakeEdgeBuf new];
        bad_eb->buf = NULL; bad_eb->len = edge0_want + edge1_want;
        pk->eb = (id)bad_eb;
        pk->stamp = nvn_dirty_now();
        nvn_dirty_protect(t.mem, t.L.size);
        pkg_edges_take(pk, &t);
        EXPECT("null eb contents: edges cleared", 1, pk->edge[0] == NULL && pk->edge[1] == NULL && pk->edge_n[0] == 0 && pk->edge_n[1] == 0);
        EXPECT("null eb contents: conservative fallback", 0, pkg_edges_clean(pk, &t));
        uint8_t tiny[16];
        bad_eb->buf = tiny; bad_eb->len = sizeof tiny;
        pkg_edges_take(pk, &t);
        EXPECT("undersized eb: edges cleared", 1, pk->edge[0] == NULL && pk->edge[1] == NULL && pk->edge_n[0] == 0 && pk->edge_n[1] == 0);
        mtl_pkg_edge_stats(&a0, &b0, &p0, &f0, &fb0);
        EXPECT("eb failures counted", 4, f0);
        pk->eb = nil;

        // 5. Migration to GPU shared buffer (pkg_edges_gpu) transfers ownership out of heap accounting.
        pk->stamp = nvn_dirty_now();
        nvn_dirty_protect(t.mem, t.L.size);
        g_arm = 1; g_calls = 0; g_fail_on = 0;
        pkg_edges_take(pk, &t);
        g_arm = 0;
        EXPECT("pre-gpu: two live heap edges", 2, g_live_n);
        uint8_t *eb_mem = calloc(1, edge0_want + edge1_want);
        FakeEdgeBuf *good_eb = [FakeEdgeBuf new];
        good_eb->buf = eb_mem; good_eb->len = edge0_want + edge1_want;
        FakeEdgeDev *fdev = [FakeEdgeDev new];
        fdev->next = good_eb;
        id old_dev = g_dev;
        g_dev = (id)fdev;
        uint64_t e0n = 0, e1s = 0, e1n = 0;
        id<MTLBuffer> got_eb = pkg_edges_gpu(pk, &t, &e0n, &e1s, &e1n);
        g_dev = old_dev;
        EXPECT("pkg_edges_gpu returned shared buffer", 1, got_eb == (id)good_eb);
        EXPECT("pkg_edges_gpu freed heap edges", 0, g_live_n);
        mtl_pkg_edge_stats(&a0, &b0, &p0, &f0, &fb0);
        EXPECT("pkg_edges_gpu heap allocs 0", 0, a0);
        EXPECT("pkg_edges_gpu heap bytes 0", 0, b0);
        EXPECT("pkg_edges_gpu copied edge0", 0, memcmp(eb_mem, pool + tex_off, edge0_want));
        EXPECT("pkg_edges_gpu copied edge1", 0, memcmp(eb_mem + edge0_want, pool + tex_off + tex_size - edge1_want, edge1_want));
        pool[tex_off - 16] ^= 0x22;
        EXPECT("pkg_edges_gpu neighbor write still clean", 1, pkg_edges_clean(pk, &t));
        pk->eb = nil;
        pk->edge[0] = pk->edge[1] = NULL;
        pk->edge_n[0] = pk->edge_n[1] = 0;
        free(eb_mem);
    }

    // 6. NvPkg dealloc subtracts heap-owned edge snapshots back to zero.
    @autoreleasepool {
        NvPkg *pk2 = [NvPkg new];
        pk2->span = nvn_dirty_span(t.mem, t.L.size);
        pk2->stamp = nvn_dirty_now();
        nvn_dirty_protect(t.mem, t.L.size);
        g_arm = 1; g_calls = 0; g_fail_on = 0;
        pkg_edges_take(pk2, &t);
        g_arm = 0;
        mtl_pkg_edge_stats(&a0, &b0, &p0, &f0, &fb0);
        EXPECT("dealloc pre: live allocs 2", 2, a0);
        EXPECT("dealloc pre: live bytes", edge0_want + edge1_want, b0);
        pk2 = nil;
    }
    mtl_pkg_edge_stats(&a0, &b0, &p0, &f0, &fb0);
    EXPECT("dealloc post: live allocs 0", 0, a0);
    EXPECT("dealloc post: live bytes 0", 0, b0);
    EXPECT("dealloc post: shim live buffers 0", 0, g_live_n);

    nvn_dirty_remove_range(pool);
    munmap(pool, pool_len);
    printf("t_pkg_edges_oom: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
