#pragma once
// The companion is generated from the exact base library/reflection pair.
// Only entries whose instance-layer expression and resource access were proven
// by export_layered_sources.py are eligible for repeated vertex execution.
static id<MTLLibrary> layered_slice_library(NSString *key, id<MTLLibrary> original,
        NSData *reflection, int *variant, NSError **error) {
    static NSDictionary *manifest;
    static dispatch_once_t once;
    NSString *directory = [g_msl_dir stringByAppendingPathComponent:@"layered-v1"];
    dispatch_once(&once, ^{
        NSData *data = [NSData dataWithContentsOfFile:[directory stringByAppendingPathComponent:@"manifest.json"]];
        manifest = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
    });
    NSString *reason = nil;
    if (![manifest[@"schema"] isEqual:@1] || ![manifest[@"chain"] isEqual:@DMSH_CHAIN] ||
        ![manifest[@"kind"] isEqual:@"layered-slices-v1"]) reason = @"Missing or mismatched layered-v1 companion manifest";
    NSDictionary *record = manifest[@"shaders"][key];
    if (!reason && !record) return original;
    if (!reason && (![record[@"supported"] isEqual:@YES] || ![record[@"proof"] isEqual:@"instance-layer-pure-v1"] ||
        ![record[@"slot"] isEqual:@30])) reason = [@"Unproven per-slice shader: " stringByAppendingString:record[@"reason"] ?: @"unknown proof"];
    NSData *source = reason ? nil : [NSData dataWithContentsOfFile:[directory stringByAppendingPathComponent:[key stringByAppendingString:@".metal"]]];
    NSData *base = reason ? nil : [NSData dataWithContentsOfFile:[g_msl_dir stringByAppendingPathComponent:[key stringByAppendingString:@".metallib"]] options:NSDataReadingMappedIfSafe error:nil];
    if (!reason && (![record[@"source_sha256"] isEqual:nonpoint_digest(source)] ||
        ![record[@"library_sha256"] isEqual:nonpoint_digest(base)] ||
        ![record[@"reflection_sha256"] isEqual:nonpoint_digest(reflection)])) reason = @"Per-slice source, reflection or library hash mismatch";
    if (reason) {
        if (error) *error = [NSError errorWithDomain:@"NVNLayeredSlices" code:1 userInfo:@{NSLocalizedDescriptionKey:reason}];
        return nil;
    }
    NSString *text = [[NSString alloc] initWithData:source encoding:NSUTF8StringEncoding];
    MTLCompileOptions *options = [MTLCompileOptions new]; options.languageVersion = MTLLanguageVersion3_0;
    options.preserveInvariance = YES; options.fastMathEnabled = NO;
    id<MTLLibrary> library = [g_dev newLibraryWithSource:text options:options error:error];
    if (library) { *variant = 1; MLOG("Per-slice shader %s: verified instance-layer proof, compatibility buffer 30", key.UTF8String); }
    return library;
}
