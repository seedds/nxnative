// Cumulative query slots outlive GPU submissions. Write real GPU buffer values
// in the same slots draw() retains, then report through the production callback.
// Alternating queues and more segments than the ring size must not overwrite
// counts that a later ReportCounter still needs.
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
static int checks, failed;
#define CHECK(c, label) \
    do { \
        checks++; \
        if (!(c)) { \
            failed++; \
            fprintf(stderr, "FAIL: %s\n", label); \
        } \
    } while (0)

static id<MTLBuffer> write_slot(uint64_t count, int *ring_index) {
    mtl_sub_resume();
    id<MTLBuffer> chosen = g_vis;
    *ring_index = g_vis_ring_i;
    id<MTLBuffer> seed;
    size_t off = arena_alloc(256, &seed);
    memcpy((uint8_t *)seed.contents + off, &count, sizeof count);
    enc_b_copy_buffer(blit(), seed, off, chosen, 0, sizeof count);
    // These are the retained slot and counting state draw() records after it
    // selects a new visibility slot for a pass. Values are produced on the GPU.
    if (g_nvs == g_vscap) {
        g_vscap = g_vscap ? g_vscap * 2 : 16;
        vis_slot *next = realloc(g_vs, g_vscap * sizeof *g_vs);
        if (!next) {
            abort();
        }
        g_vs = next;
    }
    g_vs[g_nvs++] = (vis_slot){(void *)CFBridgingRetain(chosen), 0};
    g_vis_n = 1;
    g_vis_on = g_vis_counting = 1;
    CHECK(mtl_wait(mtl_sub_commit(), UINT64_C(5000000000)), "slot write completes");
    return chosen;
}
static uint64_t report(void) {
    uint64_t result[2] = {UINT64_MAX, 0};
    mtl_sub_resume();
    post((post_op){.kind = 3, .addr = (uint64_t)(uintptr_t)result, .vis = vis_snapshot()});
    g_vis_cur = -1;
    CHECK(mtl_wait(mtl_sub_commit(), UINT64_C(5000000000)), "query report completes");
    return result[0];
}
int main(void) {
    @autoreleasepool {
        mtl_init();
        const uint64_t qa = 930001, qb = 930002;
        id<MTLBuffer> slots[VIS_RING];
        uint64_t sum_a = 0, sum_b = 0;
        mtl_queue_activate(qa);
        for (int i = 0; i < VIS_RING; i++) {
            if (i == VIS_RING / 2) {
                mtl_queue_activate(qb);
            }
            int ring_index;
            slots[i] = write_slot((uint64_t)i + 1, &ring_index);
            CHECK(ring_index >= 0, "ring grows while unallocated slots remain");
            for (int j = 0; j < i; j++) {
                CHECK(slots[i] != slots[j], "completed cumulative slot is not recycled");
            }
            if (i < VIS_RING / 2) {
                sum_a += i + 1;
            } else {
                sum_b += i + 1;
            }
        }
        CHECK(g_vis_ring_n == VIS_RING, "both queues pin the whole ring");
        for (int i = 0; i < VIS_RING; i++) {
            CHECK(vis_buffer_pinned(slots[i]), "active and saved queue slots both pin buffers");
            CHECK(((const uint64_t *)slots[i].contents)[0] == (uint64_t)i + 1,
                  "original GPU count survives later segments");
        }
        mtl_queue_activate(qa);
        int ring_index;
        id<MTLBuffer> overflow = write_slot(100, &ring_index);
        sum_a += 100;
        CHECK(ring_index == -1 && !vis_in_ring(overflow),
              "all pinned uses overflow without waiting or indexing ring -1");
        CHECK(vis_buffer_pinned(overflow), "overflow remains retained by cumulative query");
        CHECK(report() == sum_a, "resumed first queue reports all original and overflow counts");
        CHECK(vis_buffer_pinned(overflow), "report does not reset cumulative query");

        uint64_t reset_result[2] = {UINT64_MAX, 0};
        mtl_sub_resume();
        uint64_t report_fence = g_fence + 1;
        post((post_op){.kind = 3, .addr = (uint64_t)(uintptr_t)reset_result, .vis = vis_snapshot()});
        vis_clear(); // ResetCounter may run before the report callback reads A's slots.
        for (int i = 0; i < VIS_RING / 2; i++) {
            CHECK(g_vis_ring[i].fence >= report_fence,
                  "snapshot protects older buffers through the report fence after reset");
        }
        CHECK(mtl_wait(mtl_sub_commit(), UINT64_C(5000000000)), "report before reset completes");
        CHECK(reset_result[0] == sum_a, "reset does not change an earlier pending report");
        CHECK(!vis_buffer_pinned(overflow), "reset releases overflow pin");
        for (int i = 0; i < VIS_RING; i++) {
            CHECK(vis_buffer_pinned(slots[i]) == (i >= VIS_RING / 2), "reset leaves other queue's slots pinned");
        }
        id<MTLBuffer> reused = write_slot(999, &ring_index);
        CHECK(ring_index >= 0 && ring_index < VIS_RING / 2, "reset permits reuse of a completed ring buffer");
        CHECK(ring_index >= 0 && ring_index < VIS_RING && reused == slots[ring_index],
              "reset reuses original allocation");
        CHECK(report() == 999, "post-reset report excludes earlier query counts");
        mtl_queue_activate(qb);
        CHECK(report() == sum_b, "other queue's cumulative report survives first queue reset and reuse");
        mtl_queue_forget(qb);
        for (int i = VIS_RING / 2; i < VIS_RING; i++) {
            CHECK(!vis_buffer_pinned(slots[i]), "queue finalization releases saved counts");
        }
        mtl_queue_activate(qa);
        mtl_queue_forget(qa);
        CHECK(!vis_buffer_pinned(reused), "finalizing remaining queue releases last pin");
        mtl_drain();
        printf("queue visibility: %d checks, %d failed\n", checks, failed);
        return failed != 0;
    }
}
