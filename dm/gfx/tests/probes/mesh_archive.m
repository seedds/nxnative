// Does a mesh render pipeline built through MTL4Compiler (MTL4MeshRenderPipelineDescriptor) land in a Metal 4 pipeline archive, and is it found there again?
// The host builds mesh pipelines with newRenderPipelineStateWithMeshDescriptor (nvn_geometry.inc:434, :585), which no archive covers. No game: one tiny object + mesh + fragment
// shader compiled from source. Build: clang -fobjc-arc -framework Metal -framework Foundation mesh_archive.m -o mesh_archive
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include <stdio.h>
#include <mach/mach_time.h>

static const char *SRC =
"#include <metal_stdlib>\nusing namespace metal;\n"
"struct V { float4 pos [[position]]; float4 col; };\n"
"struct P { bool cull [[primitive_culled]]; };\n"
"using M = mesh<V, P, 3, 1, topology::triangle>;\n"
"struct Payload { float scale; };\n"
"[[object]] void ob(object_data Payload &p [[payload]], mesh_grid_properties g) { p.scale = 1.0; g.set_threadgroups_per_grid(uint3(1, 1, 1)); }\n"
"[[mesh]] void ms(M m, const object_data Payload &p [[payload]], uint t [[thread_index_in_threadgroup]]) {\n"
"  if (t == 0) m.set_primitive_count(1);\n"
"  if (t < 3) { V v; v.pos = float4(t == 1 ? p.scale : -p.scale, t == 2 ? p.scale : -p.scale, 0, 1); v.col = float4(1, 0, 0, 1); m.set_vertex(t, v); m.set_index(t, t); }\n"
"  if (t == 0) { P q; q.cull = false; m.set_primitive(0, q); }\n"
"}\n"
"fragment float4 fs(V v [[stage_in]]) { return v.col; }\n";

static double ms_since(uint64_t t0) { static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb); return (mach_absolute_time() - t0) * tb.numer / (double)tb.denom / 1e6; }

int main(void) { @autoreleasepool {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice(); NSError *e = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:[NSString stringWithUTF8String:SRC] options:nil error:&e];
    if (!lib) { printf("library: %s\n", e.localizedDescription.UTF8String); return 1; }
    NSString *dir = [NSTemporaryDirectory() stringByAppendingPathComponent:@"mesharch"]; [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    NSURL *url = [NSURL fileURLWithPath:[dir stringByAppendingPathComponent:@"mesh.mtl4a"]]; [NSFileManager.defaultManager removeItemAtURL:url error:nil];

    MTL4MeshRenderPipelineDescriptor *(^desc)(void) = ^{
        MTL4MeshRenderPipelineDescriptor *d = [MTL4MeshRenderPipelineDescriptor new];
        MTL4LibraryFunctionDescriptor *o = [MTL4LibraryFunctionDescriptor new]; o.library = lib; o.name = @"ob";
        MTL4LibraryFunctionDescriptor *m = [MTL4LibraryFunctionDescriptor new]; m.library = lib; m.name = @"ms";
        MTL4LibraryFunctionDescriptor *f = [MTL4LibraryFunctionDescriptor new]; f.library = lib; f.name = @"fs";
        d.objectFunctionDescriptor = o; d.meshFunctionDescriptor = m; d.fragmentFunctionDescriptor = f;
        d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm; d.rasterSampleCount = 1;
        return d;
    };

    // 1. Build with a compiler that serializes binaries, flush an archive
    MTL4PipelineDataSetSerializerDescriptor *sd = [MTL4PipelineDataSetSerializerDescriptor new]; sd.configuration = MTL4PipelineDataSetSerializerConfigurationCaptureBinaries;
    id<MTL4PipelineDataSetSerializer> ser = [dev newPipelineDataSetSerializerWithDescriptor:sd];
    MTL4CompilerDescriptor *cd = [MTL4CompilerDescriptor new]; cd.pipelineDataSetSerializer = ser;
    id<MTL4Compiler> comp = [dev newCompilerWithDescriptor:cd error:&e]; if (!comp) { printf("compiler: %s\n", e.localizedDescription.UTF8String); return 1; }
    uint64_t t0 = mach_absolute_time();
    id<MTLRenderPipelineState> p1 = [comp newRenderPipelineStateWithDescriptor:desc() dynamicLinkingDescriptor:nil compilerTaskOptions:nil error:&e];
    printf("MTL4Compiler mesh pipeline: %s in %.1f ms%s%s\n", p1 ? "built" : "FAILED", ms_since(t0), p1 ? "" : ": ", p1 ? "" : e.localizedDescription.UTF8String);
    if (!p1) return 1;
    BOOL ok = [ser serializeAsArchiveAndFlushToURL:url error:&e];
    NSDictionary *attr = [NSFileManager.defaultManager attributesOfItemAtPath:url.path error:nil];
    printf("archive flushed: %s, %llu bytes%s%s\n", ok ? "yes" : "NO", (unsigned long long)[attr fileSize], ok ? "" : ": ", ok ? "" : e.localizedDescription.UTF8String);

    // 2. Open the archive and ask it directly for the same pipeline (as the host does for its render bases: newRenderPipelineStateWithDescriptor on MTL4Archive)
    id<MTL4Archive> arch = [dev newArchiveWithURL:url error:&e]; if (!arch) { printf("archive open: %s\n", e.localizedDescription.UTF8String); return 1; }
    t0 = mach_absolute_time(); e = nil;
    id<MTLRenderPipelineState> p2 = [arch newRenderPipelineStateWithDescriptor:desc() error:&e];
    printf("MTL4Archive lookup of the mesh pipeline: %s in %.2f ms%s%s\n", p2 ? "HIT" : "miss", ms_since(t0), p2 ? "" : ": ", p2 ? "" : e.localizedDescription.UTF8String);
    // a pipeline that differs (another colour format) must miss
    MTL4MeshRenderPipelineDescriptor *d2 = desc(); d2.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float; e = nil;
    id<MTLRenderPipelineState> p3 = [arch newRenderPipelineStateWithDescriptor:d2 error:&e];
    printf("MTL4Archive lookup of a different mesh pipeline: %s (expected miss)\n", p3 ? "HIT" : "miss");

    // 3. The old device path for comparison
    MTLMeshRenderPipelineDescriptor *od = [MTLMeshRenderPipelineDescriptor new];
    od.objectFunction = [lib newFunctionWithName:@"ob"]; od.meshFunction = [lib newFunctionWithName:@"ms"]; od.fragmentFunction = [lib newFunctionWithName:@"fs"]; od.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    t0 = mach_absolute_time();
    id<MTLRenderPipelineState> p4 = [dev newRenderPipelineStateWithMeshDescriptor:od options:MTLPipelineOptionNone reflection:nil error:&e];
    printf("MTLDevice newRenderPipelineStateWithMeshDescriptor: %s in %.1f ms\n", p4 ? "built" : "FAILED", ms_since(t0));
    return p2 ? 0 : 2;
} }
