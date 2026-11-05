// Explicit device integration: real microphone -> sysmix service -> shared ring.
// No recorded audio is written to disk. Run only through capture-service.
#import <Foundation/Foundation.h>
#include "../sf_service.h"
#include <time.h>
uint64_t guest_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
void dm_guest_write_prepare(void *addr, size_t n) { (void)addr; (void)n; }
uint32_t hle_new_event(uint32_t *w) { (void)w; abort(); }
uint32_t hle_dup_readable_handle(uint32_t h) { (void)h; abort(); }
void sf_close_handle(uint32_t h) { if (h) abort(); }
int svc_thread(int n, uint64_t *r) { (void)n; (void)r; abort(); }
static Result call(sf_object o, uint32_t cmd, const void *args, size_t size, sf_reply *reply) {
    sf_request q = {.command = cmd, .data = args, .size = size};
    memset(reply, 0, sizeof *reply); return o.request(o.state, &q, reply);
}
int main(int argc, const char **argv) {
    @autoreleasepool {
        if (argc != 2 || strcmp(argv[1], "--microphone")) return 2;
        uint32_t factory[] = {1, 0, 0, 0};
        sf_request q = {.command = 0, .data = (void *)factory, .size = 16, .send_pid = 1, .pid = DM_GUEST_PROCESS_ID,
            .ncopy = 1, .copies = {0xffff8001u}};
        sf_reply reply = {0}; sf_object root = sf_audsmx_root();
        if (root.request(root.state, &q, &reply)) return 1;
        sf_object user = reply.child;
        uint8_t *work = aligned_alloc(4096, 0x4000); if (!work) return 1; memset(work, 0, 0x4000);
        uint64_t create[] = {UINT64_C(2) << 32 | 1, DM_GUEST_PROCESS_ID, (uintptr_t)work, 0x4000};
        q = (sf_request){.command = 1, .data = (void *)create, .size = 32, .send_pid = 1, .pid = DM_GUEST_PROCESS_ID,
            .na = 1, .a = {{"capture test", 12, 0}}};
        memset(&reply, 0, sizeof reply);
        if (user.request(user.state, &q, &reply)) return 1;
        sf_object stream = reply.child; uint32_t id; memcpy(&id, reply.data, 4);
        uint32_t open[] = {2, 1, 48000, 4800, 0, 0};
        if (call(stream, 0, open, sizeof open, &reply) || sf_audsmx_attach(DM_GUEST_PROCESS_ID, id, 4)) return 1;
        uint64_t total = 0, nonzero = 0;
        for (unsigned loop = 0; loop < 100; loop++) {
            [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
            // Independent ring consumer: acquire write cursor, copy/read PCM,
            // release read cursor. This permits a full ring wrap during the test.
            uint32_t rd = __atomic_load_n((uint32_t *)(work + 0x44), __ATOMIC_RELAXED);
            uint32_t wr = __atomic_load_n((uint32_t *)(work + 0x1004), __ATOMIC_ACQUIRE);
            while (rd != wr) {
                int16_t sample; memcpy(&sample, work + 0x1040 + rd * 2, 2);
                total++; nonzero += sample != 0; rd = (rd + 1) % 4801;
            }
            __atomic_store_n((uint32_t *)(work + 0x44), rd, __ATOMIC_RELEASE);
        }
        if (sf_audsmx_detach(DM_GUEST_PROCESS_ID, id)) return 1;
        uint32_t cursor = *(uint32_t *)(work + 0x1004);
        [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
        int ok = total > 1000 && cursor == *(uint32_t *)(work + 0x1004);
        if (call(stream, 1, NULL, 0, &reply)) ok = 0;
        stream.close(stream.state); user.close(user.state); free(work);
        printf("audsmx capture: %llu frames read, %llu nonzero; detach/close %s\n", (unsigned long long)total, (unsigned long long)nonzero, ok ? "PASS" : "FAIL");
        return !ok;
    }
}
