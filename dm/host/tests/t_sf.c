// Host service sessions (hle_sf.c) without a guest: a CMIF request on a served service (olsc:u) succeeds with zeroed output and no special header; the camera context of the Switch 2
// (nn::camera::sdk::CreateCameraContext) is a host session with no camera: Initialize's command 3 and Finalize's 4 succeed, 21 copies an event's read end to the client in the special header,
// and the device commands (QueryDeviceConnection's 29, any other) fail with 0x7c4, "no camera device" (Star Fox's camera module and its game code read it so; the earlier 0xdc4 read as "no camera support")
#include "../hle.h"
#undef HLE_FORCE
#define HLE_FORCE(mangled, why, ret, cname, ...) static ret cname(__VA_ARGS__)
#include "../hle_sf.c"
#include "../hle_auddmg.c"
sf_object sf_audsmx_root(void) { abort(); }
Result sf_audsmx_attach(uint64_t p, uint32_t s, uint32_t d) { (void)p; (void)s; (void)d; abort(); }
Result sf_audsmx_detach(uint64_t p, uint32_t s) { (void)p; (void)s; abort(); }

static int checks, fails, events, closed_handles;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)
uint32_t hle_new_event(uint32_t *write_end) { events++; if (write_end) *write_end = 0x2000 + events; return 0x1000 + events; }
int svc_thread(int num, uint64_t *regs) { CHECK(num == 0x16 && regs[0], "close a returned kernel handle"); closed_handles++; regs[0] = 0; return 1; }

static int roots_closed, children_closed, calls;
static void root_close(void *state) { (void)state; roots_closed++; }
static void child_close(void *state) { free(state); children_closed++; }
static Result child_request(void *state, const sf_request *q, sf_reply *r) {
    calls++;
    if (q->command != 0x12345678u) return 0x1ba0a;
    if (q->size < 4 || q->na != 1 || q->nb != 1 || q->a[0].size != 4 || q->b[0].size != 4) return 0x1ce0a;
    CHECK(q->a[0].mode == 0 && q->b[0].mode == 0, "MapAlias mode decoded independently of SF attributes");
    uint32_t input, scalar;
    memcpy(&input, q->a[0].data, 4); memcpy(&scalar, q->data, 4);
    uint32_t sum = input + scalar + *(uint32_t *)state;
    memcpy(q->b[0].data, &sum, 4);
    memcpy(r->data, &sum, 4); r->size = 4;
    return RES_OK;
}
static Result root_request(void *state, const sf_request *q, sf_reply *r) {
    (void)state; calls++;
    if (q->command != 0) return 0x1ba0a;
    if (q->size < 4 || !q->send_pid || q->na || q->nb || q->ncopy > 1 || q->nmove) return 0x1ce0a;
    if (q->ncopy) CHECK(q->copies[0] == 0xffff8001u, "copied current-process pseudo handle reaches the service intact");
    CHECK(q->pid == 0x80, "SendPid uses the guest process identity, not the caller's placeholder");
    uint32_t *value = malloc(4); memcpy(value, q->data, 4);
    r->child = (sf_object){child_request, child_close, value};
    return RES_OK;
}
// Packet fixtures follow the client wire ABI. The reply assertions below do not use the server's decoder.
static void factory_packet(uint32_t *m, uint32_t object) {
    memset(m, 0, 0x100);
    m[0] = 6; m[1] = 0x80000000u | (object ? 15 : 11); m[2] = 1;
    m[3] = 0xdeadbeef; m[4] = 0xfeedface;   // a kernel-supplied PID cannot be forged here
    unsigned at = 8;
    if (object) { m[at] = 1 | 20u << 16; m[at + 1] = object; m[at + 3] = 0xabc; at += 4; }
    m[at] = 0x49434653; m[at + 1] = 1; m[at + 2] = 0; m[at + 3] = object ? 0 : 0xabc; m[at + 4] = 10;
}
static void descriptor(uint32_t *p, const void *addr, unsigned bytes) {
    uintptr_t a = (uintptr_t)addr;
    p[0] = bytes; p[1] = (uint32_t)a; p[2] = (uint32_t)(a >> 32 & 15) << 28 | (uint32_t)(a >> 36) << 2;
}
static void buffer_packet(uint32_t *m, uint32_t object, uint32_t *in, uint32_t *out) {
    memset(m, 0, 0x100);
    m[0] = 6 | 1u << 20 | 1u << 24; m[1] = object ? 13 : 9;
    descriptor(m + 2, in, 4); descriptor(m + 5, out, 4);
    unsigned at = 8;
    if (object) { m[at] = 1 | 20u << 16; m[at + 1] = object; m[at + 3] = 0xdef; at += 4; }
    m[at] = 0x49434653; m[at + 1] = 1; m[at + 2] = 0x12345678; m[at + 3] = object ? 0 : 0xdef; m[at + 4] = 20;
}
static void typed_sessions(void) {
    _Alignas(16) uint32_t m[0x40];
    uint32_t root = sf_object_new("test", (sf_object){root_request, root_close, NULL});
    factory_packet(m, 0);
    m[2] = 3; m[5] = 0xffff8001u; // copied process handle follows SendPid, before alignment padding
    CHECK(h_sf_Send(root, m, sizeof m) == RES_OK, "factory request succeeds at transport level");
    CHECK(m[1] >> 31 && m[2] == 32 && m[3] && m[4] == 0x4f434653 && m[6] == 0, "child interface is a moved session handle, not a copy handle");
    CHECK(m[5] == 1 && m[7] == 0xabc, "reply version and inline context preserved");
    uint32_t child = m[3];
    h_sf_Close(root);
    CHECK(roots_closed == 1 && children_closed == 0, "child session survives its manager session");
    uint32_t input = 30, output[2] = {0, 0xfedcba98};
    buffer_packet(m, 0, &input, output);
    CHECK(h_sf_Send(child, m, sizeof m) == RES_OK && m[6] == 0 && m[8] == 60 && m[7] == 0xdef, "32-bit command, inline data and MapAlias buffers dispatched");
    CHECK(output[0] == 60 && output[1] == 0xfedcba98, "only the declared output buffer is written");
    buffer_packet(m, 0, &input, output); m[10] = 17;
    CHECK(h_sf_Send(child, m, sizeof m) == RES_OK && m[6] == 0x1ba0a && !(m[1] >> 31), "service failure is a CMIF result, not successful zeroed output or a transport failure");
    uint32_t dup;
    CHECK(h_sf_Dup(&dup, child) == RES_OK, "duplicate a typed session");
    h_sf_Close(child);
    CHECK(children_closed == 0, "duplicate retains service state");
    h_sf_Close(dup);
    CHECK(children_closed == 1, "last duplicate closes service state once");

    root = sf_object_new("domain-test", (sf_object){root_request, root_close, NULL});
    uint32_t object = 0;
    CHECK(h_sf_ToDomain(&object, root) == RES_OK && object == 1, "domain root object ID");
    CHECK(h_sf_Dup(&dup, root) == RES_OK, "duplicate a domain session");
    factory_packet(m, 1);
    CHECK(h_sf_Send(dup, m, sizeof m) == RES_OK && !(m[1] >> 31) && m[4] == 1 && m[8] == 0x4f434653 && m[10] == 0, "domain child uses the output object table, no moved handle");
    child = m[12];
    CHECK(child > 1 && m[11] == 0xabc, "domain child ID and domain context");
    h_sf_Close(root);
    buffer_packet(m, child, &input, output);
    CHECK(h_sf_Send(dup, m, sizeof m) == RES_OK && m[10] == 0 && m[12] == 60, "domain objects survive closing one duplicate");
    int before = calls;
    buffer_packet(m, child, &input, output); m[1] = 1;
    CHECK(h_sf_Send(dup, m, sizeof m) != RES_OK && calls == before, "truncated data words never reach the service");
    buffer_packet(m, child, &input, output); m[0] = 6 | 15u << 20 | 15u << 24;
    CHECK(h_sf_Send(dup, m, 0x40) != RES_OK && calls == before, "truncated descriptors never reach the service");
    buffer_packet(m, child, &input, output); m[8] = 1 | 0xffff0000u;
    CHECK(h_sf_Send(dup, m, sizeof m) != RES_OK && calls == before, "domain payload size bounded by HIPC data words");
    memset(m, 0, sizeof m); m[0] = 4; m[1] = 8; m[4] = 2; m[5] = child;
    CHECK(h_sf_Send(dup, m, sizeof m) == RES_OK && children_closed == 2, "domain close needs no SFCI header and destroys only its object");
    buffer_packet(m, child, &input, output);
    CHECK(h_sf_Send(dup, m, sizeof m) != RES_OK && calls == before, "closed object ID is not dispatched");
    h_sf_Close(dup);
    CHECK(roots_closed == 2 && children_closed == 2, "last domain session releases remaining objects once");
}

// A CMIF request for command cmd on a non-domain session (HIPC type 4, no special header, no descriptors): the data words from +8, "SFCI" at +0x10
static Result request(uint32_t h, uint32_t *m, unsigned cmd) {
    memset(m, 0xcc, 0x100);
    m[0] = 4; m[1] = 8;
    m[4] = 0x49434653; m[5] = 0; m[6] = cmd; m[7] = 0;
    return h_sf_Send(h, m, 0x100);
}
int main(void) {
    uint32_t m[0x40], h = 0;
    CHECK(h_sf_Connect(&h, "olsc:u") == RES_OK && h, "olsc:u is served");
    CHECK(request(h, m, 0) == RES_OK, "olsc:u request");
    CHECK(!(m[1] >> 31) && m[4] == 0x4f434653 && m[6] == RES_OK, "olsc:u: success, no special header, SFCO at +0x10");
    CHECK(h_sf_Connect(&h, "chat:u") == 0xe15u, "a service the host does not serve is not registered");

    uint32_t cam = 0;
    CHECK(h_cam_Context(&cam) == RES_OK && cam, "the camera context is a host session");
    CHECK(request(cam, m, 3) == RES_OK && m[4] == 0x4f434653 && m[6] == RES_OK && !(m[1] >> 31), "initializing the camera library: success");
    CHECK(request(cam, m, 29) == RES_OK && m[6] == 0x7c4u && !(m[1] >> 31), "querying the device: 0x7c4, no camera device");
    CHECK(events == 0, "no event made for a failing command");
    CHECK(request(cam, m, 21) == RES_OK, "the camera's event");
    CHECK(m[1] >> 31 && (m[2] >> 1 & 15) == 1 && (m[2] & 1) == 0 && (m[2] >> 5 & 15) == 0, "special header: one copy handle, no process ID, no move handle");
    CHECK(m[3] == 0x1001 && events == 1, "the copied handle is the new event's read end");
    CHECK(m[4] == 0x4f434653 && m[6] == RES_OK, "SFCO stays at +0x10 after the special header and handle");
    CHECK((m[1] & 0x3ff) == (0x100 - 0x10) / 4, "the data words start after the handle");
    uint32_t dup = 0;
    CHECK(h_sf_Dup(&dup, cam) == RES_OK && dup && dup != cam, "a duplicate of the camera session");
    CHECK(request(dup, m, 5) == RES_OK && m[6] == 0x7c4u, "the duplicate answers as the camera");
    CHECK(request(dup, m, 4) == RES_OK && m[6] == RES_OK, "finalizing the camera library: success");
    h_sf_Close(dup); h_sf_Close(cam); h_sf_Close(h);
    typed_sessions();
    printf("t_sf: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
