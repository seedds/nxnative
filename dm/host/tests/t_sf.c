// Host service sessions (hle_sf.c) without a guest: a CMIF request on a served service (olsc:u) succeeds with zeroed output and no special header; the camera context of the Switch 2
// (nn::camera::sdk::CreateCameraContext) is a host session with no camera: Initialize's command 3 and Finalize's 4 succeed, 21 copies an event's read end to the client in the special header,
// and the device commands (QueryDeviceConnection's 29, any other) fail with 0x7c4, "no camera device" (Star Fox's camera module and its game code read it so; the earlier 0xdc4 read as "no camera support")
// nsd:a and nsd:u answer as a retail console offline (PATAPON 1+2 REPLAY and Hogwarts Legacy aborted at boot when sm refused them, LNPANDY/NX2Apple#426 and #475): the environment type 1
// (lp1) as output data, "lp1" in a mapped (B) or receive-list (C) output buffer, the type a Set changed until Delete, Resolve failing and ResolveEx succeeding with a failed InnerResult, the
// answers in a domain session after the domain header, and any other command failing with ResultUnknownCommandId
#include "../hle.h"
#undef HLE_FORCE
#define HLE_FORCE(mangled, why, ret, cname, ...) static ret cname(__VA_ARGS__)
#include "../hle_sf.c"

static int checks, fails, events;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)
uint32_t hle_new_event(uint32_t *write_end) { events++; if (write_end) *write_end = 0x2000 + events; return 0x1000 + events; }

// A CMIF request for command cmd on a non-domain session (HIPC type 4, no special header, no descriptors): the data words from +8, "SFCI" at +0x10
static Result request(uint32_t h, uint32_t *m, unsigned cmd) {
    memset(m, 0xcc, 0x100);
    m[0] = 4; m[1] = 8;
    m[4] = 0x49434653; m[5] = 0; m[6] = cmd; m[7] = 0;
    return h_sf_Send(h, m, 0x100);
}
// The same with one B descriptor (an AutoSelect output buffer sent mapped) and a u32 argument: the descriptor at +8, the data words from +0x14, "SFCI" at +0x20, the argument at +0x30
static Result request_b(uint32_t h, uint32_t *m, unsigned cmd, void *buf, uint32_t n, uint32_t arg) {
    uint64_t a = (uintptr_t)buf;
    memset(m, 0xcc, 0x100);
    m[0] = 4 | 1u << 24; m[1] = 12;
    m[2] = n; m[3] = (uint32_t)a; m[4] = (uint32_t)(a >> 36 & 0x3fffff) << 2 | (uint32_t)(a >> 32 & 15) << 28;
    m[8] = 0x49434653; m[9] = 0; m[10] = cmd; m[11] = 0; m[12] = arg;
    return h_sf_Send(h, m, 0x100);
}
// No descriptors, one receive list entry (an AutoSelect output buffer sent as a pointer): mode 3 after 8 data words, at +0x28
static Result request_c(uint32_t h, uint32_t *m, unsigned cmd, void *buf, uint32_t n) {
    uint64_t a = (uintptr_t)buf;
    memset(m, 0xcc, 0x100);
    m[0] = 4; m[1] = 8 | 3u << 10;
    m[4] = 0x49434653; m[5] = 0; m[6] = cmd; m[7] = 0;
    m[10] = (uint32_t)a; m[11] = (uint32_t)(a >> 32 & 0xffff) | n << 16;
    return h_sf_Send(h, m, 0x100);
}
// A request to object 1 of a domain session: the domain header at +0x10 (type 1, a message), "SFCI" at +0x20, the argument at +0x30
static Result request_dom(uint32_t h, uint32_t *m, unsigned cmd, uint32_t arg) {
    memset(m, 0xcc, 0x100);
    m[0] = 4; m[1] = 12;
    m[4] = 1 | 0x14u << 16; m[5] = 1; m[6] = 0; m[7] = 0;
    m[8] = 0x49434653; m[9] = 0; m[10] = cmd; m[11] = 0; m[12] = arg;
    return h_sf_Send(h, m, 0x100);
}
static void check_nsd(void) {
    uint32_t m[0x40], a = 0, u = 0;
    CHECK(h_sf_Connect(&a, "nsd:a") == RES_OK && a, "nsd:a is served");
    CHECK(h_sf_Connect(&u, "nsd:u") == RES_OK && u && u != a, "nsd:u is served");
    CHECK(request(a, m, 100) == RES_OK && m[4] == 0x4f434653 && m[6] == RES_OK && (m[8] & 0xff) == 1, "GetApplicationServerEnvironmentType: 1, retail lp1");
    CHECK(request(u, m, 100) == RES_OK && m[6] == RES_OK && (m[8] & 0xff) == 1, "the same on nsd:u");
    char env[16]; memset(env, 0x55, sizeof env);
    CHECK(request_b(u, m, 11, env, 8, 0) == RES_OK && m[6] == RES_OK, "GetEnvironmentIdentifier into a mapped buffer");
    CHECK(!memcmp(env, "lp1\0\0\0\0\0", 8) && (uint8_t)env[8] == 0x55, "the identifier is lp1, 8 bytes and no more");
    memset(env, 0x55, sizeof env);
    CHECK(request_c(a, m, 11, env, 8) == RES_OK && m[6] == RES_OK && !memcmp(env, "lp1", 4), "GetEnvironmentIdentifier into a receive list buffer");
    CHECK(request(a, m, 11) == RES_OK && m[6] == RES_FAIL, "GetEnvironmentIdentifier without a buffer fails");
    CHECK(request_b(a, m, 101, env, 0, 3) == RES_OK && m[6] == RES_OK, "SetApplicationServerEnvironmentType 3");
    CHECK(request(u, m, 100) == RES_OK && (m[8] & 0xff) == 3, "the type set is the one reported, on every session");
    CHECK(request(a, m, 102) == RES_OK && m[6] == RES_OK && request(a, m, 100) == RES_OK && (m[8] & 0xff) == 1, "DeleteApplicationServerEnvironmentType: lp1 again");
    char fqdn[0x100] = "%.example.nintendo.net";
    CHECK(request_b(u, m, 20, fqdn, sizeof fqdn, 0) == RES_OK && m[6] == RES_FAIL, "Resolve fails: offline");
    CHECK(request_b(u, m, 21, fqdn, sizeof fqdn, 0) == RES_OK && m[6] == RES_OK && m[8] == RES_FAIL, "ResolveEx: success, the InnerResult a failure");
    CHECK(request(u, m, 12) == RES_OK && m[6] == RES_OK && !m[8] && !m[9] && !m[10] && !m[11], "GetDeviceId: zero");
    CHECK(request(u, m, 63) == RES_OK && m[6] == RES_OK && !(m[8] & 0xff), "IsChangeEnvironmentIdentifierDisabled: false");
    CHECK(request(a, m, 10) == RES_OK && m[6] == 0x1ba0au, "a command the host does not answer fails with ResultUnknownCommandId");
    uint32_t obj = 0;
    CHECK(h_sf_ToDomain(&obj, u) == RES_OK && obj == 1, "nsd:u as a domain");
    CHECK(request_dom(u, m, 100, 0) == RES_OK && m[8] == 0x4f434653 && m[10] == RES_OK && (m[12] & 0xff) == 1, "a domain request: SFCO after the domain header, the type after it");
    h_sf_Close(a); h_sf_Close(u);
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
    check_nsd();
    printf("t_sf: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
