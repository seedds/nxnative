#pragma once
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "nvn_mtl.h"

static inline id<MTLLibrary> mtl_runtime_shader_library(NSString *path, NSError **err) {
    if (err) *err = nil;
    NSData *data = [NSData dataWithContentsOfFile:path options:NSDataReadingMappedIfSafe error:err];
    NSString *source = data ? [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] : nil;
    if (!source || (![source containsString:@"#include <metal_stdlib>"] && ![source containsString:@"using namespace metal;"])) {
        if (err && !*err) *err = [NSError errorWithDomain:@"NxNativeMetal" code:2 userInfo:@{NSLocalizedDescriptionKey: [NSString stringWithFormat:@"%@ is not an MSL source file", path]}];
        return nil;
    }
    MTLCompileOptions *options = [MTLCompileOptions new];
    options.languageVersion = MTLLanguageVersion2_3;
    options.mathMode = MTLMathModeRelaxed;
    options.preserveInvariance = YES;
    id<MTLDevice> device = (__bridge id<MTLDevice>)mtl_device_classic();
    return [device newLibraryWithSource:source options:options error:err];
}
