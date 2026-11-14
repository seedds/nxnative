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
    CHECK("the first report keeps its time", rep_ticks(&top0, &clock, 4, c0, c1) == nvn_timestamp_from_ns(g0 + 5));
    CHECK("a stamp written after a commit leaves out the gap before it", rep_ticks(&end1, &clock, 4, c0, c1) == nvn_timestamp_from_ns(g0 + 3010));
    CHECK("a stamp written before a commit keeps the time of its command buffer", rep_ticks(&end2, &clock, 4, c0, c1) == nvn_timestamp_from_ns(g0 + 4990));
    CHECK("a stamp in the last command buffer leaves out both gaps", rep_ticks(&top4, &clock, 4, c0, c1) == nvn_timestamp_from_ns(g0 + 6000));
    CHECK("the submission's end leaves out both gaps", rep_ticks(&end4, &clock, 4, c0, c1) == nvn_timestamp_from_ns(g0 + 8000));
    CHECK("the span of the submission is its GPU work", rep_ticks(&end4, &clock, 4, c0, c1) - rep_ticks(&top0, &clock, 4, c0, c1) == nvn_timestamp_from_ns(g0 + 8000) - nvn_timestamp_from_ns(g0 + 5));
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
        CHECK("a late read preserves the GPU boundary's sampled guest time", rep_ticks(&report, &clock, 0, 0, 0) == expected);
        device.cpu += 1000;
        device.gpu += 1000;
        CHECK("a later clock pair maps the same boundary to the same report", rep_ticks(&report, &clock, 0, 0, 0) == expected);
        device.cpu = 0;
        CHECK("a missing CPU sample takes the existing feedback fallback", rep_ticks(&report, &clock, 0, 10000, 0) == nvn_timestamp_from_ns(10000));
        device.cpu = now_ns() - delay; device.gpu = device.cpu + offset;
        check_idle_gaps(device, heap);

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
