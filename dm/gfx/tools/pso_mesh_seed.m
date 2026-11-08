// Expand exact task/mesh/pixel programs with observed compatible render states.
// The production parser validates and normalizes every template and output row.
#import <Foundation/Foundation.h>
#include <stdio.h>
#include <stdlib.h>
#include "../nvn/nvn_obj.h"
@interface NvShader : NSObject { @public int stage; char key[24]; } @end
@implementation NvShader @end
#include "../nvn/nvn_pso_skey.h"
int main(int argc, char **argv) { @autoreleasepool {
    if (argc != 4) return 2;
    NSError *error = nil;
    NSString *input = [NSString stringWithContentsOfFile:@(argv[1]) encoding:NSUTF8StringEncoding error:&error];
    NSString *triplets = [NSString stringWithContentsOfFile:@(argv[2]) encoding:NSUTF8StringEncoding error:&error];
    if (!input || !triplets) return 3;
    NSMutableSet<NSData *> *templates = [NSMutableSet set];
    for (NSString *line in [input componentsSeparatedByString:@"\n"]) {
        if (![line hasPrefix:@"M\t"]) continue;
        NSArray *c = [line componentsSeparatedByString:@"\t"]; pso_skey key;
        if (c.count != 3 || pso_mesh_skey_parse([c[2] UTF8String], &key)) return 4;
        int pixel = key.fs[0] != 0;
        memset(key.vs, 0, sizeof key.vs); memset(key.fs, 0, sizeof key.fs);
        key.fs_stage = pixel ? 1 : 0;
        [templates addObject:[NSData dataWithBytes:&key length:sizeof key]];
    }
    NSMutableSet<NSString *> *rows = [NSMutableSet set];
    for (NSString *line in [triplets componentsSeparatedByString:@"\n"]) {
        if (!line.length || [line hasPrefix:@"#"]) continue;
        NSArray<NSString *> *c = [line componentsSeparatedByString:@"\t"];
        if (c.count != 3 || c[0].length > 23 || !c[1].length || c[1].length > 23 || c[2].length > 23) return 5;
        for (NSData *data in templates) {
            pso_skey key; [data getBytes:&key length:sizeof key];
            if (key.fs_stage && [c[2] isEqualToString:@"-"]) continue;
            memcpy(key.vs, c[1].UTF8String, c[1].length);
            if (key.fs_stage) memcpy(key.fs, c[2].UTF8String, c[2].length);
            char hex[sizeof key * 2 + 1]; pso_skey_hex(&key, hex);
            pso_skey checked; if (pso_mesh_skey_parse(hex, &checked)) return 6;
            [rows addObject:[NSString stringWithFormat:@"M\t%@\t%s", c[0], hex]];
        }
    }
    NSString *out = [[[[rows allObjects] sortedArrayUsingSelector:@selector(compare:)] componentsJoinedByString:@"\n"] stringByAppendingString:@"\n"];
    if (![out writeToFile:@(argv[3]) atomically:YES encoding:NSUTF8StringEncoding error:&error]) return 7;
    fprintf(stderr, "%lu normalized render-state templates, %lu prospective native mesh pipelines\n", (unsigned long)templates.count, (unsigned long)rows.count);
    for (NSData *data in templates) { pso_skey key; [data getBytes:&key length:sizeof key];
        fprintf(stderr, "pixel %u depth %u stencil %u samples %u alpha %u logic %u colors", key.fs_stage, key.k.dfmt, key.k.sfmt, key.k.samples, key.k.atest, key.k.logic);
        for (int i=0;i<8;i++) fprintf(stderr," %u",key.k.cfmt[i]); fprintf(stderr,"\n");
    }
} }
