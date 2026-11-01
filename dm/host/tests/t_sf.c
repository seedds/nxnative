// Host service sessions (hle_sf.c): the camera context Mario Kart World opens at boot (no camera on the host). The requests are built as the game's HIPC client builds them (type 4,
// CMIF "SFCI" at +0x10); the answers are read where that client reads them: a copied handle in the special header, the result in "SFCO" at +0x10
#include "../hle_sf.c"
static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)
uint32_t hle_new_idle_event(void) { return 0x1234; }
static void request(uint32_t *m, unsigned cmd) {
    memset(m, 0, 0x100);
    m[0] = 4; m[1] = 8;   // a request, 8 data words, no special header, no descriptors
    m[4] = 0x49434653; m[5] = 1; m[6] = cmd;
}
int main(void) {
    uint32_t h = 0, m[0x40];
    CHECK(h_cam_Ctx(&h) == RES_OK && sf_slot(h) >= 0, "camera context: a host session");
    request(m, 3); CHECK(h_sf_Send(h, m, sizeof m) == RES_OK, "open camera: delivered");
    CHECK(!(m[1] >> 31) && m[4] == 0x4f434653 && m[6] == 0xdc4, "open camera: no special header, SFCO result 0xdc4 (no camera)");
    request(m, 21); CHECK(h_sf_Send(h, m, sizeof m) == RES_OK, "camera event: delivered");
    CHECK(m[1] >> 31 && m[2] == 2 && m[3] == 0x1234, "camera event: one copy handle in the special header");
    CHECK(m[4] == 0x4f434653 && m[6] == RES_OK && (m[1] & 0x3ff) == (sizeof m - 0x10) / 4, "camera event: SFCO success at +0x10, data words after the special header");
    request(m, 29); h_sf_Send(h, m, sizeof m); CHECK(m[6] == 0xdc4, "other camera commands: no camera");
    h_sf_Close(h); CHECK(sf_slot(h) < 0, "camera session closes");
    printf("sf: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
