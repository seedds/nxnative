// Unit tests for nvn_rscale.h: the DM_INTERNAL_RES parser, host sizes, edge rounding, eligibility, pass consensus, aspect test and support-block keys
#include "../nvn/nvn_rscale.h"
#include <stdio.h>

static int fail, checks;
static void check(int ok, const char *what) { checks++; fail += !ok; printf("%-64s %s\n", what, ok ? "ok" : "FAIL"); }

// A 1920x1080 RGBA8 render target: the common scaled case
static nvn_rs_tex rt(uint32_t w, uint32_t h) { return (nvn_rs_tex){ .target = 1, .w = w, .h = h, .levels = 1, .bpp = 4 }; }

static void parse(void) {
    check(nvn_rs_parse(NULL) == 1.0f && nvn_rs_parse("") == 1.0f, "unset or empty: 1x");
    check(nvn_rs_parse("1") == 1.0f && nvn_rs_parse("1.0") == 1.0f, "1 and 1.0: 1x");
    check(nvn_rs_parse("1.5") == 1.5f && nvn_rs_parse("1.50") == 1.5f, "1.5: 1.5x");
    check(nvn_rs_parse("2") == 2.0f && nvn_rs_parse("2.0") == 2.0f, "2 and 2.0: 2x");
    check(nvn_rs_parse("3") == 0 && nvn_rs_parse("1.25") == 0 && nvn_rs_parse("0.5") == 0, "other numbers: invalid");
    check(nvn_rs_parse("2x") == 0 && nvn_rs_parse("high") == 0 && nvn_rs_parse(" 2") == 2.0f, "trailing text invalid, leading space as strtof");
}

static void sizes(void) {
    check(nvn_rs_host(1920, 2.0f) == 3840 && nvn_rs_host(1080, 1.5f) == 1620, "host size: exact multiples");
    check(nvn_rs_host(7, 1.5f) == 11 && nvn_rs_host(1, 1.5f) == 2, "host size: odd guest sizes round up at 1.5x");
    check(nvn_rs_host(0, 2.0f) == 0 && nvn_rs_host(5, 1.0f) == 5, "host size: 0 stays 0, 1x is identity");
    long a, b;
    nvn_rs_edges(0, 1920, 2.0f, &a, &b); check(a == 0 && b == 3840, "edges: full width at 2x");
    nvn_rs_edges(3, 5, 1.5f, &a, &b); check(a == 4 && b == 12, "edges: 1.5x floors the start and ceils the end");
    long a2, b2; nvn_rs_edges(8, 3, 1.5f, &a2, &b2);
    check(a2 == b && b2 == 17, "edges: adjacent guest rectangles stay gap-free at 1.5x");
    nvn_rs_edges(-4, 10, 2.0f, &a, &b); check(a == -8 && b == 12, "edges: negative origins scale too (caller clamps)");
    nvn_rs_edges(5, 7, 1.0f, &a, &b); check(a == 5 && b == 12, "edges: 1x is identity");
    check(nvn_rs_whole(3, 5, 2.0f), "whole texels: any span at 2x");
    check(nvn_rs_whole(2, 4, 1.5f), "whole texels: even start and width at 1.5x");
    check(!nvn_rs_whole(3, 4, 1.5f) && !nvn_rs_whole(2, 3, 1.5f), "whole texels: an odd edge at 1.5x is not (the copy resamples)");
}

static void eligibility(void) {
    nvn_rs_tex t = rt(1920, 1080);
    check(nvn_rs_eligible(&t) == NVN_RS_OK, "1920x1080 RGBA8 render target: scaled");
    t = rt(1920, 1080); t.target = 4; check(nvn_rs_eligible(&t) == NVN_RS_OK, "2D array: scaled");
    t = rt(1920, 1080); t.target = 5; t.samples = 4; check(nvn_rs_eligible(&t) == NVN_RS_OK, "2D multisample: scaled");
    t = rt(1920, 1080); t.target = 6; check(nvn_rs_eligible(&t) == NVN_RS_OK, "2D multisample array: scaled");
    t = rt(1920, 1080); t.target = 2; check(nvn_rs_eligible(&t) == NVN_RS_TARGET, "3D: not scaled (target)");
    t = rt(1920, 1080); t.target = 8; check(nvn_rs_eligible(&t) == NVN_RS_TARGET, "cube: not scaled (target)");
    t = rt(1920, 1080); t.compressed = 1; check(nvn_rs_eligible(&t) == NVN_RS_FORMAT, "compressed: not scaled (format)");
    t = rt(1920, 1080); t.flags = 0x10; check(nvn_rs_eligible(&t) == NVN_RS_FLAGS, "LINEAR: not scaled (flags)");
    t = rt(1920, 1080); t.flags = 0x40; check(nvn_rs_eligible(&t) == NVN_RS_FLAGS, "LINEAR_RENDER_TARGET: not scaled (flags)");
    t = rt(1920, 1080); t.flags = 0x4 | 0x8; check(nvn_rs_eligible(&t) == NVN_RS_OK, "IMAGE: scaled (demoted when bound as an image)");
    t = rt(1920, 1080); t.flags = 0x2; check(nvn_rs_eligible(&t) == NVN_RS_FLAGS, "VIDEO_DECODE: not scaled (flags)");
    t = rt(1920, 1080); t.flags = 0x20; check(nvn_rs_eligible(&t) == NVN_RS_FLAGS, "SPARSE: not scaled (flags)");
    t = rt(1920, 1080); t.flags = 0x1 | 0x8; check(nvn_rs_eligible(&t) == NVN_RS_OK, "DISPLAY and COMPRESSIBLE: scaled");
    t = rt(1920, 1080); t.packaged = 1; check(nvn_rs_eligible(&t) == NVN_RS_CPU, "packaged: not scaled (cpu)");
    t = rt(1920, 1080); t.init_data = 1; check(nvn_rs_eligible(&t) == NVN_RS_CPU, "storage already holds data: not scaled (cpu)");
    t = rt(1920, 1080); t.storage = 1; check(nvn_rs_eligible(&t) == NVN_RS_STORAGE, "stored back into guest memory: not scaled (storage)");
    t = rt(1920, 1080); t.sparse = 1; check(nvn_rs_eligible(&t) == NVN_RS_SPARSE, "in a sparse pool: not scaled (sparse)");
    check(!strcmp(nvn_rs_why_name[NVN_RS_UPSCALER], "upscaler") && !strcmp(nvn_rs_why_name[NVN_RS_NWHY - 1], "upscaler") && !strcmp(nvn_rs_why_name[NVN_RS_SPARSE], "sparse"), "reason names line up with the enum");
    t = rt(1920, 1080); t.levels = 3; check(nvn_rs_eligible(&t) == NVN_RS_OK, "3 levels: scaled");
    t = rt(1920, 1080); t.levels = 4; check(nvn_rs_eligible(&t) == NVN_RS_LEVELS, "4 levels: not scaled (levels)");
    t = rt(8, 24); check(nvn_rs_eligible(&t) == NVN_RS_OK, "8x24: scaled (8 is the smallest edge)");
    t = rt(7, 64); check(nvn_rs_eligible(&t) == NVN_RS_SMALL, "7 wide: not scaled (small)");
    t = rt(64, 4); check(nvn_rs_eligible(&t) == NVN_RS_SMALL, "4 high: not scaled (small)");
    t = rt(1024, 1024); check(nvn_rs_eligible(&t) == NVN_RS_SQUARE, "square colour: not scaled (square)");
    t = rt(1024, 1020); check(nvn_rs_eligible(&t) == NVN_RS_SQUARE, "possibly square after 64-byte width alignment: not scaled");
    t = rt(1024, 1024); t.depth_or_single = 1; check(nvn_rs_eligible(&t) == NVN_RS_OK, "square depth or single-channel: scaled");
    t = rt(640, 360); check(nvn_rs_eligible(&t) == NVN_RS_OK, "640x360: scaled (360 high)");
    t = rt(480, 270); check(nvn_rs_eligible(&t) == NVN_RS_BLUR, "480x270 (16:9 below 360): not scaled (blur)");
    t = rt(960, 270); check(nvn_rs_eligible(&t) == NVN_RS_OK, "960x270 (not 16:9): scaled");
    t = rt(256, 16); check(nvn_rs_eligible(&t) == NVN_RS_LUT, "256x16 (w == h*h): not scaled (lut)");
}

static void consensus(void) {
    unsigned d;
    check(nvn_rs_consensus((const uint8_t[]){ 1, 1, 1 }, 3, &d) == 1 && d == 0, "consensus: all scaled keeps the pass scaled");
    check(nvn_rs_consensus((const uint8_t[]){ 0, 0 }, 2, &d) == 0 && d == 0, "consensus: all 1x is a 1x pass");
    check(nvn_rs_consensus((const uint8_t[]){ 1, 0, 1 }, 3, &d) == 0 && d == 5, "consensus: a mix demotes the scaled ones");
    check(nvn_rs_consensus(NULL, 0, &d) == 0 && d == 0, "consensus: no attachments is a 1x pass");
}

static void aspect(void) {
    check(nvn_rs_same_aspect(1920, 1080, 1920, 1080), "aspect: same size");
    check(nvn_rs_same_aspect(960, 540, 1920, 1080), "aspect: half size");
    check(!nvn_rs_same_aspect(1920, 1080, 1024, 1024), "aspect: different");
    check(!nvn_rs_same_aspect(0, 1080, 1920, 1080), "aspect: zero width never matches");
}

static void keys(void) {
    nvn_rs_key a, b;
    nvn_rs_key_init(&a); nvn_rs_key_init(&b);
    check(!memcmp(&a, &b, sizeof a), "key: two empty keys are equal");
    nvn_rs_key_set(&a, 0, NVN_RS_V_S); check(memcmp(&a, &b, sizeof a) != 0 && nvn_rs_key_get(&a, 0) == NVN_RS_V_S, "key: slot 0 at S differs from empty");
    nvn_rs_key_set(&b, 0, NVN_RS_V_S); nvn_rs_key_set(&b, 72, NVN_RS_V_NEG); nvn_rs_key_set(&a, 72, NVN_RS_V_NEG);
    check(!memcmp(&a, &b, sizeof a) && nvn_rs_key_get(&a, 72) == NVN_RS_V_NEG, "key: the last slot holds -S");
    nvn_rs_key_set(&a, 72, NVN_RS_V_ONE); check(nvn_rs_key_get(&a, 72) == NVN_RS_V_ONE && memcmp(&a, &b, sizeof a), "key: setting back to 1 clears the slot");
    check(nvn_rs_key_val(NVN_RS_V_ONE, 2.0f) == 1.0f && nvn_rs_key_val(NVN_RS_V_S, 1.5f) == 1.5f && nvn_rs_key_val(NVN_RS_V_NEG, 2.0f) == -2.0f, "key: values 1, S, -S");
    nvn_rs_key_init(&a); check(nvn_rs_key_empty(&a), "key: empty after init");
    nvn_rs_key_set(&a, 5, NVN_RS_V_S); check(!nvn_rs_key_empty(&a), "key: not empty with a slot set");
}

int main(void) {
    parse(); sizes(); eligibility(); consensus(); aspect(); keys();
    printf("== %d checks, %d failed ==\n", checks, fail);
    return fail != 0;
}
