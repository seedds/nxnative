// What a shader reading or writing through a pointer to a buffer outside the residency set gets (dm/gfx/nvn/nvn_res_probe.inc; docs/reference/memory.md, "A read through a pointer
// to a buffer outside the residency set returns zeros"). It reports; it fails only when a resident control is wrong, so the result is the log, not the exit code.
// ./t_res_absent [rounds]. On a device, the host runs the same probe at start with DM_RES_PROBE=1
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../nvn/nvn_res_probe.inc"
static void say(const char *s) { printf("%s\n", s); fflush(stdout); }
int main(int argc, char **argv) { @autoreleasepool {
    int ok = res_probe(MTLCreateSystemDefaultDevice(), argc > 1 ? atoi(argv[1]) : 2, say);
    printf("t_res_absent: %s\n", ok ? "done" : "FAIL");
    return !ok;
} }
