// Feed a delayed CPU/GPU sample to the production counter replay without a Metal device.
#include "../nvn/nvn_mtl.m"

module_t *g_mod;
int g_nmod;
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
void *module_sym(module_t *m, const char *name) { (void)m; (void)name; return NULL; }
void *guest_sym(const char *name) { (void)name; return NULL; }
void svc_thread_dump(void) {}
void dm_guest_clock_advanced(void) {}

static unsigned checks, failures;
#define CHECK(label, condition) do { checks++; if (!(condition)) { failures++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

@interface ClockDevice : NSObject
@property MTLTimestamp cpu;
@property MTLTimestamp gpu;
@end
@implementation ClockDevice
- (void)sampleTimestamps:(MTLTimestamp *)cpu gpuTimestamp:(MTLTimestamp *)gpu { *cpu = self.cpu; *gpu = self.gpu; }
- (uint64_t)queryTimestampFrequency { return 1000000000; }
@end

@interface ClockHeap : NSObject
@property uint64_t boundary;
@property uint64_t *slots;   // when set: the stamp of each slot, 0 for slot 0 too
@property NSUInteger nslots;
@end
@implementation ClockHeap
- (NSData *)resolveCounterRange:(NSRange)range {
    if (range.length != 1) return nil;
    MTL4TimestampHeapEntry entry = {.timestamp = self.slots ? (range.location < self.nslots ? self.slots[range.location] : 0) : self.boundary};
    return self.slots || range.location == 0 ? [NSData dataWithBytes:&entry length:sizeof entry] : nil;
}
@end

// Three submissions of one frame, each a single command buffer (guest ns), and what the Switch would have done with them: A ready at 1,000 runs 1,000 to 2,000. B was submitted
// at 1,500, while A ran, but the host translated it first: the GPU ran it 4,000 to 6,000, idle 2,000 to 4,000 for the host alone. C was submitted at 7,000, after B's end on the
// Switch (4,000) and here (6,000): the GPU's wait for it is the game's own, and it ran 7,500 to 8,000. sub_lag places B at 2,000 to 4,000 and C at 7,000 to 7,500
static uint64_t lag_of(uint64_t fence, uint64_t ready, uint64_t c0, uint64_t c1) {
    static post_rec r; memset(&r, 0, sizeof r); atomic_store(&r.fence, fence); r.t_ready = ready;
    return sub_lag(&r, c0, c1);
}
static void check_submission_lag(void) {
    g_lag_fence = 0; g_lag_h_end = 0;
    post_op top = {.type = 0x10}, end = {.type = 0};
    CHECK("a submission ready when the GPU starts it keeps its times", lag_of(1, 1000, 1000, 2000) == 0);
    uint64_t lag = lag_of(2, 1500, 4000, 6000);
    CHECK("the GPU's wait for the host's translation is left out", lag == 2000);
    CHECK("its start report is where the previous submission ended", rep_ticks(&top, NULL, 0, 4000, 6000, lag) == nvn_timestamp_from_ns(2000));
    CHECK("its end report keeps its own GPU time", rep_ticks(&end, NULL, 0, 4000, 6000, lag) == nvn_timestamp_from_ns(4000));
    lag = lag_of(3, 7000, 7500, 8000);
    CHECK("the GPU's wait for the game's own submit is kept", lag == 500 && rep_ticks(&top, NULL, 0, 7500, 8000, lag) == nvn_timestamp_from_ns(7000));
    CHECK("the frame's span is its GPU work and the game's own wait: 6,500 ns (7,000 with the host's)", rep_ticks(&end, NULL, 0, 7500, 8000, lag) - nvn_timestamp_from_ns(1000) == nvn_timestamp_from_ns(7500) - nvn_timestamp_from_ns(1000));
    CHECK("a submission with no ready time is not moved", lag_of(4, 0, 9000, 9500) == 0);
    CHECK("a submission placed out of order is not moved", lag_of(2, 1500, 10000, 11000) == 0);
    // A submission of two command buffers, with a timestamp boundary inside the first (segments 0 and 1, committed after segment 1: 4,000 to 5,000) and the last 6,000 to 7,000,
    // ready at 1,500 while the one before ran 1,000 to 2,000: it is placed from its first command buffer's start, 2,000 earlier, and its own 1,000 between command buffers is out too
    g_lag_fence = 0; g_lag_h_end = 0;
    CHECK("the first of the pair keeps its times", lag_of(1, 1000, 1000, 2000) == 0);
    static seg_clk two; memset(&two, 0, sizeof two); two.ncm = 1; two.cm[0] = 1;
    atomic_store(&two.t[1][0], 4000); atomic_store(&two.t[1][1], 5000);
    static post_rec r2; memset(&r2, 0, sizeof r2); atomic_store(&r2.fence, 2); r2.t_ready = 1500; r2.sc = &two; r2.seg_last = 2;
    lag = sub_lag(&r2, 6000, 7000);
    post_op top1 = {.type = 0x10, .seg = 1}, end2 = {.type = 0, .seg = 2};
    CHECK("a submission of several command buffers is placed from its first one's start", lag == 2000);
    CHECK("its first segment's report reads where the one before ended", rep_ticks(&top1, &two, 2, 6000, 7000, lag) == nvn_timestamp_from_ns(2000));
    CHECK("its end leaves out both waits: its own GPU work after the one before", rep_ticks(&end2, &two, 2, 6000, 7000, lag) == nvn_timestamp_from_ns(4000));
    atomic_store(&two.t[1][0], 0); atomic_store(&r2.fence, 3);
    CHECK("with its first command buffer's feedback missing nothing is moved", sub_lag(&r2, 6000, 7000) == 0);
    g_lag_fence = 0; g_lag_h_end = 0;
}
// A submission of three command buffers, its GPU idle between them while the host translated (sub_cut commits only once the GPU has run out of work):
//   command buffer 0: segments 0 and 1, GPU 0 to 3,000 ns, committed at the boundary after segment 1; the boundary's stamp lands at the start of command buffer 1 (4,010)
//   command buffer 1: segment 2, 4,000 to 6,000, committed after segment 2; that boundary's stamp landed in its last encoder, before the commit (5,990)
//   the last one: segments 3 and 4, 9,000 to 12,000 (c0 / c1), the stamp at the boundary between them 10,000
// A span of reports reads the GPU's work alone: the 1,000 ns and 3,000 ns gaps are left out once a time is past their middle
static void check_idle_gaps(ClockDevice *device, ClockHeap *heap) {
    uint64_t g0 = device.cpu - 50000, off = device.gpu - device.cpu;   // guest time of the submission's start; a heap stamp is on the GPU clock, guest + off
    static seg_clk clock; memset(&clock, 0, sizeof clock);
    clock.nb = 5; for (int k = 0; k < 5; k++) clock.b[k] = (uint16_t)k;
    clock.ncm = 2; clock.cm[0] = 1; clock.cm[1] = 2;
    atomic_store(&clock.t[1][0], g0); atomic_store(&clock.t[1][1], g0 + 3000);
    atomic_store(&clock.t[2][0], g0 + 4000); atomic_store(&clock.t[2][1], g0 + 6000);
    uint64_t c0 = g0 + 9000, c1 = g0 + 12000;
    uint64_t slots[5] = {g0 + 5 + off, g0 + 1500 + off, g0 + 4010 + off, g0 + 5990 + off, g0 + 10000 + off};
    heap.slots = slots; heap.nslots = 5;
    CHECK("no idle before the first commit", seg_idle_before(&clock, g0 + 3400, c0) == 0);
    CHECK("a gap counts once a time is past its middle", seg_idle_before(&clock, g0 + 3600, c0) == 1000);
    CHECK("both gaps before the last command buffer", seg_idle_before(&clock, g0 + 9000, c0) == 4000);
    post_op top0 = {.type = 0x10, .seg = 0}, end1 = {.type = 0, .seg = 1}, end2 = {.type = 0, .seg = 2}, top4 = {.type = 0x10, .seg = 4}, end4 = {.type = 0, .seg = 4};
    CHECK("the first report keeps its time", rep_ticks(&top0, &clock, 4, c0, c1, 0) == nvn_timestamp_from_ns(g0 + 5));
    CHECK("a stamp written after a commit leaves out the gap before it", rep_ticks(&end1, &clock, 4, c0, c1, 0) == nvn_timestamp_from_ns(g0 + 3010));
    CHECK("a stamp written before a commit keeps the time of its command buffer", rep_ticks(&end2, &clock, 4, c0, c1, 0) == nvn_timestamp_from_ns(g0 + 4990));
    CHECK("a stamp in the last command buffer leaves out both gaps", rep_ticks(&top4, &clock, 4, c0, c1, 0) == nvn_timestamp_from_ns(g0 + 6000));
    CHECK("the submission's end leaves out both gaps", rep_ticks(&end4, &clock, 4, c0, c1, 0) == nvn_timestamp_from_ns(g0 + 8000));
    CHECK("the span of the submission is its GPU work", rep_ticks(&end4, &clock, 4, c0, c1, 0) - rep_ticks(&top0, &clock, 4, c0, c1, 0) == nvn_timestamp_from_ns(g0 + 8000) - nvn_timestamp_from_ns(g0 + 5));
    atomic_store(&clock.t[2][0], 0); atomic_store(&clock.t[2][1], 0);   // command buffer 1's feedback has not arrived
    CHECK("a command buffer with no feedback gives no gap", seg_idle_before(&clock, g0 + 9000, c0) == 0);
    heap.slots = NULL; heap.nslots = 0;
}

int main(void) {
    @autoreleasepool {
        ClockDevice *device = [ClockDevice new];
        ClockHeap *heap = [ClockHeap new];
        g_dev = (id<MTLDevice>)device;
        g_ts_heap = (id<MTL4CounterHeap>)heap;
        const uint64_t age = 8000000, delay = 20000000, offset = 100000000;
        // The sampled CPU time predates the report by at least 20 ms. A late guest-clock
        // read would add that scheduling delay to the value sent to the game.
        device.cpu = now_ns() - delay;
        device.gpu = device.cpu + offset;
        heap.boundary = device.gpu - age;
        seg_clk clock = {.nb = 1, .b = {0}};
        post_op report = {.type = 0x10, .seg = 0};
        uint64_t expected = nvn_timestamp_from_ns(device.cpu - age);
        CHECK("a late read preserves the GPU boundary's sampled guest time", rep_ticks(&report, &clock, 0, 0, 0, 0) == expected);
        device.cpu += 1000;
        device.gpu += 1000;
        CHECK("a later clock pair maps the same boundary to the same report", rep_ticks(&report, &clock, 0, 0, 0, 0) == expected);
        device.cpu = 0;
        CHECK("a missing CPU sample takes the existing feedback fallback", rep_ticks(&report, &clock, 0, 10000, 0, 0) == nvn_timestamp_from_ns(10000));
        device.cpu = now_ns() - delay; device.gpu = device.cpu + offset;
        check_idle_gaps(device, heap);
        check_submission_lag();

        guest_clock_freeze(1);
        uint64_t frozen = guest_ns();
        CHECK("projection keeps the frozen guest time", guest_ns_at_host_time(1) == frozen);
        usleep(1000);
        guest_clock_freeze(0);
        uint64_t host = now_ns(), guest = guest_ns_at_host_time(host);
        CHECK("projection subtracts the time spent frozen", guest < host && guest >= frozen);
        CHECK("a host sample older than the pause offset cannot underflow", guest_ns_at_host_time(1) == 0);
        CHECK("feedback conversion uses the resumed guest epoch", llabs((int64_t)gpu_s_guest_ns(host / 1e9) - (int64_t)guest) <= 2);
        CHECK("future feedback is rejected", gpu_s_guest_ns(host / 1e9 + 1) == 0);
        CHECK("invalid feedback is rejected", gpu_s_guest_ns(NAN) == 0 && gpu_s_guest_ns(INFINITY) == 0);

        setenv("DM_DETCLOCK", "1000000", 1);
        dm_guest_frame_tick();
        uint64_t deterministic = guest_ns();
        CHECK("deterministic projection uses the current guest frame", guest_ns_at_host_time(host) == deterministic);
        dm_guest_frame_tick();
        CHECK("deterministic projection advances by the configured step", guest_ns_at_host_time(host) == deterministic + 1000000);
    }
    printf("counter clock: %u checks, %u failed\n", checks, failures);
    return failures != 0;
}
