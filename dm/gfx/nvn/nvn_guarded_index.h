#pragma once

// Apple3 compatibility experiment for a suspected speculative constant-buffer
// read in a disabled branch. Replace only index seeds proven dead whenever
// the guarded read is active. Keep the original shader chain and point/TF
// entries; this source is consumed only by the non-point compatibility path.
static NSData *guarded_index_source(NSString *key, NSData *original, NSData *library, NSError **error) {
    if (g_fragment_image_native) return original; // Apple4/Mac1 and later.
    static NSDictionary *manifest;
    static dispatch_once_t once;
    NSString *directory = [g_msl_dir stringByAppendingPathComponent:@"guarded-index-v1"];
    dispatch_once(&once, ^{
        NSData *data = [NSData dataWithContentsOfFile:[directory stringByAppendingPathComponent:@"manifest.json"]];
        manifest = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
    });
    if (![manifest isKindOfClass:NSDictionary.class]) return original;
    id shaders = manifest[@"shaders"];
    if (![shaders isKindOfClass:NSDictionary.class]) {
        if (error) *error = [NSError errorWithDomain:@"NVNGuardedIndex" code:1 userInfo:@{NSLocalizedDescriptionKey:@"Invalid guarded-index shader table"}];
        return nil;
    }
    NSDictionary *record = shaders[key];
    if (!record) return original;
    NSString *reason = nil;
    if (![record isKindOfClass:NSDictionary.class]) reason = @"Invalid guarded-index shader record";
    if (!reason && (![manifest[@"schema"] isEqual:@1] || ![manifest[@"chain"] isEqual:@DMSH_CHAIN] ||
        ![manifest[@"kind"] isEqual:@"guarded-index-v1"] ||
        ![record[@"proof"] isEqual:@"dead-guarded-index-seeds-v1"] ||
        ![record[@"replacement_count"] isKindOfClass:NSNumber.class] ||
        [record[@"replacement_count"] unsignedIntegerValue] == 0)) reason = @"Invalid guarded-index source proof";
    if (reason) {
        if (error) *error = [NSError errorWithDomain:@"NVNGuardedIndex" code:1 userInfo:@{NSLocalizedDescriptionKey:reason}];
        return nil;
    }
    NSString *name = record[@"source"];
    if (!reason && (![name isKindOfClass:NSString.class] || ![name isEqual:name.lastPathComponent])) reason = @"Invalid guarded-index source path";
    NSData *source = reason ? nil : [NSData dataWithContentsOfFile:[directory stringByAppendingPathComponent:name]];
    NSData *reflection = reason ? nil : [NSData dataWithContentsOfFile:[g_msl_dir stringByAppendingPathComponent:[key stringByAppendingString:@".json"]]];
    if (!reason && (![record[@"source_sha256"] isEqual:nonpoint_digest(source)] ||
        ![record[@"original_source_sha256"] isEqual:nonpoint_digest(original)] ||
        ![record[@"base_library_sha256"] isEqual:nonpoint_digest(library)] ||
        ![record[@"reflection_sha256"] isEqual:nonpoint_digest(reflection)])) reason = @"Guarded-index source, base library or reflection hash mismatch";
    if (reason) {
        if (error) *error = [NSError errorWithDomain:@"NVNGuardedIndex" code:1 userInfo:@{NSLocalizedDescriptionKey:reason}];
        return nil;
    }
    MLOG("Guarded-index vertex %s: verified dead index seeds (%lu replacements)", key.UTF8String,
        (unsigned long)[record[@"replacement_count"] unsignedIntegerValue]);
    return source;
}
