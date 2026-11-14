// Real Metal readback: cached stream bindings survive helper overwrites, encoder/submit changes and pool replacement.
// Byte-aligned streams preserve their bytes and GPU write ordering (Arkham Knight's Scaleform streams sit at addresses 2 mod 4).
// A draw that reads past a stream's bound size reads zeros there, not the pool's next bytes (Super Mario Odyssey's 9-vertex draw from a 3-vertex stream).
#include "../nvn/nvn_mtl.m"
uint64_t g_t0_ns;
int dm_guest_as_contains(uintptr_t address) { (void)address; return 0; }   // Tests use host-thread stacks.
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define EXPECT(name, want, got) do { uint64_t w = (want), g = (got); checks++; if (w != g) { fprintf(stderr, "%s: expected %llu, got %llu\n", name, (unsigned long long)w, (unsigned long long)g); failed++; } } while (0)

static id<MTLRenderPipelineState> pipeline(NSString *fn) {   // v: the first word of streams 0, 14, 15 and the zero buffer; w: words 0, 3, 6 and 8 of stream 0 (vertices 0-2 of 12 bytes)
    NSError *err = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "vertex void v(const device uint *a[[buffer(16)]], const device uint *b[[buffer(14)]],"
        "const device uint *c[[buffer(15)]], const device uint *z[[buffer(30)]], device uint4 *o[[buffer(8)]])"
        "{ *o=uint4(*a,*b,*c,*z); }\n"
        "vertex void w(const device uint *a[[buffer(16)]], device uint4 *o[[buffer(8)]]) { *o=uint4(a[0],a[3],a[6],a[8]); }\n"
        "vertex void i(uint v[[vertex_id]],const device uint *a[[buffer(16)]],device uint *o[[buffer(8)]]) { o[v]=a[10*v]; }" options:nil error:&err];
    if (!lib) { fprintf(stderr, "%s\n", err.localizedDescription.UTF8String); abort(); }
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [lib newFunctionWithName:fn]; d.rasterizationEnabled = NO;
    id<MTLRenderPipelineState> ps = [g_dev newRenderPipelineStateWithDescriptor:d error:&err];
    if (!ps) { fprintf(stderr, "%s\n", err.localizedDescription.UTF8String); abort(); }
    return ps;
}
static id<MTL4RenderCommandEncoder> encoder_prepared(id<MTLRenderPipelineState> ps) {   // after the caller's vbuf_prepare
    ubd_land_compute();
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    MTL4RenderPassDescriptor *d = [MTL4RenderPassDescriptor new];
    d.renderTargetWidth = d.renderTargetHeight = d.defaultRasterSampleCount = 1;
    id<MTL4RenderCommandEncoder> en = enc_r_begin(d); enc_r_pso(en, ps); return en;
}
static id<MTL4RenderCommandEncoder> encoder(id<MTLRenderPipelineState> ps) { vbuf_prepare(NULL, NULL, NULL); return encoder_prepared(ps); }
static void capture(id<MTL4RenderCommandEncoder> en, id<MTLBuffer> out, unsigned row) {
    enc_r_vbuf(en, TF_SLOT, out.gpuAddress + row * 16); enc_r_draw(en, MTLPrimitiveTypePoint, 0, 1);
}
static void finish(void) {
    EXPECT("submission completed", 1, mtl_wait(mtl_sub_commit(), 5000000000ull));
    EXPECT("no GPU error", 0, mtl_last_commit_error());
}
int main(void) { @autoreleasepool {
    mtl_init(); id<MTLRenderPipelineState> ps = pipeline(@"v"), tail = pipeline(@"w");
    size_t page = (size_t)getpagesize(), size = page * 64; uint32_t *mem;
    if (posix_memalign((void **)&mem, page, size)) abort();
    memset(mem, 0, size); mem[0] = 11; mem[1] = 22; mem[2] = 33; mem[3] = 99;
    mtl_pool_add(mem, size);
    uint8_t *ub = (uint8_t *)mem + 64; uint32_t want = 0x87654321u; memcpy(ub + 2, &want, 4); ub[0] = 0x11; ub[1] = 0x22; ub[6] = 0x33; ub[7] = 0x44;
    uint8_t *producer = (uint8_t *)mem + 128; uint32_t changed = 0x12345678u; memcpy(producer, ub, 8); memcpy(producer + 2, &changed, 4);
    id<MTLBuffer> out = [g_dev newBufferWithLength:16 * 32 options:MTLResourceStorageModeShared]; res_add(out);
    state_reset();
    // Exercise the compact record through the production dispatcher before the
    // existing shader reads each stream. Its public state remains a full range.
    struct { op_hdr h; p_bind_vbuf1 b; } bindings[4] = {
        {{OP_BIND_VBUF1,24},{0,4,(uint64_t)mem}},
        {{OP_BIND_VBUF1,24},{14,4,(uint64_t)(mem+1)}},
        {{OP_BIND_VBUF1,24},{15,4,(uint64_t)(mem+2)}},
        {{OP_END,8},{0}}
    };
    uint64_t other, off; void *mb; mtl_gpu_addr((uint64_t)(mem + 3), 1, &mb, &off, &other);
    mtl_sub_begin(); mtl_exec((const uint8_t *)bindings);
    EXPECT("compact vertex replay restores first full range",1,S.vbuf[0].address==(uint64_t)mem&&S.vbuf[0].size==4);
    EXPECT("compact vertex replay restores high stream range",1,S.vbuf[15].address==(uint64_t)(mem+2)&&S.vbuf[15].size==4);
    id<MTL4RenderCommandEncoder> en = encoder(ps);
    bind_vbufs(en, NULL); capture(en, out, 0);
    uint64_t resolves = g_vbuf_resolve, sets = g_vbuf_set;
    for (int i = 0; i < 1000; i++) bind_vbufs(en, NULL);
    EXPECT("unchanged streams avoid lookups", g_vbuf_cache ? 0 : 3000, g_vbuf_resolve - resolves);
    EXPECT("unchanged table avoids writes", g_vbuf_cache ? 0 : 4000, g_vbuf_set - sets);
    capture(en, out, 1);
    // A converted stream changes the real table, even though the guest stream address is unchanged.
    enc_r_vbuf(en, VB_SLOT(0), other); capture(en, out, 2);
    bind_vbufs(en, NULL); capture(en, out, 3);
    // All vertex table writers must participate, including argument-buffer writes.
    enc_r_args(en, MTLRenderStageVertex, other, VB_SLOT(14)); bind_vbufs(en, NULL); capture(en, out, 4);
    enc_r_end(en);
    en = encoder(ps); bind_vbufs(en, NULL); capture(en, out, 5); enc_r_end(en); finish();
    mtl_sub_begin(); en = encoder(ps);
    S.vbuf[0].address = (uint64_t)(mem + 3); bind_vbufs(en, NULL); capture(en, out, 6); enc_r_end(en); finish();
    // The same guest address can acquire a new Metal GPU address. Hold the old buffer to prevent address recycling.
    id<MTLBuffer> old = (__bridge id<MTLBuffer>)mb;
    mtl_pool_del(mem); mtl_pool_add(mem, size);
    uint64_t newgpu; mtl_gpu_addr((uint64_t)(mem + 3), 1, &mb, &off, &newgpu);
    EXPECT("replacement buffer has a different GPU address", 1, old.gpuAddress + 12 != newgpu);
    mtl_sub_begin(); en = encoder(ps); resolves = g_vbuf_resolve;
    bind_vbufs(en, NULL); EXPECT("pool replacement resolves all streams", 3, g_vbuf_resolve - resolves);
    EXPECT("replacement address bound", newgpu, g_vt_addr[VB_SLOT(0)]);
    capture(en, out, 7); enc_r_end(en); finish();
    mtl_sub_begin(); S.vbuf[0].address = (uint64_t)(ub + 2); S.vbuf[0].size = 6;
    uint64_t copies = g_vbuf_copies, copy_bytes = g_vbuf_copy_bytes; en = encoder(ps);
    bind_vbufs(en, NULL); bind_vbufs(en, NULL); capture(en, out, 8); enc_r_end(en);
    EXPECT("unchanged unaligned binding reuses one copy", 1, g_vbuf_copies - copies);
    EXPECT("unaligned stream copy size", 6, g_vbuf_copy_bytes - copy_bytes);
    mtl_sub_copy((uint64_t)producer, (uint64_t)ub, 8);
    en = encoder(ps); bind_vbufs(en, NULL); capture(en, out, 9); enc_r_end(en); finish();
    EXPECT("unaligned stream readback", want, ((const uint32_t *)out.contents)[8 * 4]);
    EXPECT("unaligned stream refreshed after GPU write", changed, ((const uint32_t *)out.contents)[9 * 4]);
    memcpy(producer + 2, &want, 4);
    mtl_sub_begin(); mtl_sub_copy((uint64_t)producer, (uint64_t)ub, 8);
    en = encoder(ps); bind_vbufs(en, NULL); capture(en, out, 10); enc_r_end(en); finish();
    EXPECT("unaligned stream reads queued GPU write", want, ((const uint32_t *)out.contents)[10 * 4]);
    // A pool remap at the same guest address invalidates the snapshot even within one submission.
    mtl_sub_begin(); en = encoder(ps); bind_vbufs(en, NULL); capture(en, out, 11); enc_r_end(en);
    copies = g_vbuf_copies; mtl_pool_del(mem); mtl_pool_add(mem, size);
    en = encoder(ps); bind_vbufs(en, NULL); capture(en, out, 12); enc_r_end(en); finish();
    EXPECT("unaligned pool replacement makes a new copy", 1, g_vbuf_copies - copies);
    EXPECT("unaligned pool replacement readback", want, ((const uint32_t *)out.contents)[12 * 4]);
    // Every nonzero residue, a short odd-sized range, and a source write between two uses of the same binding.
    for (unsigned residue = 1; residue < 4; residue++) {
        uint32_t value = 0x76543210u + residue; memcpy(producer + residue, &value, 4);
        S.vbuf[0].address = (uint64_t)(ub + residue); S.vbuf[0].size = 5;
        mtl_sub_begin(); mtl_sub_copy((uint64_t)producer, (uint64_t)ub, 8);
        en = encoder(ps); bind_vbufs(en, NULL); capture(en, out, 12 + residue); enc_r_end(en); finish();
        EXPECT("byte-aligned GPU copy readback", value, ((const uint32_t *)out.contents)[(12 + residue) * 4]);
    }
    // Inline updates may still be deferred when the next draw needs their bytes as vertices.
    changed = 0xa1b2c3d4u; memcpy(producer + 3, &changed, 4);
    mtl_sub_begin(); mtl_sub_update(producer, (uint64_t)ub, 8);
    en = encoder(ps); bind_vbufs(en, NULL); capture(en, out, 16); enc_r_end(en); finish();
    EXPECT("unaligned stream reads deferred update", changed, ((const uint32_t *)out.contents)[16 * 4]);
    mtl_sub_begin(); mtl_sub_fill((uint64_t)ub, 8, 0x6d6d6d6du);
    en = encoder(ps); bind_vbufs(en, NULL); capture(en, out, 17); enc_r_end(en); finish();
    EXPECT("unaligned stream reads GPU fill", 0x6d6d6d6du, ((const uint32_t *)out.contents)[17 * 4]);
    // Begin the consumer without a CPU wait for the producer submission.
    mtl_sub_begin(); mtl_sub_copy((uint64_t)producer, (uint64_t)ub, 8); uint64_t prior = mtl_sub_commit();
    mtl_sub_begin(); en = encoder(ps); bind_vbufs(en, NULL); capture(en, out, 18); enc_r_end(en); finish();
    EXPECT("producer submission completed", 1, mtl_wait(prior, 5000000000ull));
    EXPECT("unaligned stream reads preceding submission", changed, ((const uint32_t *)out.contents)[18 * 4]);
    // A real vertex shader writes the source; storage writes are tracked separately from copy/update/fill ranges.
    S.vbuf[0].address = (uint64_t)(mem + 3); S.vbuf[0].size = 4;
    uint64_t ubgpu; mtl_gpu_addr((uint64_t)ub, 16, &mb, &off, &ubgpu);
    mtl_sub_begin(); en = encoder(ps); bind_vbufs(en, NULL); enc_r_vbuf(en, TF_SLOT, ubgpu); enc_r_draw(en, MTLPrimitiveTypePoint, 0, 1); enc_r_end(en);
    sbw_note((uint64_t)ub, 16, SBW_GPU);
    S.vbuf[0].address = (uint64_t)(ub + 2); S.vbuf[0].size = 6;
    en = encoder(ps); bind_vbufs(en, NULL); capture(en, out, 19); enc_r_end(en); finish();
    EXPECT("unaligned stream reads shader write", 22u << 16, ((const uint32_t *)out.contents)[19 * 4]);
    mtl_sub_begin(); en = encoder(ps); bind_vbufs(en, NULL); capture(en, out, 20); enc_r_end(en); copies = g_vbuf_copies;
    mtl_sub_update(producer, (uint64_t)mem + 256, 8);
    en = encoder(ps); bind_vbufs(en, NULL); capture(en, out, 21); enc_r_end(en); finish();
    EXPECT("unrelated uniform update reuses vertex copy", copies, g_vbuf_copies);
    EXPECT("reused unaligned stream readback", 22u << 16, ((const uint32_t *)out.contents)[21 * 4]);
    S.vbuf[0].address = (uint64_t)(mem + 3); S.vbuf[0].size = 4;
    const uint32_t *pixels = out.contents;
    for (int row = 0; row < 8; row++) {
        EXPECT("stream 0 readback", row == 2 || row >= 6 ? 99 : 11, pixels[row * 4]);
        EXPECT("stream 14 readback", 22, pixels[row * 4 + 1]);
        EXPECT("stream 15 readback", 33, pixels[row * 4 + 2]);
        EXPECT("zero buffer readback", 0, pixels[row * 4 + 3]);
    }
    // A 3-vertex draw of 12-byte RGB32F elements from a stream bound with 12 bytes: vertices 1 and 2 read zeros, though the pool holds other words there,
    // through the vertex table and the pulling layout alike; a draw inside the range binds the stream itself
    uint32_t *tri = mem + 128; for (int i = 0; i < 9; i++) tri[i] = 0x1000u + (uint32_t)i;
    nvn_refl rf; memset(&rf, 0, sizeof rf); rf.nv = 1; rf.v[0].nvn = 0;
    S.nattr = 1; S.attr[0] = (va_st){.fmt = 0x22, .offset = 0, .stream = 0}; S.stream[0].stride = 12; S.stream[0].divisor = 0;
    S.vbuf[0].address = (uint64_t)tri; S.vbuf[0].size = 12;
    p_draw dw = {.prim = 4, .first = 0, .count = 3, .instances = 1};
    uint64_t absent_last = 99;
    EXPECT("stream 16 has no known vertex range", 0, vstream_last(16, &dw, NULL, &absent_last));
    EXPECT("negative stream has no known vertex range", 0, vstream_last(-1, &dw, NULL, &absent_last));
    EXPECT("an absent stream leaves the range unknown", 99, absent_last);
    mtl_sub_begin(); copies = g_vbuf_copies; vbuf_prepare(&rf, &dw, NULL);
    EXPECT("bytes the draw reads past the bound size", 36, g_vbuf_need[0]);
    en = encoder_prepared(tail); bind_vbufs(en, NULL); capture(en, out, 22);
    pull_layout L; pull_layout_fill(&L);
    EXPECT("pulling layout reads the padded copy", g_vt_addr[VB_SLOT(0)] & ~3ull, L.s[0].base);
    enc_r_end(en); finish();
    EXPECT("padded stream copied once", 1, g_vbuf_copies - copies);
    EXPECT("vertex 0 inside the bound range", 0x1000u, ((const uint32_t *)out.contents)[22 * 4]);
    EXPECT("vertex 1 past the bound range reads zero", 0, ((const uint32_t *)out.contents)[22 * 4 + 1]);
    EXPECT("vertex 2 past the bound range reads zero", 0, ((const uint32_t *)out.contents)[22 * 4 + 2]);
    EXPECT("vertex 2's last word reads zero", 0, ((const uint32_t *)out.contents)[22 * 4 + 3]);
    dw.count = 1; mtl_sub_begin(); copies = g_vbuf_copies; vbuf_prepare(&rf, &dw, NULL);
    EXPECT("a draw inside the bound range needs no copy", 0, g_vbuf_need[0]);
    en = encoder_prepared(tail); bind_vbufs(en, NULL); capture(en, out, 23); enc_r_end(en); finish();
    EXPECT("in-range stream bound as it is", 0, g_vbuf_copies - copies);
    EXPECT("in-range stream reads the pool", 0x1003u, ((const uint32_t *)out.contents)[23 * 4 + 1]);
    S.stream[0].divisor = 1; p_draw inst = {.prim = 4, .first = 0, .count = 1, .instances = 2, .base_instance = 1};
    vbuf_prepare(&rf, &inst, NULL); EXPECT("per-instance stream: base instance + last instance", 36, g_vbuf_need[0]);
    p_draw_idx ix = {.prim = 4, .itype = 1, .count = 3, .instances = 2, .base_instance = 0, .ibuf = (uint64_t)mem};
    vbuf_prepare(&rf, NULL, &ix); EXPECT("indexed draw: per-instance stream bounded", 24, g_vbuf_need[0]);
    S.stream[0].divisor = 0; vbuf_prepare(&rf, NULL, &ix); EXPECT("indexed draw: per-vertex stream not scanned", 0, g_vbuf_need[0]);
    dw.count = 3; dw.ind = (uint64_t)mem; vbuf_prepare(&rf, &dw, NULL); EXPECT("indirect draw: not known on the CPU", 0, g_vbuf_need[0]);
    S.stream[0].stride = 0; dw.ind = 0; S.vbuf[0].size = 8; vbuf_prepare(&rf, &dw, NULL); EXPECT("stride 0: one element", 12, g_vbuf_need[0]);
    // A stream bound with size 0 at an unaligned address: no bytes to align, so no copy and no red line while nothing reads it; a draw that reads it gets zeros
    S.nattr = 0; S.vbuf[0] = (NVNbufferRange){1, 0}; unsigned reds = g_red_once[120];
    mtl_sub_begin(); copies = g_vbuf_copies; vbuf_prepare(NULL, NULL, NULL);
    en = encoder_prepared(tail); bind_vbufs(en, NULL); enc_r_end(en); finish();
    EXPECT("an unread empty stream is not copied", 0, g_vbuf_copies - copies);
    EXPECT("an unread empty stream logs no red line", reds, g_red_once[120]);
    S.nattr = 1; S.stream[0].stride = 12; S.vbuf[0] = (NVNbufferRange){(uint64_t)tri + 1, 0}; dw.count = 3;
    mtl_sub_begin(); copies = g_vbuf_copies; vbuf_prepare(&rf, &dw, NULL);
    EXPECT("a draw reads past an empty stream", 36, g_vbuf_need[0]);
    en = encoder_prepared(tail); bind_vbufs(en, NULL); capture(en, out, 24); enc_r_end(en); finish();
    EXPECT("the read empty stream is copied once", 1, g_vbuf_copies - copies);
    for (int i = 0; i < 4; i++) EXPECT("the read empty stream reads zero", 0, ((const uint32_t *)out.contents)[24 * 4 + i]);
    S.vbuf[0] = (NVNbufferRange){(uint64_t)tri, 8};
    S.nattr = 0; S.stream[0].stride = 0; vbuf_prepare(NULL, NULL, NULL);
    // A stream at an address in no memory pool: reported when the vertex shader reads it (the draw is skipped), not when only a stale stream holds it; the others are still bound
    {
        static uint32_t outside[16];   // never added as a pool
        S.nattr = 1; S.attr[0] = (va_st){.fmt = 1, .offset = 0, .stream = 5}; S.vbuf[5] = (NVNbufferRange){(uint64_t)outside, sizeof outside};
        mtl_sub_begin(); en = encoder_prepared(tail);
        EXPECT("a read stream in no pool is reported", 5, bind_vbufs(en, &rf));
        EXPECT("the other streams are still bound", 1, g_vt_addr[VB_SLOT(0)] != 0);
        S.attr[0].stream = 0;
        EXPECT("an unread stream in no pool is not", (uint64_t)-1, (uint64_t)bind_vbufs(en, &rf));
        EXPECT("without the shader's inputs nothing is reported", (uint64_t)-1, (uint64_t)bind_vbufs(en, NULL));
        enc_r_end(en); finish();
        S.vbuf[5] = (NVNbufferRange){0, 0}; S.attr[0] = (va_st){0}; S.nattr = 0;
    }
    // MGS4 binds a whole allocation's size at an offset into it; only the fetched bytes must fit its pool.
    for (int mis = 0; mis < 2; mis++) {
        uint8_t *end = (uint8_t *)mem + size - 64 + mis;
        for (int i = 0; i < 9; i++) { uint32_t v = 0x2000u + i; memcpy(end + i * 4, &v, 4); }
        S.nattr = 1; S.attr[0] = (va_st){.fmt = 0x22, .stream = 0}; S.stream[0].stride = 12; S.stream[0].divisor = 0;
        S.vbuf[0] = (NVNbufferRange){(uint64_t)end, size}; dw = (p_draw){.prim = 4, .count = 3, .instances = 1};
        mtl_sub_begin(); vbuf_prepare(&rf, &dw, NULL); en = encoder_prepared(tail);
        int bad = bind_vbufs(en, &rf);
        EXPECT("oversized binding with mapped fetched bytes is accepted", (uint64_t)-1, (uint64_t)bad);
        if (bad < 0) capture(en, out, 25 + mis);
        pull_layout_fill(&L); EXPECT("pulling layout resolves oversized binding", 1, L.s[0].base != 0);
        enc_r_end(en); finish();
        if (bad < 0) {
            EXPECT("oversized binding reads first vertex", 0x2000, ((const uint32_t *)out.contents)[(25 + mis) * 4]);
            EXPECT("oversized binding reads last vertex", 0x2008, ((const uint32_t *)out.contents)[(25 + mis) * 4 + 3]);
        }
        if (!mis) {
            dw.count = 8; mtl_sub_begin(); vbuf_prepare(&rf, &dw, NULL); en = encoder_prepared(tail);
            EXPECT("cached binding cannot hide a longer unmapped fetch", 0, bind_vbufs(en, &rf));
            enc_r_end(en); finish();
            dw.ind = (uint64_t)mem; mtl_sub_begin(); vbuf_prepare(&rf, &dw, NULL); en = encoder_prepared(tail);
            EXPECT("unknown fetch retains the full bound-range check", 0, bind_vbufs(en, &rf));
            enc_r_end(en); finish();
        }
    }
    // MGS4's explosion quads use the same oversized stream limit through an indexed draw.
    uint16_t *indices = (uint16_t *)((uint8_t *)mem + 256); indices[0] = 1; indices[1] = 2; indices[2] = 3;
    uint32_t *vertices = (uint32_t *)((uint8_t *)mem + size - 12 * 65536);
    for (int i = 0; i < 9; i++) vertices[i] = 0x2000u + i;
    S.vbuf[0] = (NVNbufferRange){(uint64_t)vertices, size};
    ix = (p_draw_idx){.prim = 4, .itype = 1, .count = 3, .instances = 1, .ibuf = (uint64_t)indices};
    mtl_sub_begin(); vbuf_prepare(&rf, NULL, &ix); en = encoder_prepared(tail);
    int indexed_bad = bind_vbufs(en, &rf);
    EXPECT("indexed oversized binding uses its index-format bound", (uint64_t)-1, (uint64_t)indexed_bad);
    if (indexed_bad < 0) capture(en, out, 27);
    enc_r_end(en); finish();
    if (indexed_bad < 0) EXPECT("indexed range preserves its last vertex", 0x2008, ((const uint32_t *)out.contents)[27 * 4 + 3]);
    mtl_sub_begin(); mtl_sub_fill((uint64_t)indices, 8, 0);
    vbuf_need(&rf, NULL, &ix); EXPECT("16-bit bound holds for pending GPU-written indices", 12 * 65536, vbuf_size(0));
    finish();
    ix.base_vertex = 1; vbuf_need(&rf, NULL, &ix); EXPECT("index-format bound includes base vertex", 12 * 65537, vbuf_size(0));
    ix.base_vertex = -1; vbuf_need(&rf, NULL, &ix); EXPECT("negative base vertex retains full bound check", size, vbuf_size(0));
    ix.base_vertex = 0; ix.base_instance = -1; S.stream[0].divisor = 1; vbuf_need(&rf, NULL, &ix);
    EXPECT("index bound cannot replace an unknown instance range", size, vbuf_size(0));
    ix.base_instance = 0; S.stream[0].divisor = 0;
    ix.base_vertex = 0; ix.itype = 0; vbuf_need(&rf, NULL, &ix); EXPECT("8-bit index bound", 12 * 256, vbuf_size(0));
    ix.itype = 2; vbuf_need(&rf, NULL, &ix); EXPECT("32-bit upper bound past the binding does not narrow it", size, vbuf_size(0));
    EXPECT("index upper bound does not allocate speculative padding", 0, g_vbuf_need[0]);
    // A character submesh uses only four vertices near the pool's end: even the index-type bound is too large.
    vertices = (uint32_t *)((uint8_t *)mem + size - 64);
    for (int i = 0; i < 9; i++) vertices[i] = 0x3000u + i;
    indices[0] = 1; indices[1] = 2; indices[2] = 3;
    nvn_dirty_add_range(mem, size);
    S.vbuf[0] = (NVNbufferRange){(uint64_t)vertices, 96}; ix.itype = 1;
    mtl_sub_begin(); vbuf_prepare(&rf, NULL, &ix); en = encoder_prepared(tail);
    EXPECT("small indexed mesh uses its actual range", 48, vbuf_size(0));
    indexed_bad = bind_vbufs(en, &rf);
    EXPECT("small indexed mesh is not discarded", (uint64_t)-1, (uint64_t)indexed_bad);
    if (indexed_bad < 0) capture(en, out, 28);
    pull_layout_fill(&L); EXPECT("small indexed mesh pulling address is mapped", 1, L.s[0].base != 0);
    enc_r_end(en); finish();
    if (indexed_bad < 0) EXPECT("small indexed mesh GPU readback", 0x3008, ((const uint32_t *)out.contents)[28 * 4 + 3]);
    nvn_dirty_open(indices, 6); indices[2] = 4; nvn_dirty_close();
    vbuf_need(&rf, NULL, &ix); EXPECT("changed indices invalidate the kept range", 60, vbuf_size(0));
    ix.base_vertex = 1; vbuf_need(&rf, NULL, &ix); EXPECT("actual range includes base vertex", 72, vbuf_size(0)); ix.base_vertex = 0;
    mtl_sub_begin(); sbw_note((uint64_t)indices, 6, SBW_GPU);
    vbuf_need(&rf, NULL, &ix); EXPECT("shader-written indices are not read for a bound", 96, vbuf_size(0)); finish();
    mtl_sub_begin(); mtl_sub_fill((uint64_t)indices, 8, 0);
    vbuf_need(&rf, NULL, &ix); EXPECT("pending index writes retain the full bound", 96, vbuf_size(0)); finish();
    // Grenade particles upload their indices before drawing: the destination still holds an older frame's values.
    uint16_t *isrc = (uint16_t *)((uint8_t *)mem + 2 * page + 32), *idst = (uint16_t *)((uint8_t *)mem + 3 * page + 32);
    uint16_t *imid = (uint16_t *)((uint8_t *)mem + 4 * page + 32);
    isrc[0] = 2; isrc[1] = 0; isrc[2] = 1; idst[0] = idst[1] = idst[2] = 65535;
    vertices = (uint32_t *)((uint8_t *)mem + size - 160); vertices[0] = 41; vertices[10] = 42; vertices[20] = 43;
    S.vbuf[0] = (NVNbufferRange){(uint64_t)vertices, 512}; S.stream[0].stride = 40; ix.ibuf = (uint64_t)idst;
    id<MTLRenderPipelineState> ips = pipeline(@"i");
    mtl_sub_begin(); mtl_sub_copy((uint64_t)isrc, (uint64_t)idst, 6); vbuf_prepare(&rf, NULL, &ix);
    EXPECT("uploaded indices bound a 40-byte particle stream", 92, vbuf_size(0));
    en = encoder_prepared(ips); indexed_bad = bind_vbufs(en, &rf);
    EXPECT("particle upload draw is not discarded", (uint64_t)-1, (uint64_t)indexed_bad);
    uint64_t igpu; EXPECT("uploaded index buffer is mapped", 1, mtl_gpu_addr((uint64_t)idst, 6, &mb, &off, &igpu));
    if (indexed_bad < 0) { enc_r_vbuf(en, TF_SLOT, out.gpuAddress + 29 * 16); enc_r_draw_indexed(en, MTLPrimitiveTypePoint, 3, MTLIndexTypeUInt16, igpu, 6, 1, 0, 0); }
    enc_r_end(en); finish();
    if (indexed_bad < 0) for (int i = 0; i < 3; i++) EXPECT("GPU uses the uploaded indices and particle vertices", 41 + i, ((const uint32_t *)out.contents)[29 * 4 + i]);
    mtl_sub_begin(); mtl_sub_copy((uint64_t)isrc, (uint64_t)imid, 6); mtl_sub_copy((uint64_t)imid, (uint64_t)idst, 6);
    vbuf_need(&rf, NULL, &ix); EXPECT("ordered upload chains preserve the exact range", 92, vbuf_size(0)); finish();
    mtl_sub_begin(); mtl_sub_copy((uint64_t)isrc, (uint64_t)idst, 6);
    nvn_dirty_open(isrc, 6); isrc[0] = 3; nvn_dirty_close();
    vbuf_need(&rf, NULL, &ix); EXPECT("CPU changes after the copy refuse its source", 512, vbuf_size(0)); finish();
    mtl_sub_begin(); mtl_sub_copy((uint64_t)isrc, (uint64_t)idst, 6); mtl_sub_fill((uint64_t)isrc, 8, 0);
    vbuf_need(&rf, NULL, &ix); EXPECT("later GPU source writes cannot be read backward", 512, vbuf_size(0)); finish();
    mtl_sub_begin(); mtl_sub_copy((uint64_t)isrc, (uint64_t)idst, 4);
    vbuf_need(&rf, NULL, &ix); EXPECT("partial index uploads do not prove the whole range", 512, vbuf_size(0)); finish();
    nvn_dirty_remove_range(mem);
    S.stream[0].stride = 12;
    S.vbuf[0] = (NVNbufferRange){(uint64_t)vertices, size};
    rf.nv = 2; rf.v[1].nvn = 1; S.nattr = 2; S.attr[1] = (va_st){.fmt = 0x22, .stream = 0, .offset = 48};
    vbuf_need(&rf, &dw, NULL); EXPECT("range includes every input on the stream", 84, vbuf_size(0));
    S.attr[1].fmt = 0xff; vbuf_need(&rf, &dw, NULL);
    EXPECT("unknown input format retains full bound range", size, vbuf_size(0));
    S.attr[1].fmt = 0x22; vbuf_need(&rf, &dw, NULL); vbuf_need(NULL, NULL, NULL);
    EXPECT("geometry capture clears earlier read bounds", size, vbuf_size(0));
    S.vbuf[0] = (NVNbufferRange){(uint64_t)tri, 8}; S.nattr = 0; vbuf_prepare(NULL, NULL, NULL);
    // CPU-only binding microbenchmark, after GPU validation; reports overhead, not game frame time.
    for (int mode = 0; mode < 2; mode++) {
        g_vbuf_cache = mode; bind_vbufs(nil, NULL); uint64_t start = bp_now(), r = g_vbuf_resolve, w = g_vbuf_set;
        for (int i = 0; i < 200000; i++) bind_vbufs(nil, NULL);
        printf("bindings cache=%d: %.2f ns/draw, lookups=%llu writes=%llu\n", mode, (bp_now() - start) / 200000.0,
            (unsigned long long)(g_vbuf_resolve - r), (unsigned long long)(g_vbuf_set - w));
    }
    mtl_pool_del(mem); mtl_drain(); old = nil; free(mem);
    printf("t_vbuf: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
