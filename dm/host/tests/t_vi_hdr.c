// Observable nn::vi HDR responses through the real HLE implementation. Each mode is a fresh process (make vi-hdr).
#include "../../gfx/hle_vi.c"
#include <assert.h>
#include <stddef.h>
#include <unistd.h>

uint64_t dm_nvn_frame_count(void) { return 0; }
void mtl_present_progress(uint64_t *q, uint64_t *d) { *q = *d = 0; }
double mtl_presented_time(uint64_t n) { (void)n; return 0; }
void mtl_present_layer(uint32_t w, uint32_t h) { (void)w; (void)h; }
uint64_t guest_ns(void) { return 0; }
uint64_t now_ns(void) { return 0; }
int dm_sdk_ounce(void) { return 0; }
int dm_opmode(void) { return 0; }
int front_hdr_enabled(void) { return 0; }   // This fixture has no AppKit front end; its host is SDR.

typedef struct { uint64_t before; uint8_t value, after[15]; } guarded8;
typedef struct { uint64_t before; uint16_t value; uint8_t after[14]; } guarded16;
typedef struct { uint64_t before; uint32_t value; uint8_t after[12]; } guarded32;
static void guards(const void *object, size_t bytes, size_t at, size_t width) {
    const unsigned char *p = object;
    for (size_t i = 0; i < bytes; i++) if (i < at || i >= at + width) assert(p[i] == 0xa5);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    int probe = !strcmp(argv[1], "probe"), trace = probe || !strcmp(argv[1], "trace");
    assert(probe || trace || !strcmp(argv[1], "default"));
    // Capture the actual LOG stream: check both no logs by default and a hard8 lines per HDR entry point.
    FILE *log = tmpfile(); assert(log);
    int saved = dup(STDERR_FILENO); assert(saved >= 0);
    assert(dup2(fileno(log), STDERR_FILENO) >= 0);
    void *display = (void *)(uintptr_t)0x1234, *layer = (void *)(uintptr_t)0x5678;
    for (unsigned mask = 0; mask < 8; mask++) {
        guarded8 on; guarded32 format; guarded16 peak;
        memset(&on, 0xa5, sizeof on); memset(&format, 0xa5, sizeof format); memset(&peak, 0xa5, sizeof peak);
        assert(h_vi_HdrStatus(mask & 1 ? &on.value : NULL, mask & 2 ? &format.value : NULL, mask & 4 ? &peak.value : NULL, display) == RES_OK);
        assert(on.value == (mask & 1 ? (unsigned)probe : 0xa5u));
        assert(format.value == (mask & 2 ? (unsigned)probe : 0xa5a5a5a5u));
        assert(peak.value == (mask & 4 ? (probe ? 1000u : 0u) : 0xa5a5u));
        guards(&on, sizeof on, offsetof(guarded8, value), 1);
        guards(&format, sizeof format, offsetof(guarded32, value), 4);
        guards(&peak, sizeof peak, offsetof(guarded16, value), 2);
    }
    guarded16 white; memset(&white, 0xa5, sizeof white);
    assert(h_vi_PaperWhite(&white.value, NULL) == RES_OK && white.value == 203);
    guards(&white, sizeof white, offsetof(guarded16, value), 2);
    assert(h_vi_PaperWhite(NULL, display) == RES_OK);
    for (int i = 0; i < 100; i++) {
        assert(h_vi_HdrStatus(NULL, NULL, NULL, NULL) == RES_OK);
        assert(h_vi_PaperWhite(NULL, NULL) == RES_OK);
        assert(h_vi_HdrGamut(layer, 7) == RES_OK);
        assert(h_vi_HdrToneMap(layer, -3) == RES_OK);
        assert(h_vi_HdrExtLum(layer, 17, 1000) == RES_OK);
        assert(h_vi_HdrStdLum(layer, 0, 203) == RES_OK);
        assert(h_vi_HdrBezier(layer, .25f, .5f, .75f) == RES_OK);
        assert(h_vi_HdrBezierClear(layer) == RES_OK);
        assert(h_vi_HdrGamutClear(layer) == RES_OK);
        assert(h_vi_HdrToneMapClear(layer) == RES_OK);
    }
    // Null layer metadata remains a no-op success; outputs above are the only memory written by HDR queries.
    assert(h_vi_HdrGamut(NULL, 0) == RES_OK && h_vi_HdrToneMap(NULL, 0) == RES_OK);
    assert(h_vi_HdrExtLum(NULL, 0, 0) == RES_OK && h_vi_HdrStdLum(NULL, 0, 0) == RES_OK);
    assert(h_vi_HdrBezier(NULL, 0, 0, 0) == RES_OK);
    assert(h_vi_HdrBezierClear(NULL) == RES_OK && h_vi_HdrGamutClear(NULL) == RES_OK && h_vi_HdrToneMapClear(NULL) == RES_OK);
    fflush(stderr); assert(dup2(saved, STDERR_FILENO) >= 0); close(saved); rewind(log);
    const char *methods[] = {"QueryDisplayHdrStatus", "QueryDisplayHdrPaperWhite", "SetLayerNextHdrContentGamutMetadata", "SetLayerNextHdrToSdrToneMappingMetadata", "SetLayerNextHdrExtendedContentLuminanceRangeMetadata", "SetLayerNextHdrStandardContentLuminanceRangeMetadata", "SetLayerNextHdrAdjustableBezierMetadata", "ClearLayerNextHdrAdjustableBezierMetadata", "ClearLayerNextHdrContentGamutMetadata", "ClearLayerNextHdrToSdrToneMappingMetadata"};
    unsigned counts[10] = {0}, total = 0; char line[1024];
    while (fgets(line, sizeof line, log)) {
        total++; assert(strstr(line, "[vi-hdr]") && strstr(line, "result=0"));
        assert(strstr(line, probe ? "HDR capability probe (forensic, NOT physical display HDR support)" : "trace only (host SDR)"));
        if (strstr(line, "QueryDisplayHdrStatus")) assert(strstr(line, probe ? "on=1 format=1 peak_nits=1000" : "on=0 format=0 peak_nits=0"));
        if (strstr(line, "QueryDisplayHdrPaperWhite")) assert(strstr(line, "paper_white_nits=203"));
        if (strstr(line, "SetLayerNextHdrContentGamutMetadata")) assert(strstr(line, "gamut=7"));
        if (strstr(line, "SetLayerNextHdrToSdrToneMappingMetadata")) assert(strstr(line, "mode=-3"));
        if (strstr(line, "SetLayerNextHdrExtendedContentLuminanceRangeMetadata")) assert(strstr(line, "low=17 high=1000"));
        if (strstr(line, "SetLayerNextHdrStandardContentLuminanceRangeMetadata")) assert(strstr(line, "low=0 high=203"));
        if (strstr(line, "SetLayerNextHdrAdjustableBezierMetadata")) assert(strstr(line, "values=0.25/0.5/0.75"));
        int found = 0;
        for (unsigned i = 0; i < 10; i++) if (strstr(line, methods[i])) { counts[i]++; found++; }
        assert(found == 1);
    }
    fclose(log);
    assert(total == (trace ? 80u : 0u));
    for (unsigned i = 0; i < 10; i++) assert(counts[i] == (trace ? 8u : 0u));
    printf("vi HDR %s: byte-width canaries, null outputs, metadata success, bounded logs=%u\n", argv[1], total);
    return 0;
}
