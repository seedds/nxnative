// Exercise idle trimming through mtl_sub_begin, including an expired slot with pending GPU work.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }

static int checks, failed;
#define CHECK(c, msg) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)
enum { EDGE = 256, LARGE = 24 << 20, MAX_COPIES = 16 };
static unsigned copies;
static id<MTLBuffer> output;

static void copy_upload(size_t size) {
    size_t off;
    id<MTLBuffer> block = up_alloc(size, &off);
    CHECK(block.length >= off + size, "staging allocation fits");
    [g_up_ce fillBuffer:block range:NSMakeRange(off, EDGE) value:(uint8_t)(copies + 1)];
    [g_up_ce barrierAfterEncoderStages:MTLStageBlit beforeEncoderStages:MTLStageBlit visibilityOptions:MTL4VisibilityOptionDevice];
    enc_b_copy_buffer(g_up_ce, block, off, output, copies++ * EDGE, EDGE);
}

static void upload_batch(size_t size, unsigned count) {
    up_begin();
    for (unsigned i = 0; i < count; i++) copy_upload(size);
    up_flush();
}

static void finish(void) {
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "submission completes");
    CHECK(!mtl_last_commit_error(), "GPU has no submission error");
    up_wait_zero(&g_up_pending, WS_DIAG);
}

int main(void) { @autoreleasepool {
    mtl_init();
    uint64_t idle = up_idle_period();
    const char *env = getenv("DM_NVN_UPLOAD_IDLE_MS");
    CHECK(idle == (env ? strtoull(env, NULL, 10) * 1000000 : 0), "idle period matches the setting, defaulting to zero");
    output = [g_dev newBufferWithLength:MAX_COPIES * EDGE options:MTLResourceStorageModeShared];
    memset(output.contents, 0, output.length);
    res_add(output);

    id<MTLSharedEvent> gate = [g_dev newSharedEvent];
    if (!gate) { fprintf(stderr, "FAIL: could not create the upload gate event\n"); return 1; }
    [g_q4 waitForEvent:gate value:1];
    mtl_sub_begin();
    for (unsigned s = 0; s < 3; s++) upload_batch(LARGE, 2);
    mtl_sub_commit();
    id<MTLBuffer> primary[3], overflow[3];
    uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC), expired = now - MAX(idle, 2000000000ull);
    for (unsigned s = 0; s < 3; s++) {
        primary[s] = g_up_slot_blk[s];
        overflow[s] = g_up_slot_over[s];
        CHECK(primary[s].length == UP_BLOCK_SIZE && overflow[s].length == UP_BLOCK_SIZE, "burst fills both retained blocks");
        g_up_idle_slot[s].used_ns = expired;
    }
    mtl_sub_begin();
    for (unsigned s = 0; s < 3; s++) {
        CHECK(atomic_load(&g_up_alloc_busy[s]), "upload is still pending behind the gate");
        CHECK(g_up_slot_blk[s] == primary[s] && g_up_slot_over[s] == overflow[s] &&
              [g_res containsAllocation:primary[s]] && [g_res containsAllocation:overflow[s]], "expired pending slot keeps both blocks resident");
    }
    gate.signaledValue = 1;
    finish();

    if (idle) {
        // Close the burst's demand window, then populate the next window with actual small uploads.
        now = clock_gettime_nsec_np(CLOCK_MONOTONIC);
        for (unsigned s = 0; s < 3; s++) {
            g_up_idle_slot[s].used_ns = now;
            g_up_idle_slot[s].overflow_ns = now;
            g_up_idle_slot[s].window_ns = expired;
        }
        mtl_sub_begin();
        for (unsigned s = 0; s < 3; s++) upload_batch(EDGE, 1);
        finish();
        g_up_idle_slot[1].overflow_ns = expired;
        g_up_idle_slot[1].window_ns = expired;
    }
    g_up_idle_slot[0].used_ns = expired;
    mtl_sub_begin();
    if (idle) {
        CHECK(!g_up_slot_blk[0] && !g_up_slot_over[0] && !g_up_slot_want[0], "fully idle slot clears its old size and both blocks");
        CHECK(!g_up_slot_blk[1] && !g_up_slot_over[1] && g_up_slot_want[1] == EDGE, "active slot drops unused overflow and shrinks to the recent peak");
        for (unsigned s = 0; s < 2; s++)
            CHECK(![g_res containsAllocation:primary[s]] && ![g_res containsAllocation:overflow[s]], "trimmed buffers leave Metal residency before new work");
        CHECK(g_up_slot_blk[2] == primary[2] && g_up_slot_over[2] == overflow[2], "recent slot keeps both blocks");
        CHECK(g_up_idle_n == 4 && g_up_idle_bytes == 4u * UP_BLOCK_SIZE, "trimming counts actual retired blocks and their sizes");
        __weak id<MTLBuffer> retired = primary[0];
        primary[0] = nil;
        CHECK(!retired, "completed staging is released before the next render fence");
        upload_batch(EDGE, 1);
        upload_batch(EDGE, 1);
        CHECK(g_up_slot_blk[0].length == UP_SLOT_BLOCK && g_up_slot_blk[1].length == UP_SLOT_BLOCK, "first small upload recreates only a minimum-size primary");
    } else {
        CHECK(!g_up_idle_n && g_up_slot_blk[0] == primary[0] && g_up_slot_over[0] == overflow[0] &&
              [g_res containsAllocation:primary[0]] && [g_res containsAllocation:overflow[0]], "zero keeps both completed blocks resident after expiry");
    }
    finish();

    const uint8_t *bytes = output.contents;
    for (unsigned i = 0; i < copies; i++) {
        unsigned j = 0;
        while (j < EDGE && bytes[i * EDGE + j] == i + 1) j++;
        CHECK(j == EDGE, "every upload reads back its data");
    }
    printf("t_upload_idle: %d checks, %d failed; %llu blocks trimmed\n", checks, failed, (unsigned long long)g_up_idle_n);
    return failed != 0;
} }
