// Host service sessions (hle_sf.c) without a guest: a CMIF request on a served service (olsc:u; mnpp:app with its PID / Uid and input buffer; notif:a as a domain) succeeds with zeroed output and no special header; the camera context of the Switch 2
// (nn::camera::sdk::CreateCameraContext) is a host session with no camera: Initialize's command 3 and Finalize's 4 succeed, 21 copies an event's read end to the client in the special header,
// and the device commands (QueryDeviceConnection's 29, any other) fail with 0x7c4, "no camera device" (Star Fox's camera module and its game code read it so; the earlier 0xdc4 read as "no camera support")
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
// The same on a domain session as the SDK's notif:a client sends it: the process ID in a special header when pid, one B descriptor (the buffer buf) when buf, the domain header for
// object 1 at the first 16-byte boundary after them, then "SFCI"
static Result domain_request(uint32_t h, uint32_t *m, unsigned cmd, int pid, void *buf, uint32_t size) {
    memset(m, 0xcc, 0x100);
    m[0] = 4 | (buf ? 1u << 24 : 0); m[1] = 16 | (pid ? 1u << 31 : 0);
    uint32_t *p = m + 2;
    if (pid) { *p++ = 1; *p++ = 0; *p++ = 0; }
    if (buf) { uint64_t a = (uint64_t)(uintptr_t)buf; *p++ = size; *p++ = (uint32_t)a; *p++ = (uint32_t)(a >> 32 & 0xf) << 28 | (uint32_t)(a >> 36 & 7) << 2; }
    size_t cm = ((size_t)((uint8_t *)p - (uint8_t *)m) + 15) & ~(size_t)15;
    uint32_t *d = (uint32_t *)((uint8_t *)m + cm);
    d[0] = 1 | 16u << 16; d[1] = 1; d[2] = d[3] = 0;   // domain header: send a message to object 1, 16 bytes of data
    d[4] = 0x49434653; d[5] = 0; d[6] = cmd; d[7] = 0;
    return h_sf_Send(h, m, 0x100);
}
int main(void) {
    uint32_t m[0x40], h = 0;
    CHECK(h_sf_Connect(&h, "olsc:u") == RES_OK && h, "olsc:u is served");
    CHECK(request(h, m, 0) == RES_OK, "olsc:u request");
    CHECK(!(m[1] >> 31) && m[4] == 0x4f434653 && m[6] == RES_OK, "olsc:u: success, no special header, SFCO at +0x10");
    CHECK(h_sf_Connect(&h, "chat:u") == 0xe15u, "a service the host does not serve is not registered");

    uint32_t mn = 0, unserved = 0;
    CHECK(h_sf_Connect(&mn, "mnpp:app") == RES_OK && mn && mn != h, "mnpp:app is served");
    CHECK(h_sf_Connect(&unserved, "mnpp:sys") == 0xe15u && h_sf_Connect(&unserved, "mnpp:web") == 0xe15u, "other mnpp service names remain unserved");
    if (mn) {
        uint8_t empty[sizeof m - 0x20] = {0};
        // Initialize: PID in a special header, reserved u64 after SFCI; 3 alignment words + 4 CMIF words + 2 input words.
        memset(m, 0xcc, sizeof m);
        m[0] = 4; m[1] = 9 | 1u << 31;
        m[2] = 1; m[3] = 0x1234; m[4] = 0;   // PID descriptor, no copied or moved handles
        m[8] = 0x49434653; m[9] = 0; m[10] = 0; m[11] = 0;
        m[12] = m[13] = 0;
        CHECK(h_sf_Send(mn, m, sizeof m) == RES_OK, "mnpp:app Initialize with a PID and reserved u64");
        CHECK(m[0] == 0 && !(m[1] >> 31) && m[4] == 0x4f434653 && m[6] == RES_OK, "Initialize: success, no descriptors or handles");
        CHECK(!memcmp(m + 8, empty, sizeof empty), "Initialize returns no output data");

        // SubmitEvent: Uid after SFCI and a type-0x5 input buffer (one A descriptor); no output or retained event.
        uint8_t event[32], before[sizeof event];
        memset(event, 0xa5, sizeof event); memcpy(before, event, sizeof event);
        memset(m, 0xcc, sizeof m);
        uint64_t address = (uint64_t)(uintptr_t)event;
        m[0] = 4 | 1u << 20; m[1] = 11;   // 3 alignment words + 4 CMIF words + 4 Uid words
        m[2] = sizeof event; m[3] = (uint32_t)address;
        m[4] = (uint32_t)(address >> 32 & 0xf) << 28 | (uint32_t)(address >> 36 & 0x3fffff) << 2;
        m[8] = 0x49434653; m[9] = 0; m[10] = 1; m[11] = 0;
        m[12] = 1; m[13] = 2; m[14] = 3; m[15] = 4;
        CHECK(h_sf_Send(mn, m, sizeof m) == RES_OK, "mnpp:app SubmitEvent with a Uid and input buffer");
        CHECK(m[0] == 0 && !(m[1] >> 31) && m[4] == 0x4f434653 && m[6] == RES_OK, "SubmitEvent: success, no descriptors or handles");
        CHECK(!memcmp(m + 8, empty, sizeof empty), "SubmitEvent returns no output data");
        CHECK(!memcmp(event, before, sizeof event), "SubmitEvent leaves the input buffer untouched");
        h_sf_Close(mn);
    }

    // notif:a as nn::notification::ListAlarmSettings reaches it: connect, a domain, Initialize with the process ID, then List into the caller's array, which stays untouched
    uint32_t notif = 0, obj = 0; uint8_t alarms[8 * 0x40], before[sizeof alarms];
    memset(alarms, 0xa5, sizeof alarms); memcpy(before, alarms, sizeof alarms);
    CHECK(h_sf_Connect(&notif, "notif:a") == RES_OK && notif, "notif:a is served");
    CHECK(h_sf_ToDomain(&obj, notif) == RES_OK && obj == 1, "notif:a converts to a domain");
    CHECK(domain_request(notif, m, 1000, 1, NULL, 0) == RES_OK && m[8] == 0x4f434653 && m[10] == RES_OK, "notif:a Initialize with the process ID: success");
    CHECK(domain_request(notif, m, 520, 0, alarms, sizeof alarms) == RES_OK && m[8] == 0x4f434653 && m[10] == RES_OK, "notif:a ListAlarmSettings: success");
    CHECK(m[12] == 0, "no alarms listed");
    CHECK(!memcmp(alarms, before, sizeof alarms), "the caller's alarm array is not written");
    CHECK(domain_request(notif, m, 500, 0, alarms, 0x40) == RES_OK && m[10] == RES_OK && m[12] == 0, "notif:a RegisterAlarmSetting: success, ID 0");

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
    printf("t_sf: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
