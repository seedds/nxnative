// Newly allocated Switch texture storage is zero, including every MSAA color sample.
// Poison the Metal storage first so this cannot pass because the driver happened to zero it.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static id<MTLBuffer> read_samples(id<MTLCommandQueue> q, id<MTLComputePipelineState> reader, id<MTLTexture> texture, unsigned layers) {
    id<MTLBuffer> out = [g_dev newBufferWithLength:64 * texture.sampleCount * layers * 16 options:MTLResourceStorageModeShared];
    CHECK(out != nil); if (!out) return nil;
    id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
    [e setComputePipelineState:reader]; [e setTexture:texture atIndex:0]; [e setBuffer:out offset:0 atIndex:0];
    [e dispatchThreads:MTLSizeMake(8,8,layers) threadsPerThreadgroup:MTLSizeMake(8,8,1)];
    [e endEncoding]; [cb commit]; [cb waitUntilCompleted]; CHECK(cb.status == MTLCommandBufferStatusCompleted);
    return out;
}

int main(void) { @autoreleasepool {
    setenv("DM_PSO_ARCHIVE", "off", 1); setenv("DM_PSO_PREBUILD", "0", 1); setenv("DM_PSO_MANIFEST", "off", 1);
    setenv("DM_PSO_USER", "/dev/null", 1); setenv("DM_PSO_RECORD", "/dev/null", 1);
    mtl_init(); NSError *error = nil;
    NSString *src = @"#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void read_ms(texture2d_ms<float,access::read> t [[texture(0)]], device float4 *o [[buffer(0)]], uint2 p [[thread_position_in_grid]]) { for(uint s=0;s<t.get_num_samples();s++) o[(p.y*8+p.x)*t.get_num_samples()+s]=t.read(p,s); }\n"
        "kernel void read_ms_array(texture2d_ms_array<float,access::read> t [[texture(0)]], device float4 *o [[buffer(0)]], uint3 p [[thread_position_in_grid]]) { for(uint s=0;s<t.get_num_samples();s++) o[((p.z*8+p.y)*8+p.x)*t.get_num_samples()+s]=t.read(p.xy,p.z,s); }\n";
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:src options:nil error:&error]; CHECK(lib != nil); if (!lib) return 1;
    id<MTLComputePipelineState> readers[2];
    for (int array = 0; array < 2; array++) {
        id<MTLFunction> fn = [lib newFunctionWithName:array ? @"read_ms_array" : @"read_ms"];
        CHECK(fn != nil); if (!fn) return 1;
        readers[array] = [g_dev newComputePipelineStateWithFunction:fn error:&error]; CHECK(readers[array] != nil); if (!readers[array]) return 1;
    }
    MTLPixelFormat formats[] = {MTLPixelFormatRGBA8Unorm, MTLPixelFormatRG11B10Float, MTLPixelFormatRGBA16Float};
    id<MTLCommandQueue> q = [g_dev newCommandQueue]; CHECK(q != nil); if (!q) return 1;
    for (int array = 0; array < 2; array++) for (int fi = 0; fi < 3; fi++) for (int samples = 2; samples <= 4; samples *= 2) {
        unsigned layers = array ? 3 : 1;
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:formats[fi] width:8 height:8 mipmapped:NO];
        td.textureType = array ? MTLTextureType2DMultisampleArray : MTLTextureType2DMultisample;
        td.arrayLength = layers; td.sampleCount = samples; td.storageMode = MTLStorageModePrivate;
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        id<MTLTexture> t = [g_dev newTextureWithDescriptor:td]; CHECK(t != nil); if (!t) continue;
        id<MTLCommandBuffer> cb = [q commandBuffer];
        for (unsigned layer = 0; layer < layers; layer++) {
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = t; rp.colorAttachments[0].slice = layer; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore; rp.colorAttachments[0].clearColor = MTLClearColorMake(.75-.125*layer,.5,.25,.875);
            [[cb renderCommandEncoderWithDescriptor:rp] endEncoding];
        }
        [cb commit]; [cb waitUntilCompleted];
        CHECK(cb.status == MTLCommandBufferStatusCompleted);
        // Verify every poisoned sample before zeroing; an untouched array slice
        // must not pass merely because its new allocation happened to be zero.
        id<MTLBuffer> before = read_samples(q, readers[array], t, layers); if (!before) continue;
        float *poison = before.contents; int filled = 1;
        for (unsigned layer = 0; layer < layers; layer++) for (int i = 0; i < 64*samples; i++) {
            float *p = poison + (layer*64*samples+i)*4;
            filled &= fabsf(p[0]-(.75f-.125f*layer)) < .01f && fabsf(p[1]-.5f) < .01f && fabsf(p[2]-.25f) < .01f;
        }
        CHECK(filled);
        id<MTLBuffer> ordered = [g_dev newBufferWithLength:before.length options:MTLResourceStorageModeShared];
        CHECK(ordered != nil); if (!ordered) continue;
        memset(ordered.contents, 0xcd, ordered.length);
        MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new]; ad.maxBufferBindCount = ad.maxTextureBindCount = 1;
        id<MTL4ArgumentTable> table = [g_dev newArgumentTableWithDescriptor:ad error:&error];
        CHECK(table != nil); if (!table) continue;
        res_add(t); res_add(ordered); state_reset(); memset(&R,0,sizeof R); mtl_sub_begin();
        table = rec_table(table);
        [table setTexture:t.gpuResourceID atIndex:0]; [table setAddress:ordered.gpuAddress atIndex:0];
        tex_zero_fill(t);
        // Read in the same production submission: a read after draining alone
        // cannot establish that the upload initializer precedes its consumer.
        id<MTL4ComputeCommandEncoder> ce = enc_c_begin(nil); [ce setArgumentTable:table]; enc_c_pso(ce, readers[array]);
        enc_c_dispatch(ce, MTLSizeMake(1,1,layers), MTLSizeMake(8,8,1)); enc_c_end(ce);
        mtl_sub_commit(); mtl_drain();
        CHECK(!mtl_last_commit_error());
        float *seen = ordered.contents; int ordered_zero = 1;
        for (unsigned i = 0; i < 64*samples*layers; i++) for (int c = 0; c < 4; c++) ordered_zero &= seen[4*i+c] == (c == 3 && fi == 1 ? 1.0f : 0.0f);
        CHECK(ordered_zero);
        id<MTLBuffer> out = read_samples(q, readers[array], t, layers); if (!out) continue;
        float *p = out.contents; int zero = 1;
        for (unsigned i = 0; i < 64*samples*layers; i++) for (int c = 0; c < 4; c++) zero &= p[4*i+c] == (c == 3 && fi == 1 ? 1.0f : 0.0f);
        CHECK(zero); printf("MSAA color initialization format %lu samples %d layers %u: %s\n", (unsigned long)formats[fi], samples, layers, zero ? "PASS" : "FAIL");
        res_remove(ordered); res_remove(t);
    }
    printf("MSAA color initialization: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
