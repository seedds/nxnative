#pragma once
static id<MTLLibrary> zero_image_library(NSString *key, id<MTLLibrary> original, NSData *reflection,
        int *variant, int *image_aid, int *image_reg, NSError **error) {
    static NSDictionary *manifest;
    static dispatch_once_t once;
    NSString *directory = [g_msl_dir stringByAppendingPathComponent:@"zero-image-v1"];
    dispatch_once(&once, ^{
        NSData *data = [NSData dataWithContentsOfFile:[directory stringByAppendingPathComponent:@"manifest.json"]];
        manifest = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
    });
    // A missing companion affects only fragment image writers on old GPUs.
    NSDictionary *record = manifest[@"shaders"][key];
    NSString *reason = nil;
    if (![manifest[@"schema"] isEqual:@1] || ![manifest[@"chain"] isEqual:@DMSH_CHAIN] ||
        ![manifest[@"kind"] isEqual:@"zero-image-clear-v1"] || !record ||
        ![record[@"proof"] isEqual:@"fragment-zero-writes-v1"] ||
        ![record[@"parameter_slot"] isEqual:@29] || ![record[@"image_aid"] isKindOfClass:NSNumber.class] ||
        ![record[@"image_reg"] isKindOfClass:NSNumber.class]) reason = @"Missing or unproven zero-image fragment companion";
    NSString *name = record[@"source"];
    if (!reason && (![name isKindOfClass:NSString.class] || ![name isEqual:name.lastPathComponent])) reason = @"Invalid zero-image source path";
    NSData *source = reason ? nil : [NSData dataWithContentsOfFile:[directory stringByAppendingPathComponent:name]];
    NSData *base = reason ? nil : [NSData dataWithContentsOfFile:[g_msl_dir stringByAppendingPathComponent:[key stringByAppendingString:@".metallib"]] options:NSDataReadingMappedIfSafe error:nil];
    if (!reason && (![record[@"source_sha256"] isEqual:nonpoint_digest(source)] ||
        ![record[@"base_library_sha256"] isEqual:nonpoint_digest(base)] ||
        ![record[@"reflection_sha256"] isEqual:nonpoint_digest(reflection)])) reason = @"Zero-image source, base library or reflection hash mismatch";
    if (reason) {
        if (error) *error = [NSError errorWithDomain:@"NVNZeroImage" code:1 userInfo:@{NSLocalizedDescriptionKey:reason}];
        return nil;
    }
    NSString *text = [[NSString alloc] initWithData:source encoding:NSUTF8StringEncoding];
    MTLCompileOptions *options = [MTLCompileOptions new]; options.languageVersion = MTLLanguageVersion3_0;
    options.preserveInvariance = YES; options.fastMathEnabled = NO;
    id<MTLLibrary> library = [g_dev newLibraryWithSource:text options:options error:error];
    if (library) {
        *variant = 1; *image_aid = [record[@"image_aid"] intValue]; *image_reg = [record[@"image_reg"] intValue];
        MLOG("Zero-image fragment %s: verified zero-write proof, compatibility buffer 29", key.UTF8String);
    }
    return library;
}
