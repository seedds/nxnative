// The pulling vertex fetch (dm/gfx/tools/dmsh_pull.metal) against Metal's own: for every NVN vertex format the host knows (nvn_mtl.m vfmt), the words dm_fetch gives must be bit
// for bit the ones Metal's vertex-descriptor fetch gives an input of the format's class (float4, uint4 or int4); integer-read-as-float formats also check the CPU fallback.
// Production pso_desc / bind_vbufs / vconv_bind readback checks native i2f float inputs and integer shadows, mixed native / CPU slots, explicit fallbacks, ordinary integer register
// semantics, default padding, zero stride and divisors. Every byte value, half and float specials, snorm -128, 10:10:10:2 extremes, 8-bit attributes at aligned and
// at 2-byte offsets, a stride-1 stream; i2f signedness/rounding boundaries, unaligned CPU elements and zero stride against the independent GPU decoder; then the host's layout block.
#include "../nvn/nvn_mtl.m"
uint64_t g_t0_ns;
int dm_guest_as_contains(uintptr_t address) { (void)address; return 0; }
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define EXPECT(name, want, got) do { uint64_t w = (want), g = (got); checks++; if (w != g) { fprintf(stderr, "%s: expected %llu, got %llu\n", name, (unsigned long long)w, (unsigned long long)g); failed++; } } while (0)

typedef struct { uint64_t base; uint32_t stride, divisor; } dm_vstream_c;
typedef struct { dm_vstream_c s[16]; uint32_t attr[16]; } dm_vlayout_c;   // dmsh_pull.metal dm_vlayout
_Static_assert(sizeof(dm_vlayout_c) == 320, "dm_vlayout");
static id<MTLCommandQueue> q; static id<MTLTexture> rt; static id<MTLLibrary> lib;
static NSString *k_test = @"\n"
"vertex void pull_test(constant dm_vlayout &L [[buffer(16)]], device uint4 *out [[buffer(1)]], constant uint &mode [[buffer(2)]], uint vid [[vertex_id]], uint iid [[instance_id]], uint bi [[base_instance]]) { out[mode ? iid : vid] = dm_fetch(L, 0, vid, iid, bi); }\n"
"struct vinf { float4 a [[attribute(0)]]; }; struct vinu { uint4 a [[attribute(0)]]; }; struct vini { int4 a [[attribute(0)]]; };\n"
"vertex void vd_f(vinf in [[stage_in]], device uint4 *out [[buffer(1)]], constant uint &mode [[buffer(2)]], uint vid [[vertex_id]], uint iid [[instance_id]]) { out[mode ? iid : vid] = as_type<uint4>(in.a); }\n"
"vertex void vd_u(vinu in [[stage_in]], device uint4 *out [[buffer(1)]], constant uint &mode [[buffer(2)]], uint vid [[vertex_id]], uint iid [[instance_id]]) { out[mode ? iid : vid] = in.a; }\n"
"vertex void vd_i(vini in [[stage_in]], device uint4 *out [[buffer(1)]], constant uint &mode [[buffer(2)]], uint vid [[vertex_id]], uint iid [[instance_id]]) { out[mode ? iid : vid] = as_type<uint4>(in.a); }\n";

static id<MTLRenderPipelineState> rp(NSString *fn, MTLVertexDescriptor *vd) {
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new]; d.vertexFunction = [lib newFunctionWithName:fn]; d.rasterizationEnabled = NO; d.vertexDescriptor = vd;
    d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    NSError *e = nil; id<MTLRenderPipelineState> ps = [g_dev newRenderPipelineStateWithDescriptor:d error:&e];
    if (!ps) { fprintf(stderr, "pipeline %s: %s\n", fn.UTF8String, e.localizedDescription.UTF8String); abort(); }
    return ps;
}
static void run(id<MTLRenderPipelineState> ps, id<MTLBuffer> vb, int vd_slot, id<MTLBuffer> lay, id<MTLBuffer> out, uint32_t mode, int nvert, int ninst, int binst) {
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor new]; p.colorAttachments[0].texture = rt; p.colorAttachments[0].loadAction = MTLLoadActionDontCare; p.colorAttachments[0].storeAction = MTLStoreActionDontCare;
    id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:p];
    [e setRenderPipelineState:ps];
    if (vd_slot >= 0) [e setVertexBuffer:vb offset:0 atIndex:(NSUInteger)vd_slot];
    if (lay) { [e setVertexBuffer:lay offset:0 atIndex:16]; [e useResource:vb usage:MTLResourceUsageRead stages:MTLRenderStageVertex]; }
    [e setVertexBuffer:out offset:0 atIndex:1]; [e setVertexBytes:&mode length:4 atIndex:2];
    [e drawPrimitives:MTLPrimitiveTypePoint vertexStart:0 vertexCount:(NSUInteger)nvert instanceCount:(NSUInteger)ninst baseInstance:(NSUInteger)binst];
    [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
    EXPECT("command buffer without error", 0, cb.error != nil);
}
static int int_class(MTLVertexFormat f) {   // 1 unsigned, 2 signed integer input; 0 float
    switch (f) {
        case MTLVertexFormatUChar: case MTLVertexFormatUChar2: case MTLVertexFormatUChar3: case MTLVertexFormatUChar4: case MTLVertexFormatUShort: case MTLVertexFormatUShort2:
        case MTLVertexFormatUShort3: case MTLVertexFormatUShort4: case MTLVertexFormatUInt: case MTLVertexFormatUInt2: case MTLVertexFormatUInt3: case MTLVertexFormatUInt4: return 1;
        case MTLVertexFormatChar: case MTLVertexFormatChar2: case MTLVertexFormatChar3: case MTLVertexFormatChar4: case MTLVertexFormatShort: case MTLVertexFormatShort2:
        case MTLVertexFormatShort3: case MTLVertexFormatShort4: case MTLVertexFormatInt: case MTLVertexFormatInt2: case MTLVertexFormatInt3: case MTLVertexFormatInt4: return 2;
        default: return 0;
    }
}
static int elem_size(int f) {   // bytes of one element of NVN format f
    int ib, is, in;
    if ((in = vconv_i2f(f, &ib, &is)) || (in = vconv_int(f, &ib, &is))) return in * ib / 8;
    switch (vfmt(f)) {
        case MTLVertexFormatUCharNormalized: case MTLVertexFormatCharNormalized: return 1;
        case MTLVertexFormatHalf: case MTLVertexFormatUShortNormalized: case MTLVertexFormatShortNormalized: case MTLVertexFormatUChar2Normalized: case MTLVertexFormatChar2Normalized: return 2;
        case MTLVertexFormatUChar3Normalized: case MTLVertexFormatChar3Normalized: return 3;
        case MTLVertexFormatHalf3: case MTLVertexFormatUShort3Normalized: case MTLVertexFormatShort3Normalized: return 6;
        case MTLVertexFormatHalf4: case MTLVertexFormatUShort4Normalized: case MTLVertexFormatShort4Normalized: case MTLVertexFormatFloat2: case MTLVertexFormatUInt2: case MTLVertexFormatInt2: return 8;
        case MTLVertexFormatFloat3: case MTLVertexFormatUInt3: case MTLVertexFormatInt3: return 12;
        case MTLVertexFormatFloat4: case MTLVertexFormatUInt4: case MTLVertexFormatInt4: return 16;
        default: return 4;
    }
}
enum { ROUTE_NV = 32 };
static id<MTLLibrary> route_lib;
static uint8_t *route_mem;
static id<MTLBuffer> route_out;

static void route_init(void) {
    NSMutableString *msl = [NSMutableString stringWithString:@"#include <metal_stdlib>\nusing namespace metal;\n"];
    const char *types[] = { "float4", "uint4", "int4" };
    for (int ct = 0; ct < 3; ct++) {
        [msl appendFormat:@"struct r%d { %s a [[attribute(0)]]; %s b [[attribute(1)]]; };\n"
            "vertex void r%d_base(r%d in [[stage_in]], device uint4 *out [[buffer(13)]], uint v [[vertex_id]], uint i [[instance_id]])"
            "{ out[2*(i*%d+v)] = as_type<uint4>(in.a); out[2*(i*%d+v)+1] = as_type<uint4>(in.b); }\n",
            ct, types[ct], types[ct], ct, ct, ROUTE_NV, ROUTE_NV];
        if (ct == NVN_CT_FLOAT) continue;
        const char *opp = types[ct == NVN_CT_UINT ? NVN_CT_INT : NVN_CT_UINT];
        [msl appendFormat:@"struct s%d { %s a [[attribute(0)]]; %s b [[attribute(1)]];"
            "float4 af [[attribute(16)]]; %s ao [[attribute(17)]]; float4 bf [[attribute(18)]]; %s bo [[attribute(19)]]; };\n"
            "vertex void r%d_shadow(s%d in [[stage_in]], device uint4 *out [[buffer(13)]], uint v [[vertex_id]], uint i [[instance_id]])"
            "{ out[2*(i*%d+v)] = as_type<uint4>(in.a) | as_type<uint4>(in.af) | as_type<uint4>(in.ao);"
            "out[2*(i*%d+v)+1] = as_type<uint4>(in.b) | as_type<uint4>(in.bf) | as_type<uint4>(in.bo); }\n",
            ct, types[ct], types[ct], opp, opp, ct, ct, ROUTE_NV, ROUTE_NV];
    }
    NSError *error = nil; route_lib = [g_dev newLibraryWithSource:msl options:nil error:&error];
    if (!route_lib) { fprintf(stderr, "routing shader: %s\n", error.localizedDescription.UTF8String); abort(); }
    size_t pg = (size_t)getpagesize();
    if (posix_memalign((void **)&route_mem, pg, pg * 2)) abort();
    memset(route_mem, 0, pg * 2); mtl_pool_add(route_mem, pg * 2);
    route_out = [g_dev newBufferWithLength:ROUTE_NV * 12 * 32 options:MTLResourceStorageModeShared]; res_add(route_out);
}

// Independent scalar register reference: numeric floats only for i2f, integer words (including w=1) for ordinary integer formats.
static void route_ref(int f, const uint8_t *p, uint32_t out[4]) {
    int bits, sign, n = vconv_i2f(f, &bits, &sign), numeric = n != 0;
    if (!n) n = vconv_int(f, &bits, &sign);
    out[0] = out[1] = out[2] = 0; out[3] = numeric ? 0x3f800000 : 1;
    for (int c = 0; c < n; c++) {
        uint32_t u = 0; memcpy(&u, p + c * bits / 8, (size_t)bits / 8);
        int32_t s = bits == 8 ? (int8_t)u : bits == 16 ? (int16_t)u : (int32_t)u;
        if (numeric) { float v = sign ? (float)s : (float)u; memcpy(out + c, &v, 4); }
        else out[c] = sign ? (uint32_t)s : u;
    }
}

// fallback: 0 usable shadows, 1 absent metadata, 2 rejected library, 3 missing float shadow, 4/5/6 capture/geometry/discard, 7 missing library on disk.
static void route_case(int f, int ct, int step, int fallback) {
    char label[128]; snprintf(label, sizeof label, "route format %#x ct %d step %d fallback %d", f, ct, step, fallback);
    int bits, sign, i2f = vconv_i2f(f, &bits, &sign) != 0;
    const char *cpu_env = getenv("DM_NVN_VCONV_CPU");
    int cpu = cpu_env && !strcmp(cpu_env, "1");
    int special = fallback >= 4 && fallback <= 6;
    int native = i2f && bits < 32 && !cpu && !special && (ct == NVN_CT_FLOAT || fallback == 0);
    NvShader *vs = [NvShader new];
    vs->fn = [route_lib newFunctionWithName:[NSString stringWithFormat:@"r%d_base", ct]];
    vs->tf_fn = vs->geo_fn = vs->discard_fn = vs->fn;
    vs->rf.nv = 2; vs->rf.has_vin = ct != NVN_CT_FLOAT && fallback != 1;
    for (int a = 0; a < 2; a++) {
        vs->rf.v[a].nvn = vs->rf.v[a].loc = (uint8_t)a; vs->rf.v[a].ct = (uint8_t)ct;
        memset(vs->rf.v[a].sh, 0xff, sizeof vs->rf.v[a].sh);
        if (ct != NVN_CT_FLOAT) {
            if (fallback != 3) vs->rf.v[a].sh[NVN_CT_FLOAT] = (uint8_t)(16 + 2 * a);
            vs->rf.v[a].sh[ct == NVN_CT_UINT ? NVN_CT_INT : NVN_CT_UINT] = (uint8_t)(17 + 2 * a);
        }
    }
    vs->vfn_state = fallback == 2 ? 2 : fallback == 7 ? 0 : 1;
    if (vs->vfn_state == 1 && ct != NVN_CT_FLOAT) vs->vfn = [route_lib newFunctionWithName:[NSString stringWithFormat:@"r%d_shadow", ct]];
    if (fallback == 7) { strcpy(vs->key, "missing_i2f_test_z"); vs->ventry = @"main0_z"; }
    state_reset(); S.nattr = 2;
    const int formats[2] = { f, 0x68 };   // second attribute always needs CPU conversion: RGBA32_I2F, including integer-to-float rounding boundaries
    static const uint32_t edges[] = { 0, 1, 127, 128, 255, 32767, 32768, 65535, 0xffffff, 0x1000001, 0x1000003, 0x7fffffff, 0x80000000, 0xffffffff };
    for (int a = 0; a < 2; a++) {
        uint8_t *src = route_mem + 64 + a * 1024;
        int b, s, n = vconv_i2f(formats[a], &b, &s); if (!n) n = vconv_int(formats[a], &b, &s);
        for (int v = 0; v < ROUTE_NV; v++) for (int c = 0; c < n; c++) {
            uint32_t u = edges[(v + 3 * c) % (sizeof edges / sizeof *edges)];
            memcpy(src + 20 * v + 4 + c * b / 8, &u, (size_t)b / 8);
        }
        S.attr[a].fmt = (uint32_t)formats[a]; S.attr[a].offset = 4; S.attr[a].stream = (uint32_t)a;
        S.stream[a].stride = step == 1 ? 0 : 20; S.stream[a].divisor = step == 2 ? 3 : 0;
        S.vbuf[a].address = (uint64_t)(uintptr_t)src; S.vbuf[a].size = ROUTE_NV * 20;
    }
    pso_key key; uint32_t cf[8] = {0}; pso_key_fill(&key, vs, nil, cf, 0, 0, 1, 0);
    key.fsfn = fallback == 4 ? TF_MARK : fallback == 5 ? GEO_MARK : fallback == 6 ? DISCARD_MARK : NULL;
    int on = vin_on(vs, &key, special);
    EXPECT("key and draw choose identical routes", on, vin_on(vs, NULL, special));
    MTLRenderPipelineDescriptor *d = pso_desc(&key, vs, nil, nil, 0); d.rasterizationEnabled = NO;
    // Observable routing, not just equal conversion output: old unconditional CPU routing must fail these checks for native cases.
    int at = native && ct != NVN_CT_FLOAT ? 16 : 0;
    int conv0 = !native;
    if (!i2f) conv0 = ct == NVN_CT_FLOAT;   // ordinary unsigned input below uses a float input: preserve raw register words on the CPU
    EXPECT(label, conv0 ? VCONV_SLOT0 : VB_SLOT(0), d.vertexDescriptor.attributes[at].bufferIndex);
    EXPECT("mixed CPU slot follows only converted attributes", VCONV_SLOT0 + conv0, d.vertexDescriptor.attributes[1].bufferIndex);
    EXPECT("native route keeps raw format", native ? vfmt(f) : ct == NVN_CT_FLOAT ? MTLVertexFormatFloat4 : ct == NVN_CT_UINT ? MTLVertexFormatUInt4 : MTLVertexFormatInt4,
        d.vertexDescriptor.attributes[at].format);
    EXPECT("shadow entry selected only when usable and needed", native && ct != NVN_CT_FLOAT, d.vertexFunction == vs->vfn);
    for (int a = 0; a < 2; a++) EXPECT("descriptor and bind conversion eligibility agree", a ? 1 : conv0, vconv_wanted(&vs->rf, a, NULL, on));
    NSError *error = nil; id<MTLRenderPipelineState> ps = [g_dev newRenderPipelineStateWithDescriptor:d error:&error];
    EXPECT(label, 1, ps != nil);
    if (!ps) { fprintf(stderr, "%s: %s\n", label, error.localizedDescription.UTF8String); return; }
    p_draw dw = {0}; dw.count = step == 2 ? 1 : ROUTE_NV; dw.instances = step == 2 ? 7 : 1; dw.base_instance = step == 2 ? 5 : 0;
    memset(route_out.contents, 0xcd, route_out.length);
    mtl_sub_begin(); vbuf_prepare(&vs->rf, &dw, NULL);
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    MTL4RenderPassDescriptor *rd = [MTL4RenderPassDescriptor new]; rd.renderTargetWidth = rd.renderTargetHeight = rd.defaultRasterSampleCount = 1;
    id<MTL4RenderCommandEncoder> en = enc_r_begin(rd); enc_r_pso(en, ps); bind_vbufs(en);
    uint64_t before = g_vc_conv; vconv_bind(en, vs, special, &dw, NULL);
    EXPECT("native attribute does not allocate / fill CPU conversion", step == 1 ? 1 + conv0 : ROUTE_NV * (1 + conv0), g_vc_conv - before);
    enc_r_vbuf(en, 13, route_out.gpuAddress); enc_r_draw_instanced(en, MTLPrimitiveTypePoint, 0, dw.count, dw.instances, dw.base_instance); enc_r_end(en);
    EXPECT("routing submission completed", 1, mtl_wait(mtl_sub_commit(), 5000000000ull));
    EXPECT("routing submission without GPU error", 0, mtl_last_commit_error());
    int bad = 0;
    for (int i = dw.base_instance; i < dw.base_instance + dw.instances; i++) for (int v = 0; v < dw.count; v++) for (int a = 0; a < 2; a++) {
        int idx = step == 1 ? 0 : step == 2 ? (i - dw.base_instance) / 3 + dw.base_instance : v;
        uint32_t want[4]; route_ref(formats[a], route_mem + 64 + a * 1024 + idx * 20 + 4, want);
        const uint32_t *got = (const uint32_t *)route_out.contents + 8 * (i * ROUTE_NV + v) + a * 4;
        for (int c = 0; c < 4; c++) if (got[c] != want[c]) {
            if (!bad++) fprintf(stderr, "%s input %d vertex %d instance %d component %d: got %08x expected %08x\n", label, a, v, i, c, got[c], want[c]);
        }
    }
    EXPECT(label, 0, bad);
}

static void routing(void) {
    route_init();
    for (int f = 0x51; f <= 0x68; f++) for (int ct = 0; ct < 3; ct++) for (int step = 0; step < 3; step++) route_case(f, ct, step, 0);
    for (int ct = 0; ct < 3; ct++) for (int fallback = 1; fallback <= 7; fallback++) {
        route_case(0x57, ct, 0, fallback); route_case(0x5a, ct, 1, fallback);
    }
    // Ordinary integer bytes into a float declaration must remain their integer register bits, never numerical i2f.
    const int ordinary[] = { 0x03, 0x04, 0x14, 0x15, 0x27, 0x2d };
    for (unsigned f = 0; f < sizeof ordinary / sizeof *ordinary; f++) route_case(ordinary[f], NVN_CT_FLOAT, 0, 0);
}

int main(void) { @autoreleasepool {
    mtl_init(); q = [g_dev newCommandQueue]; NSError *err = nil;
    NSString *fetch = [NSString stringWithContentsOfFile:@"../tools/dmsh_pull.metal" encoding:NSUTF8StringEncoding error:&err];
    if (!fetch) { fprintf(stderr, "dmsh_pull.metal: %s\n", err.localizedDescription.UTF8String); return 1; }
    MTLCompileOptions *o = [MTLCompileOptions new]; o.preserveInvariance = YES;   // as the chain compiles (fast math on)
    lib = [g_dev newLibraryWithSource:[NSString stringWithFormat:@"#include <metal_stdlib>\nusing namespace metal;\n%@\n%@", fetch, k_test] options:o error:&err];
    if (!lib) { fprintf(stderr, "%s\n", err.localizedDescription.UTF8String); return 1; }
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4 height:4 mipmapped:NO]; td.usage = MTLTextureUsageRenderTarget; rt = [g_dev newTextureWithDescriptor:td];
    enum { NV = 512 };
    id<MTLBuffer> out = [g_dev newBufferWithLength:NV * 16 options:MTLResourceStorageModeShared], lay = [g_dev newBufferWithLength:sizeof(dm_vlayout_c) options:MTLResourceStorageModeShared];
    id<MTLRenderPipelineState> pull = rp(@"pull_test", nil);
    srand(12345);
    int nfmt = 0, nvd = 0, ncpu = 0;
    for (int f = 1; f < 256; f++) {
        int ib, is, i2f = vconv_i2f(f, &ib, &is) != 0;
        MTLVertexFormat mf = vfmt(f);
        if (mf == MTLVertexFormatInvalid) continue;   // a format the host does not know: no fetch either way
        nfmt++;
        int es = elem_size(f), ic = int_class(mf);
        EXPECT("vattr_bytes (the bound-range check's element size) matches Metal's element", es, vattr_bytes(f));
        int k8 = !i2f && (mf == MTLVertexFormatUCharNormalized || mf == MTLVertexFormatCharNormalized || mf == MTLVertexFormatUChar || mf == MTLVertexFormatChar || mf == MTLVertexFormatUChar2Normalized ||
                 mf == MTLVertexFormatChar2Normalized || mf == MTLVertexFormatUChar2 || mf == MTLVertexFormatChar2 || mf == MTLVertexFormatUChar3Normalized || mf == MTLVertexFormatChar3Normalized ||
                 mf == MTLVertexFormatUChar3 || mf == MTLVertexFormatChar3 || mf == MTLVertexFormatUChar4Normalized || mf == MTLVertexFormatChar4Normalized || mf == MTLVertexFormatUChar4 ||
                 mf == MTLVertexFormatChar4 || mf == MTLVertexFormatUChar4Normalized_BGRA);
        for (int pass = 0; pass < (k8 || i2f ? 3 : 1); pass++) {   // i2f: normal, unaligned CPU copy, constant element; other 8-bit formats: aligned, offset 2, stride 1
            int stride = ((es + 3) & ~3) + 4, off = 4;
            if (k8 && pass == 1) off = 2;
            if (k8 && pass == 2) { if (es != 1) continue; stride = 1; off = 0; }
            id<MTLBuffer> vb = [g_dev newBufferWithLength:(NSUInteger)(NV * stride + 64) options:MTLResourceStorageModeShared];
            uint8_t *p = vb.contents; for (int i = 0; i < NV * stride + 64; i++) p[i] = (uint8_t)rand();
            for (int v = 0; v < NV && stride > 1; v++) {   // edge values
                uint8_t *e = p + v * stride + off;
                if (k8) for (int c = 0; c < es; c++) e[c] = (uint8_t)(v + 37 * c);   // every byte value in each component, -128 included
                if (i2f) for (int c = 0; c < es * 8 / ib; c++) {
                    static const uint32_t E[] = { 0, 1, 127, 128, 255, 32767, 32768, 65535, 0xffffff, 0x1000000, 0x1000001, 0x1000003, 0x7fffffff, 0x80000000, 0xffffffff };
                    uint32_t w = E[(v + c) % (sizeof E / sizeof *E)];
                    if (ib == 8) e[c] = (uint8_t)(v + 37 * c);
                    else if (ib == 16) { uint16_t h = (uint16_t)w; memcpy(e + 2 * c, &h, 2); }
                    else memcpy(e + 4 * c, &w, 4);
                }
                if ((mf == MTLVertexFormatHalf || mf == MTLVertexFormatHalf2 || mf == MTLVertexFormatHalf3 || mf == MTLVertexFormatHalf4) && v < 16) {
                    static const uint16_t H[16] = { 0x0000, 0x8000, 0x0001, 0x03ff, 0x0400, 0x7bff, 0x7c00, 0xfc00, 0x7e00, 0x7d01, 0xfe00, 0x3c00, 0xbc00, 0x8001, 0x7fff, 0xffff };
                    for (int c = 0; c < es / 2; c++) memcpy(e + 2 * c, &H[(v + c) & 15], 2); }
                if ((mf == MTLVertexFormatFloat || mf == MTLVertexFormatFloat2 || mf == MTLVertexFormatFloat3 || mf == MTLVertexFormatFloat4) && v < 8) {
                    static const uint32_t F[8] = { 0x7fc12345, 0x7f800001, 0x00000001, 0x80000000, 0x7f800000, 0xff800000, 0x807fffff, 0x3f800000 };
                    for (int c = 0; c < es / 4; c++) memcpy(e + 4 * c, &F[(v + c) & 7], 4); }
                if ((mf == MTLVertexFormatInt1010102Normalized || mf == MTLVertexFormatUInt1010102Normalized) && v < 4) {
                    uint32_t w = v == 0 ? 0x80000000u | 0x200u | (0x200u << 10) | (0x200u << 20) : v == 1 ? 0x40000000u | 0x1ffu : v == 2 ? 0xc0000000u : 0x3fffffffu; memcpy(e, &w, 4); }
            }
            dm_vlayout_c L; memset(&L, 0, sizeof L); L.s[0].base = vb.gpuAddress; L.s[0].stride = i2f && pass == 2 ? 0 : (uint32_t)stride; L.attr[0] = (uint32_t)f | (uint32_t)off << 8; memcpy(lay.contents, &L, sizeof L);
            uint32_t got[NV][4], want[NV][4];
            memset(out.contents, 0xcd, NV * 16); run(pull, vb, -1, lay, out, 0, NV, 1, 0); memcpy(got, out.contents, sizeof got);
            if (i2f) {   // retain the CPU fallback comparison, including 32-bit formats that cannot use native numeric stage-in
                // Keep GPU components aligned; only the CPU copy is deliberately unaligned, including its per-element stride.
                uint8_t unaligned[NV * 19 + 3]; const uint8_t *cpu = p + off; uint32_t step = L.s[0].stride;
                if (pass == 1) {
                    step = (uint32_t)es + 3; cpu = unaligned + 3;
                    for (int v = 0; v < NV; v++) memcpy(unaligned + 3 + v * step, p + off + v * stride, (size_t)es);
                }
                float conv[NV][4]; vconv_fill(f, (uint64_t)(uintptr_t)cpu, step, NV, &conv[0][0]); memcpy(want, conv, sizeof want); ncpu++;
            } else {
                MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor]; vd.attributes[0].format = mf; vd.attributes[0].offset = (NSUInteger)off; vd.attributes[0].bufferIndex = 20;
                vd.layouts[20].stride = (NSUInteger)stride; vd.layouts[20].stepFunction = MTLVertexStepFunctionPerVertex;
                memset(out.contents, 0xcd, NV * 16); run(rp(ic == 1 ? @"vd_u" : ic == 2 ? @"vd_i" : @"vd_f", vd), vb, 20, nil, out, 0, NV, 1, 0); memcpy(want, out.contents, sizeof want); nvd++;
            }
            int bad = 0, ex = 0;
            for (int v = 0; v < NV; v++) for (int c = 0; c < 4; c++) if (got[v][c] != want[v][c]) { bad++; if (ex++ < 3) fprintf(stderr, "  format %#x stride %d offset %d vertex %d component %d: pulled %08x, %s %08x\n", f, stride, off, v, c, got[v][c], i2f ? "CPU conversion" : "Metal fetch", want[v][c]); }
            char name[96]; snprintf(name, sizeof name, "format %#x (stride %u, offset %d, pass %d): words differing", f, L.s[0].stride, off, pass);
            EXPECT(name, 0, bad);
        }
    }
    printf("%d formats: %d comparisons with Metal's fetch, %d with the host's CPU conversion\n", nfmt, nvd, ncpu);
    EXPECT("formats vfmt knows (0x01-0x30, 10:10:10:2 both, R11G11B10F, BGRA8, 24 integer-as-float)", 76, nfmt);
    // stride 0 (Metal: constant step) and a divisor with a base instance (Metal: per-instance step rate): the element each instance reads
    id<MTLBuffer> vb = [g_dev newBufferWithLength:64 * 16 options:MTLResourceStorageModeShared];
    float *fp = vb.contents; for (int i = 0; i < 64 * 4; i++) fp[i] = (float)i;
    for (int t = 0; t < 4; t++) {
        int divisor = t, zero = t == 0, binst = 5, ninst = 7;
        dm_vlayout_c L; memset(&L, 0, sizeof L); L.s[0].base = vb.gpuAddress; L.s[0].stride = zero ? 0 : 16; L.s[0].divisor = (uint32_t)divisor; L.attr[0] = 0x2E; memcpy(lay.contents, &L, sizeof L);
        MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor]; vd.attributes[0].format = MTLVertexFormatFloat4; vd.attributes[0].bufferIndex = 20;
        vd.layouts[20].stride = zero ? 4 : 16; vd.layouts[20].stepFunction = zero ? MTLVertexStepFunctionConstant : MTLVertexStepFunctionPerInstance; vd.layouts[20].stepRate = zero ? 0 : (NSUInteger)divisor;
        uint32_t a[16][4], b[16][4];
        memset(out.contents, 0, NV * 16); run(rp(@"vd_f", vd), vb, 20, nil, out, 1, 1, ninst, binst); memcpy(a, out.contents, sizeof a);
        memset(out.contents, 0, NV * 16); run(pull, vb, -1, lay, out, 1, 1, ninst, binst); memcpy(b, out.contents, sizeof b);
        for (int i = binst; i < binst + ninst; i++) {
            char name[96]; snprintf(name, sizeof name, "%s, base instance 5: element instance %d reads", zero ? "stride 0" : divisor == 1 ? "divisor 1" : divisor == 2 ? "divisor 2" : "divisor 3", i);
            EXPECT(name, a[i][0], b[i][0]);
        }
    }
    // the host's layout block (pull_layout_fill): a stream whose base is not 4-byte aligned, its low bits folded into the attribute offsets. Each element must read as the same bytes
    // copied to an aligned place do. Elements keep their components' alignment, as for Metal's fetch (16-bit components at even addresses, packed 32-bit words at multiples of 4;
    // 8-bit ones anywhere); an attribute whose stream has no buffer reads zeros
    {
        size_t pg = (size_t)getpagesize(); uint8_t *mem; if (posix_memalign((void **)&mem, pg, pg)) abort();
        for (size_t i = 0; i < pg; i++) mem[i] = (uint8_t)rand();
        mtl_pool_add(mem, pg);
        enum { ST = 24, NE = 32 };
        int fmts[4] = { 0x29, 0x69, 0x25, 0x0F }, size[4] = { 8, 4, 4, 2 };   // RGBA16F, RGB10A2 snorm, RGBA8 unorm, RG8UI
        void *mb; uint64_t mo, gpu0; mtl_gpu_addr((uint64_t)(uintptr_t)mem, 1, &mb, &mo, &gpu0);
        id<MTLBuffer> one = [g_dev newBufferWithLength:sizeof(dm_vlayout_c) options:MTLResourceStorageModeShared], ref = [g_dev newBufferWithLength:NE * 16 options:MTLResourceStorageModeShared];
        for (int mis = 1; mis < 4; mis++) {
            int offs[4] = { mis & 1, 12 - mis, 13, 17 };   // element addresses base + mis + offset: even, a multiple of 4, odd, odd
            state_reset(); S.nattr = 5; S.vbuf[2].address = (uint64_t)(uintptr_t)(mem + 64 + mis); S.stream[2].stride = ST;
            for (int a = 0; a < 4; a++) { S.attr[a].fmt = (uint32_t)fmts[a]; S.attr[a].offset = (uint32_t)offs[a]; S.attr[a].stream = 2; }
            S.attr[4].fmt = 0x2E; S.attr[4].stream = 5;   // stream 5 has no buffer
            pull_layout P; pull_layout_fill(&P);
            EXPECT("base kept 4-byte aligned", 0, P.s[2].base & 3);
            EXPECT("attribute with no buffer reads zeros", 0, P.attr[4]);
            for (int a = 0; a < 4; a++) {
                uint32_t got[NE][4], want[NE][4];
                dm_vlayout_c L1; memset(&L1, 0, sizeof L1); L1.s[2].base = P.s[2].base; L1.s[2].stride = P.s[2].stride; L1.attr[0] = P.attr[a]; memcpy(one.contents, &L1, sizeof L1);
                memset(out.contents, 0, NV * 16); run(pull, (__bridge id<MTLBuffer>)mb, -1, one, out, 0, NE, 1, 0); memcpy(got, out.contents, sizeof got);
                for (int v = 0; v < NE; v++) memcpy((uint8_t *)ref.contents + v * 16, mem + 64 + mis + v * ST + offs[a], (size_t)size[a]);   // each element at an aligned place
                dm_vlayout_c L2; memset(&L2, 0, sizeof L2); L2.s[2].base = ref.gpuAddress; L2.s[2].stride = 16; L2.attr[0] = (uint32_t)fmts[a] | 2u << 27; memcpy(one.contents, &L2, sizeof L2);
                memset(out.contents, 0, NV * 16); run(pull, ref, -1, one, out, 0, NE, 1, 0); memcpy(want, out.contents, sizeof want);
                char name[96]; snprintf(name, sizeof name, "base +%d, format %#x at offset %d: words as from an aligned copy", mis, fmts[a], offs[a]);
                EXPECT(name, 0, (uint64_t)(memcmp(got, want, sizeof got) != 0));
            }
        }
    }
    routing();
    printf("t_pull: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
