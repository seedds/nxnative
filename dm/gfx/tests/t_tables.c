// Library tables (nvn_tables.h): the copy of c1_full.tsv / cb_extent.tsv that names the loaded library is taken, the profile's first, then the one beside the library. A host whose
// profile folder held another library's tables (PRAGMATA r11 runs pointed at the r6 build's profile) took none: "[RED] c1_full.tsv is stale or missing", every c1 its own 64 KiB
#include "../nvn/nvn_tables.h"
#include <stdio.h>

static int fail, checks;
static void check(int ok, const char *what) { checks++; fail += !ok; printf("%-78s %s\n", what, ok ? "✓" : "✗"); }

int main(void) {
    const char *want = "# chain b-784be5412349 inputs c193f60bbad2 ";
    const char *cur = "# chain b-784be5412349 inputs c193f60bbad2 2026-11-08T17:04:25+00:00", *old = "# chain b-916e8d674d87 inputs c0fa3d2c08c9 2026-11-07T10:00:04+00:00",
               *inputs = "# chain b-784be5412349 inputs 0b406f64e4dc 2026-11-08T18:09:21+00:00", *nostamp = "# chain b-784be5412349 inputs none 2026-11-08T17:04:25+00:00";
    { const char *h[2] = {cur, cur}; check(nvn_table_pick(h, 2, want) == 0, "both current: the profile's copy"); }
    { const char *h[2] = {old, cur}; check(nvn_table_pick(h, 2, want) == 1, "profile's from another chain, library's current: the library's copy"); }
    { const char *h[2] = {NULL, cur}; check(nvn_table_pick(h, 2, want) == 1, "profile's missing, library's current: the library's copy"); }
    { const char *h[2] = {inputs, cur}; check(nvn_table_pick(h, 2, want) == 1, "profile's from other inputs: the library's copy"); }
    { const char *h[2] = {old, NULL}; check(nvn_table_pick(h, 2, want) == -1, "neither current: none (every c1 its own 64 KiB, as before)"); }
    { const char *h[2] = {old, inputs}; check(nvn_table_pick(h, 2, want) == -1, "beside the library but from other inputs: none"); }
    { const char *h[2] = {nostamp, nostamp}; check(nvn_table_pick(h, 2, "# chain b-784be5412349 inputs - ") == -1, "a library without a stamp takes no table"); }
    { const char *h[1] = {cur}; check(nvn_table_pick(h, 1, "") == -1, "no library name: none"); }
    printf("%d/%d checks passed\n", checks - fail, checks);
    return fail != 0;
}
