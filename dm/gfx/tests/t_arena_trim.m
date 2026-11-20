// Per-frame arena blocks (nvn_mtl.m arena_new / arena_begin / arena_commit / arena_trim). With the GPU held by a shared-event gate, 24 submissions of 3 MB each make a block apiece,
// past ARENA_KEEP_BLOCKS: the idle trims at the starts after them free none of those while their GPU work is pending, and once the gate opens every submission's GPU copy reads the
// bytes it wrote. ARENA_IDLE small submissions later the blocks above the keep count have left the residency set and are freed (DM_NVN_ARENA_TRIM=0: all kept). Shared blocks: 32 small
// submissions with the GPU held take one block between them (DM_NVN_ARENA_SHARE=0: one each), and each one's GPU copy still reads its own bytes. A shared block cannot be reset after
// only its first writer completes. Shader argument caches borrow their blocks, so an idle block is actually released even while the shader stays alive.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s (line %d)\n", label, __LINE__); } else printf("PASS: %s\n", label); } while (0)
enum { MB = 1 << 20, HELD = 24, BIG = 3 * MB, SMALL_N = 32, SMALL = 64 << 10, EDGE = 256, OUT = (HELD + 1 + SMALL_N) * 2 * EDGE };
static int resident(id o) { pthread_mutex_lock(&g_res_mu); int r = [g_res_objects containsObject:o]; pthread_mutex_unlock(&g_res_mu); return r; }
static unsigned long queued(void) { pthread_mutex_lock(&g_deferred_mu); unsigned long n = g_deferred.count; pthread_mutex_unlock(&g_deferred_mu); return n; }
static void settle(void) { for (int i = 0; i < 400 && queued(); i++) { @autoreleasepool { defer_drain(); res_commit(); } usleep(5000); } @autoreleasepool { res_commit(); } }
static uint32_t word(unsigned tag, size_t i) { return (uint32_t)(tag * 2654435761u + (uint32_t)i * 40503u + 1); }
static id<MTLBuffer> g_out;
// One submission: n bytes from the arena, filled with the tag's words; its GPU work copies the first and last EDGE bytes into g_out at slot (read once the GPU runs). Returns the block, weak
static void submission(size_t n, unsigned tag, unsigned slot, __weak id *block, uint64_t *fence) { @autoreleasepool {
    mtl_sub_begin();
    __strong id<MTLBuffer> ab; size_t off = arena_alloc(n, &ab);
    uint32_t *p = (uint32_t *)(void *)((uint8_t *)ab.contents + off); for (size_t i = 0; i < n / 4; i++) p[i] = word(tag, i);
    id<MTL4ComputeCommandEncoder> e = blit();
    enc_b_copy_buffer(e, ab, off, g_out, (size_t)slot * 2 * EDGE, EDGE); enc_b_copy_buffer(e, ab, off + n - EDGE, g_out, (size_t)slot * 2 * EDGE + EDGE, EDGE);
    if (block) *block = ab;
    *fence = mtl_sub_commit();
} }
static int copied(size_t n, unsigned tag, unsigned slot) {   // the GPU read what this submission wrote, not a later one's bytes
    const uint32_t *o = (const uint32_t *)(const void *)((const uint8_t *)g_out.contents + (size_t)slot * 2 * EDGE);
    for (size_t i = 0; i < EDGE / 4; i++) if (o[i] != word(tag, i) || o[EDGE / 4 + i] != word(tag, (n - EDGE) / 4 + i)) return 0;
    return 1;
}
static void empty(int n) { for (int i = 0; i < n; i++) { @autoreleasepool { mtl_sub_begin(); mtl_sub_commit(); } } }
static unsigned usual(void) { unsigned u = 0; for (NvArena *a in g_arenas) u += a->len <= ARENA_KEEP; for (NvArena *a in g_sub_arenas) u += a->len <= ARENA_KEEP; return u; }
static int count_alive(__weak id w[], int n) { int r = 0; @autoreleasepool { for (int i = 0; i < n; i++) { id o = w[i]; r += o != nil; } } return r; }
static int count_resident(__weak id w[], int n) { int r = 0; @autoreleasepool { for (int i = 0; i < n; i++) { id o = w[i]; r += o && resident(o); } } return r; }
// HELD submissions of BIG bytes with the GPU held; returns the last fence. More submissions start while the GPU is held, and their starts run the idle trim
static uint64_t grow(__weak id held[], unsigned tag0, uint64_t *freed_while_held) {
    id<MTLSharedEvent> gate = [g_dev newSharedEvent]; if (!gate) abort();
    rec_drain(); [g_q4 waitForEvent:gate value:1];
    uint64_t f = 0, freed0 = g_arena_freed;
    for (int i = 0; i < HELD; i++) submission(BIG, tag0 + (unsigned)i, (unsigned)i, &held[i], &f);
    empty(8);
    unsigned trimmed = arena_trim(0);   // make every completed block eligible now: the pending-fence guard, not the 600-submission age, must protect these
    CHECK(trimmed == 0, "an eligible trim releases no pending block");
    *freed_while_held = g_arena_freed - freed0;
    CHECK(count_alive(held, HELD) == HELD && count_resident(held, HELD) == HELD, "with the GPU held, no block a pending submission wrote is freed or leaves the residency set");
    CHECK(f > g_done, "the submissions are still pending");
    gate.signaledValue = 1;   // release the GPU before any check can stop the test
    CHECK(mtl_wait(f, 5000000000ull), "the held submissions complete once the gate opens");
    int good = 1; for (int i = 0; i < HELD; i++) good &= copied(BIG, tag0 + (unsigned)i, (unsigned)i);
    CHECK(good, "each held submission's GPU copy read its own bytes: no block was reused or freed before its GPU work ran");
    return f;
}
int main(void) { @autoreleasepool {
    mtl_init();
    int trim_on = arena_trim_on(), share_on = arena_share_on();
    g_out = [g_dev newBufferWithLength:OUT options:MTLResourceStorageModeShared]; res_add(g_out); memset(g_out.contents, 0, OUT);
    printf("arena trim %s, arena share %s\n", trim_on ? "on" : "off", share_on ? "on" : "off");
    // 1. Growth past the keep count with the GPU held; the idle trims meanwhile free nothing pending
    __weak id held[HELD]; uint64_t made0 = g_arena_made, during = 0;
    grow(held, 100, &during);
    printf("grown: %llu blocks made, %llu alive (%llu MB); %llu freed while held\n", (unsigned long long)(g_arena_made - made0), (unsigned long long)g_arena_live, (unsigned long long)(g_arena_live_bytes / MB), (unsigned long long)during);
    CHECK(g_arena_made - made0 >= HELD, "a block per held 3 MB submission");
    CHECK(usual() >= HELD && g_arena_live == usual(), "all of them alive, the count matches the blocks");
    // 2. Idle: ARENA_IDLE small submissions later, the blocks above the keep count go
    mtl_drain(); settle();
    uint64_t gpu0 = g_dev.currentAllocatedSize, freed0 = g_arena_freed, f = 0;
    // Shared, the small submissions go on in one block however far the CPU runs ahead. A block each (DM_NVN_ARENA_SHARE=0), as many blocks stay in use as submissions are in flight, so
    // each waits for its GPU work here, a calm after the peak
    for (int i = 0; i < ARENA_IDLE + 4; i++) { submission(EDGE * 2, 1000 + (unsigned)i, HELD, NULL, &f); if (!share_on) mtl_wait(f, 5000000000ull); }
    mtl_wait(f, 5000000000ull); mtl_drain(); settle();
    uint64_t gpu1 = g_dev.currentAllocatedSize, gone = g_arena_freed - freed0;
    printf("idle: %u usual blocks alive, %llu released (%llu MB in total), %d of %d held blocks alive, %d resident; GPU allocated %llu -> %llu MB\n", usual(), (unsigned long long)gone,
           (unsigned long long)(g_arena_freed_bytes / MB), count_alive(held, HELD), HELD, count_resident(held, HELD), (unsigned long long)(gpu0 / MB), (unsigned long long)(gpu1 / MB));
    CHECK(copied(EDGE * 2, 1000 + ARENA_IDLE + 3, HELD), "the small submissions' copies read their bytes");
    if (trim_on) {
        CHECK(usual() == ARENA_KEEP_BLOCKS, "idle: the keep count is left");
        CHECK(gone >= HELD - ARENA_KEEP_BLOCKS, "idle: the others were released and counted");
        CHECK(count_resident(held, HELD) <= ARENA_KEEP_BLOCKS && count_alive(held, HELD) <= ARENA_KEEP_BLOCKS, "idle: they left the residency set and were freed");
        CHECK((int64_t)gpu0 - (int64_t)gpu1 >= (int64_t)(HELD - ARENA_KEEP_BLOCKS - 1) * 4 * MB, "idle: GPU allocated fell by their bytes");
    } else {
        CHECK(gone == 0 && usual() >= HELD && count_alive(held, HELD) == HELD, "DM_NVN_ARENA_TRIM=0: every block kept");
    }
    // 3. Shared blocks: small submissions with the GPU held go on in the block the one before ended in
    id<MTLSharedEvent> gate = [g_dev newSharedEvent]; if (!gate) abort();
    rec_drain(); [g_q4 waitForEvent:gate value:1];
    uint64_t made1 = g_arena_made; f = 0; __weak id blk[SMALL_N];
    for (int i = 0; i < SMALL_N; i++) submission(SMALL, 300 + (unsigned)i, HELD + 1 + (unsigned)i, &blk[i], &f);
    uint64_t made = g_arena_made - made1; int distinct = 0;
    @autoreleasepool { NSMutableSet *s = [NSMutableSet set]; for (int i = 0; i < SMALL_N; i++) { id o = blk[i]; if (o) [s addObject:o]; } distinct = (int)s.count; }
    gate.signaledValue = 1;
    CHECK(mtl_wait(f, 5000000000ull), "the small held submissions complete");
    int good = 1; for (int i = 0; i < SMALL_N; i++) good &= copied(SMALL, 300 + (unsigned)i, HELD + 1 + (unsigned)i);
    printf("shared: %d small submissions with the GPU held used %d blocks, %llu made\n", SMALL_N, distinct, (unsigned long long)made);
    CHECK(good, "each small submission's GPU copy read its own bytes");
    if (share_on) CHECK(distinct <= 2 && made <= 1, "shared: one block between them (two when the last one's room runs out)");
    else CHECK(distinct == SMALL_N, "DM_NVN_ARENA_SHARE=0: a block each");
    mtl_drain();
    // 4. Fill one block across two submissions, with only the earlier writer complete. The next allocation must not reset that block.
    if (share_on) { @autoreleasepool {
        __weak id first_block, last_block, next_block; uint64_t first, last, next;
        submission(4 * MB - EDGE, 400, 0, &first_block, &first);
        CHECK(mtl_wait(first, 5000000000ull), "the first shared writer completes");
        id<MTLSharedEvent> later = [g_dev newSharedEvent]; if (!later) abort();
        rec_drain(); [g_q4 waitForEvent:later value:1];
        submission(EDGE, 401, 1, &last_block, &last);
        CHECK(first_block == last_block, "the later writer fills the first writer's block tail");
        uint64_t block_fence = 0; for (NvArena *a in g_arenas) if (a->buf == last_block) block_fence = a->fence;
        CHECK(first <= g_done && last > g_done && block_fence == last, "a shared block tracks its latest writer while only the first has completed");
        submission(4 * MB, 402, 2, &next_block, &next);
        CHECK(next_block != last_block, "another allocation cannot reset the shared block before its last writer completes");
        later.signaledValue = 1;
        CHECK(mtl_wait(next, 5000000000ull) && copied(EDGE, 401, 1), "the later shared writer reads its own bytes after the intervening allocation");
    } }
    // 5. Large idle blocks also count as released. The shader remains alive after its borrowed argument buffer has gone.
    mtl_drain(); settle();
    NvShader *cached = [NvShader new]; __weak id retired;
    uint64_t retired_bytes = 0;
    @autoreleasepool {
        mtl_sub_begin(); __strong id<MTLBuffer> b; arena_at at = arena_take(ARENA_KEEP + MB, &b);
        cached->ab_buf = b; cached->ab_gpu = at.gpu; cached->ab_off = at.off; retired = b; retired_bytes = b.length;
        arena_trim(0);
        CHECK(resident(b), "a submission's current block stays resident during an eligible trim");
        CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "the large block's submission completes");
    }
    mtl_drain(); settle();
    uint64_t released0 = g_arena_freed, released_bytes0 = g_arena_freed_bytes;
    @autoreleasepool { arena_trim(0); }
    settle();
    CHECK(g_arena_freed > released0 && g_arena_freed_bytes - released_bytes0 >= retired_bytes, "idle release totals include blocks above 32 MB");
    CHECK(retired == nil, "an idle argument buffer is freed while its cached shader remains alive");
    CHECK(cached->ab_gpu != 0, "the shader cache outlives its retired argument buffer");
    printf("t_arena_trim: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
