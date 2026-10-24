// nvn_mtl.m, part: state
// Draw state: vertex buffer binding, the pull layout, stage binding, vertex format conversion (vconv_*), the vertex range cache and pso_desc, the description of a pipeline.
// Included by nvn_mtl.m in its original order, as one translation unit: statics are shared with the other parts.
// ---------- Draw state ----------
static struct {
    const prog_t *prog;
    const prog_t *cprog;       // compute program: NVN binds it apart from the graphics stages (see OP_BIND_PROGRAM in state_op)
    p_prim_restart restart;
    const prog_t *bst[5];      // program bound per stage (NVN stage bits 1 vertex 2 fragment 4 geometry 8 / 16 tessellation, indices as in NvProg.sh), see bind_stages
    blend_st blend[8]; color_st color; chmask_st chmask; ms_st ms; poly_st poly; ds_st ds;
    va_st attr[16]; int nattr; vs_st stream[16]; int nstream;
    NVNbufferRange vbuf[16];
    p_bind_buf ubo[6][16], ssbo[6][16];
    uint64_t tex[6][NTEX], samp[6][128], img[6][32];
    uint32_t st_vmask[2], st_wmask[2], st_ref[2];
    p_poly_offset po; float vp[16][4]; float dr[2]; int has_vp; int depth_clamp; uint8_t vp_sw[16][4];   // vp_sw: viewport swizzles XOR the identity (p_vp_swizzle)
    float blend_color[4]; int64_t copy_stride, copy_img_stride;
    p_tf_bind tf[4]; uint64_t tf_off[4]; int tf_on, rast_discard;   // transform feedback buffers, bytes written into each since Begin, capture active; rasterizer discard (tf_draw)
    int tex_base, samp_base;   // host-table bases of the texture / sampler pool bound (OP_SET_TEX_POOL / OP_SET_SAMP_POOL), for handles read at replay from uniform buffers (encode_args)
} S;
// The handle c2 holds at byte b, as encode_args fills the table (DRV2): a combined slot (past slot 31 the image there, tex_slot_h), a separate texture, else none
static uint64_t c2_handle(int stage, unsigned b) {
    if (b >= C2_TEX && b < C2_SEPTEX && !((b - C2_TEX) & 7)) return tex_slot_h(S.tex[stage], S.img[stage], (b - C2_TEX) / 8);
    if (b >= C2_SEPTEX && b < C2_SAMP && !((b - C2_SEPTEX) & 7)) return S.tex[stage][NVN_SEPTEX_SLOT0 + (b - C2_SEPTEX) / 8];
    return 0;
}
// A deferred inline update changes what the draws after it read through the uniform bindings it overlaps (they get a shadow): the argument blocks of those stages are encoded again.
// Only those: marking every stage made each later draw encode all of its blocks again, which with the updates kept across passes cost about 1.5 ms a frame in Breath of the Wild
static uint8_t g_args_dirty[6];
static void ubd_dirty(uint64_t lo, uint64_t hi) {
    for (int st = 0; st < 6; st++) { if (g_args_dirty[st]) continue;
        for (int r = 0; r < 16; r++) { const p_bind_buf *u = &S.ubo[st][r]; if (u->addr && lo < u->addr + u->size && u->addr < hi) { g_args_dirty[st] = 1; break; } } }
}
// Resolved guest streams are separate from the table cache: vconv_bind may overwrite a stream's table slot.
// Take the pool generation before resolving, so a concurrent pool change forces a fresh lookup next time.
static void bind_vbufs(id<MTL4RenderCommandEncoder> en) {
    static struct { uint64_t addr, gpu, gen; } cache[16];
    uint64_t gen = atomic_load_explicit(&g_pool_gen, memory_order_acquire);
    for (int s = 0; s < 16; s++) {
        uint64_t addr = S.vbuf[s].address, gpu;
        if (!addr) continue;
        if (g_vbuf_cache && cache[s].addr == addr && cache[s].gen == gen) { gpu = cache[s].gpu; g_vbuf_reuse++; }
        else {
            void *mb; uint64_t mo; g_vbuf_resolve++;
            if (!mtl_gpu_addr(addr, 1, &mb, &mo, &gpu)) continue;
            cache[s].addr = addr; cache[s].gpu = gpu; cache[s].gen = gen;
        }
        enc_r_vbuf(en, VB_SLOT(s), gpu);
    }
    static id<MTLBuffer> zero; static uint64_t zero_gpu;
    if (!zero) { zero = [g_dev newBufferWithLength:64 options:MTLResourceStorageModeShared]; res_add(zero); zero_gpu = zero.gpuAddress; }
    enc_r_vbuf(en, 30, zero_gpu);
}
// The per-draw layout block of a pulling pipeline (dm/gfx/tools/dmsh_pull.metal dm_vlayout, at buffer 16): per stream the base address, stride and divisor, per NVN attribute its
// state word. A base address is kept 4-byte aligned, its low bits moved into its attributes' offsets (dm_fetch reads whole words where the element is aligned); elements keep their
// components' alignment as for Metal's own fetch (16-bit components at even addresses, packed 32-bit words at multiples of 4). An attribute whose stream has no buffer reads zeros,
// as through the zero buffer (t_pull.m)
enum { PULL_SLOT = 16 };   // = dmsh_pull.py SLOT
typedef struct { uint64_t base; uint32_t stride, divisor; } pull_stream;
typedef struct { pull_stream s[16]; uint32_t attr[16]; } pull_layout;
_Static_assert(sizeof(pull_layout) == 320, "dmsh_pull.metal dm_vlayout");
static void pull_layout_fill(pull_layout *Lp) {
    pull_layout L; memset(&L, 0, sizeof L); uint32_t lo[16] = {0};
    for (int st = 0; st < 16; st++) {
        uint64_t addr = S.vbuf[st].address, gpu; void *mb; uint64_t mo;
        if (!addr || !mtl_gpu_addr(addr, 1, &mb, &mo, &gpu)) continue;
        L.s[st].base = gpu & ~3ull; lo[st] = (uint32_t)(gpu & 3); L.s[st].stride = S.stream[st].stride; L.s[st].divisor = S.stream[st].divisor;
    }
    for (int a = 0; a < 16 && a < S.nattr; a++) {
        va_st v = S.attr[a];
        if (!v.fmt || v.stream >= 16 || !L.s[v.stream].base) continue;
        v.offset += lo[v.stream]; memcpy(&L.attr[a], &v, 4);
    }
    *Lp = L;
}
static void pull_bind(id<MTL4RenderCommandEncoder> en) {
    pull_layout L; pull_layout_fill(&L);
    id<MTLBuffer> ab; size_t off = arena_alloc(sizeof L, &ab);
    memcpy((uint8_t *)ab.contents + off, &L, sizeof L);
    enc_r_vbuf(en, PULL_SLOT, ab.gpuAddress + off);
}
// Size of an attachment-less pass (rp_begin_ex_): the far corner of viewport 0, else of the scissor, clamped to Metal's 1..16384
static void bare_dims(NSUInteger *w, NSUInteger *h) {
    double x1 = S.has_vp ? MAX(S.vp[0][0], S.vp[0][0] + S.vp[0][2]) : R.has_sc ? (double)R.sc.x + R.sc.w : 1;
    double y1 = S.has_vp ? MAX(S.vp[0][1], S.vp[0][1] + S.vp[0][3]) : R.has_sc ? (double)R.sc.y + R.sc.h : 1;
    *w = (NSUInteger)MIN(MAX(ceil(x1), 1.0), 16384.0); *h = (NSUInteger)MIN(MAX(ceil(y1), 1.0), 16384.0);
}
// The open draw pass has a draw whose shaders write an image or a storage buffer. NVN orders later draws after those writes when the game asks with nvnCommandBufferBarrier
// (ORDER_FRAGMENTS / ORDER_FRAGMENTS_TILED); Metal gives no order between draws of one render pass for shader writes, so such a barrier ends the pass and the next draw opens one
// behind the full consumer barrier. FINAL FANTASY TACTICS - The Ivalice Chronicles composites its UI into the window texture in software, a read-modify-write of the image by draw after
// draw in one attachment-less pass with barrier bits 0x12 between them: without the split a later element read the window before the earlier one had landed and wrote the old pixels
// back (the dialog boxes had holes the size of their text)
static void swrite_note(NvProg *pr) {
    if (!pr || g_pass_swrite) return;
    for (int st = 0; st < 6; st++) { __unsafe_unretained NvShader *sh = pr->sh[st]; if (!sh) continue;
        for (int i = 0; i < sh->rf.nb; i++) if (sh->rf.b[i].rw && (sh->rf.b[i].kind == NVN_RB_IMG || sh->rf.b[i].kind == NVN_RB_IMGA || sh->rf.b[i].kind == NVN_RB_SBUFP)) { g_pass_swrite = 1; return; } }
}
static void udc_note_draw(NvProg *pr) {
    if (!tbdr_on()) {   // without the TBDR set only the duplicate-slot report below is live (the zero-source skip and the unwritten-attachment store are part of the set): skip the per-draw scans
        uint32_t cm = pr && pr->sh[1] ? pr->sh[1]->rf.color_mask : 0;
        if (R.has_rt && cm >> 1) for (int i = 1; i < R.rt.n && i < 8; i++)
            if ((cm >> i & 1) && ((S.chmask.mask >> (4 * i)) & 0xf) && rt_dup_slot(&R.rt, i))
                RED_ONCE(79, "color slot %d holds the same texture as an earlier slot (ser=%llu) and this draw writes it (mask %#x): these writes are lost", i, (unsigned long long)R.rt.color[i]->serial, (S.chmask.mask >> (4 * i)) & 0xf);
        return;
    }
    if (R.has_rt) for (int i = 0; i < R.rt.n && i < 8; i++) if (R.rt.color[i]) { zskip_del(R.rt.color[i]->serial); g_zskip_cand_ok[i] = 0; }   // Zero-source set: once really drawn it is no longer all zeros
    if (pr) for (int st = 0; st < 6; st++) { __unsafe_unretained NvShader *sh = pr->sh[st]; if (!sh) continue; for (int i = 0; i < sh->rf.nb; i++) if (sh->rf.b[i].kind == NVN_RB_IMG) { zskip_clear(); break; } }   // Image slots are writable: conservatively clear
    int image_write = 0;
    uint32_t shader_color_mask = 0;
    if (pr) for (int st = 0; st < 6; st++) { __unsafe_unretained NvShader *sh = pr->sh[st]; if (!sh) continue;
        if (st == 1) shader_color_mask = sh->rf.color_mask;
        for (int i = 0; i < sh->rf.nb; i++) if (sh->rf.b[i].kind == NVN_RB_IMG && sh->rf.b[i].rw) { image_write = 1; break; }
        if (image_write) break;
    }
    if (R.has_rt) for (int i = 1; i < R.rt.n && i < 8; i++)   // a slot attached twice (left unattached) really written: the writes are lost (see rt_dup_slot)
        if ((shader_color_mask >> i & 1) && ((S.chmask.mask >> (4 * i)) & 0xf) && rt_dup_slot(&R.rt, i))
            RED_ONCE(79, "color slot %d holds the same texture as an earlier slot (ser=%llu) and this draw writes it (mask %#x): these writes are lost", i, (unsigned long long)R.rt.color[i]->serial, (S.chmask.mask >> (4 * i)) & 0xf);
    for (int i = 0; i < g_nsd; i++) {
        sd_att *t = &g_sd[i];
        if (image_write) { t->wrote = 1; t->wwhy = 1; }
        else if (t->depth == 0 && t->idx >= 0 && (((S.chmask.mask >> (4 * t->idx)) & 0xf) || (shader_color_mask >> t->idx & 1))) { t->wrote = 1; t->wwhy = (S.chmask.mask >> (4 * t->idx)) & 0xf ? 2 : 3; }
        else if (t->depth == 1 && S.ds.dw) { t->wrote = 1; t->wwhy = 4; }
        else if (t->depth == 2 && S.ds.st && (S.st_wmask[0] || S.st_wmask[1])
                && (S.ds.fail_f != NVN_STENCIL_OP_KEEP || S.ds.zfail_f != NVN_STENCIL_OP_KEEP || S.ds.zpass_f != NVN_STENCIL_OP_KEEP
                    || S.ds.fail_b != NVN_STENCIL_OP_KEEP || S.ds.zfail_b != NVN_STENCIL_OP_KEEP || S.ds.zpass_b != NVN_STENCIL_OP_KEEP)) { t->wrote = 1; t->wwhy = 5; }
    }
}
// NVN's BindProgram(program, stage bits) binds per stage: the stages in the bits take that program's shaders (a null program unbinds them), the other stages stay.
// Metroid Dread's depth prepass calls BindProgram(program, vertex | fragment), then BindProgram(null, fragment): only the vertex shader remains and depth is drawn.
// Every bind used to replace S.prog as a whole, a null program meant "no program bound", and the prepass's ~140 draws per frame were all dropped: depth stayed empty, the G-buffer's depth EQUAL test passed no pixel, and scenes and characters were black.
// When every stage comes from the same program, that program is the effective one (all of Mario's and Engage's binds are like this, so nothing changes for them); otherwise a composite is made: a host-made prog_t carrying only a serial, whose NvProg in the serial table takes each stage from its source program (a null fragment = a vertex-only pipeline)
static const prog_t *bind_stages(const prog_t *p, int stages) {
    for (int st = 0; st < 5; st++) if (stages >> st & 1) S.bst[st] = p;
    const prog_t *v = S.bst[0];
    if (!v) return NULL;
    int same = 1; for (int st = 1; st < 5; st++) if (S.bst[st] && S.bst[st] != v) same = 0;
    if (same && S.bst[1] == v) return v;
    enum { NCMP = 64 };
    static prog_t cp[NCMP]; static const prog_t *ck[NCMP][5]; static uint64_t cs[NCMP][5]; static int ncp, next;
    for (int i = 0; i < ncp; i++) {
        int hit = 1; for (int st = 0; st < 5 && hit; st++) hit = ck[i][st] == S.bst[st] && (!S.bst[st] || cs[i][st] == S.bst[st]->serial);
        if (hit) return &cp[i];
    }
    int i = ncp < NCMP ? ncp++ : next++ % NCMP;
    if (cp[i].serial) serial_del(cp[i].serial);
    NvProg *o = [NvProg new];
    for (int st = 0; st < 5; st++) { ck[i][st] = S.bst[st]; cs[i][st] = S.bst[st] ? S.bst[st]->serial : 0; if (S.bst[st]) { __unsafe_unretained NvProg *src = prog_get(S.bst[st]); o->sh[st] = src ? src->sh[st] : nil; } }
    prog_fold_gs(o);   // the geometry stage came from another program (see prog_fold_gs)
    memset(&cp[i], 0, sizeof cp[i]); cp[i].magic = MAGIC('P', 'R', 'O', 'G'); cp[i].serial = nvn_new_serial(); serial_put(cp[i].serial, o);
    { static int once; if (!once++) MLOG("per-stage program binding: first combined program (vertex %s fragment %s)", o->sh[0] ? o->sh[0]->key : "-", o->sh[1] ? o->sh[1]->key : "-"); }
    return &cp[i];
}
// S.prog after S was copied back from a saved state (the precompile prediction, the fusion probe and redraw, deferred draws). A combined program is a slot of the ring above, and binds
// replayed after the save can give the slot the saved pointer names to another combination. AI: THE SOMNIUM FILES (Unity 2017.4) binds its vertex and fragment programs apart; the
// prediction over a 280-draw submission (pso_predict_rest) made more than NCMP combinations, and the draw after it ran Legacy Shaders/VertexLit's shadow vertex shader with
// PSYNC/BG_AlphaTest's shadow pixel shader, a pipeline Metal refused (the pixel shader reads two outputs that vertex shader does not write). The combination is taken again from S.bst
static void prog_resolve(void) {
    const prog_t *np = bind_stages(NULL, 0); if (np == S.prog) return;
    S.prog = np; g_pso_dirty = 1;
    static unsigned n; if (!n++ || !(n & (n - 1))) MLOG("combined program taken again after a state restore: its ring slot had been reused (%u times so far)", n);
}
static void state_reset(void) {
    memset(&S, 0, sizeof S);
    g_pso_dirty = g_dss_dirty = 1;
    for (int i = 0; i < 8; i++) S.blend[i] = (blend_st){i, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD};
    S.color = (color_st){0, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS}; S.chmask.mask = 0xffffffffu;
    S.poly = (poly_st){NVN_FACE_NONE, NVN_FRONT_FACE_CCW, NVN_POLYGON_MODE_FILL, 0};
    S.ds = (ds_st){0, 1, NVN_DEPTH_FUNC_LESS, 0, NVN_STENCIL_FUNC_ALWAYS, NVN_STENCIL_FUNC_ALWAYS, 1, 1, 1, 1, 1, 1};
    S.st_vmask[0] = S.st_vmask[1] = S.st_wmask[0] = S.st_wmask[1] = 0xff; S.dr[1] = 1;
}

// Correct the vertex format by the shader input's component type: Metal requires integer inputs to get integer formats of the same signedness, and float inputs float / normalized formats.
// Integer input (inferred at translation time as "only used as an integer", dmsh_build.py int_inputs) with an integer format: on a signedness mismatch switch to the same width with the other sign (bit pattern unchanged for 32-bit components;
// 8 / 16-bit ones would be widened the wrong way, so vconv_wanted converts them first and this switch is only the fallback when the conversion slots run out); with a 32-bit float format read it as an integer of the same width (bit pattern unchanged).
// Other mismatches (integer input with a normalized / half format, float input with an integer format) cannot keep the bit pattern: report once and keep the format.
static MTLVertexFormat vfmt_for_ct(MTLVertexFormat f, int ct, int nvn_fmt) {
    static const MTLVertexFormat U[] = { MTLVertexFormatUChar, MTLVertexFormatUChar2, MTLVertexFormatUChar3, MTLVertexFormatUChar4, MTLVertexFormatUShort, MTLVertexFormatUShort2, MTLVertexFormatUShort3, MTLVertexFormatUShort4, MTLVertexFormatUInt, MTLVertexFormatUInt2, MTLVertexFormatUInt3, MTLVertexFormatUInt4 };
    static const MTLVertexFormat I[] = { MTLVertexFormatChar, MTLVertexFormatChar2, MTLVertexFormatChar3, MTLVertexFormatChar4, MTLVertexFormatShort, MTLVertexFormatShort2, MTLVertexFormatShort3, MTLVertexFormatShort4, MTLVertexFormatInt, MTLVertexFormatInt2, MTLVertexFormatInt3, MTLVertexFormatInt4 };
    static const MTLVertexFormat F[] = { MTLVertexFormatFloat, MTLVertexFormatFloat2, MTLVertexFormatFloat3, MTLVertexFormatFloat4 };
    int iu = -1, ii = -1, iff = -1;
    for (int i = 0; i < 12; i++) { if (U[i] == f) iu = i; if (I[i] == f) ii = i; }
    for (int i = 0; i < 4; i++) if (F[i] == f) iff = i;
    if (ct == NVN_CT_UINT) { if (iu >= 0) return f; if (ii >= 0) return U[ii]; if (iff >= 0) return U[8 + iff]; }
    else if (ct == NVN_CT_INT) { if (ii >= 0) return f; if (iu >= 0) return I[iu]; if (iff >= 0) return I[8 + iff]; }
    else { if (iu < 0 && ii < 0) return f; if ((iu >= 8 && iu < 12) || (ii >= 8 && ii < 12)) return F[(iu >= 8 ? iu : ii) - 8]; }   // 32-bit integers into a float input: the same bits as floats
    // An integer input fed a normalized, half or packed format: Maxwell would hand the shader the bits of the converted float, which Metal cannot, and Metal refuses the whole pipeline
    // ("Vertex attribute … of type uint4 cannot be read using MTLAttributeFormatHalf2"). Read the same bytes as integers of the same layout instead, so the pipeline exists and the draw is kept.
    // Fire Emblem: Three Houses: KTGL's character shaders read attribute 5 as integer bits (skinning data) and static meshes bind a Half2 there; with the pipeline refused, the characters'
    // shading draws were dropped and only black silhouettes remained (gender choice, in the prologue)
    if (ct == NVN_CT_UINT || ct == NVN_CT_INT) {
        static const struct { MTLVertexFormat f; uint8_t u; } P[] = {   // u = index into U / I with the same component size and count
            { MTLVertexFormatUCharNormalized, 0 }, { MTLVertexFormatCharNormalized, 0 }, { MTLVertexFormatUChar2Normalized, 1 }, { MTLVertexFormatChar2Normalized, 1 },
            { MTLVertexFormatUChar3Normalized, 2 }, { MTLVertexFormatChar3Normalized, 2 }, { MTLVertexFormatUChar4Normalized, 3 }, { MTLVertexFormatChar4Normalized, 3 }, { MTLVertexFormatUChar4Normalized_BGRA, 3 },
            { MTLVertexFormatUShortNormalized, 4 }, { MTLVertexFormatShortNormalized, 4 }, { MTLVertexFormatHalf, 4 }, { MTLVertexFormatUShort2Normalized, 5 }, { MTLVertexFormatShort2Normalized, 5 }, { MTLVertexFormatHalf2, 5 },
            { MTLVertexFormatUShort3Normalized, 6 }, { MTLVertexFormatShort3Normalized, 6 }, { MTLVertexFormatHalf3, 6 }, { MTLVertexFormatUShort4Normalized, 7 }, { MTLVertexFormatShort4Normalized, 7 }, { MTLVertexFormatHalf4, 7 },
            { MTLVertexFormatUInt1010102Normalized, 8 }, { MTLVertexFormatInt1010102Normalized, 8 }, { MTLVertexFormatFloatRG11B10, 8 },
        };
        for (unsigned i = 0; i < sizeof P / sizeof *P; i++) if (P[i].f == f) {
            RED_ONCE(91, "vertex attribute format %#x feeds a %s input: Metal does not allow it and the conversion slots are full; the raw bytes are read with the integer format of the same layout (the bit pattern differs from Maxwell's converted one)", nvn_fmt, ct == NVN_CT_UINT ? "uint" : "int");
            return ct == NVN_CT_UINT ? U[P[i].u] : I[P[i].u];
        }
    }
    RED_ONCE(66, "vertex attribute format %#x does not match shader input type %s; the bit pattern is not preserved (original format kept)", nvn_fmt, ct == NVN_CT_UINT ? "uint" : ct == NVN_CT_INT ? "int" : "float");
    return f;
}
// Integer shader inputs fed float-type vertex formats (half, unorm / snorm 8 and 16, 10:10:10:2, 11:11:10, BGRA8). Maxwell's fetch converts by the format and the register holds float32
// bits, which such shaders take back with as_type<float> (`int_inputs` declared the input uint because its first translation only bit-cast it). Metal can neither convert into an integer
// input nor build such a pipeline, and reading the raw bytes as integers (vfmt_for_ct's fallback) gives denormals: Fire Emblem: Three Houses' and Age of Calamity's soldiers and Claude came
// out flat tan, their UVs collapsed onto one texel. So the replay converts these attributes on the CPU into float32 bits, four components with the missing ones (0, 0, 0, 1.0) as the fetch
// fills them, in the frame arena, and the pipeline reads them as UInt4 / Int4 from buffer slots of their own (VCONV_SLOT0 + n, in reflection order; slots 1-12 are unused by translated shaders)
enum { VCONV_SLOT0 = 1, VCONV_MAX = 12 };
static int vconv_ncomp(int f) {   // components of an NVN float-type vertex format that needs the conversion; 0 = not one
    switch (f) {
        case 0x01: case 0x02: case 0x05: case 0x06: case 0x07: return 1;
        case 0x0D: case 0x0E: case 0x11: case 0x12: case 0x13: return 2;
        case 0x19: case 0x1A: case 0x1D: case 0x1E: case 0x1F: case 0x3F: return 3;
        case 0x25: case 0x26: case 0x29: case 0x2A: case 0x2B: case 0x3D: case 0x69: case 0x9A: return 4;
    }
    return 0;
}
// 8- and 16-bit integer formats: Maxwell's fetch widens each component into the 32-bit register by the format's own signedness (sign for SINT, zero for UINT). Metal reads them only into
// an input of the same signedness, and vfmt_for_ct's switch to the other sign keeps the 8 / 16 bits but widens them the other way. So an input of the other signedness takes the 32-bit
// values converted here too. Fire Emblem: Three Houses: the terrain's heights are RGBA16I into a uint input; negative heights read as 32768-65535 and the ground under close-up
// cameras was never drawn (the lighting then saw depth 0 and painted fog there)
static int vconv_int(int f, int *bits, int *sgn) {   // components of an 8 / 16-bit integer format (with its component size and signedness); 0 = not one
    static const uint8_t T[16] = { 0x03, 0x04, 0x08, 0x09, 0x0F, 0x10, 0x14, 0x15, 0x1B, 0x1C, 0x20, 0x21, 0x27, 0x28, 0x2C, 0x2D };   // per component count: UI8, I8, UI16, I16
    for (int i = 0; i < 16; i++) if (T[i] == f) { *bits = i & 2 ? 16 : 8; *sgn = i & 1; return i / 4 + 1; }
    return 0;
}
// *_UI2F / *_I2F formats (0x51-0x68, R8 R16 R32 RG8 … RGBA32, unsigned then signed): integers that the fetch converts to float without normalizing (200 → 200.0). Metal has no such vertex
// format and refuses an integer format into a float input, so they are converted here too; an integer input of the same format takes the float32 bits, as from Maxwell's fetch.
// Breath of the Wild: an RG8_UI2F attribute (0x57, at the title) had no Metal format and read 0
static int vconv_i2f(int f, int *bits, int *sgn) {   // components of an integer-read-as-float format (with its component size and signedness); 0 = not one
    if (f < 0x51 || f > 0x68) return 0;
    int i = (f - 0x51) >> 1; *sgn = (f - 0x51) & 1; *bits = 8 << (i % 3); return i / 3 + 1;
}
static int vconv_size(int f) {   // bytes of one element
    int ib, is, in = vconv_int(f, &ib, &is); if (in) return in * ib / 8;
    if ((in = vconv_i2f(f, &ib, &is))) return in * ib / 8;
    switch (f) { case 0x01: case 0x02: return 1; case 0x05: case 0x06: case 0x07: case 0x0D: case 0x0E: return 2; case 0x19: case 0x1A: return 3;
                 case 0x1D: case 0x1E: case 0x1F: return 6; case 0x29: case 0x2A: case 0x2B: return 8; }
    return 4;
}
static float vconv_uf(uint32_t v, int mbits) {   // unsigned small float of R11G11B10F: 5-bit exponent (bias 15), mbits-bit mantissa
    uint32_t e = v >> mbits & 31, m = v & ((1u << mbits) - 1);
    if (!e) return ldexpf((float)m, -14 - mbits);
    if (e == 31) return m ? NAN : INFINITY;
    return ldexpf(1.0f + (float)m / (float)(1u << mbits), (int)e - 15);
}
static void vconv_elem(int f, const uint8_t *p, float o[4]) {
    int ib, is, in = vconv_int(f, &ib, &is);
    if (in) {   // integer format: the register's 32-bit value, the missing components (0, 0, 0, 1) as integers
        uint32_t u[4] = { 0, 0, 0, 1 };
        for (int i = 0; i < in; i++) {
            if (ib == 8) u[i] = is ? (uint32_t)(int32_t)(int8_t)p[i] : p[i];
            else { uint16_t h; memcpy(&h, p + 2 * i, 2); u[i] = is ? (uint32_t)(int32_t)(int16_t)h : h; } }
        memcpy(o, u, sizeof u); return; }
    o[0] = o[1] = o[2] = 0; o[3] = 1;
    if ((in = vconv_i2f(f, &ib, &is))) {   // integers read as floats: each value as a float, the missing components (0, 0, 0, 1.0)
        for (int i = 0; i < in; i++) {
            if (ib == 8) o[i] = is ? (float)(int8_t)p[i] : (float)p[i];
            else if (ib == 16) { uint16_t h; memcpy(&h, p + 2 * i, 2); o[i] = is ? (float)(int16_t)h : (float)h; }
            else { uint32_t w; memcpy(&w, p + 4 * i, 4); o[i] = is ? (float)(int32_t)w : (float)w; } }
        return; }
    int n = vconv_ncomp(f);
    for (int i = 0; i < n; i++) switch (f) {
        case 0x01: case 0x0D: case 0x19: case 0x25: o[i] = p[i] / 255.0f; break;
        case 0x02: case 0x0E: case 0x1A: case 0x26: o[i] = fmaxf((int8_t)p[i] / 127.0f, -1.0f); break;
        case 0x05: case 0x11: case 0x1D: case 0x29: { _Float16 h; memcpy(&h, p + 2 * i, 2); o[i] = (float)h; break; }
        case 0x06: case 0x12: case 0x1E: case 0x2A: { uint16_t u; memcpy(&u, p + 2 * i, 2); o[i] = u / 65535.0f; break; }
        case 0x07: case 0x13: case 0x1F: case 0x2B: { int16_t s; memcpy(&s, p + 2 * i, 2); o[i] = fmaxf(s / 32767.0f, -1.0f); break; }
        case 0x9A: o[i] = p[i == 3 ? 3 : 2 - i] / 255.0f; break;   // BGRA8 unorm: the fetch hands the shader R, G, B, A
        case 0x3D: case 0x69: case 0x3F: {
            uint32_t w; memcpy(&w, p, 4);
            if (f == 0x3F) o[i] = vconv_uf(i == 0 ? w & 0x7ff : i == 1 ? w >> 11 & 0x7ff : w >> 22, i == 2 ? 5 : 6);
            else { uint32_t bits = i == 3 ? 2 : 10, v = w >> (10 * i) & ((1u << bits) - 1);
                   if (f == 0x3D) o[i] = (float)v / (float)((1u << bits) - 1);
                   else { int32_t sv = (int32_t)(v << (32 - bits)) >> (32 - bits); o[i] = fmaxf((float)sv / (float)((1u << (bits - 1)) - 1), -1.0f); } }
            break; }
    }
}
// Shadow inputs instead of the conversion: a vertex shader with integer inputs also has a library <key>_bv (dmsh_vin.py) whose entry points declare each integer input again as a float4
// and as the integer of the other sign, at Metal attribute indices 16-30, and OR the three into the input the body reads. A pipeline sends each attribute to the declaration of its
// format's class (half / normalized formats to the float one, 8 / 16-bit integers to the one of their own sign) and the zero buffer to the others, and Metal's fetch then gives the
// shader Maxwell's register bits: no CPU conversion, whatever mix of formats the pipeline has (Age of Calamity's soldiers feed Half2 beside RGBA8UI and Short2 to one shader; the
// conversion was about a quarter of its replay thread in battle). Transform feedback captures (main0_tf) keep the conversion. DM_NVN_VCONV_CPU=1 converts on the CPU as before
static int vin_cpu(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_NVN_VCONV_CPU"); v = e && !strcmp(e, "1"); if (v) MLOG("DM_NVN_VCONV_CPU=1: integer vertex inputs fed formats of another class are converted on the CPU (no shadow inputs)"); } return v; }
static os_unfair_lock g_vin_lock = OS_UNFAIR_LOCK_INIT;
static uint64_t g_vin_loaded, g_vin_failed, g_vin_psos;   // shadow-input libraries loaded / not usable, pipelines built with them
static id<MTLFunction> vin_fn(NvShader *vs) {   // the shadow-input entry point, loaded on first need; a failure is final for the shader (it keeps the conversion)
    if (!vs->rf.has_vin || vin_cpu()) return nil;
    os_unfair_lock_lock(&g_vin_lock);
    if (!vs->vfn_state) {
        vs->vfn_state = 2;
        size_t kl = strlen(vs->key); NSString *path = [NSString stringWithFormat:@"%@/%.*s_bv.metallib", g_msl_dir, (int)(kl > 2 ? kl - 2 : kl), vs->key];
        NSError *e = nil; id<MTLLibrary> lib = lib_from_file(path, &e); id<MTLFunction> f = lib && vs->ventry ? fn4_note([lib newFunctionWithName:vs->ventry], lib, vs->ventry, nil) : nil;
        if (f && f.functionConstantsDictionary.count) { MTLFunctionConstantValues *cv = [MTLFunctionConstantValues new]; f = fn4_note([lib newFunctionWithName:vs->ventry constantValues:cv error:&e], lib, vs->ventry, cv); }   // as shader_load
        const char *why = !lib ? "no library" : !f ? "no entry point" : NULL;
        if (f && vs->rf.nb && [f newArgumentEncoderWithBufferIndex:vs->rf.abi].encodedLength != vs->arg_len) { why = "its argument buffer differs from the base library's"; f = nil; }
        if (f) { vs->vfn = f; vs->vfn_state = 1; g_vin_loaded++; }
        else if (g_vin_failed++ < 8) MLOG("[RED] shadow-input library of %s not usable (%s%s%s): its integer inputs keep the CPU conversion", vs->key, why, e ? ": " : "", e ? e.localizedDescription.UTF8String : "");
    }
    id<MTLFunction> f = vs->vfn_state == 1 ? vs->vfn : nil;
    os_unfair_lock_unlock(&g_vin_lock);
    return f;
}
// The pulling entry point (the same name as the shader's own, main0 or main0_z, in <key>_bp: dmsh_pull.py), loaded on first need; a failure is final for the shader (its draws
// keep waiting for their pipelines as before)
static os_unfair_lock g_pull_lock = OS_UNFAIR_LOCK_INIT;
// Loads the pulling entry into vs (pfn_state 1, or 2 when it is not usable). pull_fn holds g_pull_lock around it; shader_prefetch_rest calls it on a shader no other thread has yet
static void pull_load(NvShader *vs) {
    {
        vs->pfn_state = 2;
        size_t kl = strlen(vs->key); NSString *path = [NSString stringWithFormat:@"%@/%.*s_bp.metallib", g_msl_dir, (int)(kl > 2 ? kl - 2 : kl), vs->key];
        NSError *e = nil; id<MTLLibrary> lib = lib_from_file(path, &e); id<MTLFunction> f = lib && vs->ventry ? fn4_note([lib newFunctionWithName:vs->ventry], lib, vs->ventry, nil) : nil;
        if (f && f.functionConstantsDictionary.count) { MTLFunctionConstantValues *cv = [MTLFunctionConstantValues new]; f = fn4_note([lib newFunctionWithName:vs->ventry constantValues:cv error:&e], lib, vs->ventry, cv); }
        const char *why = !lib ? "no library" : !f ? "no entry point" : NULL;
        if (f && vs->rf.nb && [f newArgumentEncoderWithBufferIndex:vs->rf.abi].encodedLength != vs->arg_len) { why = "its argument buffer differs from the base library's"; f = nil; }
        if (f) { vs->pfn = f; vs->pfn_state = 1; }
        else { static _Atomic int n; if (n++ < 8) MLOG("[RED] pulling library of %s not usable (%s%s%s): its draws wait for their pipelines", vs->key, why, e ? ": " : "", e ? e.localizedDescription.UTF8String : ""); }
    }
}
static id<MTLFunction> pull_fn(NvShader *vs) {
    if (!vs->has_pull || vs->layer_out) return nil;
    os_unfair_lock_lock(&g_pull_lock);
    if (!vs->pfn_state) pull_load(vs);
    id<MTLFunction> f = vs->pfn_state == 1 ? vs->pfn : nil;
    os_unfair_lock_unlock(&g_pull_lock);
    return f;
}
static inline int vin_fmt(const pso_key *k, int a) { return a >= 16 ? 0 : k ? k->attr[a].fmt : a < S.nattr ? S.attr[a].fmt : 0; }   // as pso_key_fill fills the key
// The component type input v takes its attribute as: the class of the attribute's format when a shadow of that class exists (on), else the declared type
static int vin_ct(const nvn_refl *rf, int v, int f, int on) {
    int ct = rf->v[v].ct, ib, is;
    if (!on || ct == NVN_CT_FLOAT) return ct;
    int want = vconv_ncomp(f) ? NVN_CT_FLOAT : vconv_int(f, &ib, &is) ? (is ? NVN_CT_INT : NVN_CT_UINT) : ct;
    return want != ct && rf->v[v].sh[want] != 0xff ? want : ct;
}
// Whether this pipeline (k, or the current state at bind time: both give the same answer) uses the shadow inputs: some input takes a shadow and the library loads. Asked by pso_desc
// and vconv_bind alike, so the converted attributes and the pipeline's buffer slots agree
static int vin_on(NvShader *vs, const pso_key *k, int tf) {
    const nvn_refl *rf = &vs->rf;
    if (tf || !rf->has_vin || vin_cpu()) return 0;
    int need = 0; for (int v = 0; v < rf->nv && !need; v++) need = vin_ct(rf, v, vin_fmt(k, rf->v[v].nvn), 1) != rf->v[v].ct;
    return need && vin_fn(vs) != nil;
}
// A float input fed an 8 / 16-bit integer format takes the same 32-bit register values through a Float4 attribute, which Metal passes through bit for bit: Monster Hunter Rise's
// skinned vertex shaders (40609cd5c1443df4, 1369a37113da0012) read RGBA8UI attribute 5 as flags (as_type<int>(x) & 2 ...) and also as a float, so the build leaves the input float
static int vconv_wanted(const nvn_refl *rf, int v, const pso_key *k, int on) {   // does Metal input v of this pipeline take converted data (on: it uses the shadow inputs, vin_on)
    int a = rf->v[v].nvn, ct = rf->v[v].ct, ib, is;
    if (a >= 16) return 0;
    int f = k ? k->attr[a].fmt : S.attr[a].fmt;
    if (vconv_i2f(f, &ib, &is)) return 1;   // whatever the input type
    if (ct == NVN_CT_FLOAT) return vconv_int(f, &ib, &is) > 0;
    if (ct != NVN_CT_UINT && ct != NVN_CT_INT) return 0;
    if (vin_ct(rf, v, f, on) != ct) return 0;   // a shadow input takes it
    return vconv_ncomp(f) || (vconv_int(f, &ib, &is) && is != (ct == NVN_CT_INT));
}
// nv elements from src (stride bytes apart) into d, four 32-bit values each. The 8 / 16-bit integer and the half formats (Age of Calamity's soldiers: Short2, Half2) have loops
// of their own with the format decoded once; vconv_elem decodes it for every element
static void vconv_fill(int f, uint64_t src, uint32_t stride, uint64_t nv, float *d) {
    const uint8_t *p = (const uint8_t *)(uintptr_t)src; uint32_t *u = (uint32_t *)d; int ib, is, in = vconv_int(f, &ib, &is);
    if (in) {   // as vconv_elem: the register's 32-bit value, the missing components (0, 0, 0, 1)
        for (uint64_t i = 0; i < nv; i++, p += stride, u += 4) {
            u[0] = u[1] = u[2] = 0; u[3] = 1;
            for (int c = 0; c < in; c++) {
                if (ib == 8) u[c] = is ? (uint32_t)(int32_t)(int8_t)p[c] : p[c];
                else { uint16_t h; memcpy(&h, p + 2 * c, 2); u[c] = is ? (uint32_t)(int32_t)(int16_t)h : h; } } }
        return; }
    if (f == 0x05 || f == 0x11 || f == 0x1D || f == 0x29) {   // R16F … RGBA16F
        int n = vconv_ncomp(f);
        for (uint64_t i = 0; i < nv; i++, p += stride, d += 4) {
            d[0] = d[1] = d[2] = 0; d[3] = 1;
            for (int c = 0; c < n; c++) { _Float16 h; memcpy(&h, p + 2 * c, 2); d[c] = (float)h; } }
        return; }
    for (uint64_t i = 0; i < nv; i++) vconv_elem(f, p + i * stride, d + 4 * i);
}
// Profile vconv_range (Age of Calamity): convert only the vertices each draw reads. KTGL binds whole mesh pools (up to ~590,000 vertices) and draws parts of them, so
// converting the bound range came to millions of vertices a frame in a battle with hundreds of soldiers: 24 of the 32 ms the replay took per frame on the Mac (26 fps at
// a 30 or a 60 fps target). The range is first / count, or for a direct indexed draw the indices' smallest and largest plus the base vertex; the conversion is bound as many
// vertices before its start as the range starts at. The indices' range is kept while their pages stay unwritten (write-protected as packaged textures, nvn_dirty_protect),
// and never trusted again once a GPU copy, update or fill is queued on them (pend_add). Keeping the conversions too did not pay: the soldiers' attributes change every frame.
// DM_NVN_VCONV_CHECK=1 scans the indices again at every reuse and reports differences. DM_NVN_VCONV_RANGE=0 / 1 overrides the profile
enum { VC_RGN = 1 << 16 };   // bits of the region filter
typedef struct { uint64_t ibuf; int32_t count, itype; } ir_key;
typedef struct { uint32_t mn, mx; uint64_t stamp, seen, lo, n; const void *span; int gpuw; } ir_ent;   // gpuw: a GPU write was queued on the indices: never trusted again
static nvn_hmap g_ir = {.klen = sizeof(ir_key)};
static uint64_t g_vc_rgn[VC_RGN / 64], g_vc_stat, g_vc_conv, g_vc_whole, g_vc_checks, g_vc_bad;   // g_vc_rgn: 1 MB regions holding a kept index range (hashed); g_vc_conv / g_vc_whole: vertices converted / in the bound ranges
static int vc_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_NVN_VCONV_RANGE"); v = e && *e ? atoi(e) != 0 : DM_GAME_VCONV_RANGE; MLOG("vertex conversion of the draw's vertices only: %s", v ? "on" : "off"); } return v; }
static int vc_check(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_NVN_VCONV_CHECK"); v = e && *e && atoi(e); } return v; }
static inline uint32_t vc_rgn(uint64_t r) { return (uint32_t)((r * 0x9E3779B97F4A7C15ull) >> 48) & (VC_RGN - 1); }
static void vc_rgn_mark(uint64_t lo, uint64_t n) { for (uint64_t r = lo >> 20; r <= (lo + n - 1) >> 20; r++) { uint32_t b = vc_rgn(r); g_vc_rgn[b >> 6] |= 1ull << (b & 63); } }
static int vc_rgn_hit(uint64_t lo, uint64_t n) { for (uint64_t r = lo >> 20; r <= (lo + n - 1) >> 20; r++) { uint32_t b = vc_rgn(r); if (g_vc_rgn[b >> 6] >> (b & 63) & 1) return 1; } return 0; }
static void vc_gpu_write(uint64_t dst, uint64_t n) {   // pend_add: a GPU write queued on [dst, dst + n)
    if (!n || !g_ir.n || !vc_rgn_hit(dst, n)) return;
    for (uint32_t i = 0; i < g_ir.cap; i++) { ir_ent *e = g_ir.vals[i]; if (e && dst < e->lo + e->n && dst + n > e->lo) e->gpuw = 1; }
}
static void ix_minmax(const void *p, int isz, int32_t count, uint32_t *mn, uint32_t *mx) {
    uint32_t a = UINT32_MAX, b = 0;
    if (isz == 1) for (int32_t i = 0; i < count; i++) { uint32_t x = ((const uint8_t *)p)[i]; if (x < a) a = x; if (x > b) b = x; }
    else if (isz == 2) for (int32_t i = 0; i < count; i++) { uint32_t x = ((const uint16_t *)p)[i]; if (x < a) a = x; if (x > b) b = x; }
    else for (int32_t i = 0; i < count; i++) { uint32_t x = ((const uint32_t *)p)[i]; if (x < a) a = x; if (x > b) b = x; }
    *mn = a; *mx = b;
}
// Smallest and largest index of a direct indexed draw whose indices no GPU write queued in this submission covers; 0 = not known (the whole bound range is converted)
static int ix_range(const p_draw_idx *di, uint32_t *mn, uint32_t *mx) {
    int isz = di->itype == 0 ? 1 : di->itype == 1 ? 2 : 4; uint64_t n = (uint64_t)di->count * (uint64_t)isz;
    if (di->ind || di->mdi_cnt || di->count <= 0 || !di->ibuf || pend_hits(di->ibuf, n)) return 0;
    const void *p = (const void *)(uintptr_t)di->ibuf;
    ir_key k = {di->ibuf, di->count, di->itype}; ir_ent *e = nvn_hm_get(&g_ir, &k); uint64_t now = nvn_dirty_now();
    if (e && e->gpuw) return 0;
    if (e && (now == e->seen || !nvn_dirty_since_span(e->span, p, (size_t)n, e->stamp))) {
        e->seen = now; *mn = e->mn; *mx = e->mx;
        if (vc_check()) { uint32_t a, b; ix_minmax(p, isz, di->count, &a, &b); g_vc_checks++;
            if ((a != e->mn || b != e->mx) && g_vc_bad++ < 20) MLOG("[RED] vertex conversion: the kept index range of %#llx (%d indices) is %u-%u, the indices now %u-%u", (unsigned long long)di->ibuf, di->count, e->mn, e->mx, a, b); }
        return 1;
    }
    const void *span = nvn_dirty_span(p, (size_t)n);
    if (!span) return 0;   // not in a tracked memory pool
    if (!e) { if (g_ir.n >= 1 << 16) { nvn_hm_clear(&g_ir, free); memset(g_vc_rgn, 0, sizeof g_vc_rgn); } e = calloc(1, sizeof *e); nvn_hm_put(&g_ir, &k, e); }
    nvn_dirty_protect(p, (size_t)n); e->stamp = e->seen = nvn_dirty_now(); e->span = span; e->lo = di->ibuf; e->n = n; vc_rgn_mark(e->lo, e->n);   // protect, take the stamp, then read (as pkg_upload)
    ix_minmax(p, isz, di->count, &e->mn, &e->mx); *mn = e->mn; *mx = e->mx;
    return 1;
}
// The vertex ids a direct draw reads: [*lo, *hi]; 0 = not known (indirect, multi-draw, indices not known)
static int vc_draw_range(const p_draw *dw, const p_draw_idx *di, int64_t *lo, int64_t *hi) {
    if (dw) { if (dw->ind || dw->mdi_cnt || dw->count <= 0) return 0; *lo = dw->first; *hi = (int64_t)dw->first + dw->count - 1; return 1; }
    uint32_t mn, mx; if (!di || !ix_range(di, &mn, &mx)) return 0;
    *lo = (int64_t)mn + di->base_vertex; *hi = (int64_t)mx + di->base_vertex; return 1;
}
static void vc_stat(void) {   // every 3600 submissions
    if (g_sub_serial - g_vc_stat < 3600) return;
    if (g_vc_stat) MLOG("vertex conversion: %.0f vertices per submission (the bound ranges: %.0f), %u index ranges kept%s; shadow-input libraries loaded %llu (not usable %llu), pipelines built with them %llu", g_vc_conv / 3600.0, g_vc_whole / 3600.0, g_ir.n,
        vc_check() ? [NSString stringWithFormat:@"; checked %llu, %llu differed", (unsigned long long)g_vc_checks, (unsigned long long)g_vc_bad].UTF8String : "",
        (unsigned long long)g_vin_loaded, (unsigned long long)g_vin_failed, (unsigned long long)g_vin_psos);
    g_vc_stat = g_sub_serial; g_vc_conv = g_vc_whole = 0;
}
// Per submission, one conversion per (range, format, layout): KTGL draws the same mesh several times a frame (depth, shadow, G-buffer)
static struct { uint64_t addr, size, serial, v0, nv; uint32_t fmt, off, stride; uint64_t gpu; } g_vconv[64]; static int g_vconv_next;
static void vconv_bind(id<MTL4RenderCommandEncoder> en, NvShader *vs, int tf, const p_draw *dw, const p_draw_idx *di) {   // tf: a transform feedback capture (no shadow inputs)
    const nvn_refl *rf = &vs->rf; int slot = VCONV_SLOT0, vc = vc_on(), rk = -1, on = vin_on(vs, NULL, tf); int64_t rlo = 0, rhi = 0;   // rk: the draw's vertex range is known (-1 = not looked up yet)
    vc_stat();
    for (int v = 0; v < rf->nv && slot < VCONV_SLOT0 + VCONV_MAX; v++) {
        if (!vconv_wanted(rf, v, NULL, on) || rf->v[v].nvn >= S.nattr) continue;
        int a = rf->v[v].nvn, st = S.attr[a].stream, f = S.attr[a].fmt; uint32_t off = S.attr[a].offset, stride = st < 16 ? S.stream[st].stride : 0;
        uint64_t addr = st < 16 ? S.vbuf[st].address : 0, size = st < 16 ? S.vbuf[st].size : 0, gpu = 0;
        uint64_t es = (uint64_t)vconv_size(f), v0 = 0, nv = stride == 0 ? 1 : size >= off + es ? (size - off - es) / stride + 1 : 0, whole = nv;
        if (vc && nv && stride && !S.stream[st].divisor) {   // per-vertex stream: only the vertices the draw reads
            if (rk < 0) rk = vc_draw_range(dw, di, &rlo, &rhi);
            int64_t a0 = rlo < 0 ? 0 : rlo, a1 = rhi < (int64_t)nv - 1 ? rhi : (int64_t)nv - 1;
            if (rk && a0 <= a1) { v0 = (uint64_t)a0; nv = (uint64_t)(a1 - a0 + 1); }
        }
        for (int i = 0; i < 64 && !gpu; i++) if (g_vconv[i].serial == g_sub_serial && g_vconv[i].addr == addr && g_vconv[i].size == size && g_vconv[i].v0 == v0 && g_vconv[i].nv == nv && g_vconv[i].fmt == (uint32_t)f && g_vconv[i].off == off && g_vconv[i].stride == stride) gpu = g_vconv[i].gpu;
        if (!gpu && addr) {
            if (nv && nv * 16 <= (4u << 20)) {
                id<MTLBuffer> b; size_t o = arena_alloc((size_t)nv * 16, &b); vconv_fill(f, addr + off + v0 * stride, stride, nv, (float *)((uint8_t *)b.contents + o)); gpu = b.gpuAddress + o;
                g_vc_conv += nv; g_vc_whole += whole;
                int c = g_vconv_next++ % 64; g_vconv[c].addr = addr; g_vconv[c].size = size; g_vconv[c].v0 = v0; g_vconv[c].nv = nv; g_vconv[c].serial = g_sub_serial; g_vconv[c].fmt = (uint32_t)f; g_vconv[c].off = off; g_vconv[c].stride = stride; g_vconv[c].gpu = gpu;
                int ib, is;
                if (vconv_i2f(f, &ib, &is)) { static int once; if (!once++) MLOG("vertex attribute format %#x (integers read as floats, which Metal has no vertex format for): converted on the CPU at replay and bound separately", f); }
                else if (rf->v[v].ct == NVN_CT_FLOAT) RED_ONCE(99, "vertex attribute format %#x (8 / 16-bit integer) feeds a float input: the 32-bit register values Maxwell's fetch makes are bound as float bit patterns, converted on the CPU at replay", f);
                else if (vconv_ncomp(f)) RED_ONCE(89, "vertex attribute format %#x feeds an integer input: converted to float32 bit patterns on the CPU at replay and bound separately (the conversion Maxwell does on vertex fetch)", f);
                else RED_ONCE(96, "vertex attribute format %#x (8 / 16-bit integer) feeds an integer input of the opposite signedness: extended to 32 bits with the format's own signedness on the CPU at replay and bound separately", f);
            } else if (nv) RED_ONCE(90, "vertex attribute conversion: a range of %llu vertices exceeds the per-frame staging (format %#x)", (unsigned long long)nv, f);
        }
        if (gpu) enc_r_vbuf(en, (NSUInteger)slot, gpu - v0 * 16);   // vertex id v0 reads the first converted element
        slot++;
    }
}
static MTLVertexFormat vfmt(int f) {
    switch (f) {
        case 0x01: return MTLVertexFormatUCharNormalized; case 0x02: return MTLVertexFormatCharNormalized; case 0x03: return MTLVertexFormatUChar; case 0x04: return MTLVertexFormatChar;
        case 0x05: return MTLVertexFormatHalf; case 0x06: return MTLVertexFormatUShortNormalized; case 0x07: return MTLVertexFormatShortNormalized;
        case 0x08: return MTLVertexFormatUShort; case 0x09: return MTLVertexFormatShort; case 0x0A: return MTLVertexFormatFloat; case 0x0B: return MTLVertexFormatUInt; case 0x0C: return MTLVertexFormatInt;
        case 0x0D: return MTLVertexFormatUChar2Normalized; case 0x0E: return MTLVertexFormatChar2Normalized; case 0x0F: return MTLVertexFormatUChar2; case 0x10: return MTLVertexFormatChar2;
        case 0x11: return MTLVertexFormatHalf2; case 0x12: return MTLVertexFormatUShort2Normalized; case 0x13: return MTLVertexFormatShort2Normalized; case 0x14: return MTLVertexFormatUShort2;
        case 0x15: return MTLVertexFormatShort2; case 0x16: return MTLVertexFormatFloat2; case 0x17: return MTLVertexFormatUInt2; case 0x18: return MTLVertexFormatInt2;
        case 0x19: return MTLVertexFormatUChar3Normalized; case 0x1A: return MTLVertexFormatChar3Normalized; case 0x1B: return MTLVertexFormatUChar3; case 0x1C: return MTLVertexFormatChar3;
        case 0x1D: return MTLVertexFormatHalf3; case 0x1E: return MTLVertexFormatUShort3Normalized; case 0x1F: return MTLVertexFormatShort3Normalized; case 0x20: return MTLVertexFormatUShort3;
        case 0x21: return MTLVertexFormatShort3; case 0x22: return MTLVertexFormatFloat3; case 0x23: return MTLVertexFormatUInt3; case 0x24: return MTLVertexFormatInt3;
        case 0x25: return MTLVertexFormatUChar4Normalized; case 0x26: return MTLVertexFormatChar4Normalized; case 0x27: return MTLVertexFormatUChar4; case 0x28: return MTLVertexFormatChar4;
        case 0x29: return MTLVertexFormatHalf4; case 0x2A: return MTLVertexFormatUShort4Normalized; case 0x2B: return MTLVertexFormatShort4Normalized; case 0x2C: return MTLVertexFormatUShort4;
        case 0x2D: return MTLVertexFormatShort4; case 0x2E: return MTLVertexFormatFloat4; case 0x2F: return MTLVertexFormatUInt4; case 0x30: return MTLVertexFormatInt4;
        case 0x3D: return MTLVertexFormatUInt1010102Normalized; case 0x69: return MTLVertexFormatInt1010102Normalized; case 0x3F: return MTLVertexFormatFloatRG11B10; case 0x9A: return MTLVertexFormatUChar4Normalized_BGRA;
    }
    int ib, is, in = vconv_i2f(f, &ib, &is);   // integers read as floats: the integer format of the same layout (the pipeline reads vconv_bind's conversion)
    if (in) {
        static const MTLVertexFormat U[3][4] = { { MTLVertexFormatUChar, MTLVertexFormatUChar2, MTLVertexFormatUChar3, MTLVertexFormatUChar4 }, { MTLVertexFormatUShort, MTLVertexFormatUShort2, MTLVertexFormatUShort3, MTLVertexFormatUShort4 }, { MTLVertexFormatUInt, MTLVertexFormatUInt2, MTLVertexFormatUInt3, MTLVertexFormatUInt4 } };
        static const MTLVertexFormat I[3][4] = { { MTLVertexFormatChar, MTLVertexFormatChar2, MTLVertexFormatChar3, MTLVertexFormatChar4 }, { MTLVertexFormatShort, MTLVertexFormatShort2, MTLVertexFormatShort3, MTLVertexFormatShort4 }, { MTLVertexFormatInt, MTLVertexFormatInt2, MTLVertexFormatInt3, MTLVertexFormatInt4 } };
        return (is ? I : U)[ib == 8 ? 0 : ib == 16 ? 1 : 2][in - 1];
    }
    return MTLVertexFormatInvalid;
}
static MTLBlendFactor bfac(int f) {
    switch (f) { case 1: return MTLBlendFactorZero; case 2: return MTLBlendFactorOne; case 3: return MTLBlendFactorSourceColor; case 4: return MTLBlendFactorOneMinusSourceColor;
        case 5: return MTLBlendFactorSourceAlpha; case 6: return MTLBlendFactorOneMinusSourceAlpha; case 7: return MTLBlendFactorDestinationAlpha; case 8: return MTLBlendFactorOneMinusDestinationAlpha;
        case 9: return MTLBlendFactorDestinationColor; case 10: return MTLBlendFactorOneMinusDestinationColor; case 11: return MTLBlendFactorSourceAlphaSaturated;
        case 0x10: return MTLBlendFactorSource1Color; case 0x11: return MTLBlendFactorOneMinusSource1Color; case 0x12: return MTLBlendFactorSource1Alpha; case 0x13: return MTLBlendFactorOneMinusSource1Alpha;
        case 0x61: return MTLBlendFactorBlendColor; case 0x62: return MTLBlendFactorOneMinusBlendColor; case 0x63: return MTLBlendFactorBlendAlpha; case 0x64: return MTLBlendFactorOneMinusBlendAlpha; }
    return MTLBlendFactorOne;
}
// An advanced blend mode (blend_st.adv, hle_nvn.c) replaces the blend functions. The Porter-Duff modes with uncorrelated overlap are sums of the source and destination weighted by
// 1, 0, As, Ad and their complements (NV_blend_equation_advanced, with premultiplied colours), which the fixed-function factors give exactly; the result goes through the ADD equation.
// A source that is not premultiplied is multiplied by As first, which only a source factor of ONE can take (SRC_ALPHA). The other modes (multiply, screen, overlay, HSL, ...) need
// the destination colour in the shader and keep the state's own functions, with a red line
static blend_st adv_blend(blend_st b) {
    if (!b.adv) return b;
    enum { Z = NVN_BLEND_FUNC_ZERO, O = NVN_BLEND_FUNC_ONE, SA = NVN_BLEND_FUNC_SRC_ALPHA, ISA = NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA, DA = NVN_BLEND_FUNC_DST_ALPHA, IDA = NVN_BLEND_FUNC_ONE_MINUS_DST_ALPHA };
    // {source factor, destination factor} for colour, then for alpha
    static const uint8_t pd[][4] = {
        [NVN_BLEND_ADVANCED_MODE_ZERO] = {Z, Z, Z, Z},        [NVN_BLEND_ADVANCED_MODE_SRC] = {O, Z, O, Z},           [NVN_BLEND_ADVANCED_MODE_DST] = {Z, O, Z, O},
        [NVN_BLEND_ADVANCED_MODE_SRC_OVER] = {O, ISA, O, ISA}, [NVN_BLEND_ADVANCED_MODE_DST_OVER] = {IDA, O, IDA, O},
        [NVN_BLEND_ADVANCED_MODE_SRC_IN] = {DA, Z, DA, Z},     [NVN_BLEND_ADVANCED_MODE_DST_IN] = {Z, SA, Z, SA},
        [NVN_BLEND_ADVANCED_MODE_SRC_OUT] = {IDA, Z, IDA, Z},  [NVN_BLEND_ADVANCED_MODE_DST_OUT] = {Z, ISA, Z, ISA},
        [NVN_BLEND_ADVANCED_MODE_SRC_ATOP] = {DA, ISA, Z, O},  [NVN_BLEND_ADVANCED_MODE_DST_ATOP] = {IDA, SA, O, Z},
        [NVN_BLEND_ADVANCED_MODE_XOR] = {IDA, ISA, IDA, ISA},  [NVN_BLEND_ADVANCED_MODE_PLUS] = {O, O, O, O},
        [NVN_BLEND_ADVANCED_MODE_PLUS_CLAMPED] = {O, O, O, O}, [NVN_BLEND_ADVANCED_MODE_PLUS_CLAMPED_ALPHA] = {O, O, O, O},
    };
    const uint8_t *f = b.adv < sizeof pd / sizeof *pd && pd[b.adv][0] ? pd[b.adv] : NULL;
    if (!f || b.adv_ovl || (b.adv_npm && f[0] != O && f[0] != Z)) {
        static uint64_t seen; if (!(seen >> (b.adv & 63) & 1)) { seen |= 1ull << (b.adv & 63);
            LOG("[RED] advanced blend mode %#x (overlap %d, %spremultiplied source) has no fixed-function equivalent: the state's blend functions are used", (unsigned)b.adv, (int)b.adv_ovl, b.adv_npm ? "not " : ""); }
        return b;
    }
    b.src_rgb = b.adv_npm && f[0] == O ? SA : f[0]; b.dst_rgb = f[1]; b.src_a = f[2]; b.dst_a = f[3];
    b.eq_rgb = b.eq_a = NVN_BLEND_EQUATION_ADD;
    return b;
}
static MTLBlendOperation bop(int e) { switch (e) { case 2: return MTLBlendOperationSubtract; case 3: return MTLBlendOperationReverseSubtract; case 4: return MTLBlendOperationMin; case 5: return MTLBlendOperationMax; } return MTLBlendOperationAdd; }

// Pipeline: cached by (program, target formats, blend / write mask, vertex layout, sample count)
static const uint8_t *g_pso_lmap; static id<MTLFunction> g_pso_fsfn; static pso_fz_desc g_pso_fz, g_fz_desc;
static os_unfair_lock g_fz_fn_lock = OS_UNFAIR_LOCK_INIT;
static NSMutableDictionary<NSString *, id<MTLFunction>> *g_bf_fn_cache;
static NSMutableDictionary<NSString *, id<MTLLibrary>> *g_bf_lib_cache;
static nvn_hmap g_pso = {.klen = sizeof(pso_key)}, g_dss;
static nvn_hmap g_pso_fail = {.klen = sizeof(pso_key)};   // pipelines that failed to build: remembered and never retried (previously every draw recompiled it, over 10,000 times in one iPhone session)
static const char g_tf_mark;
static char g_geo_mark;
#define GEO_MARK ((const void *)&g_geo_mark)
#define TF_MARK ((const void *)&g_tf_mark)   // pso_key.fsfn of a transform feedback capture pipeline (tf_pso)
static int mtl_fmt_uint(MTLPixelFormat f) {
    switch (f) { case MTLPixelFormatR8Uint: case MTLPixelFormatR16Uint: case MTLPixelFormatR32Uint: case MTLPixelFormatRG8Uint: case MTLPixelFormatRG16Uint: case MTLPixelFormatRG32Uint:
        case MTLPixelFormatRGBA8Uint: case MTLPixelFormatRGB10A2Uint: case MTLPixelFormatRGBA16Uint: case MTLPixelFormatRGBA32Uint: return 1; default: return 0; }
}
// Every colour attachment the fragment shader writes is an unsigned integer format (at least one): the pipeline takes its integer-output entry point (fn_u). One that writes a float
// attachment as well keeps main0, and a mismatch still fails with the red line naming the shader
static int pso_all_uint(const pso_key *k, NvShader *fs, int merged) {
    int n = 0;
    for (int i = 0; i < 8; i++) {
        if (!k->cfmt[i]) continue;
        int l = merged ? k->lmap[i] : i;
        if (l == 0xff || (fs->rf.color_known && !(fs->rf.color_mask >> l & 1))) continue;   // an output the shader does not write
        if (!mtl_fmt_uint((MTLPixelFormat)k->cfmt[i])) return 0;
        n++;
    }
    return n > 0;
}
static int pf_int_kind(MTLPixelFormat f);
// Some written colour attachments are unsigned integer formats and some are not, and the integer ones are exactly the outputs the mixed entry point returns as integers (fn_m, mask_m:
// Pokémon Sword's G-buffer, colour 1 RGBA16Uint between two float targets)
static int pso_mixed_uint(const pso_key *k, NvShader *fs, int merged) {
    int ints = 0, floats = 0;
    for (int i = 0; i < 8; i++) {
        if (!k->cfmt[i]) continue;
        int l = merged ? k->lmap[i] : i;
        if (l == 0xff || (fs->rf.color_known && !(fs->rf.color_mask >> l & 1))) continue;   // an output the shader does not write
        int u = mtl_fmt_uint((MTLPixelFormat)k->cfmt[i]);
        if (u != (l < 8 && (fs->mask_m >> l & 1))) return 0;
        if (u) ints++; else floats++;
    }
    return ints && floats;
}
static MTLRenderPipelineDescriptor *pso_desc(const pso_key *k, NvShader *vs, NvShader *fs, id<MTLFunction> fsfn, int merged) {
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = vs->fn; d.fragmentFunction = fsfn ? fsfn : fs ? fs->fn : nil;
    // Diagnostic DM_NVN_HILITE=fragment shader key,...: these draws use a solid-color fragment instead (keys 1–6 in order magenta / green / cyan / yellow / blue / white), to see where the geometry lands
    // Pipelines are built on several threads at once: the keys and the library are set up once (a lazy setup raced, and a thread used a library another had just replaced: Hades II, 6 keys)
    if (fs) { static char keys[6][24]; static int nk; static id<MTLLibrary> hl; static dispatch_once_t once;
        dispatch_once(&once, ^{ const char *e = getenv("DM_NVN_HILITE");
            for (const char *q = e; q && *q && nk < 6; q = strchr(q, ',') ? strchr(q, ',') + 1 : q + strlen(q)) { size_t n = strcspn(q, ","); if (n > 23) n = 23; memcpy(keys[nk], q, n); keys[nk++][n] = 0; }
            if (!nk) return;
            NSError *err = nil; hl = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\nstruct O { float4 c0 [[color(0)]]; float4 c1 [[color(1)]]; float4 c2 [[color(2)]]; };\n"
                "fragment O hl0() { return {float4(1,0,1,1),float4(1,0,1,1),float4(1,0,1,1)}; } fragment O hl1() { return {float4(0,1,0,1),float4(0,1,0,1),float4(0,1,0,1)}; }\n"
                "fragment O hl2() { return {float4(0,1,1,1),float4(0,1,1,1),float4(0,1,1,1)}; } fragment O hl3() { return {float4(1,1,0,1),float4(1,1,0,1),float4(1,1,0,1)}; }\n"
                "fragment O hl4() { return {float4(0,0,1,1),float4(0,0,1,1),float4(0,0,1,1)}; } fragment O hl5() { return {float4(1,1,1,1),float4(1,1,1,1),float4(1,1,1,1)}; }\n" options:nil error:&err];
            if (!hl) MLOG("[RED] HILITE shader compile failed %s", err.localizedDescription.UTF8String); });
        for (int i = 0; i < nk; i++) if (!strncmp(fs->key, keys[i], strlen(keys[i]))) {
            if (hl) d.fragmentFunction = [hl newFunctionWithName:[NSString stringWithFormat:@"hl%d", i]];
            break; } }
    if (fs && fs->fn_u && !fsfn && d.fragmentFunction == fs->fn && pso_all_uint(k, fs, merged)) d.fragmentFunction = fs->fn_u;   // integer colour targets (dmsh_build.py fs_uint)
    else if (fs && fs->fn_m && !fsfn && d.fragmentFunction == fs->fn && pso_mixed_uint(k, fs, merged)) d.fragmentFunction = fs->fn_m;   // integer and float colour targets
    if (vs->layer_out && k->fsfn != GEO_MARK) d.inputPrimitiveTopology = vs->geo_output == 1 ? MTLPrimitiveTopologyClassPoint : vs->geo_output == 6 ? MTLPrimitiveTopologyClassLine : MTLPrimitiveTopologyClassTriangle;
    if (k->fsfn == TF_MARK || k->fsfn == GEO_MARK) { d.vertexFunction = k->fsfn == GEO_MARK ? vs->geo_fn : vs->tf_fn; d.fragmentFunction = nil; d.rasterizationEnabled = NO; }   // transform feedback capture (tf_draw)
    d.rasterSampleCount = k->samples ? k->samples : 1;
    d.alphaToCoverageEnabled = k->a2c;
    for (int i = 0; i < 8; i++) {
        if (!k->cfmt[i]) continue;
        MTLRenderPipelineColorAttachmentDescriptor *c = d.colorAttachments[i];
        c.pixelFormat = (MTLPixelFormat)k->cfmt[i];
        int l = merged ? k->lmap[i] : i;
        uint32_t m = l == 0xff ? 0 : (k->chmask >> (4 * l)) & 0xf;
        // Output numbers the fragment shader does not write: zero write mask, no blending. Maxwell writes only the targets in the shader's output map and leaves the others untouched (whatever the blend state and channel mask);
        // on Metal an attachment the shader does not write has undefined contents, and with blending enabled that undefined value is blended in. Super Mario 3D World + Bowser's Fury's volume draws after the G-buffer (50d813761ca3f1f9 and others) write only outputs 0 and 2,
        // while targets 1 (normals) and 3 (pre-lighting) have SRC_ALPHA blending and all channels enabled
        int unwritten = fs && fs->rf.color_known && !fsfn && l != 0xff && !(fs->rf.color_mask >> l & 1);
        if (unwritten) m = 0;
        c.writeMask = (m & 1 ? MTLColorWriteMaskRed : 0) | (m & 2 ? MTLColorWriteMaskGreen : 0) | (m & 4 ? MTLColorWriteMaskBlue : 0) | (m & 8 ? MTLColorWriteMaskAlpha : 0);
        // Integer targets: no blending. Maxwell ignores the blend enable on them; Metal refuses the pipeline (an assertion even without the API validation layer): Luigi's Mansion 3's integer G-buffer (PR #105)
        if (l != 0xff && k->blend_en >> l & 1 && !unwritten && !pf_int_kind(c.pixelFormat)) {
            c.blendingEnabled = YES; blend_st bl = adv_blend(k->blend[l]);
            c.sourceRGBBlendFactor = bfac(bl.src_rgb); c.destinationRGBBlendFactor = bfac(bl.dst_rgb);
            c.sourceAlphaBlendFactor = bfac(bl.src_a); c.destinationAlphaBlendFactor = bfac(bl.dst_a);
            c.rgbBlendOperation = bop(bl.eq_rgb); c.alphaBlendOperation = bop(bl.eq_a);
        }
    }
    d.depthAttachmentPixelFormat = (MTLPixelFormat)k->dfmt; d.stencilAttachmentPixelFormat = (MTLPixelFormat)k->sfmt;
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    const nvn_refl *rf = &vs->rf; int nconv = 0, on = vin_on(vs, k, k->fsfn == TF_MARK || k->fsfn == GEO_MARK);
    if (on) { d.vertexFunction = vs->vfn; g_vin_psos++; }   // the shadow-input entry point (see vin_on)
    #define ZERO_ATTR(i, ct_) do { vd.attributes[i].format = (ct_) == NVN_CT_UINT ? MTLVertexFormatUInt4 : (ct_) == NVN_CT_INT ? MTLVertexFormatInt4 : MTLVertexFormatFloat4; vd.attributes[i].offset = 0; vd.attributes[i].bufferIndex = 30; \
                                  vd.layouts[30].stride = 16; vd.layouts[30].stepFunction = MTLVertexStepFunctionConstant; vd.layouts[30].stepRate = 0; } while (0)   // reads the zero buffer
    for (int v = 0; v < rf->nv; v++) {
        int loc = rf->v[v].loc, a = rf->v[v].nvn;   // Metal slot loc ← NVN attribute a
        MTLVertexFormat vf = a < 16 ? vfmt(k->attr[a].fmt) : MTLVertexFormatInvalid;
        // With the shadow inputs the attribute goes to the declaration of its format's class (at) and the input's other declarations read the zero buffer
        int ct = on ? vin_ct(rf, v, vin_fmt(k, a), 1) : rf->v[v].ct, at = ct == rf->v[v].ct ? loc : rf->v[v].sh[ct];
        if (on) { if (at != loc) ZERO_ATTR(loc, rf->v[v].ct); for (int q = 0; q < 3; q++) if (rf->v[v].sh[q] != 0xff && rf->v[v].sh[q] != at) ZERO_ATTR(rf->v[v].sh[q], q); }
        // When a >= S.nattr, k.attr[a] keeps the 0 from memset, vfmt(0) is Invalid, the same fallback branch is taken and the RED_ONCE condition of fmt being non-0 does not hold.
        if (a < 16 && k->attr[a].fmt && vf == MTLVertexFormatInvalid) RED_ONCE(15, "vertex attribute format %#x not mapped", k->attr[a].fmt);
        if (vf == MTLVertexFormatInvalid) { ZERO_ATTR(at, ct); continue; }
        int st = k->attr[a].stream;
        if (vconv_wanted(rf, v, k, on) && nconv < VCONV_MAX) {   // converted on the CPU at draw time (vconv_bind): the register's 32-bit values, 4 components, its own slot
            int slot = VCONV_SLOT0 + nconv++;
            vd.attributes[loc].format = rf->v[v].ct == NVN_CT_UINT ? MTLVertexFormatUInt4 : rf->v[v].ct == NVN_CT_INT ? MTLVertexFormatInt4 : MTLVertexFormatFloat4; vd.attributes[loc].offset = 0; vd.attributes[loc].bufferIndex = slot;
            vd.layouts[slot].stride = 16;
            vd.layouts[slot].stepFunction = k->stream[st].stride == 0 ? MTLVertexStepFunctionConstant : k->stream[st].divisor ? MTLVertexStepFunctionPerInstance : MTLVertexStepFunctionPerVertex;
            vd.layouts[slot].stepRate = k->stream[st].stride == 0 ? 0 : k->stream[st].divisor ? k->stream[st].divisor : 1;
            continue;
        }
        vf = vfmt_for_ct(vf, ct, k->attr[a].fmt);
        vd.attributes[at].format = vf; vd.attributes[at].offset = k->attr[a].offset; vd.attributes[at].bufferIndex = VB_SLOT(st);
        vd.layouts[VB_SLOT(st)].stride = k->stream[st].stride ? k->stream[st].stride : 4;
        vd.layouts[VB_SLOT(st)].stepFunction = k->stream[st].stride == 0 ? MTLVertexStepFunctionConstant : k->stream[st].divisor ? MTLVertexStepFunctionPerInstance : MTLVertexStepFunctionPerVertex;
        vd.layouts[VB_SLOT(st)].stepRate = k->stream[st].stride == 0 ? 0 : k->stream[st].divisor ? k->stream[st].divisor : 1;
    }
    #undef ZERO_ATTR
    d.vertexDescriptor = vd;
    return d;
}
