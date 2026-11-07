// Upload staging shrinks back when it sits idle (nvn_mtl.m up_idle_release, DM_NVN_UPLOAD_IDLE_MS): after a burst of large batches each slot keeps a large primary block; a slot no batch
// used for the interval gives both blocks back at the next submission's start, and a slot that goes on with small batches gives back a primary larger than its window's largest batch,
// so the next batch makes one of the smallest size. The uploads themselves must still land.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(label, c) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s (line %d)\n", label, __LINE__); } } while (0)
static unsigned g_seq;
static void batch(size_t size, id<MTLBuffer> input, id<MTLBuffer> output) {   // one allocation of size bytes; its first 256 bytes go through it from input to output at the batch's own place
    up_begin();
    size_t off; id<MTLBuffer> b = up_alloc(size, &off); size_t at = (g_seq++ % 64) * 256;
    CHECK("allocation fits its block", b && off + size <= b.length);
    enc_b_copy_buffer(g_up_ce, input, at, b, off, 256);
    [g_up_ce barrierAfterEncoderStages:MTLStageBlit beforeEncoderStages:MTLStageBlit visibilityOptions:MTL4VisibilityOptionDevice];
    enc_b_copy_buffer(g_up_ce, b, off, output, at, 256);
    up_flush();
}
static void submission(void) { CHECK("submission completes", mtl_wait(mtl_sub_commit(), 5000000000ull)); CHECK("GPU has no submission error", !mtl_last_commit_error()); mtl_drain(); }
static size_t held(void) { size_t n = 0; for (unsigned s = 0; s < 3; s++) n += (g_up_slot_blk[s] ? g_up_slot_blk[s].length : 0) + (g_up_slot_over[s] ? g_up_slot_over[s].length : 0); return n; }
int main(void) { @autoreleasepool {
    setenv("DM_NVN_UPLOAD_IDLE_MS", "200", 1);
    mtl_init();
    id<MTLBuffer> input = [g_dev newBufferWithLength:64 * 256 options:MTLResourceStorageModeShared], output = [g_dev newBufferWithLength:64 * 256 options:MTLResourceStorageModeShared];
    res_add(input); res_add(output);
    for (unsigned i = 0; i < 64 * 256; i++) ((uint8_t *)input.contents)[i] = (uint8_t)(i * 13 + i / 256 + 1);
    // a burst: every slot keeps a 32 MB primary block
    mtl_sub_begin(); for (int i = 0; i < 3; i++) batch(24u << 20, input, output); submission();
    CHECK("each slot keeps a large primary block", g_up_slot_blk[0].length >= (24u << 20) && g_up_slot_blk[1].length >= (24u << 20) && g_up_slot_blk[2].length >= (24u << 20));
    size_t burst = held();
    // idle past the interval: the next submission's start gives every slot's blocks back
    usleep(300000);
    uint64_t n0 = g_up_idle_n;
    mtl_sub_begin();
    CHECK("idle slots give their blocks back", held() == 0 && g_up_idle_n == n0 + 3 && g_up_idle_bytes >= burst);
    for (int i = 0; i < 3; i++) batch(24u << 20, input, output);
    submission();
    CHECK("the next burst makes them again", g_up_slot_blk[0].length >= (24u << 20));
    // small batches every 20 ms for longer than the interval: a slot that stays in use gives back a primary larger than its window needed, and makes the smallest one next
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_MONOTONIC);
    while (clock_gettime_nsec_np(CLOCK_MONOTONIC) - t0 < 700000000ull) { mtl_sub_begin(); for (int i = 0; i < 3; i++) batch(512, input, output); submission(); usleep(20000); }
    for (unsigned s = 0; s < 3; s++) CHECK("a busy slot's primary shrinks to its recent batches", g_up_slot_blk[s] && g_up_slot_blk[s].length == UP_SLOT_BLOCK);
    for (unsigned i = 0; i < 64 * 256; i++) if (((uint8_t *)output.contents)[i] != ((uint8_t *)input.contents)[i]) { CHECK("every upload landed", 0); break; }
    printf("t_upload_idle: %d checks, %d failed; %llu releases, %llu MB released\n", checks, failed, (unsigned long long)g_up_idle_n, (unsigned long long)(g_up_idle_bytes >> 20));
    return failed != 0;
} }
