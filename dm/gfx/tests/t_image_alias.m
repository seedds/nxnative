// Live dynamic-resolution descriptors must not multiply their NVN storage.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

static tex_t image(pool_t *pool, uint64_t serial, unsigned w, unsigned h) {
    tex_t t = {.magic=MAGIC('T','E','X',0), .serial=serial, .pool=pool, .mem=pool->mem};
    t.d = (nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D, .format=NVN_FORMAT_RGBA8, .flags=NVN_TEXTURE_FLAGS_COMPRESSIBLE|NVN_TEXTURE_FLAGS_IMAGE,
                      .width=w, .height=h, .depth=1, .levels=1, .samples=1};
    nvn_tex_layout(&t.d, &t.L); return t;
}
static id<MTLBuffer> read_image(id<MTLTexture> t) {
    NSUInteger row=t.width*4, bytes=row*t.height;
    id<MTLBuffer> b=[g_dev newBufferWithLength:bytes options:MTLResourceStorageModeShared]; res_add(b);
    enc_b_copy_texture_buffer(blit(), t, 0, 0, MTLOriginMake(0,0,0), MTLSizeMake(t.width,t.height,1), b, 0, row, bytes);
    mtl_sub_commit(); mtl_drain(); CHECK(!mtl_last_commit_error(), "image read completes without GPU error");
    res_remove(b); mtl_sub_begin(); return b;
}
static void pixels(id<MTLTexture> t, uint32_t seed) {
    NSUInteger bytes=t.width*t.height*4;
    id<MTLBuffer> b=[g_dev newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    uint32_t *p=b.contents; for (NSUInteger i=0;i<bytes/4;i++) p[i]=seed^(uint32_t)i;
    res_add(b); enc_b_copy_buffer_texture(blit(), b, 0, t.width*4, bytes, MTLSizeMake(t.width,t.height,1), t, 0, 0, MTLOriginMake(0,0,0));
    fam_mark_valid(t);
    id<MTLBuffer> out=read_image(t); CHECK(!memcmp(out.contents,b.contents,bytes), "every literal pixel survives alternating resolutions"); res_remove(b);
}
static void is_zero(id<MTLTexture> t) {
    id<MTLBuffer> b=read_image(t); uint32_t *p=b.contents; int zero=1;
    for (NSUInteger i=0;i<b.length/4;i++) zero &= p[i]==0;
    CHECK(zero, "fresh family is zero, including beyond its first-used small descriptor");
}
static void fragment_copy(id<MTLTexture> source, uint32_t seed) {
    MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:source.pixelFormat width:source.width height:source.height mipmapped:NO];
    d.storageMode=MTLStorageModePrivate; d.usage=MTLTextureUsageRenderTarget;
    id<MTLTexture> target=[g_dev newTextureWithDescriptor:d]; res_add(target);
    NVNcopyRegion r={.width=(int)source.width,.height=(int)source.height,.depth=1};
    CHECK(copy_t2t_draw(source,target,0,0,0,0,&r,&r,0,0),"heap-backed texture is copied by the fragment pipeline");
    id<MTLBuffer> out=read_image(target); uint32_t *p=out.contents; int valid=1;
    for (NSUInteger i=0;i<out.length/4;i++) valid &= p[i]==(seed^(uint32_t)i);
    CHECK(valid,"fragment sampling reads every heap-backed texel");
    res_remove(target);
}
static void indirect_pixels(id<MTLTexture> t, uint32_t seed) {
    NSError *error=nil;
    id<MTLLibrary> lib=[g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct Args { texture2d<float,access::read> tex [[id(0)]]; };\n"
        "kernel void read_image(constant Args &a [[buffer(0)]], device uint *o [[buffer(1)]], uint2 p [[thread_position_in_grid]]) { if(p.x>=a.tex.get_width()||p.y>=a.tex.get_height())return; uint4 v=uint4(round(a.tex.read(p)*255.0f)); o[p.y*a.tex.get_width()+p.x]=v.x|(v.y<<8)|(v.z<<16)|(v.w<<24); }\n" options:nil error:&error];
    id<MTLFunction> fn=[lib newFunctionWithName:@"read_image"];
    id<MTLComputePipelineState> ps=fn?[g_dev newComputePipelineStateWithFunction:fn error:&error]:nil;
    CHECK(ps!=nil,"indirect texture reader compiles"); if (!ps) return;
    id<MTLArgumentEncoder> ae=[fn newArgumentEncoderWithBufferIndex:0];
    id<MTLBuffer> args=[g_dev newBufferWithLength:ae.encodedLength options:MTLResourceStorageModeShared];
    id<MTLBuffer> out=[g_dev newBufferWithLength:t.width*t.height*4 options:MTLResourceStorageModeShared];
    [ae setArgumentBuffer:args offset:0]; [ae setTexture:t atIndex:0]; res_add(args); res_add(out);
    MTL4ArgumentTableDescriptor *ad=[MTL4ArgumentTableDescriptor new]; ad.maxBufferBindCount=2;
    id<MTL4ArgumentTable> at=rec_table([g_dev newArgumentTableWithDescriptor:ad error:&error]);
    [at setAddress:args.gpuAddress atIndex:0]; [at setAddress:out.gpuAddress atIndex:1];
    id<MTL4ComputeCommandEncoder> e=enc_c_begin(); [e setArgumentTable:at]; enc_c_pso(e,ps);
    enc_c_dispatch(e,MTLSizeMake((t.width+7)/8,(t.height+7)/8,1),MTLSizeMake(8,8,1)); enc_c_end(e);
    mtl_sub_commit(); mtl_drain();
    CHECK(!mtl_last_commit_error(),"indirect Metal 4 read completes with only the heap resident");
    uint32_t *p=out.contents; int valid=1;
    for (NSUInteger i=0;i<out.length/4;i++) valid &= p[i]==(seed^(uint32_t)i);
    CHECK(valid,"indirect access reads every literal pixel");
    res_remove(args); res_remove(out); mtl_sub_begin();
}
static void atomic_pixel(id<MTLTexture> t, uint32_t seed) {
    NSError *error=nil;
    id<MTLLibrary> lib=[g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void bump(texture2d<uint,access::read_write> image [[texture(0)]]) { image.atomic_fetch_add(uint2(0,0),5u); }\n" options:nil error:&error];
    id<MTLFunction> fn=[lib newFunctionWithName:@"bump"];
    id<MTLComputePipelineState> ps=fn?[g_dev newComputePipelineStateWithFunction:fn error:&error]:nil;
    CHECK(ps!=nil,"native texture-atomic reader compiles"); if (!ps) return;
    MTL4ArgumentTableDescriptor *ad=[MTL4ArgumentTableDescriptor new]; ad.maxTextureBindCount=1;
    id<MTL4ArgumentTable> at=rec_table([g_dev newArgumentTableWithDescriptor:ad error:&error]);
    [at setTexture:t.gpuResourceID atIndex:0];
    id<MTL4ComputeCommandEncoder> e=enc_c_begin(); [e setArgumentTable:at]; enc_c_pso(e,ps);
    enc_c_dispatch(e,MTLSizeMake(1,1,1),MTLSizeMake(1,1,1)); enc_c_end(e);
    id<MTLBuffer> out=read_image(t); uint32_t *p=out.contents; int valid=p[0]==seed+5;
    for (NSUInteger i=1;i<out.length/4;i++) valid &= p[i]==(seed^(uint32_t)i);
    CHECK(valid,"native atomic modifies the heap-backed integer image and preserves every other pixel");
}
// A 32-bit colour texture a shader runs image atomics on (Ocarina of Time's d17eefaff4f0bc23 ORs into RGBA8): R32 storage, the same bytes, with its own format as a view for everything else
static void atomic_colour(pool_t *pool) {
    tex_t t=image(pool,995000,16,16); mtl_tex_init(&t);
    id<MTLTexture> base=tex_mtl(&t); uint32_t seed=0x11223344; pixels(base,seed);
    id<MTLTexture> up=tex_upgrade(&t,base,MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite|MTLTextureUsageShaderAtomic,MTLPixelFormatR32Uint);
    CHECK(up.pixelFormat==MTLPixelFormatRGBA8Unorm && up.parentTexture.pixelFormat==MTLPixelFormatR32Uint,"a colour texture moves to R32 storage and keeps its format as a view");
    CHECK(tex_mtl(&t)==up,"the texture's serial now names the view");
    id<MTLBuffer> copied=read_image(up); CHECK(((uint32_t *)copied.contents)[3]==(seed^3u),"the upgrade copies the pixels into the new storage");
    id<MTLTexture> raw=up.parentTexture; atomic_pixel(raw,seed);
    id<MTLBuffer> out=read_image(up); uint32_t *p=out.contents; int valid=p[0]==seed+5;
    for (NSUInteger i=1;i<out.length/4;i++) valid &= p[i]==(seed^(uint32_t)i);
    CHECK(valid,"the colour view reads the pixels the copy kept and the atomic changed");
    mtl_tex_release(&t);
}
static void depth_planes(pool_t *pool) {
    tex_t d[2];
    for (int i=0;i<2;i++) { d[i]=image(pool,992000+i,32u<<i,32u<<i); d[i].off=12<<20; d[i].mem=pool->mem+d[i].off;
        d[i].d.format=NVN_FORMAT_DEPTH32F_STENCIL8; nvn_tex_layout(&d[i].d,&d[i].L); mtl_tex_init(&d[i]); }
    NSError *error=nil;
    id<MTLLibrary> lib=[g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void planes(texture2d<uint,access::read> s [[texture(0)]], depth2d<float,access::read> d [[texture(1)]], device uint2 *o [[buffer(0)]], uint2 p [[thread_position_in_grid]]) { o[p.y*d.get_width()+p.x]=uint2(s.read(p).r,as_type<uint>(d.read(p))); }\n" options:nil error:&error];
    id<MTLComputePipelineState> ps=[g_dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"planes"] error:&error];
    CHECK(ps!=nil,"depth/stencil reader compiles"); if (!ps) return;
    for (int i=0;i<3;i++) {
        tex_t *t=&d[i%2]; id<MTLTexture> base=tex_mtl(t);
        CHECK(base.heap!=nil,"depth/stencil resolutions share physical storage");
        state_reset(); memset(&R,0,sizeof R); R.has_rt=1; R.rt.depth=t;
        struct { op_hdr h; p_clear_ds clear; op_hdr end; } ops={0};
        ops.h=(op_hdr){OP_CLEAR_DS,sizeof(op_hdr)+sizeof(p_clear_ds)};
        ops.clear=(p_clear_ds){.depth=.25,.depth_mask=1,.stencil=0x30+i,.stencil_mask=255}; ops.end=(op_hdr){OP_END,sizeof(op_hdr)};
        mtl_exec((const uint8_t *)&ops); mtl_sub_commit(); mtl_drain();
        CHECK(!mtl_last_commit_error(),"production depth/stencil clear completes");
        id<MTLTexture> stencil=[base newTextureViewWithPixelFormat:MTLPixelFormatX32_Stencil8];
        id<MTLBuffer> out=[g_dev newBufferWithLength:base.width*base.height*8 options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cb=q_cb(); id<MTLComputeCommandEncoder> e=[cb computeCommandEncoder];
        [e setComputePipelineState:ps]; [e setTexture:stencil atIndex:0]; [e setTexture:base atIndex:1]; [e setBuffer:out offset:0 atIndex:0];
        [e dispatchThreads:MTLSizeMake(base.width,base.height,1) threadsPerThreadgroup:MTLSizeMake(8,8,1)]; [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
        uint32_t *p=out.contents; int valid=cb.status==MTLCommandBufferStatusCompleted;
        for (NSUInteger j=0;j<out.length/8;j++) valid &= p[j*2]==0x30u+i && p[j*2+1]==0x3e800000;
        CHECK(valid,"both planes survive resolution changes and stencil views"); mtl_sub_begin();
    }
    for (int i=0;i<2;i++) mtl_tex_release(&d[i]);
    memset(&R,0,sizeof R);
}
static void separate_storage(pool_t *pool, id<MTLHeap> original) {
    pool_t other_pool=*pool; other_pool.mem+=14<<20; other_pool.size=2<<20;
    pool_t cpu_pool=*pool; cpu_pool.flags=NVN_MEMORY_POOL_FLAGS_CPU_CACHED|NVN_MEMORY_POOL_FLAGS_GPU_CACHED|NVN_MEMORY_POOL_FLAGS_COMPRESSIBLE;
    tex_t t[8];
    for (int i=0;i<8;i++) {
        t[i]=image(i<2?&other_pool:i<4?&cpu_pool:pool,994000+i,(i%2+1)*64,64);
        if (i>=4) { t[i].off=8<<20; t[i].mem=pool->mem+t[i].off; }
        if (i>=6) t[i].d.format=NVN_FORMAT_R32UI;
        nvn_tex_layout(&t[i].d,&t[i].L); mtl_tex_init(&t[i]);
    }
    id<MTLTexture> p=tex_mtl(&t[0]), o=tex_mtl(&t[4]), f=tex_mtl(&t[6]);
    CHECK(p.heap && p.heap!=original,"another physical pool gets a separate heap");
    CHECK(o.heap && o.heap!=original && o.heap!=p.heap,"another offset gets a separate heap");
    CHECK(f.heap && f.heap!=o.heap && f.heap!=original,"another format cannot join an existing heap");
    CHECK(f.heap==tex_mtl(&t[7]).heap,"integer images reuse backing when Metal hides atomic usage");
    CHECK(tex_atomic_capable(f) && tex_atomic_capable(tex_mtl(&t[7])),"integer aliases retain shader-atomic capability");
    CHECK(!tex_mtl(&t[2]).heap && !tex_mtl(&t[3]).heap,"CPU-visible resolution families retain their original backing");
    pixels(p,0x18273645); pixels(o,0x29384756); pixels(f,0x3a495867);
    atomic_pixel(f,0x3a495867);
    for (int i=0;i<8;i++) mtl_tex_release(&t[i]);
    mtl_alias_drop_pool(&other_pool); mtl_alias_drop_pool(&cpu_pool);
}
// Two families whose offsets lie in one 64 KB cell of the sharing table's index (alias_index): the owner lookup reads that cell and takes only the entries at its own offset
static void same_cell(pool_t *pool) {
    tex_t t[4];
    for (int i=0;i<4;i++) { t[i]=image(pool,996000+i,(i%2+1)*64,64); t[i].off=(11<<20)+(i/2)*0x4000; t[i].mem=pool->mem+t[i].off; mtl_tex_init(&t[i]); }
    id<MTLTexture> m[4]; for (int i=0;i<4;i++) m[i]=tex_mtl(&t[i]);
    CHECK(m[0].heap && m[0].heap==m[1].heap && m[2].heap && m[2].heap==m[3].heap,"each family in the cell shares its own heap");
    CHECK(m[0].heap!=m[2].heap,"an offset 16 KB further in the same cell is another family");
    for (int i=0;i<4;i++) mtl_tex_release(&t[i]);
}
// Resolution-family cross-size safety: with compression on (default), cross-size loads clear to 0 (ASTRAL CHAIN #1935, OoT bloom), cross-size copies/binds return 0 without disturbing
// the live owner, and combined depth-stencil half-clears initialize both planes. With DM_NVN_FAMILY_NOCOMPRESS=1, raw reads see misplaced texels of the same format
static void cross_size_read(void) {
    void *mem=NULL; posix_memalign(&mem,16384,32<<20); memset(mem,0,32<<20);
    pool_t pool={.magic=MAGIC('P','O','O','L'), .flags=NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS|NVN_MEMORY_POOL_FLAGS_GPU_CACHED|NVN_MEMORY_POOL_FLAGS_COMPRESSIBLE, .mem=mem, .size=16<<20};
    mtl_pool_add(pool.mem, pool.size);
    tex_t a[2]; unsigned w[2]={1600,1440}, h[2]={900,810};
    for (int i=0;i<2;i++) { a[i]=image(&pool,993000+i,w[i],h[i]); a[i].d.format=NVN_FORMAT_RGBA16F; a[i].d.flags=NVN_TEXTURE_FLAGS_COMPRESSIBLE; nvn_tex_layout(&a[i].d,&a[i].L); mtl_tex_init(&a[i]); }
    id<MTLTexture> big=tex_mtl(&a[0]), small=tex_mtl(&a[1]);
    CHECK(big.heap && big.heap==small.heap,"a render-target family shares one heap");
    state_reset(); memset(&R,0,sizeof R); R.has_rt=1; R.rt.n=1; R.rt.color[0]=&a[1];
    struct { op_hdr h; p_clear_color clear; op_hdr end; } ops={0};
    ops.h=(op_hdr){OP_CLEAR_COLOR,sizeof(op_hdr)+sizeof(p_clear_color)};
    ops.clear=(p_clear_color){.index=0,.mask=15,.rgba={1,.5f,.25f,1}}; ops.end=(op_hdr){OP_END,sizeof(op_hdr)};
    mtl_exec((const uint8_t *)&ops); mtl_sub_commit(); mtl_drain(); mtl_sub_begin(); memset(&R,0,sizeof R);
    if (fam_nocomp()) {
        NSUInteger row=big.width*8, bytes=row*big.height;
        id<MTLBuffer> b=[g_dev newBufferWithLength:bytes options:MTLResourceStorageModeShared]; res_add(b);
        enc_b_copy_texture_buffer(blit(), big, 0, 0, MTLOriginMake(0,0,0), MTLSizeMake(big.width,big.height,1), b, 0, row, bytes);
        mtl_sub_commit(); mtl_drain(); res_remove(b); mtl_sub_begin();
        const uint64_t *p=b.contents, clear=0x3c00340038003c00ull; NSUInteger nclear=0, nzero=0, other=0;
        for (NSUInteger i=0;i<bytes/8;i++) { if (p[i]==clear) nclear++; else if (!p[i]) nzero++; else other++; }
        printf("cross-size read (uncompressed): %lu clear, %lu zero, %lu other texels of %lu (compression %d)\n",(unsigned long)nclear,(unsigned long)nzero,(unsigned long)other,(unsigned long)(bytes/8),(int)big.allowGPUOptimizedContents);
        CHECK(!other && nclear,"another size's writes read back as its texels, misplaced");
    } else {
        CHECK(big.allowGPUOptimizedContents && small.allowGPUOptimizedContents,"family textures keep lossless compression by default");
        CHECK(fam_is_valid(small) && !fam_is_valid(big),"clearing small marks small valid and big invalid");
        // 1. Cross-size OP_COPY_T2B returns zeros without disturbing the live owner (small).
        size_t bytes=(size_t)big.width*big.height*8;
        void *rb=NULL; posix_memalign(&rb,16384,bytes); memset(rb,0x5a,bytes);
        mtl_pool_add(rb, bytes);
        struct { op_hdr h; p_copy_b2t cp; op_hdr end; } t2b={0};
        t2b.h=(op_hdr){OP_COPY_T2B,sizeof(op_hdr)+sizeof(p_copy_b2t)};
        t2b.cp=(p_copy_b2t){.tex=&a[0], .src=(uint64_t)(uintptr_t)rb, .r={0,0,0,(int)big.width,(int)big.height,1}};
        t2b.end=(op_hdr){OP_END,sizeof(op_hdr)};
        mtl_exec((const uint8_t *)&t2b); mtl_sub_commit(); mtl_drain(); mtl_sub_begin();
        const uint64_t *rp=(const uint64_t *)rb; NSUInteger bad_t2b=0;
        for (NSUInteger i=0;i<bytes/8;i++) if (rp[i]!=0) bad_t2b++;
        CHECK(!bad_t2b,"cross-size texture-to-buffer copy yields zeros, no compressed-block garbage");
        CHECK(fam_is_valid(small),"cross-size copy leaves the live owner valid");
        // Small must still hold its exact clear colour.
        NSUInteger srow=small.width*8, sbytes=srow*small.height;
        id<MTLBuffer> sb=[g_dev newBufferWithLength:sbytes options:MTLResourceStorageModeShared]; res_add(sb);
        enc_b_copy_texture_buffer(blit(), small, 0, 0, MTLOriginMake(0,0,0), MTLSizeMake(small.width,small.height,1), sb, 0, srow, sbytes);
        mtl_sub_commit(); mtl_drain(); res_remove(sb); mtl_sub_begin();
        const uint64_t *sp=sb.contents, clear=0x3c00340038003c00ull; NSUInteger sbad=0;
        for (NSUInteger i=0;i<sbytes/8;i++) if (sp[i]!=clear) sbad++;
        CHECK(!sbad,"cross-size copy leaves the live owner's compressed pixels intact");
        mtl_pool_del(rb); free(rb);
        // 2. Bindless pool gating: small is live, big is dummy until big becomes valid.
        mtl_reg_tex(101, &a[1], NULL); mtl_reg_tex(102, &a[0], NULL);
        int g0=0, l0=0, lr0=0, tr0=0, d0=0;
        os_unfair_lock_lock(&g_rmu);
        uint64_t rid_s=pool_entry(0, MTLTextureType2D, 101, UINT64_MAX, &g0, &l0, &lr0, &tr0, &d0, NULL);
        uint64_t rid_b=pool_entry(0, MTLTextureType2D, 102, UINT64_MAX, &g0, &l0, &lr0, &tr0, &d0, NULL);
        os_unfair_lock_unlock(&g_rmu);
        CHECK(rid_s!=0 && rid_b==0,"bindless pool exposes the valid family size and masks other sizes as dummy");
        // 3. Cross-size render-pass Load (ASTRAL CHAIN #1935): load big with no full clear, write only a 16x16 scissor corner.
        // The cross-size load is rewritten to Clear(0), so every untouched pixel is exact 0.0 and none is non-finite or garbage.
        uint64_t sg_before=atomic_load(&g_ser_gen);
        state_reset(); memset(&R,0,sizeof R); R.has_rt=1; R.rt.n=1; R.rt.color[0]=&a[0];
        struct { op_hdr h1; p_scissor sc; op_hdr h2; p_clear_color clear; op_hdr end; } sc_ops={0};
        sc_ops.h1=(op_hdr){OP_SCISSOR,sizeof(op_hdr)+sizeof(p_scissor)};
        sc_ops.sc=(p_scissor){0,0,16,16};
        sc_ops.h2=(op_hdr){OP_CLEAR_COLOR,sizeof(op_hdr)+sizeof(p_clear_color)};
        sc_ops.clear=(p_clear_color){.index=0,.mask=15,.rgba={0.25f,0.5f,1,1}};
        sc_ops.end=(op_hdr){OP_END,sizeof(op_hdr)};
        mtl_exec((const uint8_t *)&sc_ops); mtl_sub_commit(); mtl_drain(); mtl_sub_begin(); memset(&R,0,sizeof R);
        CHECK(fam_is_valid(big) && !fam_is_valid(small),"stored cross-size render pass transfers compressed family ownership to big");
        CHECK(atomic_load(&g_ser_gen)>sg_before,"size switch bumps serial generation for pool-tracked family members");
        os_unfair_lock_lock(&g_rmu);
        uint64_t rid_s2=pool_entry(0, MTLTextureType2D, 101, UINT64_MAX, &g0, &l0, &lr0, &tr0, &d0, NULL);
        uint64_t rid_b2=pool_entry(0, MTLTextureType2D, 102, UINT64_MAX, &g0, &l0, &lr0, &tr0, &d0, NULL);
        os_unfair_lock_unlock(&g_rmu);
        CHECK(rid_s2==0 && rid_b2!=0,"bindless pool updates to expose big and mask small after size switch");
        NSUInteger row=big.width*8;
        id<MTLBuffer> b=[g_dev newBufferWithLength:bytes options:MTLResourceStorageModeShared]; res_add(b);
        enc_b_copy_texture_buffer(blit(), big, 0, 0, MTLOriginMake(0,0,0), MTLSizeMake(big.width,big.height,1), b, 0, row, bytes);
        mtl_sub_commit(); mtl_drain(); res_remove(b); mtl_sub_begin();
        const uint64_t *p=b.contents, corner=0x3c003c0038003400ull; NSUInteger ncorner=0, nzero=0, other=0;
        for (NSUInteger y=0;y<big.height;y++) for (NSUInteger x=0;x<big.width;x++) {
            uint64_t v=p[y*big.width+x];
            if (x<16 && y<16) { if (v==corner) ncorner++; else other++; }
            else { if (!v) nzero++; else other++; }
        }
        printf("cross-size pass load (compressed): %lu corner, %lu zero, %lu other texels of %lu (compression %d)\n",(unsigned long)ncorner,(unsigned long)nzero,(unsigned long)other,(unsigned long)(bytes/8),(int)big.allowGPUOptimizedContents);
        CHECK(!other && ncorner==256 && nzero==big.width*big.height-256,"cross-size render-pass load clears untouched pixels to 0 without compressed-block garbage");
        // 4. Combined Depth32Float_Stencil8 half-clear across sizes: clear both on 32x32, then clear ONLY depth on 64x64; stencil must load as clean 0 and store valid blocks.
        tex_t ds[2];
        for (int i=0;i<2;i++) { ds[i]=image(&pool,993100+i,32u<<i,32u<<i); ds[i].off=12<<20; ds[i].mem=pool.mem+ds[i].off;
            ds[i].d.format=NVN_FORMAT_DEPTH32F_STENCIL8; nvn_tex_layout(&ds[i].d,&ds[i].L); mtl_tex_init(&ds[i]); }
        id<MTLTexture> ds0=tex_mtl(&ds[0]), ds1=tex_mtl(&ds[1]);
        state_reset(); memset(&R,0,sizeof R); R.has_rt=1; R.rt.depth=&ds[0];
        struct { op_hdr h; p_clear_ds clear; op_hdr end; } ds_ops={0};
        ds_ops.h=(op_hdr){OP_CLEAR_DS,sizeof(op_hdr)+sizeof(p_clear_ds)};
        ds_ops.clear=(p_clear_ds){.depth=0.5f,.depth_mask=1,.stencil=0x77,.stencil_mask=0xff}; ds_ops.end=(op_hdr){OP_END,sizeof(op_hdr)};
        mtl_exec((const uint8_t *)&ds_ops); mtl_sub_commit(); mtl_drain(); mtl_sub_begin();
        state_reset(); memset(&R,0,sizeof R); R.has_rt=1; R.rt.depth=&ds[1];
        ds_ops.clear=(p_clear_ds){.depth=0.25f,.depth_mask=1,.stencil=0,.stencil_mask=0};   // depth clear only, stencil loaded across sizes
        mtl_exec((const uint8_t *)&ds_ops); mtl_sub_commit(); mtl_drain(); mtl_sub_begin(); memset(&R,0,sizeof R);
        CHECK(fam_is_valid(ds1) && ds0.heap==ds1.heap,"combined depth-stencil half-clear transfers ownership to 64x64");
        id<MTLBuffer> dbuf=[g_dev newBufferWithLength:ds1.width*ds1.height*4 options:MTLResourceStorageModeShared];
        id<MTLBuffer> sbuf=[g_dev newBufferWithLength:ds1.width*ds1.height options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cb=q_cb(); id<MTLBlitCommandEncoder> be=[cb blitCommandEncoder];
        [be copyFromTexture:ds1 sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(ds1.width,ds1.height,1) toBuffer:dbuf destinationOffset:0 destinationBytesPerRow:ds1.width*4 destinationBytesPerImage:ds1.width*ds1.height*4 options:MTLBlitOptionDepthFromDepthStencil];
        [be copyFromTexture:ds1 sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(ds1.width,ds1.height,1) toBuffer:sbuf destinationOffset:0 destinationBytesPerRow:ds1.width destinationBytesPerImage:ds1.width*ds1.height options:MTLBlitOptionStencilFromDepthStencil];
        [be endEncoding]; [cb commit]; [cb waitUntilCompleted];
        const uint32_t *dp=dbuf.contents; const uint8_t *stp=sbuf.contents; int ds_ok=cb.status==MTLCommandBufferStatusCompleted;
        for (NSUInteger i=0;i<ds1.width*ds1.height;i++) ds_ok &= dp[i]==0x3e800000u && stp[i]==0;
        CHECK(ds_ok,"combined depth-stencil half-clear stores clean 0 stencil and cleared depth");
        mtl_reg_tex_gone(101); mtl_reg_tex_gone(102);
        for (int i=0;i<2;i++) mtl_tex_release(&ds[i]);
    }
    for (int i=0;i<2;i++) mtl_tex_release(&a[i]);
    mtl_alias_drop_pool(&pool); mtl_pool_del(pool.mem); free(mem);
}
// A pool ID re-registered repeatedly with a new texture over one family's storage (each re-initialization gets a new serial), and a transient burst of live IDs later unregistered:
// fam_track_pool_serial drops unregistered serials on each new-serial miss so the list keeps only live pool serials and fam_set_valid does not flood own_note past SER_NOTES.
static void pool_serials(void) {
    if (fam_nocomp()) return;
    void *mem=NULL; posix_memalign(&mem,16384,32<<20); memset(mem,0,32<<20);
    pool_t pool={.magic=MAGIC('P','O','O','L'), .flags=NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS|NVN_MEMORY_POOL_FLAGS_GPU_CACHED|NVN_MEMORY_POOL_FLAGS_COMPRESSIBLE, .mem=mem, .size=16<<20};
    mtl_pool_add(pool.mem, pool.size);
    enum { ROUNDS=1000, ID=201, BURST=16, BURST_ID=210 };
    tex_t t[2], big; int cur=0; uint64_t wh=0, live=0;
    big=image(&pool,995999,1600,900); big.d.format=NVN_FORMAT_RGBA16F; big.d.flags=NVN_TEXTURE_FLAGS_COMPRESSIBLE; nvn_tex_layout(&big.d,&big.L); mtl_tex_init(&big);   // the target's other size, live throughout
    NvImageHeap *o=nil;
    for (int i=0;i<ROUNDS;i++) { @autoreleasepool {
        tex_t *n=&t[cur]; *n=image(&pool,996000+i,1440,810); n->d.format=NVN_FORMAT_RGBA16F; n->d.flags=NVN_TEXTURE_FLAGS_COMPRESSIBLE; nvn_tex_layout(&n->d,&n->L); mtl_tex_init(n);
        if (!i) { o=fam_owner(tex_mtl(&big),NULL); CHECK(o && o->compressed,"the target's two sizes form a compressed family"); }
        NvImageHeap *no=fam_owner(tex_mtl(n),&wh);
        CHECK(no && no==o,"each texture over the storage joins that family");
        mtl_reg_tex(ID,n,NULL); live=n->serial;
        int g=0,l=0,lr=0,tr=0,d=0;
        os_unfair_lock_lock(&g_rmu); (void)pool_entry(0,MTLTextureType2D,ID,UINT64_MAX,&g,&l,&lr,&tr,&d,NULL); os_unfair_lock_unlock(&g_rmu);
        if (i) mtl_tex_release(&t[!cur]);   // the texture it replaced ends
        cur=!cur;
    } }
    if (o) {
        os_unfair_lock_lock(&o->pool_lk); uint32_t n=o->npool_sers; int has=n==1 && o->pool_sers[0].serial==live; os_unfair_lock_unlock(&o->pool_lk);
        printf("family pool serials after %d registrations of one ID: %u\n",ROUNDS,n);
        CHECK(n==1,"the family keeps only the live registered serial, not obsolete serials from re-registrations");
        CHECK(has,"it keeps the serial the ID holds now");
        uint64_t sg=atomic_load(&g_ser_gen);
        fam_set_valid(o,wh); fam_set_valid(o,0);   // a size switch to the ID's size and away: each notes only the live serial
        uint64_t notes=atomic_load(&g_ser_gen)-sg;
        CHECK(notes==2 && notes<SER_NOTES,"a size round trip notes only the live serial (2 notes), not obsolete serials");
        // Grow capacity via a burst of live IDs, unregister them all, then insert one new serial: all burst serials must drop immediately without waiting to refill capacity.
        tex_t burst[BURST];
        for (int i=0;i<BURST;i++) {
            burst[i]=image(&pool,998000+i,1440,810); burst[i].d.format=NVN_FORMAT_RGBA16F; burst[i].d.flags=NVN_TEXTURE_FLAGS_COMPRESSIBLE; nvn_tex_layout(&burst[i].d,&burst[i].L); mtl_tex_init(&burst[i]);
            CHECK(fam_owner(tex_mtl(&burst[i]),NULL)==o,"burst texture joins the compressed family");
            mtl_reg_tex(BURST_ID+i,&burst[i],NULL);
            int g=0,l=0,lr=0,tr=0,d=0;
            os_unfair_lock_lock(&g_rmu); (void)pool_entry(0,MTLTextureType2D,BURST_ID+i,UINT64_MAX,&g,&l,&lr,&tr,&d,NULL); os_unfair_lock_unlock(&g_rmu);
        }
        os_unfair_lock_lock(&o->pool_lk); uint32_t nb=o->npool_sers, capb=o->cappool_sers; os_unfair_lock_unlock(&o->pool_lk);
        CHECK(nb==1+BURST && capb>=1+BURST,"live burst serials grow the family list while registered");
        for (int i=0;i<BURST;i++) { mtl_reg_tex(BURST_ID+i,NULL,NULL); mtl_tex_release(&burst[i]); }
        // Even before another fam_track_pool_serial miss, fam_set_valid must prune the unregistered burst serials and emit notes only for the live serial.
        sg=atomic_load(&g_ser_gen);
        fam_set_valid(o,wh); fam_set_valid(o,0);
        uint64_t notes_b=atomic_load(&g_ser_gen)-sg;
        os_unfair_lock_lock(&o->pool_lk); uint32_t nb2=o->npool_sers; int hasb2=nb2==1 && o->pool_sers[0].serial==live; os_unfair_lock_unlock(&o->pool_lk);
        CHECK(nb2==1 && hasb2 && notes_b==2,"size round trip immediately after unregistering a burst prunes dead serials and emits only 2 notes");
        // Re-create a burst, unregister it, then insert one new serial: fam_track_pool_serial must also drop all unregistered burst serials immediately.
        for (int i=0;i<BURST;i++) {
            burst[i]=image(&pool,998500+i,1440,810); burst[i].d.format=NVN_FORMAT_RGBA16F; burst[i].d.flags=NVN_TEXTURE_FLAGS_COMPRESSIBLE; nvn_tex_layout(&burst[i].d,&burst[i].L); mtl_tex_init(&burst[i]);
            CHECK(fam_owner(tex_mtl(&burst[i]),NULL)==o,"second burst texture joins the compressed family");
            mtl_reg_tex(BURST_ID+i,&burst[i],NULL);
            int g=0,l=0,lr=0,tr=0,d=0;
            os_unfair_lock_lock(&g_rmu); (void)pool_entry(0,MTLTextureType2D,BURST_ID+i,UINT64_MAX,&g,&l,&lr,&tr,&d,NULL); os_unfair_lock_unlock(&g_rmu);
        }
        for (int i=0;i<BURST;i++) { mtl_reg_tex(BURST_ID+i,NULL,NULL); mtl_tex_release(&burst[i]); }
        tex_t *after=&t[cur]; *after=image(&pool,999000,1440,810); after->d.format=NVN_FORMAT_RGBA16F; after->d.flags=NVN_TEXTURE_FLAGS_COMPRESSIBLE; nvn_tex_layout(&after->d,&after->L); mtl_tex_init(after);
        CHECK(fam_owner(tex_mtl(after),NULL)==o,"post-burst texture joins the compressed family");
        mtl_reg_tex(ID,after,NULL); live=after->serial;
        int g=0,l=0,lr=0,tr=0,d=0;
        os_unfair_lock_lock(&g_rmu); (void)pool_entry(0,MTLTextureType2D,ID,UINT64_MAX,&g,&l,&lr,&tr,&d,NULL); os_unfair_lock_unlock(&g_rmu);
        mtl_tex_release(&t[!cur]); cur=!cur;
        os_unfair_lock_lock(&o->pool_lk); uint32_t n2=o->npool_sers; int has2=n2==1 && o->pool_sers[0].serial==live; os_unfair_lock_unlock(&o->pool_lk);
        CHECK(n2==1 && has2,"a new serial after a transient burst prunes all unregistered burst serials immediately");
        sg=atomic_load(&g_ser_gen);
        fam_set_valid(o,wh); fam_set_valid(o,0);
        uint64_t notes2=atomic_load(&g_ser_gen)-sg;
        CHECK(notes2==2,"size round trip after post-burst prune emits only 2 notes");
    }
    mtl_reg_tex_gone(ID); mtl_tex_release(&t[!cur]); mtl_tex_release(&big);
    mtl_alias_drop_pool(&pool); mtl_pool_del(pool.mem); free(mem);
}
int main(void) { @autoreleasepool {
    mtl_init(); __weak id<MTLHeap> weak_heap; void *mem=NULL;
    posix_memalign(&mem,16384,16<<20); memset(mem,0,16<<20);
    @autoreleasepool {
    mtl_sub_begin();
    pool_t pool={.magic=MAGIC('P','O','O','L'), .flags=NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS|NVN_MEMORY_POOL_FLAGS_GPU_CACHED|NVN_MEMORY_POOL_FLAGS_COMPRESSIBLE, .mem=mem, .size=16<<20};
    enum { N=128 }; tex_t a[N];
    for (int i=0;i<N;i++) { a[i]=image(&pool, 990000+i, 512+i*4, 512); mtl_tex_init(&a[i]); }
    uint64_t before=g_dev.currentAllocatedSize;
    id<MTLTexture> small=tex_mtl(&a[0]);
    CHECK(small.width==512 && small.height==512,"the first descriptor retains its NVN dimensions");
    CHECK(small.heap!=nil,"live resolution family has physical backing");
    CHECK(!(small.usage&MTLTextureUsagePixelFormatView),"a family texture gets no forced format-view usage");
    CHECK(small.allowGPUOptimizedContents == !fam_nocomp(),"family textures keep lossless compression by default (DM_NVN_FAMILY_NOCOMPRESS=1 disables it)");
    MTLTextureDescriptor *largest=tex_desc(&a[N-1],0); largest.allowGPUOptimizedContents=!fam_nocomp();   // family members are sized with the same compression mode they are made with (image_texture)
    MTLSizeAndAlign max_size=[g_dev heapTextureSizeAndAlignWithDescriptor:largest];
    CHECK(small.heap.size>=max_size.size && small.heap.size<=(sw_on(rh_mode()) ? max_size.size+max_size.size/4 : max_size.size),"heap capacity holds the largest declared size (with the recycled heaps' eighth of headroom)");
    is_zero(small);
    id<MTLTexture> big=tex_mtl(&a[N-1]); is_zero(big);
    pixels(small,0x10203040);
    for (int i=1;i<N;i++) {
        id<MTLTexture> t=tex_mtl(&a[i]);
        CHECK(t.heap==small.heap,"all live descriptors share one allocation");
        CHECK(t!=small && t.width==512u+i*4 && t.height==512,"each descriptor remains a distinct texture with its own dimensions");
    }
    // Descriptor creation must not enqueue an upload-batch zero over an earlier writer.
    id<MTLBuffer> saved=read_image(small); uint32_t *p=saved.contents; int intact=1;
    for (NSUInteger i=0;i<saved.length/4;i++) intact &= p[i]==(0x10203040u^(uint32_t)i);
    CHECK(intact,"creating aliases preserves an earlier writer");
    uint64_t delta=g_dev.currentAllocatedSize-before;
    printf("128 live image descriptors: device allocation delta %llu bytes, heap %lu bytes\n",(unsigned long long)delta,(unsigned long)small.heap.size);
    CHECK(delta<max_size.size+(12<<20),"allocation is bounded by one image plus upload/readback scratch");
    pixels(big,0x55667788); pixels(tex_mtl(&a[63]),0xa1b2c3d4); pixels(small,0x76543210);
    separate_storage(&pool,small.heap);
    same_cell(&pool);
    fragment_copy(small,0x76543210);
    indirect_pixels(small,0x76543210);   // writes to other pools, offsets and formats left it intact
    atomic_colour(&pool);
    depth_planes(&pool);
    cross_size_read();
    pool_serials();
    CHECK([g_res_objects containsObject:small.heap],"the parent heap is resident for indirect access");
    CHECK([g_res_objects containsObject:small],"the view parent descriptor is resident alongside its heap");
    // Different offsets, formats, CPU-visible and sparse flags cannot join this family.
    tex_t other=image(&pool,991000,64,64); other.off=10<<20; other.mem=pool.mem+other.off; mtl_tex_init(&other);
    CHECK(!objc_getAssociatedObject(tex_mtl(&other),&g_image_heap_owner_key),"single-description storage stays out of the family heaps (its memory is a recycled heap of its own, rh_texture)");
    tex_t sparse=image(&pool,991001,128,64); sparse.d.flags|=NVN_TEXTURE_FLAGS_SPARSE; mtl_tex_init(&sparse);
    CHECK(!tex_mtl(&sparse).heap,"sparse images stay outside physical image sharing");
    // A dynamic-resolution depth buffer has no image flag (Ocarina of Time: COMPRESSIBLE alone); its sizes share one allocation. A linear image keeps its own
    tex_t dz[3]; for (int i=0;i<3;i++) { dz[i]=image(&pool,992000+i,1882+i*19,1882+i*19); dz[i].off=12<<20; dz[i].mem=pool.mem+dz[i].off; dz[i].d.format=NVN_FORMAT_DEPTH32F;
        dz[i].d.flags=NVN_TEXTURE_FLAGS_COMPRESSIBLE; nvn_tex_layout(&dz[i].d,&dz[i].L); mtl_tex_init(&dz[i]); }
    id<MTLTexture> dz0=tex_mtl(&dz[0]), dz2=tex_mtl(&dz[2]);
    CHECK(dz0.heap!=nil && dz0.heap==dz2.heap,"depth sizes without the image flag share one allocation");
    CHECK(dz0.width==1882 && dz2.width==1920,"each depth size keeps its own dimensions");
    tex_t lin=image(&pool,992010,128,64); lin.off=14<<20; lin.mem=pool.mem+lin.off; lin.d.flags|=NVN_TEXTURE_FLAGS_LINEAR; mtl_tex_init(&lin);
    tex_t lin2=lin; lin2.serial=992011; lin2.d.width=256; mtl_tex_init(&lin2);
    CHECK(!tex_mtl(&lin).heap,"linear images stay outside physical image sharing");
    weak_heap=small.heap;
    for (int i=0;i<N;i++) mtl_tex_release(&a[i]);
    mtl_tex_release(&other); mtl_tex_release(&sparse); for (int i=0;i<3;i++) mtl_tex_release(&dz[i]); mtl_tex_release(&lin); mtl_tex_release(&lin2); dz0=nil; dz2=nil; mtl_alias_drop_pool(&pool);
    small=nil; big=nil; saved=nil;
    mtl_sub_commit(); mtl_drain();
    // Finish a second submission: release calls are deferred to the next fence.
    mtl_sub_begin(); mtl_sub_commit(); mtl_drain();
    // GPU completion publishes the fence before its feedback handler drains deferred objects.
    dispatch_sync(g_feedback_q, ^{}); defer_drain(); res_commit();
    CHECK(g_image_desc.n==0,"finalization releases every recorded description");
    }
    res_commit();   // apply descriptor retirements queued while the fixture's autorelease pool drained
    if (sw_on(rh_mode())) {   // the heap waits, resident, for the next texture it fits (rh_wait)
        int waits=0; os_unfair_lock_lock(&g_rh_lk); for (unsigned i=0;i<g_rh_n;i++) waits |= (__bridge id)g_rh[i].heap==weak_heap; os_unfair_lock_unlock(&g_rh_lk);
        CHECK(weak_heap && waits && [g_res_objects containsObject:weak_heap],"the last texture hands its heap to the recycled heaps, resident while it waits");
    } else CHECK(!weak_heap || ![g_res_objects containsObject:weak_heap],"the last texture retires its heap from residency");
    CHECK(g_image_heap_n==0,"every family heap is released after its textures and views go");
    uint64_t settled=g_dev.currentAllocatedSize;
    for (int i=0;i<8;i++) { @autoreleasepool {
        pool_t pool={.magic=MAGIC('P','O','O','L'),.flags=NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS|NVN_MEMORY_POOL_FLAGS_GPU_CACHED|NVN_MEMORY_POOL_FLAGS_COMPRESSIBLE,.mem=mem,.size=16<<20};
        tex_t a=image(&pool,993000+i*2,512,512), b=image(&pool,993001+i*2,1024,512);
        mtl_tex_init(&a); mtl_tex_init(&b); mtl_sub_begin();
        CHECK(tex_mtl(&a).heap!=nil,"a recreated family allocates its backing");
        mtl_tex_release(&a); mtl_tex_release(&b); mtl_alias_drop_pool(&pool);
        mtl_sub_commit(); mtl_drain(); mtl_sub_begin(); mtl_sub_commit(); mtl_drain();
        dispatch_sync(g_feedback_q,^{}); defer_drain(); res_commit();
    } }
    res_commit();
    CHECK(g_image_heap_n==0,"repeated destruction leaves no heap owner");
    CHECK(g_dev.currentAllocatedSize<=settled+(3<<20),"repeated families do not retain their old physical allocations");
    free(mem);
    printf("image aliases: %d checks, %d failed\n",checks,failed); return failed!=0;
} }
