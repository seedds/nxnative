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
@end
@implementation ClockHeap
- (NSData *)resolveCounterRange:(NSRange)range {
    MTL4TimestampHeapEntry entry = {.timestamp = self.boundary};
    return range.location == 0 && range.length == 1 ? [NSData dataWithBytes:&entry length:sizeof entry] : nil;
}
@end

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
