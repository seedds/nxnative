// Native CMIF report fixtures with real event handles, including domain lifetime.
#include "../hle.h"
#undef HLE_FORCE
#define HLE_FORCE(mangled, why, ret, cname, ...) static ret cname(__VA_ARGS__)
#include "../hle_sf.c"
#include "../hle_prepo.c"
#include "../hle_thread.c"
#include <assert.h>
uint64_t dm_entropy64(void) { return 42; }
struct dm_transfer_memory { unsigned mapped; };
static struct dm_transfer_memory memory;
static uint8_t payload[0x12000];
uint32_t dm_transfer_map(uint32_t h, size_t size, unsigned perm, dm_transfer_memory **out, const void **data) {
    assert(perm == 0);
    if (h != 0xe0000) return 0xe401;
    if (size != sizeof payload) return 0xca01;
    if (memory.mapped) return 0xfa01;
    memory.mapped = 1; *out = &memory; *data = payload; return 0;
}
void dm_transfer_unmap(dm_transfer_memory *m) { if (m) { assert(m == &memory && m->mapped); m->mapped = 0; } }
static const uint8_t report[] = {0x81, 0xa5, 'S', 't', 'a', 'r', 't', 0xc3};
static const char name[] = "session_start";
static uint32_t packet[128];
static void pointer(uint32_t *p, const void *data, size_t n) {
    uintptr_t a = (uintptr_t)data;
    p[0] = (uint32_t)n << 16 | ((a >> 32) & 15) << 12 | ((a >> 36) & 63) << 6;
    p[1] = (uint32_t)a;
}
static void request(unsigned cmd, unsigned domain, unsigned object, int async) {
    memset(packet, 0, sizeof packet);
    packet[0] = 4 | (async ? 1u << 16 : 0);
    unsigned at = 8;
    if (async) {
        packet[1] = 0x80000000u; packet[2] = 3; // SendPid and one copy handle
        packet[3] = 0xdeadbeef; packet[4] = 0xffffffff; packet[5] = 0xe0000;
        pointer(packet + 6, name, sizeof name); at = 32;
    }
    unsigned bytes = async ? (cmd == 10501 ? 24 : 40) : 0;
    unsigned cm = (at + 15) & ~15u, end = cm + 16 + bytes;
    if (domain) {
        packet[cm / 4] = 1 | ((16 + bytes) << 16);
        packet[cm / 4 + 1] = object; cm += 16; end += 16;
    }
    packet[cm / 4] = 0x49434653; packet[cm / 4 + 2] = cmd;
    if (async) {
        uint8_t *args = (uint8_t *)packet + cm + 16;
        args[0] = 3; // only the low Boolean bit matters
        uint32_t size = sizeof payload, used = sizeof report;
        memcpy(args + 4, &size, 4); memcpy(args + 8, &used, 4); if (cmd == 10500) args[16] = 1;
    }
    packet[1] |= (end - at) / 4;
}
static void send(uint32_t h, unsigned domain, Result result) {
    assert(!h_sf_Send(h, packet, sizeof packet));
    assert(packet[domain ? 8 : 4] == 0x4f434653);
    if (packet[domain ? 10 : 6] != result) fprintf(stderr, "CMIF %#x expected %#x\n", packet[domain ? 10 : 6], result);
    assert(packet[domain ? 10 : 6] == result);
}
static void protocol(void) {
    memcpy(payload, report, sizeof report);
    for (unsigned domain = 0; domain <= 1; domain++) {
        uint32_t root, id, clone; assert(!h_sf_Connect(&root, "prepo:u"));
        uint16_t capacity; assert(!h_sf_PtrBuf(&capacity, root) && capacity >= sizeof name);
        if (domain) assert(!h_sf_ToDomain(&id, root) && id == 1);
        request(10500, domain, 1, 1); send(root, domain, 0);
        uint32_t child = packet[domain ? 12 : 3]; assert(child && memory.mapped);
        if (domain) assert(packet[4] == 1); else assert(packet[2] == 32);
        assert(!h_sf_Dup(&clone, root)); h_sf_Close(root); root = clone;
        uint32_t session = domain ? root : child;
        request(0, domain, child, 0); send(session, domain, 0);
        assert(packet[2] == 2); uint32_t event = packet[3];
        uint64_t regs[32] = {0, (uintptr_t)&event, 1, 0};
        assert(svc_thread(0x18, regs) && !regs[0] && !regs[1]);
        request(2, domain, child, 0); send(session, domain, 0);
        request(1, domain, child, 0); send(session, domain, 0);
        sf_close_handle(event);
        request(0, domain, child, 0); send(session, domain, 0); event = packet[3];
        h_sf_Close(root); if (!domain) h_sf_Close(child);
        assert(!memory.mapped);
        regs[0] = 0; regs[1] = (uintptr_t)&event;
        assert(svc_thread(0x18, regs) && !regs[0]); // copied event survives task close
        sf_close_handle(event);
    }
    uint32_t root; assert(!h_sf_Connect(&root, "prepo:u"));
    request(10500, 0, 1, 1); packet[16] = packet[17] = packet[18] = packet[19] = 0; send(root, 0, PP_ARGUMENT);
    request(10500, 0, 1, 1); packet[5] = 0xdead; send(root, 0, 0xe401);
    request(10500, 0, 1, 1); packet[13] = sizeof payload - 1; send(root, 0, 0xca01);
    request(10500, 0, 1, 1); packet[14] = sizeof payload + 1; send(root, 0, PP_PAYLOAD); assert(!memory.mapped);
    request(10500, 0, 1, 1); pointer(packet + 6, (void *)1, 10); send(root, 0, PP_ARGUMENT);
    request(40100, 0, 1, 0); send(root, 0, PP_PERMISSION);
    request(10400, 0, 1, 0); send(root, 0, 0); uint32_t sid = packet[8]; assert(sid);
    request(10400, 0, 1, 0); send(root, 0, 0); assert(sid == packet[8]);
    // Transfer-memory reports have their mapping extent as the limit, not the
    // direct-report 64 KiB limit. Exercise the no-UID command with a large map.
    static const uint8_t prefix[] = {0x81, 0xa1, 'K', 0xdb, 0, 1, 0, 0};
    memcpy(payload, prefix, sizeof prefix); memset(payload + sizeof prefix, 'v', 65536);
    request(10501, 0, 1, 1); packet[14] = 65536 + sizeof prefix; send(root, 0, 0);
    h_sf_Close(packet[3]); assert(!memory.mapped);
    h_sf_Close(root);
    assert(!h_sf_Connect(&root, "prepo:s")); request(10500, 0, 1, 1); send(root, 0, PP_PERMISSION); h_sf_Close(root);
}
static void validation(void) {
    assert(pp_payload(report, sizeof report, 1)); assert(!pp_payload(report, sizeof report, 0));
    for (size_t i = 0; i < sizeof report; i++) assert(!pp_payload(report, i, 1));
    static const uint8_t duplicate[] = {0x82, 0xa1, 'K', 0, 0xa1, 'k', 0};
    static const uint8_t nested[] = {0x81, 0xa1, 'K', 0x92, 0x81, 0xa1, 'A', 0, 0x81, 0xa1, 'A', 0xff};
    assert(!pp_payload(duplicate, sizeof duplicate, 1)); assert(pp_payload(nested, sizeof nested, 1));
    uint8_t bad[sizeof nested]; memcpy(bad, nested, sizeof bad); bad[10] = 'B'; assert(!pp_payload(bad, sizeof bad, 1));
    static const uint8_t invalid[][12] = {
        {0x80}, {0x81,0xa1,'K',0xc0}, {0x81,0xa1,'K',0xca,0x7f,0x80,0,0},
        {0x81,0xa1,'K',0x91,0x90}, {0x81,0xa1,'K',0x81,0xa1,'A',0xc4,0},
        {0x81,0xa1,'K',0xa2,0xc0,0x80}, {0x81,0xa1,'1',0},
        {0x81,0xa1,'K',0x81,0xa1,'A',0x81,0xa1,'B',0x81,0xa1,'C'},
    };
    static const size_t lengths[] = {1, 4, 8, 5, 8, 6, 4, 12};
    for (unsigned i = 0; i < sizeof invalid / sizeof *invalid; i++) assert(!pp_payload(invalid[i], lengths[i], 1));
    sf_object user = sf_prepo_root(1), system = sf_prepo_root(2);
    uint8_t args[32] = {0}; sf_reply reply = {0};
    sf_request q = {.command=10102, .data=args, .size=8, .nx=1, .na=1, .send_pid=1, .pid=DM_GUEST_PROCESS_ID,
        .x={{(void *)name,sizeof name,0}}, .a={{(void *)report,sizeof report,0}}};
    assert(!user.request(user.state, &q, &reply)); q.command = 10100; assert(user.request(user.state, &q, &reply) == PP_PAYLOAD);
    q.command = 10103; q.size = 24; assert(user.request(user.state, &q, &reply) == PP_ARGUMENT);
    args[0] = 1; assert(!user.request(user.state, &q, &reply));
    q.a[0].size = 65537; assert(user.request(user.state, &q, &reply) == PP_PAYLOAD); q.a[0].size = sizeof report; q.send_pid = 0; assert(user.request(user.state, &q, &reply) == PP_SHAPE);
    q.command = 20101; assert(!system.request(system.state, &q, &reply));
    q.x[0].data = "Bad"; q.x[0].size = 4; assert(system.request(system.state, &q, &reply) == PP_NAME);
}
int main(void) {
    svc_thread_main_init(); validation(); protocol();
    int handles, threads, objects; thr_counts(&handles, &threads, &objects);
    assert(handles == 1 && threads == 1 && objects == 1);
    puts("PASS: prepo validation, native CMIF sessions, domains, completion events and lifetime");
}
