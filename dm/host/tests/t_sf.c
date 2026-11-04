// Host service sessions (hle_sf.c) without a guest: a CMIF request on a served service (olsc:u) succeeds with zeroed output and no special header; the camera context of the Switch 2
// (nn::camera::sdk::CreateCameraContext) is a host session with no camera: every command fails with 0xdc4 except 21, which copies an event's read end to the client in the special header
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
int main(void) {
    uint32_t m[0x40], h = 0;
    CHECK(h_sf_Connect(&h, "olsc:u") == RES_OK && h, "olsc:u is served");
    CHECK(request(h, m, 0) == RES_OK, "olsc:u request");
    CHECK(!(m[1] >> 31) && m[4] == 0x4f434653 && m[6] == RES_OK, "olsc:u: success, no special header, SFCO at +0x10");
    CHECK(h_sf_Connect(&h, "chat:u") == 0xe15u, "a service the host does not serve is not registered");

    uint32_t cam = 0;
    CHECK(h_cam_Context(&cam) == RES_OK && cam, "the camera context is a host session");
    CHECK(request(cam, m, 3) == RES_OK && m[4] == 0x4f434653 && m[6] == 0xdc4u && !(m[1] >> 31), "opening a camera: 0xdc4, no camera");
    CHECK(events == 0, "no event made for a failing command");
    CHECK(request(cam, m, 21) == RES_OK, "the camera's event");
    CHECK(m[1] >> 31 && (m[2] >> 1 & 15) == 1 && (m[2] & 1) == 0 && (m[2] >> 5 & 15) == 0, "special header: one copy handle, no process ID, no move handle");
    CHECK(m[3] == 0x1001 && events == 1, "the copied handle is the new event's read end");
    CHECK(m[4] == 0x4f434653 && m[6] == RES_OK, "SFCO stays at +0x10 after the special header and handle");
    CHECK((m[1] & 0x3ff) == (0x100 - 0x10) / 4, "the data words start after the handle");
    uint32_t dup = 0;
    CHECK(h_sf_Dup(&dup, cam) == RES_OK && dup && dup != cam, "a duplicate of the camera session");
    CHECK(request(dup, m, 5) == RES_OK && m[6] == 0xdc4u, "the duplicate answers as the camera");
    h_sf_Close(dup); h_sf_Close(cam); h_sf_Close(h);
    printf("t_sf: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
