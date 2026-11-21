// Mesh / task shader sweep under Metal shader validation, no game: each translated shader (<key>_b.metal + <key>_b.json from dmsh_build.py) is compiled from its source with a
// test stage appended, every argument buffer entry points at zeroed memory, and one draw runs. A zero meshlet count is what made the guest's index writes wrap past the threadgroup
// array (Ocarina of Time's 387 max_p 126 meshes), so zeroed inputs alone reach that path. Validation reports out-of-bounds device / threadgroup accesses as the command buffer's
// error; a GPU timeout shows as a hang. Run with MTL_SHADER_VALIDATION=1 in the environment.
// mesh shader: an object stage is appended that zeroes the payload and launches one mesh workgroup. task shader: a mesh stage is appended that reads nothing and emits nothing.
// Usage: mesh_sweep <dir> <key>   (prints one line: key, stage, result)
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include <stdio.h>

int main(int argc, char **argv) { @autoreleasepool {
    if (argc < 3) { fprintf(stderr, "usage: mesh_sweep <dir> <key>\n"); return 2; }
    NSString *dir = @(argv[1]), *key = @(argv[2]);
    NSData *jd = [NSData dataWithContentsOfFile:[dir stringByAppendingFormat:@"/%@_b.json", key]];
    NSDictionary *j = jd ? [NSJSONSerialization JSONObjectWithData:jd options:0 error:nil] : nil;
    NSString *src = [NSString stringWithContentsOfFile:[dir stringByAppendingFormat:@"/%@_b.metal", key] encoding:NSUTF8StringEncoding error:nil];
    if (!j || !src) { printf("%s - MISSING\n", argv[2]); return 1; }
    NSDictionary *ms = j[@"mesh_stage"]; NSString *stage = j[@"stage"];
    int threads = [ms[@"threads"] intValue] ?: 32, payload = [ms[@"payload"] intValue];
    BOOL isMesh = [stage isEqualToString:@"ms"], hasTask = [ms[@"task"] boolValue];
    if (isMesh && hasTask)
        src = [src stringByAppendingString:@"\n[[object]] void dm_sweep_obj(object_data dm_payload_t& p [[payload]], mesh_grid_properties g, uint lid [[thread_index_in_threadgroup]])\n"
               "{ if (lid == 0) { for (uint i = 0; i < sizeof(dm_payload_t) / 4; i++) ((object_data uint*)&p)[i] = 0u; g.set_threadgroups_per_grid(uint3(1, 1, 1)); } }\n"];
    else if (!isMesh)
        src = [src stringByAppendingString:@"\nstruct dm_sweep_v { float4 p [[position]]; };\n[[mesh]] void dm_sweep_mesh(metal::mesh<dm_sweep_v, void, 3, 1, metal::topology::triangle> m, const object_data dm_payload_t& p [[payload]]) { }\n"];
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice(); id<MTLCommandQueue> q = [dev newCommandQueue];
    NSError *err = nil; id<MTLLibrary> lib = [dev newLibraryWithSource:src options:[MTLCompileOptions new] error:&err];
    if (!lib) { printf("%s %s COMPILE %s\n", argv[2], stage.UTF8String, [[err.localizedDescription componentsSeparatedByString:@"\n"].firstObject UTF8String]); return 1; }
    NSString *entry = j[@"entry"] ?: @"main0";
    MTLMeshRenderPipelineDescriptor *d = [MTLMeshRenderPipelineDescriptor new];
    id<MTLLibrary> nop = [dev newLibraryWithSource:@"fragment void dm_sweep_frag() {}" options:nil error:nil];
    d.fragmentFunction = [nop newFunctionWithName:@"dm_sweep_frag"]; d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    if (isMesh) { d.meshFunction = [lib newFunctionWithName:entry]; d.maxTotalThreadsPerMeshThreadgroup = threads;
        if (hasTask) { d.objectFunction = [lib newFunctionWithName:@"dm_sweep_obj"]; d.payloadMemoryLength = (payload + 15) & ~15; d.maxTotalThreadsPerObjectThreadgroup = 1; } }
    else { d.objectFunction = [lib newFunctionWithName:entry]; d.maxTotalThreadsPerObjectThreadgroup = threads; d.payloadMemoryLength = (payload + 15) & ~15;
        d.meshFunction = [lib newFunctionWithName:@"dm_sweep_mesh"]; d.maxTotalThreadsPerMeshThreadgroup = 1; }
    id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithMeshDescriptor:d options:MTLPipelineOptionNone reflection:nil error:&err];
    if (!ps) { printf("%s %s PIPELINE %s\n", argv[2], stage.UTF8String, [[err.localizedDescription componentsSeparatedByString:@"\n"].firstObject UTF8String]); return 1; }
    // every argument buffer entry: a zeroed 1 MB buffer (constant and storage buffers, the pool table, the site entries); textures are not expected in these stages
    id<MTLBuffer> zero = [dev newBufferWithLength:1 << 20 options:MTLResourceStorageModeShared]; memset(zero.contents, 0, zero.length);
    int maxid = 0; for (NSDictionary *b in j[@"bindings"]) maxid = MAX(maxid, [b[@"arg_id"] intValue] + MAX(1, [b[@"n"] intValue]));
    id<MTLBuffer> ab = [dev newBufferWithLength:(size_t)MAX(maxid, 1) * 8 + 64 options:MTLResourceStorageModeShared]; uint64_t *slots = ab.contents;
    for (int i = 0; i < maxid; i++) slots[i] = zero.gpuAddress;
    for (NSDictionary *b in j[@"bindings"]) { NSString *t = b[@"msl_type"]; if ([t containsString:@"texture"] || [t containsString:@"sampler"]) { printf("%s %s SKIP texture/sampler binding %s\n", argv[2], stage.UTF8String, [b[@"msl_name"] UTF8String]); return 0; } }
    id<MTLTexture> rt = [dev newTextureWithDescriptor:({ MTLTextureDescriptor *t = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:16 height:16 mipmapped:NO]; t.usage = MTLTextureUsageRenderTarget; t.storageMode = MTLStorageModePrivate; t; })];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor]; rp.colorAttachments[0].texture = rt; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionDontCare;
    id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
    [e setRenderPipelineState:ps];
    [e setObjectBuffer:ab offset:0 atIndex:0]; [e setMeshBuffer:ab offset:0 atIndex:0];
    [e useResource:zero usage:MTLResourceUsageRead stages:MTLRenderStageObject | MTLRenderStageMesh];
    uint32_t first = 0; [e setObjectBytes:&first length:4 atIndex:30]; [e setMeshBytes:&first length:4 atIndex:30];
    id<MTLBuffer> flag = [dev newBufferWithLength:16 options:MTLResourceStorageModeShared]; uint32_t *fl = flag.contents; fl[0] = 0xffffffffu; fl[1] = fl[2] = 0;   // guard.py: changed guard words, first, array words
    [e setObjectBuffer:flag offset:0 atIndex:1]; [e setMeshBuffer:flag offset:0 atIndex:1];
    [e drawMeshThreadgroups:MTLSizeMake(1, 1, 1) threadsPerObjectThreadgroup:MTLSizeMake(isMesh ? 1 : threads, 1, 1) threadsPerMeshThreadgroup:MTLSizeMake(isMesh ? threads : 1, 1, 1)];
    [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
    if (cb.error) {
        NSString *s = cb.error.description; NSRange r = [s rangeOfString:@"Invalid"]; NSString *what = r.location != NSNotFound ? [s substringWithRange:NSMakeRange(r.location, MIN(200, s.length - r.location))] : [s substringToIndex:MIN(200, s.length)];
        printf("%s %s ERROR %s\n", argv[2], stage.UTF8String, [[what stringByReplacingOccurrencesOfString:@"\n" withString:@" "] UTF8String]); return 1; }
    if (fl[0] == 0xffffffffu) printf("%s %s OK (no guard check ran)\n", argv[2], stage.UTF8String);
    else if (fl[0]) printf("%s %s GUARD %u guard words written past the %u-word stage array, the first at +%u\n", argv[2], stage.UTF8String, fl[0], fl[2], fl[1]);
    else printf("%s %s OK (guard intact, %u-word array)\n", argv[2], stage.UTF8String, fl[2]);
    return 0;
} }
