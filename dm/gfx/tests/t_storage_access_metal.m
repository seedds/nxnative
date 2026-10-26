// Asset-free Metal pointer-access reflection and sparse argument-ID fixture.
// clang -fobjc-arc -O1 -Wall -Wextra -Werror -framework Foundation \
//   -framework Metal dm/gfx/tests/t_storage_access_metal.m -o /tmp/t_storage_access_metal
// A missing local Metal device prints SKIP, never GPU PASS.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../nvn/nvn_refl.h"
#include "../nvn/nvn_storage_access_metal.h"
#include "../nvn/nvn_resource_usage.h"

// These public APIs predate iOS 16. Current SDKs deprecate their old names in
// favor of newer binding reflection APIs that are unavailable on iOS 16.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

static const char k_storage_msl[] =
    "#include <metal_stdlib>\n"
    "#include <metal_atomic>\n"
    "using namespace metal;\n"
    "struct StorageArgs {\n"
    "  const device uint *input [[id(2)]];\n"
    "  device uint *output [[id(7)]];\n"
    "  device atomic_uint *counter [[id(11)]];\n"
    "  device uint *unused [[id(15)]];\n"
    "  constant uint *cfg [[id(17)]];\n"
    "};\n"
    "kernel void storage_access_fixture(constant StorageArgs &a [[buffer(0)]],\n"
    "    uint gid [[thread_position_in_grid]]) {\n"
    "  if (gid >= a.cfg[0]) return;\n"
    "  a.output[gid] = a.input[gid] + a.cfg[1];\n"
    "  atomic_fetch_add_explicit(a.counter, 1u, memory_order_relaxed);\n"
    "}\n"
    "struct RouteBArgs {\n"
    "  const device uint *input [[id(0)]];\n"
    "  device uint *output [[id(1)]];\n"
    "  device atomic_uint *counter [[id(2)]];\n"
    "  device uint *unused [[id(3)]];\n"
    "  constant uint *cfg [[id(4)]];\n"
    "};\n"
    "kernel void storage_access_route_b(constant RouteBArgs &a [[buffer(0)]],\n"
    "    uint gid [[thread_position_in_grid]]) {\n"
    "  if (gid >= a.cfg[0]) return;\n"
    "  a.output[gid] = a.input[gid] + a.cfg[1];\n"
    "  atomic_fetch_add_explicit(a.counter, 1u, memory_order_relaxed);\n"
    "}\n";

static const NSUInteger k_sparse_ids[] = {2, 7, 11, 15, 17};
static const NSUInteger k_route_b_ids[] = {0, 1, 2, 3, 4};

static int g_checks, g_failures;
static int check(int ok, const char *what) {
    g_checks++;
    if (!ok) { g_failures++; fprintf(stderr, "FAIL: %s\n", what); }
    return ok;
}
static int writable(MTLPointerType *p) {
    return p && (p.access == MTLArgumentAccessWriteOnly || p.access == MTLArgumentAccessReadWrite);
}
static MTLStructMember *member_by_id(MTLStructType *type, NSUInteger aid) {
    MTLStructMember *found = nil;
    for (MTLStructMember *member in type.members) if (member.argumentIndex == aid) {
        if (found) { check(0, "compiled argument IDs are unique"); return nil; }
        found = member;
    }
    return found;
}
static int inspect_reflection(id<MTLArgumentEncoder> encoder, MTLArgument *argument,
        const NSUInteger ids[5], int sparse) {
    if (!check(encoder != nil && argument != nil, "argument encoder returns real compiled reflection")) return 0;
    if (!check(argument.type == MTLArgumentTypeBuffer && argument.index == 0,
            "root reflection describes buffer(0)")) return 0;
    MTLStructType *type = argument.bufferStructType;
    if (!type && argument.bufferPointerType) type = argument.bufferPointerType.elementStructType;
    if (!check(type != nil && type.members.count > 0, "argument-buffer struct members are available")) return 0;
    printf("Compiled argument buffer: encodedLength=%lu members=%lu\n",
        (unsigned long)encoder.encodedLength, (unsigned long)type.members.count);
    for (MTLStructMember *member in type.members) {
        MTLPointerType *pointer = member.dataType == MTLDataTypePointer ? member.pointerType : nil;
        printf("  member=%s argumentIndex=%lu offset=%lu type=%lu pointerAccess=%ld\n",
            member.name.UTF8String, (unsigned long)member.argumentIndex, (unsigned long)member.offset,
            (unsigned long)member.dataType, pointer ? (long)pointer.access : -1l);
        check(member.offset < encoder.encodedLength, "reflected member offset is inside encoded storage");
    }
    // The sparse IDs deliberately differ from member enumeration ordinals.
    // Use argumentIndex to identify a resource; do not infer it from offset.
    MTLStructMember *input = member_by_id(type, ids[0]), *output = member_by_id(type, ids[1]);
    MTLStructMember *counter = member_by_id(type, ids[2]), *cfg = member_by_id(type, ids[4]);
    check(input && input.dataType == MTLDataTypePointer && input.pointerType,
        "compiled input argument ID is a pointer");
    check(output && output.dataType == MTLDataTypePointer && writable(output.pointerType),
        "compiled output argument ID permits writes");
    check(counter && counter.dataType == MTLDataTypePointer && writable(counter.pointerType),
        "compiled atomic argument ID permits writes");
    check(cfg && cfg.dataType == MTLDataTypePointer && cfg.pointerType,
        "used constant argument ID survives compilation");
    if (sparse) check(member_by_id(type, 0) == nil, "sparse ID 0 does not alias member ordinal zero");
    MTLStructMember *unused = member_by_id(type, ids[3]);
    printf("Unused argument ID %lu: %s; absence is not evidence of read-only access\n", (unsigned long)ids[3],
        unused ? "retained by this compiler" : "omitted by this compiler");
    // A retained unused member is allowed. Some compilers report declaration
    // access conservatively rather than eliminating all unused pointer fields.
    nvn_refl rf = {.nb = 5, .abi = 0, .route_b = 1};
    for (unsigned i = 0; i < 5; i++) rf.b[i] = (nvn_rbind){
        .kind = i == 4 ? NVN_RB_CBUF : NVN_RB_SBUFP, .aid = (uint16_t)ids[i], .reg = (uint16_t)i};
    nvn_storage_access_promote(&rf, argument, encoder.encodedLength, sparse ? "synthetic_sparse" : "synthetic_route_b", 5);
    for (unsigned i = 0; i < 5; i++) {
        MTLStructMember *m = member_by_id(type, ids[i]);
        int raw_layout = m && m.offset == 8 * ids[i] && m.offset <= encoder.encodedLength && 8 <= encoder.encodedLength - m.offset;
        if (!raw_layout || i == 4) check(!rf.b[i].rw, "helper rejects an incompatible raw slot or non-storage binding");
    }
    if (!sparse) {
        check(rf.b[1].rw, "helper promotes actual compiled route-B output pointer");
        check(rf.b[2].rw, "helper promotes actual compiled route-B atomic pointer");
    }
    rf.b[0].rw = 1;
    nvn_storage_access_promote(&rf, argument, encoder.encodedLength, "synthetic_preserve", 5);
    check(rf.b[0].rw, "compiled reflection never clears an existing write flag");
    for (unsigned i = 0; i < 5; i++) rf.b[i].rw = 0;
    nvn_storage_access_promote(&rf, argument, 1, "synthetic_short_block", 5);
    for (unsigned i = 0; i < 5; i++) check(!rf.b[i].rw, "undersized raw argument storage never promotes a field");
    return g_failures == 0;
}

static int declare_usage(id<MTLComputeCommandEncoder> compute, nvn_resource_usage_set *set,
        id<MTLResource> resource, MTLResourceUsage requested, uint64_t *declared) {
    int skip = -1;
    if (!check(nvn_resource_usage_add(set, (uintptr_t)(__bridge void *)resource,
            (uint64_t)requested, 0, declared, &skip), "resource-usage union insertion succeeds")) return 0;
    check(!skip, "dedup-off still emits every Metal declaration");
    [compute useResource:resource usage:(MTLResourceUsage)*declared];
    return 1;
}

static int execute_readback(id<MTLDevice> device, id<MTLFunction> function,
        id<MTLArgumentEncoder> encoder, const NSUInteger ids[5], int alias) {
    enum { COUNT = 16 }; uint32_t values[COUNT], expected[COUNT], config[2] = {COUNT, 37};
    for (unsigned i = 0; i < COUNT; i++) { values[i] = i * 11u + 3u; expected[i] = values[i] + config[1]; }
    id<MTLBuffer> input = [device newBufferWithBytes:values length:sizeof values options:MTLResourceStorageModeShared];
    id<MTLBuffer> output = alias ? input : [device newBufferWithLength:sizeof values options:MTLResourceStorageModeShared];
    id<MTLBuffer> counter = [device newBufferWithLength:sizeof(uint32_t) options:MTLResourceStorageModeShared];
    id<MTLBuffer> cfg = [device newBufferWithBytes:config length:sizeof config options:MTLResourceStorageModeShared];
    id<MTLBuffer> arguments = [device newBufferWithLength:encoder.encodedLength options:MTLResourceStorageModeShared];
    if (!check(input && output && counter && cfg && arguments, "synthetic buffers allocate")) return 0;
    memset(counter.contents, 0, counter.length); memset(arguments.contents, 0, arguments.length);
    if (!alias) memset(output.contents, 0xa5, output.length);
    [encoder setArgumentBuffer:arguments offset:0];
    [encoder setBuffer:input offset:0 atIndex:ids[0]];
    [encoder setBuffer:output offset:0 atIndex:ids[1]];
    [encoder setBuffer:counter offset:0 atIndex:ids[2]];
    [encoder setBuffer:cfg offset:0 atIndex:ids[4]];
    // Never encode the unused field: an optimizer may omit its argument ID.
    // Public encoder setters handle physical offsets for sparse IDs; raw ABI
    // slots are a separate contract checked by the production route-B loader.
    NSError *error = nil;
    id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function error:&error];
    if (!check(pipeline != nil, "synthetic compute pipeline builds")) {
        fprintf(stderr, "Pipeline error: %s\n", error.localizedDescription.UTF8String); return 0;
    }
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLComputeCommandEncoder> compute = [command computeCommandEncoder];
    if (!check(queue && command && compute, "synthetic command encoder allocates")) return 0;
    [compute setComputePipelineState:pipeline]; [compute setBuffer:arguments offset:0 atIndex:0];
    nvn_resource_usage_set declarations = {0}; nvn_resource_usage_begin(&declarations);
    uint64_t output_usage = 0, input_usage = 0, counter_usage = 0, cfg_usage = 0;
    // Deliberately declare the writer before the reader. When both arguments
    // alias one MTLBuffer, the later Read must emit Read|Write rather than
    // downgrade the encoder's latest declaration for that resource.
    int declarations_ok = declare_usage(compute, &declarations, output, MTLResourceUsageWrite, &output_usage)
        && declare_usage(compute, &declarations, input, MTLResourceUsageRead, &input_usage)
        && declare_usage(compute, &declarations, counter, MTLResourceUsageRead | MTLResourceUsageWrite, &counter_usage)
        && declare_usage(compute, &declarations, cfg, MTLResourceUsageRead, &cfg_usage);
    if (!declarations_ok) {
        [compute endEncoding]; nvn_resource_usage_dispose(&declarations); return 0;
    }
    check(output_usage == MTLResourceUsageWrite, "first output declaration requests Write");
    check(input_usage == (alias ? MTLResourceUsageRead | MTLResourceUsageWrite : MTLResourceUsageRead),
        "later input Read preserves Write exactly when input/output alias");
    check(counter_usage == (MTLResourceUsageRead | MTLResourceUsageWrite) && cfg_usage == MTLResourceUsageRead,
        "counter and constant declarations retain their separate access contracts");
    [compute dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(COUNT, 1, 1)];
    [compute endEncoding]; [command commit]; [command waitUntilCompleted];
    nvn_resource_usage_dispose(&declarations);
    if (!check(command.status == MTLCommandBufferStatusCompleted && !command.error,
            "synthetic GPU command completed")) {
        fprintf(stderr, "GPU command error: %s\n", command.error.localizedDescription.UTF8String); return 0;
    }
    check(!memcmp(output.contents, expected, sizeof expected), "GPU output matches synthetic expected values");
    check(*(const uint32_t *)counter.contents == COUNT, "GPU atomic counter matches dispatched work");
    if (!g_failures) printf("GPU readback (%s buffers): PASS\n", alias ? "aliased input/output" : "separate input/output");
    return g_failures == 0;
}

int main(void) { @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) {
        puts("Storage-access Metal fixture: SKIP (no local Metal device; reflection/GPU execution not validated)");
        return 0;
    }
    printf("Storage-access Metal fixture device: %s\n", device.name.UTF8String);
    MTLCompileOptions *options = [MTLCompileOptions new]; options.languageVersion = MTLLanguageVersion3_0;
    options.fastMathEnabled = NO;
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:@(k_storage_msl) options:options error:&error];
    if (!check(library != nil, "synthetic sparse-ID MSL compiles")) {
        fprintf(stderr, "MSL error: %s\n", error.localizedDescription.UTF8String); return 1;
    }
    for (int sparse = 1; sparse >= 0; sparse--) {
        NSString *entry = sparse ? @"storage_access_fixture" : @"storage_access_route_b";
        const NSUInteger *ids = sparse ? k_sparse_ids : k_route_b_ids;
        id<MTLFunction> function = [library newFunctionWithName:entry];
        if (!check(function != nil, "synthetic entry point exists")) return 1;
        printf("Inspecting %s\n", entry.UTF8String);
        MTLArgument *argument = nil;
        id<MTLArgumentEncoder> encoder = nvn_storage_access_encoder(function, 0, &argument);
        if (!inspect_reflection(encoder, argument, ids, sparse)) return 1;
        if (!execute_readback(device, function, encoder, ids, 0) || !execute_readback(device, function, encoder, ids, 1)) return 1;
    }
    printf("Storage-access Metal fixture: PASS (%d checks; real compiled reflection and GPU readback)\n", g_checks);
    return g_failures != 0;
} }
#pragma clang diagnostic pop
