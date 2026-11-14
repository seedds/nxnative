#import <Foundation/Foundation.h>
#include "../host/nextendo_bcat.h"
#include <stdio.h>
#include <string.h>
int main(int argc, const char **argv) { @autoreleasepool {
    if (argc == 3 && !strcmp(argv[1], "supported")) return dm_bcat_supported(argv[2]) ? 0 : 2;
    if (argc == 5 && !strcmp(argv[1], "validate")) return dm_bcat_validate(argv[2],argv[3],argv[4]) ? 0 : 1;
    if (argc != 5) { fprintf(stderr, "usage: bcat_prepare title version signed-catalog destination\n"); return 2; }
    char error[256] = {0};
    if (!dm_bcat_prepare(argv[1], argv[2], argv[3], argv[4], error, sizeof error)) {
        fprintf(stderr, "BCAT: %s\n", error); return 1;
    }
    puts("BCAT: verified calendar snapshot prepared (no account data)"); return 0;
} }
