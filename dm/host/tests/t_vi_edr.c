// Real nn::vi EDR responses, linked with the strong fixture frontend capability.
// Both modes start a fresh process (make vi-edr); no forensic probe is involved.
#include "../../gfx/hle_vi.c"
#include <assert.h>
#include <stddef.h>

uint64_t dm_nvn_frame_count(void) { return 0; }
void mtl_present_progress(uint64_t *queued, uint64_t *done) { *queued = *done = 0; }
double mtl_presented_time(uint64_t ticket) { (void)ticket; return 0; }
void mtl_present_layer(uint32_t width, uint32_t height) { (void)width; (void)height; }
uint64_t guest_ns(void) { return 0; }
uint64_t now_ns(void) { return 0; }
int dm_sdk_ounce(void) { return 0; }
int dm_opmode(void) { return 0; }

typedef struct { uint64_t before; uint8_t value, after[15]; } guarded8;
typedef struct { uint64_t before; uint16_t value; uint8_t after[14]; } guarded16;
typedef struct { uint64_t before; uint32_t value; uint8_t after[12]; } guarded32;
static void guards(const void *object, size_t bytes, size_t at, size_t width) {
    const unsigned char *p = object;
    for (size_t i = 0; i < bytes; i++) if (i < at || i >= at + width) assert(p[i] == 0xa5);
}
static void status(int hdr) {
    void *display = (void *)(uintptr_t)0x1234;
    for (unsigned mask = 0; mask < 8; mask++) {
        guarded8 on; guarded32 format; guarded16 peak;
        memset(&on, 0xa5, sizeof on); memset(&format, 0xa5, sizeof format); memset(&peak, 0xa5, sizeof peak);
        assert(h_vi_HdrStatus(mask & 1 ? &on.value : NULL, mask & 2 ? &format.value : NULL, mask & 4 ? &peak.value : NULL, display) == RES_OK);
        assert(on.value == (mask & 1 ? (unsigned)hdr : 0xa5u));
        assert(format.value == (mask & 2 ? (unsigned)hdr : 0xa5a5a5a5u));
        assert(peak.value == (mask & 4 ? (hdr ? 1000u : 0u) : 0xa5a5u));
        guards(&on, sizeof on, offsetof(guarded8, value), 1);
        guards(&format, sizeof format, offsetof(guarded32, value), 4);
        guards(&peak, sizeof peak, offsetof(guarded16, value), 2);
    }
    assert(h_vi_HdrStatus(NULL, NULL, NULL, NULL) == RES_OK);
    guarded16 white; memset(&white, 0xa5, sizeof white);
    assert(h_vi_PaperWhite(&white.value, NULL) == RES_OK && white.value == 203);
    guards(&white, sizeof white, offsetof(guarded16, value), 2);
    assert(h_vi_PaperWhite(NULL, display) == RES_OK);
}
int main(void) {
    const char *value = getenv("DM_TEST_HDR");
    int hdr = value && !strcmp(value, "1");
    assert(front_hdr_enabled() == hdr);
    status(hdr);
    assert(dm_vi_hdr_content() == (203u | (1000u << 16)));
    assert(h_vi_HdrExtLum(NULL, 0, 710) == RES_OK);
    assert(dm_vi_hdr_content() == (203u | (710u << 16)));
    assert(h_vi_HdrExtLum(NULL, 0, 0) == RES_OK);
    assert(dm_vi_hdr_content() == (203u | (710u << 16)));
    assert(h_vi_HdrExtLum(NULL, 0, 65536) == RES_OK);
    assert(dm_vi_hdr_content() == (203u | (710u << 16)));
    // Content peak updates do not change the virtual display capability or paper white.
    status(hdr);
    printf("vi EDR hdr=%d: status widths/nulls, SDR default, paper white203, content peak1000->710, invalid peaks ignored\n", hdr);
    return 0;
}
