// Two bindings of the same pool address expose different ranges to the translated fragment shader.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }

int main(int argc, char **argv) { @autoreleasepool {
    if (argc != 2) return 1;
    mtl_init();
    NSError *err = nil;
    NSString *source = [NSString stringWithContentsOfFile:@(argv[1]) encoding:NSUTF8StringEncoding error:&err];
    id<MTLLibrary> lib = source ? [g_dev newLibraryWithSource:source options:nil error:&err] : nil;
    if (!lib) { fprintf(stderr, "%s\n", err.localizedDescription.UTF8String); return 1; }
    NvShader *fs = [NvShader new];
    fs->stage = 1;
    fs->fn = [lib newFunctionWithName:@"read_bank"];
    fs->ae = [fs->fn newArgumentEncoderWithBufferIndex:0];
    fs->rf.nb = 2;
    fs->rf.b[0] = (nvn_rbind){.kind=NVN_RB_CBUF, .reg=11, .aid=0};
    fs->rf.b[1] = (nvn_rbind){.kind=NVN_RB_CBUF_BYTES, .reg=11, .aid=1};
    MTLRenderPipelineDescriptor *desc = [MTLRenderPipelineDescriptor new];
    desc.vertexFunction = [lib newFunctionWithName:@"tri"];
    desc.fragmentFunction = fs->fn;
    desc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Uint;
    id<MTLRenderPipelineState> pipeline = [g_dev newRenderPipelineStateWithDescriptor:desc error:&err];
    if (!pipeline) { fprintf(stderr, "%s\n", err.localizedDescription.UTF8String); return 1; }
    size_t page = getpagesize();
    uint32_t *pool = NULL;
    if (posix_memalign((void **)&pool, page, page)) return 1;
    memset(pool, 0, page);
    pool[0] = 7; pool[144*4] = 0xaabbccdd; pool[144*4+1] = 23;
    mtl_pool_add(pool, page);
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Uint width:1 height:1 mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> tex = [g_dev newTextureWithDescriptor:td]; res_add(tex);
    id<MTLBuffer> result = [g_dev newBufferWithLength:256 options:MTLResourceStorageModeShared]; res_add(result);
    const uint32_t cases[][4] = {
        {2384,7,0xaabbccdd,23}, {16,7,0,0}, {2306,7,0xccdd,0}, {0,0,0,0}, {2384,7,0xaabbccdd,23}
    };
    for (size_t i=0; i<sizeof cases/sizeof *cases; i++) {
        uint32_t size = cases[i][0];
        S.ubo[1][11] = (p_bind_buf){1,11,size ? (uintptr_t)pool : 0,size};
        g_args_dirty[1] = 1;
        mtl_sub_begin();
        MTL4RenderPassDescriptor *pass = [MTL4RenderPassDescriptor new];
        pass.colorAttachments[0].texture = tex;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTL4RenderCommandEncoder> enc = enc_r_begin(pass);
        enc_r_pso(enc,pipeline); enc_r_viewport(enc,(MTLViewport){0,0,1,1,0,1});
        if (!encode_args((id<MTLCommandEncoder>)enc,fs,1,0)) return 2;
        enc_r_draw(enc,MTLPrimitiveTypeTriangle,0,3); enc_r_end(enc);
        enc_b_copy_texture_buffer(blit(),tex,0,0,MTLOriginMake(0,0,0),MTLSizeMake(1,1,1),result,0,256,256);
        if (!mtl_wait(mtl_sub_commit(),5000000000ull) || mtl_last_commit_error()) return 2;
        const uint32_t *got = result.contents;
        printf("bound %u: %#x %#x %#x\n",size,got[0],got[1],got[2]);
        if (memcmp(got, cases[i]+1, 3*sizeof *got)) return 3;
    }
    mtl_pool_del(pool); mtl_drain(); free(pool);
    return 0;
} }
