// Physical event chunks append into a logical submission's allocator. Real GPU
// copies exercise bytes retained before/after interleaving and lease pressure.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) {
    return NULL;
}
module_t *module_of(const void *p) {
    (void)p;
    return NULL;
}
module_t *g_mod;
int g_nmod;
void svc_thread_dump(void) {
}
uint64_t now_ns(void) {
    return bp_now();
}
static int checks, failed, legacy;
#define CHECK(c, label) \
    do { \
        checks++; \
        if (!(c)) { \
            failed++; \
            fprintf(stderr, "FAIL: %s\n", label); \
        } \
    } while (0)

static uint64_t copy_chunk(mtl_submit_alloc *a, uint64_t queue, id<MTLBuffer> src, unsigned si, id<MTLBuffer> dst,
                           unsigned di) {
    mtl_queue_activate(queue);
    // Fixture-only negative control: the previous scheduler called ordinary
    // begin/resume for every chunk, acquiring/resetting a new allocator each time.
    if (legacy) {
        mtl_sub_resume();
    } else {
        mtl_sub_begin_alloc(a, 1);
    }
    enc_b_copy_buffer(blit(), src, si * 4, dst, di * 4, 4);
    uint64_t f = mtl_sub_commit();
    mtl_queue_seal();
    return f;
}
static void open_later(id<MTLSharedEvent> gate) {
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 500000000), dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0),
                   ^{
                     gate.signaledValue = 1;
                   });
}
static void wait_done(uint64_t f) {
    CHECK(mtl_wait(f, UINT64_C(5000000000)), "GPU prefix completes");
}

int main(void) {
    @autoreleasepool {
        legacy = getenv("DM_TEST_ALLOC_LEGACY") != NULL;
        setenv("DM_NVN_WAITS", "1", 1);
        mtl_init();
        id<MTLBuffer> src = [g_dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];
        id<MTLBuffer> dst = [g_dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];
        res_add(src);
        res_add(dst);
        for (unsigned i = 0; i < 1024; i++) {
            ((uint32_t *)src.contents)[i] = 0xa1300000u + i;
        }
        memset(dst.contents, 0, 4096);
        // Warm the normal/direct path before a gate makes waits deterministic.
        wait_done(copy_chunk(NULL, 940000, src, 0, dst, 0));

        mtl_submit_alloc *a = mtl_submit_alloc_new(), *b = mtl_submit_alloc_new();
        id<MTLSharedEvent> gate = [g_dev newSharedEvent];
        [g_q4 waitForEvent:gate value:1];
        uint64_t waits = g_alloc_waits, fence = 0, started = bp_now();
        int slot_a = -1, slot_b = -1;
        for (unsigned i = 0; i < 32; i++) {
            if (i == 8) {
                open_later(gate); // old code blocks here until the gate opens
            }
            mtl_submit_alloc *owner = i & 1 ? b : a;
            fence = copy_chunk(owner, 940001 + (i & 1), src, i, dst, i);
            if (i == 0) {
                slot_a = g_alloc_cur;
            }
            if (i == 1) {
                slot_b = g_alloc_cur;
            }
            CHECK(g_alloc_cur == (i & 1 ? slot_b : slot_a), "interleaved logical submission keeps its allocator");
        }
        CHECK(slot_a != slot_b, "independent logical submissions use separate leases");
        CHECK(g_alloc_waits == waits, "more than eight chunks do not introduce allocator waits");
        CHECK(gate.signaledValue == 0, "all split chunks encode while the GPU remains gated");
        CHECK(g_vis_ring_n == 1, "unused visibility buffers do not become an artificial chunk limit");
        printf("split chunks: 32, allocator waits: %llu, encode wall: %.3f ms, GPU gate: %s\n",
               (unsigned long long)(g_alloc_waits - waits), (bp_now() - started) / 1e6,
               gate.signaledValue ? "opened" : "closed");
        mtl_submit_alloc_close(a);
        mtl_submit_alloc_close(b);
        CHECK(a->slot == -1 && b->slot == -1, "logical END drops ownership before GPU completion");
        gate.signaledValue = 1;
        wait_done(fence);
        for (unsigned i = 0; i < 32; i++) {
            CHECK(((uint32_t *)dst.contents)[i] == ((uint32_t *)src.contents)[i],
                  "all gated GPU copies survive later encoding");
        }
        mtl_submit_alloc_release(a);
        mtl_submit_alloc_release(b);

        // More blocked logical submissions than slots: none has reached logical
        // END, and their first GPU prefixes cannot complete until the gate opens.
        // The ninth must wait only for that GPU prefix, revoke its lease, and return
        // so the same CPU thread can eventually submit the event producer.
        enum { N = ALLOC_MAX + 4 };
        mtl_submit_alloc *blocked[N];
        gate = [g_dev newSharedEvent];
        [g_q4 waitForEvent:gate value:1];
        waits = g_alloc_waits;
        for (unsigned i = 0; i < N; i++) {
            blocked[i] = mtl_submit_alloc_new();
            if (i == ALLOC_MAX) {
                open_later(gate);
            }
            fence = copy_chunk(blocked[i], 941000 + i, src, 64 + i, dst, 64 + i);
            if (i + 1 == ALLOC_MAX) {
                CHECK(g_alloc_waits == waits, "eight blocked prefixes fit without waiting");
                CHECK(gate.signaledValue == 0, "no live GPU prefix was reclaimed");
            }
        }
        CHECK(g_alloc_n == ALLOC_MAX, "allocator pool remains bounded with more logical submissions");
        CHECK(g_alloc_waits > waits, "capacity pressure waits for live GPU bytes");
        CHECK(blocked[0]->slot == -1, "completed suspended prefix lease is revoked under pressure");
        wait_done(fence);
        for (unsigned i = 0; i < N; i++) {
            CHECK(((uint32_t *)dst.contents)[64 + i] == ((uint32_t *)src.contents)[64 + i],
                  "reclaimed prefixes retain correct GPU output");
        }
        mtl_submit_alloc *producer = mtl_submit_alloc_new();
        fence = copy_chunk(producer, 942000, src, 127, dst, 127);
        mtl_submit_alloc_close(producer);
        for (unsigned i = 0; i < N; i++) {
            fence = copy_chunk(blocked[i], 941000 + i, dst, 127, dst, 96 + i);
            mtl_submit_alloc_close(blocked[i]);
        }
        wait_done(fence);
        for (unsigned i = 0; i < N; i++) {
            CHECK(((uint32_t *)dst.contents)[96 + i] == ((uint32_t *)src.contents)[127],
                  "revoked continuation reads late producer data");
            mtl_submit_alloc_release(blocked[i]);
        }
        mtl_submit_alloc_release(producer);
        for (int i = 0; i < g_alloc_n; i++) {
            CHECK(!g_alloc_owner[i], "logical cleanup leaves no allocator pins");
        }

        // Reusing one allocator removes the old eight-chunk limit, but completion
        // records remain bounded. A long submission must wait for an old physical
        // fence before wrapping the table, preserving every event and report.
        enum { LONG_CHUNKS = POST_RING + 8 };
        volatile uint32_t signals = 0;
        uint64_t reports[LONG_CHUNKS][2] = {{0}};
        mtl_submit_alloc *long_submit = mtl_submit_alloc_new();
        gate = [g_dev newSharedEvent];
        [g_q4 waitForEvent:gate value:1];
        uint64_t post_waits = atomic_load(&g_ws_blk[WS_POST]);
        waits = g_alloc_waits;
        for (unsigned i = 0; i < LONG_CHUNKS; i++) {
            if (i == POST_RING) {
                open_later(gate);
            }
            mtl_sub_begin_alloc(long_submit, 1);
            enc_b_copy_buffer(blit(), src, (256 + i) * 4, dst, (256 + i) * 4, 4);
            mtl_sub_event(&signals, 1, 1);
            mtl_sub_counter((uint64_t)(uintptr_t)reports[i], NVN_COUNTER_TYPE_TIMESTAMP_TOP);
            fence = mtl_sub_commit();
        }
        mtl_submit_alloc_close(long_submit);
        wait_done(fence);
        CHECK(atomic_load(&g_ws_blk[WS_POST]) > post_waits, "completion ring waits before overwriting live records");
        CHECK(g_alloc_waits == waits, "long logical submission does not wait on its own allocator");
        CHECK(signals == LONG_CHUNKS, "all wrapped completion events publish exactly once");
        for (unsigned i = 0; i < LONG_CHUNKS; i++) {
            CHECK(reports[i][1] != 0, "every wrapped timestamp report publishes");
            CHECK(((uint32_t *)dst.contents)[256 + i] == ((uint32_t *)src.contents)[256 + i],
                  "long submission preserves GPU command bytes across callback wrap");
        }
        mtl_submit_alloc_release(long_submit);
        // Direct API still uses ordinary completed-fence-based allocator reuse.
        wait_done(copy_chunk(NULL, 940000, src, 200, dst, 200));
        CHECK(((uint32_t *)dst.contents)[200] == ((uint32_t *)src.contents)[200],
              "direct submission path retains its behavior");
        res_remove(src);
        res_remove(dst);
        mtl_drain();
        printf("queue allocators%s: %d checks, %d failed\n", legacy ? " (legacy negative control)" : "", checks,
               failed);
        return failed != 0;
    }
}
