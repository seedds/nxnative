// nvn_mtl.m, part: indirect
// Draws Metal cannot take as they are: indirect and 8-bit index draws, masked multi-draws, transform feedback and the last-use cache (lc_*).
// Included by nvn_mtl.m in its original order, as one translation unit: statics are shared with the other parts.
// ---------- Indirect draws Metal cannot take as they are ----------
// Quads and triangle fans (DrawArraysIndirect), and indexed quads / fans / 8-bit indices (DrawElementsIndirect). The {count, instances, first, ...} record is usually written by the GPU earlier
// in the frame (Hyrule Warriors: Age of Calamity draws GPU-driven effects as indirect quads in its first battle), so the CPU cannot expand it at replay. A compute pass converts each record into
// MTLDrawIndexedPrimitivesIndirectArguments in the frame arena, and the draw becomes an indexed indirect draw: arrays index the shared g_quad_ib / g_fan_ib with first as the base vertex,
// indexed ones are expanded into a 32-bit list of at most IXC_CAP indices. The compute pass cannot run inside a render pass. When the first such draw is met, ixc_run converts it and every
// such draw after it up to the next buffer write outside a render pass (copy, update / fill, texture → buffer, dispatch: they bump g_wr_epoch), so each record is read after its last writer
// and the later draws find their conversion. Only a draw met inside an open render pass with no conversion yet splits that pass.
#define IXC_MAX 128
#define IXC_CAP (3 * 16384)
typedef struct { uint64_t ind, ibuf; int32_t prim, itype; uint64_t args, ixb; uint32_t ixlen; } ixc_ent;   // args 0: the record could not be converted
static ixc_ent g_ixc[IXC_MAX]; static int g_ixc_n; static uint64_t g_ixc_epoch, g_ixc_sub, g_ixc_runs, g_ixc_splits;
static const char k_ixc_msl[] =
"#include <metal_stdlib>\nusing namespace metal;\n"
"struct ajob { device const uint *rec; device uint *args; uint prim, cap; };\n"   // arrays record {count, instances, first, base instance}; cap: indices in the shared index buffer
"kernel void ixc_arrays(device const ajob *j [[buffer(0)]], uint i [[threadgroup_position_in_grid]]) {\n"
"  device const uint *s = j[i].rec; uint n = s[0], c = j[i].prim == 7 ? n / 4 * 6 : n >= 3 ? (n - 2) * 3 : 0;\n"
"  device uint *d = j[i].args; d[0] = min(c, j[i].cap); d[1] = s[1]; d[2] = 0; d[3] = s[2]; d[4] = s[3]; }\n"
"struct ejob { device const uint *rec; device const uchar *ib; device uint *out; device uint *args; uint prim, isz, ilen, cap; };\n"   // elements record {count, instances, first index, base vertex, base instance}; ilen: bytes readable at ib
"kernel void ixc_expand(constant ejob &j [[buffer(0)]], uint i [[thread_position_in_grid]]) {\n"
"  uint n = j.rec[0], m = min(j.prim == 7 ? n / 4 * 6 : j.prim == 6 ? (n >= 3 ? (n - 2) * 3 : 0) : n, j.cap);\n"
"  if (i == 0) { j.args[0] = m; j.args[1] = j.rec[1]; j.args[2] = 0; j.args[3] = j.rec[3]; j.args[4] = j.rec[4]; }\n"
"  if (i >= m) return;\n"
"  uint t = i / 3, c = i % 3, p = j.prim == 7 ? t / 2 * 4 + (c ? c + (t & 1) : 0) : j.prim == 6 ? (c ? t + c : 0) : i, x = j.rec[2] + p;\n"   // quads (q, q+1+h, q+2+h), fans (0, t+1, t+2), as the CPU expansion in draw
"  j.out[i] = (ulong)x * j.isz + j.isz > j.ilen ? 0u : j.isz == 1 ? uint(j.ib[x]) : j.isz == 2 ? uint(((device const ushort *)j.ib)[x]) : ((device const uint *)j.ib)[x]; }\n";
static int ixc_wanted(const p_draw *dw, const p_draw_idx *di) {
    if (dw) return dw->ind && (dw->prim == 6 || dw->prim == 7);
    return di->ind && (di->prim == 6 || di->prim == 7 || di->itype == 0);
}
static const ixc_ent *ixc_find(const p_draw *dw, const p_draw_idx *di) {
    if (g_ixc_epoch != g_wr_epoch || g_ixc_sub != g_sub_serial) return NULL;
    uint64_t ind = dw ? dw->ind : di->ind, ib = dw ? 0 : di->ibuf; int prim = dw ? dw->prim : di->prim, it = dw ? -1 : di->itype;
    for (int i = 0; i < g_ixc_n; i++) if (g_ixc[i].ind == ind && g_ixc[i].ibuf == ib && g_ixc[i].prim == prim && g_ixc[i].itype == it) return &g_ixc[i];
    return NULL;
}
static void ixc_take(const p_draw *dw, const p_draw_idx *di) {
    if (g_ixc_n < IXC_MAX && !ixc_find(dw, di)) g_ixc[g_ixc_n++] = (ixc_ent){dw ? dw->ind : di->ind, dw ? 0 : di->ibuf, dw ? dw->prim : di->prim, dw ? -1 : di->itype, 0, 0, 0};
}
static void ixc_run(const p_draw *dw, const p_draw_idx *di) {
    static id<MTLComputePipelineState> ps[2]; static id<MTL4ArgumentTable> at; static int failed;
    if (failed) return;
    if (!at) { NSError *e = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(k_ixc_msl) options:nil error:&e];
        for (int k = 0; k < 2 && lib; k++) { id<MTLFunction> f = [lib newFunctionWithName:k ? @"ixc_expand" : @"ixc_arrays"]; ps[k] = f ? [g_dev newComputePipelineStateWithFunction:f error:&e] : nil; }
        MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new]; ad.maxBufferBindCount = 1; at = ps[0] && ps[1] ? rec_table([g_dev newArgumentTableWithDescriptor:ad error:&e]) : nil;
        if (!at) { failed = 1; MLOG("[RED] indirect draw conversion: cannot create the compute pipeline %s (indirect draws of quads / fans / 8-bit indices are skipped)", e.localizedDescription.UTF8String); return; } }
    if (g_ixc_epoch != g_wr_epoch || g_ixc_sub != g_sub_serial || g_ixc_n > IXC_MAX - 8) { g_ixc_n = 0; g_ixc_epoch = g_wr_epoch; g_ixc_sub = g_sub_serial; }
    int n0 = g_ixc_n; ixc_take(dw, di);
    if (g_exec_next && g_ret_n <= 8) {   // Later draws in replay order (current list → callers → the rest of this submission), up to the next buffer write
        fz_cursor cur = {.p = g_exec_next, .ret_n = g_ret_n}; memcpy(cur.ret, g_ret, sizeof(*g_ret) * (size_t)g_ret_n);
        const uint8_t *q; op_hdr h; const void *a;
        for (int guard = 0; guard < 200000 && g_ixc_n < IXC_MAX && fz_next(&cur, &q, &h, &a); guard++) {
            if (h.op == OP_DISPATCH || h.op == OP_COPY_B2B || h.op == OP_UPDATE_UB || h.op == OP_CLEAR_BUF || h.op == OP_COPY_T2B) break;
            p_draw dt; p_draw_idx it; const p_draw *d = h.op == OP_DRAW ? draw_rec(a, h.size, &dt) : NULL; const p_draw_idx *x = h.op == OP_DRAW_IDX ? draw_idx_rec(a, h.size, &it) : NULL;
            if (d && ixc_wanted(d, NULL)) ixc_take(d, NULL);
            else if (x && ixc_wanted(NULL, x)) ixc_take(NULL, x);
        }
    }
    if (g_enc) { g_ixc_splits++; PD("M %llu indirect draw conversion: render pass split here", g_stat_pass); rp_end(); }
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    int n = g_ixc_n - n0, na = 0; if (n <= 0) return;
    typedef struct { uint64_t rec, args; uint32_t prim, cap; } ajob;
    typedef struct { uint64_t rec, ib, out, args; uint32_t prim, isz, ilen, cap; } ejob;
    id<MTLBuffer> ab; size_t ao = arena_alloc((size_t)n * (32 + sizeof(ajob)), &ab); uint64_t ag = ab.gpuAddress + ao;   // n argument records (32 bytes apart), then the arrays jobs
    ajob *aj = (ajob *)((uint8_t *)ab.contents + ao + (size_t)n * 32);
    id<MTL4ComputeCommandEncoder> ce = enc_c_begin(NULL); [ce setArgumentTable:at];
    for (int i = 0; i < n; i++) {
        ixc_ent *e = &g_ixc[n0 + i]; uint64_t rec, ib = 0; void *mb; uint64_t mo; e->args = 0;
        if (!mtl_gpu_addr(e->ind, e->itype < 0 ? 16 : 20, &mb, &mo, &rec)) continue;
        if (e->itype < 0) {   // DrawArraysIndirect quads / fans: the shared index buffers, one thread per record
            if (e->prim == 7) quad_ib_make(); else fan_ib_make();
            e->ixb = e->prim == 7 ? g_quad_gpu : g_fan_gpu; e->ixlen = (uint32_t)(e->prim == 7 ? g_quad_ib.length : g_fan_ib.length);
            aj[na++] = (ajob){rec, ag + 32 * (uint64_t)i, (uint32_t)e->prim, e->ixlen / 4}; e->args = ag + 32 * (uint64_t)i;
        } else {   // DrawElementsIndirect: expanded into a 32-bit list, one thread per output index
            if (!mtl_gpu_addr(e->ibuf, 1, &mb, &mo, &ib)) continue;
            id<MTLBuffer> ob; size_t oo = arena_alloc(IXC_CAP * 4 + sizeof(ejob), &ob);
            ejob *j = (ejob *)((uint8_t *)ob.contents + oo + IXC_CAP * 4);
            *j = (ejob){rec, ib, ob.gpuAddress + oo, ag + 32 * (uint64_t)i, (uint32_t)e->prim, e->itype == 0 ? 1u : e->itype == 1 ? 2u : 4u, (uint32_t)MIN(((__bridge id<MTLBuffer>)mb).length - mo, 0xffffffffull), IXC_CAP};
            [at setAddress:ob.gpuAddress + oo + IXC_CAP * 4 atIndex:0]; [ce setComputePipelineState:ps[1]];
            [ce dispatchThreadgroups:MTLSizeMake(IXC_CAP / 64, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
            e->ixb = ob.gpuAddress + oo; e->ixlen = IXC_CAP * 4; e->args = ag + 32 * (uint64_t)i;
            { static int once; if (!once++) MLOG("indirect indexed draw (primitive %d, %d-byte indices) expanded to 32-bit indices on the GPU, at most %d", e->prim, e->itype == 0 ? 1 : e->itype == 1 ? 2 : 4, IXC_CAP); }
        }
    }
    if (na) { [at setAddress:ag + (uint64_t)n * 32 atIndex:0]; [ce setComputePipelineState:ps[0]]; [ce dispatchThreadgroups:MTLSizeMake((NSUInteger)na, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)]; }
    enc_c_end(ce);
    args_dirty_all();
    if (!g_ixc_runs++) MLOG("indirect draw conversion: first time, %d draws (records for quads / fans / 8-bit indices become indexed indirect arguments in a compute pass before the render pass)", n);
    else if (g_ixc_runs % 3600 == 0) MLOG("indirect draw conversion: %llu times, render pass split %llu times", (unsigned long long)g_ixc_runs, (unsigned long long)g_ixc_splits);
    // DM_NVN_IXCLOG=<n>: logs the first n conversions (record → arguments) once the GPU has run them
    static int lg = -1; if (lg < 0) { const char *v = getenv("DM_NVN_IXCLOG"); lg = v ? atoi(v) : 0; }
    for (int i = 0; i < n && lg > 0; i++) { const ixc_ent *e = &g_ixc[n0 + i]; if (!e->args) continue; lg--;
        const uint32_t *src = (const uint32_t *)(uintptr_t)e->ind, *arg = (const uint32_t *)((const uint8_t *)ab.contents + ao + 32 * (size_t)i);
        int prim = e->prim, it = e->itype; unsigned long long fr = (unsigned long long)atomic_load(&g_cur_frame), pass = g_stat_pass, splits = g_ixc_splits; id<MTLBuffer> keep = ab;
        [g_commit_blocks addObject:^{ (void)keep; MLOG("indirect draw conversion frame %llu pass %llu: primitive %d index type %d record %u,%u,%u,%u → arguments %u,%u,%u,%d,%u (splits so far %llu)", fr, pass, prim, it, src[0], src[1], src[2], src[3], arg[0], arg[1], arg[2], (int)arg[3], arg[4], splits); }]; }
}
// ---------- 8-bit indices written by the GPU earlier in the submission ----------
// Metal has no 8-bit index type, so draw widens 8-bit indices to 16-bit on the CPU at replay. When a buffer write encoded earlier in this submission (copy / update / fill,
// see pend_hits) covers them, the CPU reads the bytes from before that write (Tears of the Kingdom). Those draws are widened on the GPU instead: a compute pass reads the
// 8-bit indices from a 16-byte-aligned address, with the byte shift as a constant (index data may start anywhere), and writes 16-bit ones into the frame arena, which lives
// until the submission completes. As in ixc_run, a compute pass cannot run inside a render pass: the first such draw widens itself and every such draw after it up to the next
// buffer write outside a render pass, so only a draw met inside an open render pass with no widening yet splits it. Draws whose indices no write covers keep the CPU widening.
// Adapted from MacNativeGames' widen_u8_gpu (github.com/ly05010419/MacNativeGames, MIT).
#define WIDE_MAX 256
typedef struct { uint64_t ibuf; int32_t count; uint64_t gpu; } wide_ent;   // gpu: the 16-bit indices in the frame arena (0: not widened)
static wide_ent g_wide[WIDE_MAX]; static int g_wide_n; static uint64_t g_wide_epoch, g_wide_sub, g_wide_runs, g_wide_draws, g_wide_splits;
static const char k_wide_msl[] =
"#include <metal_stdlib>\nusing namespace metal;\n"
"struct wjob { device const uchar *src; device ushort *dst; uint n, shift; };\n"   // src: 16-byte aligned, the indices start shift bytes after it
"kernel void wide_u8(device const wjob &j [[buffer(0)]], uint i [[thread_position_in_grid]]) { if (i < j.n) j.dst[i] = ushort(j.src[j.shift + i]); }\n";   // as the CPU widening (0xff stays 0x00ff)
static int wide_wanted(const p_draw_idx *di) { return !di->ind && di->itype == 0 && di->prim != 6 && di->prim != 7 && di->count > 0 && pend_hits(di->ibuf, (uint64_t)di->count); }
static const wide_ent *wide_find(const p_draw_idx *di) {
    if (g_wide_epoch != g_wr_epoch || g_wide_sub != g_sub_serial) return NULL;
    for (int i = 0; i < g_wide_n; i++) if (g_wide[i].ibuf == di->ibuf && g_wide[i].count == di->count) return &g_wide[i];
    return NULL;
}
static void wide_take(const p_draw_idx *di) { if (g_wide_n < WIDE_MAX && !wide_find(di)) g_wide[g_wide_n++] = (wide_ent){di->ibuf, di->count, 0}; }
static void wide_run(const p_draw_idx *di) {
    static id<MTLComputePipelineState> ps; static id<MTL4ArgumentTable> at; static int failed;
    if (failed) return;
    if (!at) { NSError *e = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(k_wide_msl) options:nil error:&e]; id<MTLFunction> f = lib ? [lib newFunctionWithName:@"wide_u8"] : nil;
        ps = f ? [g_dev newComputePipelineStateWithFunction:f error:&e] : nil;
        MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new]; ad.maxBufferBindCount = 1; at = ps ? rec_table([g_dev newArgumentTableWithDescriptor:ad error:&e]) : nil;
        if (!at) { failed = 1; MLOG("[RED] 8-bit index GPU expansion: cannot create the compute pipeline %s (the CPU expands them as before)", e.localizedDescription.UTF8String); return; } }
    if (g_wide_epoch != g_wr_epoch || g_wide_sub != g_sub_serial || g_wide_n > WIDE_MAX - 8) { g_wide_n = 0; g_wide_epoch = g_wr_epoch; g_wide_sub = g_sub_serial; }
    int n0 = g_wide_n; wide_take(di);
    if (g_exec_next && g_ret_n <= 8) {   // Later draws in replay order (current list → callers → the rest of this submission), up to the next buffer write
        fz_cursor cur = {.p = g_exec_next, .ret_n = g_ret_n}; memcpy(cur.ret, g_ret, sizeof(*g_ret) * (size_t)g_ret_n);
        const uint8_t *q; op_hdr h; const void *a;
        for (int guard = 0; guard < 200000 && g_wide_n < WIDE_MAX && fz_next(&cur, &q, &h, &a); guard++) {
            if (h.op == OP_DISPATCH || h.op == OP_COPY_B2B || h.op == OP_UPDATE_UB || h.op == OP_CLEAR_BUF || h.op == OP_COPY_T2B) break;
            p_draw_idx it; const p_draw_idx *x = h.op == OP_DRAW_IDX ? draw_idx_rec(a, h.size, &it) : NULL;
            if (x && wide_wanted(x)) wide_take(x);
        }
    }
    int n = g_wide_n - n0; if (n <= 0) return;
    if (g_enc) { g_wide_splits++; PD("M %llu 8-bit index GPU expansion: render pass split here", g_stat_pass); rp_end(); }
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    typedef struct { uint64_t src, dst; uint32_t n, shift; } wjob;
    id<MTLBuffer> jb; size_t jo = arena_alloc((size_t)n * sizeof(wjob), &jb); wjob *jp = (wjob *)((uint8_t *)jb.contents + jo);
    id<MTL4ComputeCommandEncoder> ce = enc_c_begin(NULL); [ce setArgumentTable:at]; [ce setComputePipelineState:ps];
    for (int i = 0; i < n; i++) {
        wide_ent *e = &g_wide[n0 + i]; void *mb; uint64_t mo, src; e->gpu = 0;
        if (!mtl_gpu_addr(e->ibuf, (uint64_t)e->count, &mb, &mo, &src)) continue;   // the draw reports it (RED_ONCE 22)
        id<MTLBuffer> ob; size_t oo = arena_alloc((size_t)e->count * 2, &ob);
        jp[i] = (wjob){src & ~15ull, ob.gpuAddress + oo, (uint32_t)e->count, (uint32_t)(src & 15)};
        [at setAddress:jb.gpuAddress + jo + (uint64_t)i * sizeof(wjob) atIndex:0];
        [ce dispatchThreadgroups:MTLSizeMake(((NSUInteger)e->count + 63) / 64, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        e->gpu = ob.gpuAddress + oo;
    }
    enc_c_end(ce);
    args_dirty_all();
    g_wide_draws += (uint64_t)n;
    if (!g_wide_runs++) MLOG("8-bit index GPU expansion: first time, %d draws (the indices have GPU-side writes in this submission; a compute pass expands them to 16 bits before the render pass)", n);
    else if (g_wide_runs % 3600 == 0) MLOG("8-bit index GPU expansion: %llu times, %llu draws, render pass split %llu times", (unsigned long long)g_wide_runs, (unsigned long long)g_wide_draws, (unsigned long long)g_wide_splits);
}
// Diagnostic DM_NVN_UBP_CHECK=1: each shadow made from memory (ubp_get) is checked against the GPU. The pass ends, the deferred updates land, the replay waits for the GPU (as an indirect-count
// multi-draw does), and the bytes the draw would have read from the shadow are compared with memory, which now holds exactly what the GPU reads. The draw then goes on as without the shadows
static void mdi_wait(void);
static uint64_t g_ubpk_same, g_ubpk_diff;
static int ubp_check_on(void) { static int v = -1; if (v < 0) { v = getenv("DM_NVN_UBP_CHECK") != NULL; if (v) MLOG("DM_NVN_UBP_CHECK: every uniform shadow made from memory is compared with the GPU's memory after the updates land (slow)"); } return v; }
static void ubp_check(uint64_t lo, uint64_t hi, const uint8_t *sh) {
    ubc_wait(g_ubp_ready);   // the shadow ubp_get just returned may still be filled by the copy worker
    size_t n = (size_t)(hi - lo); uint8_t *tmp = malloc(n); memcpy(tmp, sh, n);
    mdi_wait();
    const uint8_t *m = (const uint8_t *)(uintptr_t)lo; size_t bad = 0, first = 0;
    for (size_t i = 0; i < n; i++) if (tmp[i] != m[i]) { if (!bad++) first = i; }
    if (bad) { g_ubpk_diff++; if (g_ubpk_diff <= 10) MLOG("[RED] uniform shadow check: [%#llx +%zu] differs from the GPU's memory in %zu bytes, first at +%zu (shadow %02x, memory %02x) frame %llu pass %llu",
        (unsigned long long)lo, n, bad, first, tmp[first], m[first], (unsigned long long)atomic_load(&g_cur_frame), g_stat_pass); }
    else g_ubpk_same++;
    if ((g_ubpk_same + g_ubpk_diff) % 2000 == 0) MLOG("uniform shadow check: %llu equal, %llu different", (unsigned long long)g_ubpk_same, (unsigned long long)g_ubpk_diff);
    free(tmp);
}
// Whether this draw needs more from the inline updates deferred in the open pass than the uniform shadows give (see mtl_sub_update): a uniform binding they cover only in part whose rest
// cannot be read from memory (ubp_get; a binding counts only as far as the shader can read it, cb_len), a bindless handle in such a part, or their data read as vertices, indices, indirect
// arguments or a storage buffer. Then the pass ends first and the updates land before the draw, as before
static int ubd_conflict_sh(NvShader *sh, int st) {   // one stage's bindings (encode_args binds the vertex, fragment and compute stages)
    for (int i = 0; i < sh->rf.nb; i++) { const nvn_rbind *b = &sh->rf.b[i]; if (b->reg >= 16) continue;
        const p_bind_buf *u = b->kind == NVN_RB_SBUFP ? &S.ssbo[st][b->reg] : &S.ubo[st][b->reg];
        if (!u->addr) continue;
        if (b->kind == NVN_RB_CBUF) { uint64_t g, ul = cb_len(sh, b->reg, u->size); const uint8_t *c;
            if (ubd_hits(u->addr, u->addr + ul) && !ubd_covers(u->addr, u->addr + ul)) {
                if (!ubp_get(u->addr, u->addr + ul, &g, &c)) { g_ubp_refused++; return 1; }
                if (ubp_check_on()) { ubp_check(u->addr, u->addr + ul, c); return 1; } } }
        else if (b->kind == NVN_RB_TEXU || b->kind == NVN_RB_SAMPU || b->kind == NVN_RB_SBUFU) { uint64_t h = u->addr + (b->n ? 0 : b->off), n = b->n ? 8ull * b->n : b->kind == NVN_RB_SBUFU ? 8 : 4; uint64_t g; const uint8_t *c;   // an array: its handles at 8k
            if (ubd_hits(h, h + n) && !ubd_covers(h, h + n) && !ubp_get(h, h + n, &g, &c)) { g_ubp_refused++; return 1; } }
        else if (b->kind == NVN_RB_SBUFP || b->kind == NVN_RB_UBOP) { if (ubd_hits(u->addr, u->addr + (u->size ? u->size : 1))) return 1; } }
    return 0;
}
static int ubd_conflict(NvProg *pr, const p_draw *dw, const p_draw_idx *di) {
    for (int s_ = 0; s_ < 16; s_++) if (S.vbuf[s_].address && ubd_hits(S.vbuf[s_].address, S.vbuf[s_].address + S.vbuf[s_].size)) return 1;
    if (di && di->ibuf && ubd_hits(di->ibuf, di->ibuf + (uint64_t)di->count * 4)) return 1;
    uint64_t ind = dw ? dw->ind : di ? di->ind : 0; if (ind && ubd_hits(ind, ind + 32)) return 1;
    for (int st = 0; pr && st < 2; st++) if (pr->sh[st] && ubd_conflict_sh(pr->sh[st], st)) return 1;
    return 0;
}
// The same for a dispatch: updates kept past their pass (DM_NVN_UB_LAZY) land before a dispatch that reads or writes their memory through a storage buffer or its indirect arguments, or reads a
// part of a uniform binding the shadows cannot give (else they would land after it, at the next copy into the encoder it shares, dispatch_enc). Uniform reads they cover take shadows (encode_args)
static int ubd_conflict_cs(NvShader *cs, uint64_t ind) { return (ind && ubd_hits(ind, ind + 12)) || ubd_conflict_sh(cs, 5); }
// Draws of an indirect-count multi-draw (hle_nvn.c nvnCommandBufferMultiDrawElementsIndirectCount): draw i is made only while i < the u32 count the game's GPU culling wrote earlier in the
// submission. Metal cannot skip draws by a count in GPU memory. Most such draws are masked on the GPU (mdx_run below); the rest (quads, fans and 8-bit indices, whose records ixc_run
// converts, and records or counts in no Metal buffer) wait here: before the first such draw after GPU writes (a dispatch, copy, update or fill since the last wait, g_wr_epoch; or a new
// submission) the replay ends the render pass, commits what it has encoded and waits for it, then reads the count on the CPU; later draws of the group use the same value
static id<MTLSharedEvent> g_mdi_ev; static uint64_t g_mdi_v, g_mdi_waits, g_mdi_epoch = ~0ull, g_mdi_sub, g_mdi_addr; static uint32_t g_mdi_count;
static void mdi_wait(void) {
    rp_end(); ubd_land();
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    up_flush(); res_commit();
    [g_cb endCommandBuffer];
    rec_commit(g_cb, nil);
    if (!g_mdi_ev) g_mdi_ev = [g_dev newSharedEvent];
    rec_signal(g_mdi_ev, ++g_mdi_v); rec_drain();
    uint64_t w0 = ws_t0(); int ok = [g_mdi_ev waitUntilSignaledValue:g_mdi_v timeoutMS:10000]; if (ws_on()) ws_put(WS_MDI, w0, 1);
    if (!ok) RED_ONCE(97, "indirect-count multi-draw: the GPU did not finish within 10 s; the count is read anyway");
    g_cb = rec_cb_new();
    [g_cb beginCommandBufferWithAllocator:g_alloc4[g_alloc_cur]];
    if (!g_mdi_waits++) MLOG("indirect-count multi-draw: first wait for the GPU-written count at frame %llu pass %llu", (unsigned long long)atomic_load(&g_cur_frame), g_stat_pass);
}
static int mdi_live(uint64_t cnt, int i) {
    if (cnt != g_mdi_addr || g_mdi_epoch != g_wr_epoch || g_mdi_sub != g_sub_serial) {
        if (g_mdi_epoch != g_wr_epoch || g_mdi_sub != g_sub_serial) { mdi_wait(); g_mdi_epoch = g_wr_epoch; g_mdi_sub = g_sub_serial; }
        uint32_t v = 0;
        if (nvn_host_readable(cnt, 4)) memcpy(&v, (const void *)(uintptr_t)cnt, 4);
        else RED_ONCE(98, "indirect-count multi-draw: the count at %#llx is in an unmapped page of a sparse pool, nothing drawn", (unsigned long long)cnt);
        g_mdi_addr = cnt; g_mdi_count = v;
    }
    return (uint32_t)i < g_mdi_count;
}
// ---------- Indirect-count multi-draws masked on the GPU ----------
// The wait above held the submitting thread for the GPU at every GPU-driven pass: in Monster Hunter Rise's Kamura Village, 14 of the 20 ms a submission took on an iPhone were that wait
// (6 ms of work), and the game ran at 36–54 fps. Instead a compute pass copies each such draw's record into the frame arena, or zeros for a draw past the count, and the draw reads the copy:
// past the count it draws nothing, and the CPU never waits. As in ixc_run, a compute pass cannot run inside a render pass: at the first such draw with no copy yet, mdx_run copies it and every
// such draw after it in replay order up to the next write that could change a record or count (a dispatch, copy, fill or texture → buffer copy, which bump g_mdx_epoch; a uniform update only
// when it covers one: g_mdx_stop), so only a draw met inside an open render pass splits it. Copies are found by record and count address in an open-addressing table, valid for one g_mdx_tag
// (Kamura Village draws the same records with two counts between the same writes: keyed by the record alone, the second multi-draw found no copy and waited for the GPU each frame)
#define MDX_SLOTS 16384   // a power of two; a scan fills at most a quarter
typedef struct { uint64_t ind, cnt, out, tag; } mdx_ent;   // out: GPU address of the copy, 0 when the record or count is in no Metal buffer (those draws wait, mdi_live)
static mdx_ent g_mdx[MDX_SLOTS]; static int g_mdx_n; static uint64_t g_mdx_tag = 1, g_mdx_ep = ~0ull, g_mdx_sub, g_mdx_runs, g_mdx_splits, g_mdx_draws, g_mdx_fast; static const uint8_t *g_mdx_stop;
static const char k_mdx_msl[] =
"#include <metal_stdlib>\nusing namespace metal;\n"
"struct mjob { device const uint *rec; device const uint *cnt; device uint *out; uint i, words; };\n"   // draw i's record (words long), its multi-draw's count, the copy (null: padding)
"kernel void mdx_mask(device const mjob *j [[buffer(0)]], uint t [[thread_position_in_grid]]) {\n"
"  mjob m = j[t]; if (!m.out) return; bool live = m.i < *m.cnt;\n"
"  for (uint k = 0; k < m.words; k++) m.out[k] = live ? m.rec[k] : 0u; }\n";
static int mdx_wanted(const p_draw *dw, const p_draw_idx *di) { return dw ? dw->mdi_cnt && !ixc_wanted(dw, NULL) : di->mdi_cnt && !ixc_wanted(NULL, di); }
static mdx_ent *mdx_slot(uint64_t ind, uint64_t cnt) {   // its entry in this tag, or the free slot for it
    for (uint64_t h = ((ind ^ cnt * 0xD6E8FEB86659FD93ull) * 0x9E3779B97F4A7C15ull) >> 50;; h = (h + 1) & (MDX_SLOTS - 1)) { mdx_ent *e = &g_mdx[h]; if (e->tag != g_mdx_tag || (e->ind == ind && e->cnt == cnt)) return e; }
}
static int mdx_find(uint64_t ind, uint64_t cnt, uint64_t *out) {   // → 1 when mdx_run has taken this draw (*out: its copy, or 0)
    if (g_mdx_ep != g_mdx_epoch || g_mdx_sub != g_sub_serial) return 0;
    const mdx_ent *e = mdx_slot(ind, cnt); if (e->tag != g_mdx_tag) return 0;
    *out = e->out; return 1;
}
static void mdx_run(const p_draw *dw, const p_draw_idx *di) {
    static id<MTLComputePipelineState> ps; static id<MTL4ArgumentTable> at; static int failed;
    if (failed) return;
    if (!at) { NSError *e = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(k_mdx_msl) options:nil error:&e]; id<MTLFunction> f = [lib newFunctionWithName:@"mdx_mask"];
        ps = f ? [g_dev newComputePipelineStateWithFunction:f error:&e] : nil;
        MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new]; ad.maxBufferBindCount = 1; at = ps ? rec_table([g_dev newArgumentTableWithDescriptor:ad error:&e]) : nil;
        if (!at) { failed = 1; MLOG("[RED] indirect-count multi-draw masking: cannot create the compute pipeline %s (those draws wait for the GPU)", e.localizedDescription.UTF8String); return; } }
    if (g_mdx_ep != g_mdx_epoch || g_mdx_sub != g_sub_serial || g_mdx_n > MDX_SLOTS / 4) { g_mdx_tag++; g_mdx_n = 0; g_mdx_ep = g_mdx_epoch; g_mdx_sub = g_sub_serial; }
    enum { QMAX = MDX_SLOTS / 4 };
    static struct { mdx_ent *e; uint32_t i, words; } q[QMAX]; int nq = 0;
    uint64_t lo = ~0ull, hi = 0;   // the hull of the records and counts taken, for the uniform update check
    #define MDX_TAKE(IND, CNT, I, W) do { mdx_ent *e_ = mdx_slot((IND), (CNT)); if (e_->tag != g_mdx_tag && nq < QMAX) { *e_ = (mdx_ent){(IND), (CNT), 0, g_mdx_tag}; g_mdx_n++; \
        q[nq].e = e_; q[nq].i = (uint32_t)(I); q[nq++].words = (W); lo = MIN(lo, MIN((IND), (CNT))); hi = MAX(hi, MAX((IND) + 4 * (W), (CNT) + 4)); } } while (0)
    if (dw) MDX_TAKE(dw->ind, dw->mdi_cnt, dw->mdi_i, 4); else MDX_TAKE(di->ind, di->mdi_cnt, di->mdi_i, 5);
    if (g_exec_next && g_ret_n <= 8) {   // later draws in replay order (current list → callers → the rest of this submission)
        fz_cursor cur = {.p = g_exec_next, .ret_n = g_ret_n}; memcpy(cur.ret, g_ret, sizeof(*g_ret) * (size_t)g_ret_n);
        const uint8_t *p; op_hdr h; const void *a;
        for (int guard = 0; guard < 200000 && nq < QMAX && fz_next(&cur, &p, &h, &a); guard++) {
            if (h.op == OP_DISPATCH || h.op == OP_COPY_B2B || h.op == OP_CLEAR_BUF || h.op == OP_COPY_T2B) break;
            if (h.op == OP_UPDATE_UB) { uint64_t d, n; memcpy(&d, a, 8); memcpy(&n, (const uint8_t *)a + 8, 8);
                int hit = 0; for (int k = 0; d < hi && lo < d + n && k < nq && !hit; k++) { const mdx_ent *e = q[k].e; hit = (d < e->ind + 4 * q[k].words && e->ind < d + n) || (d < e->cnt + 4 && e->cnt < d + n); }
                if (hit) { g_mdx_stop = p; break; } continue; }
            p_draw dt; p_draw_idx it; const p_draw *d = h.op == OP_DRAW ? draw_rec(a, h.size, &dt) : NULL; const p_draw_idx *x = h.op == OP_DRAW_IDX ? draw_idx_rec(a, h.size, &it) : NULL;
            if (d && mdx_wanted(d, NULL)) MDX_TAKE(d->ind, d->mdi_cnt, d->mdi_i, 4);
            else if (x && mdx_wanted(NULL, x)) MDX_TAKE(x->ind, x->mdi_cnt, x->mdi_i, 5);
        }
    }
    #undef MDX_TAKE
    if (!nq) return;
    if (g_enc) { g_mdx_splits++; PD("M %llu indirect-count multi-draw masking: render pass split here", g_stat_pass); rp_end(); }
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    typedef struct { uint64_t rec, cnt, out; uint32_t i, words; } mjob;
    int nj = (nq + 63) & ~63;
    id<MTLBuffer> ab; size_t ao = arena_alloc((size_t)nj * sizeof(mjob) + (size_t)nq * 32, &ab);   // the jobs (padded to whole threadgroups), then the copies 32 bytes apart
    mjob *j = (mjob *)((uint8_t *)ab.contents + ao); uint64_t og = ab.gpuAddress + ao + (uint64_t)nj * sizeof(mjob);
    memset(j, 0, (size_t)nj * sizeof(mjob));
    for (int k = 0; k < nq; k++) { mdx_ent *e = q[k].e; void *mb; uint64_t mo, rg, cg;
        if (!mtl_gpu_addr(e->ind, 4 * q[k].words, &mb, &mo, &rg) || !mtl_gpu_addr(e->cnt, 4, &mb, &mo, &cg)) continue;
        e->out = og + 32 * (uint64_t)k; j[k] = (mjob){rg, cg, e->out, q[k].i, q[k].words}; }
    id<MTL4ComputeCommandEncoder> ce = enc_c_begin(NULL); [ce setArgumentTable:at]; [at setAddress:ab.gpuAddress + ao atIndex:0]; [ce setComputePipelineState:ps];
    [ce dispatchThreadgroups:MTLSizeMake((NSUInteger)nj / 64, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
    enc_c_end(ce);
    args_dirty_all();
    g_mdx_draws += (uint64_t)nq;
    if (!g_mdx_runs++) MLOG("indirect-count multi-draw masking: first time, %d draws (their records are copied on the GPU, zeros past the count, before the render pass)", nq);
    else if (g_mdx_runs % 3600 == 0) MLOG("indirect-count multi-draw masking: %llu times, %llu draws (%llu encoded alone, mdx_next), render pass split %llu times; waits for the GPU-written count %llu",
                                          (unsigned long long)g_mdx_runs, (unsigned long long)g_mdx_draws, (unsigned long long)g_mdx_fast, (unsigned long long)g_mdx_splits, (unsigned long long)g_mdi_waits);
}
// The draws of one multi-draw are recorded back to back with the same state, and only the first few are live: each one replayed right after the one before it (no op between them,
// the same open pass) is encoded alone, without the rest of draw's work, which cost a whole draw for each masked one (about 1.5 µs on an M4 Pro, 1,100 a frame in Kamura Village)
static struct { const uint8_t *next; const void *enc; uint64_t cnt, ig, il, pass; int i; MTLPrimitiveType mp; MTLIndexType it; } g_mdxc;
static void mdx_chain(uint64_t cnt, int i, MTLPrimitiveType mp, MTLIndexType it, uint64_t ig, uint64_t il) {
    g_mdxc = g_defer_replaying || !g_exec_next ? (typeof(g_mdxc)){0} : (typeof(g_mdxc)){g_exec_next, (__bridge const void *)g_enc, cnt, ig, il, g_stat_pass, i, mp, it};
}
static int mdx_next(const p_draw *dw, const p_draw_idx *di) {
    uint64_t cnt = dw ? dw->mdi_cnt : di->mdi_cnt, out; int i = dw ? dw->mdi_i : di->mdi_i;
    if (!g_mdxc.next || g_mdxc.next != g_exec_cur || !g_enc || g_mdxc.enc != (__bridge const void *)g_enc || g_mdxc.pass != g_stat_pass || g_mdxc.cnt != cnt || g_mdxc.i + 1 != i
        || g_defer_replaying || passdump() || !mdx_find(dw ? dw->ind : di->ind, cnt, &out) || !out) return 0;
    if (dw) enc_r_draw_indirect(g_enc, g_mdxc.mp, out); else enc_r_draw_indexed_indirect(g_enc, g_mdxc.mp, g_mdxc.it, g_mdxc.ig, g_mdxc.il, out);
    g_stat_draw++; g_mdx_fast++; g_mdxc.next = g_exec_next; g_mdxc.i = i;
    return 1;
}
#include "nvn_feedback.inc"

#include "nvn_geometry.inc"

// Lazy clears. A set of targets that gets only clears before the game sets other targets used to open a pass just to land them (rp_flush), storing the cleared attachments, and the next
// pass that attached them loaded them back. Breath of the Wild clears its G-buffer one target at a time (depth with the lighting target, then albedo, normals, the fourth), then attaches all
// five: four passes stored 27 MB of cleared memory at 1600x900 and the G-buffer pass loaded it again. Such clears now wait (g_lc): the next pass that attaches the same texture level and
// layer through the same view takes the clear as its load action. The rest land as before, in order, each in a pass of its own (lc_land), before anything else could see their textures: a
// draw whose program samples one, the pass of a draw that does not attach it (before that pass opens), and any operation other than targets, clears, state, uniform updates and buffer
// copies or fills, and the end of a command list (not of a sub-list it calls). A newer clear of the same subresource replaces an older one. Off in TBDR mode (its hoisting moves clear-only passes its own way).
// DM_NVN_LAZY_CLEAR=0 turns it off
typedef struct { p_set_rt rt; uint8_t clear_c[8]; MTLClearColor cc[8]; uint8_t clear_d, clear_s; double cd; uint32_t cs; } lc_group;
enum { LC_MAX = 8 };
static lc_group g_lc[LC_MAX]; static int g_nlc;
static uint64_t g_lc_taken, g_lc_landed;   // clears a later pass took as its load action / clear-only passes still opened for them (the residency set line)
void mtl_lc_stats(uint64_t *taken, uint64_t *landed) { *taken = g_lc_taken; *landed = g_lc_landed; }
static int lc_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_NVN_LAZY_CLEAR"); v = !(e && !strcmp(e, "0")); if (!v) MLOG("DM_NVN_LAZY_CLEAR=0: clears with no draw open a pass of their own as before"); } return v && !tbdr_on(); }
static int lc_same(const p_set_rt *a, int ka, const p_set_rt *b, int kb) {   // the same subresource through the same view (k = 8: depth / stencil)
    uint64_t s1, s2; int l1, l2, x1, x2; rt_sub(a, ka, &s1, &l1, &x1); rt_sub(b, kb, &s2, &l2, &x2);
    if (!s1 || s1 != s2 || l1 != l2 || x1 != x2) return 0;
    int h1 = ka == 8 ? a->has_dview : a->has_cview[ka], h2 = kb == 8 ? b->has_dview : b->has_cview[kb];
    return h1 == h2 && (!h1 || !memcmp(ka == 8 ? &a->dview : &a->cview[ka], kb == 8 ? &b->dview : &b->cview[kb], sizeof(view_t)));
}
// Land every waiting clear, oldest first, each group in a pass of its own (rp_flush's way)
static void lc_land(void) {
    if (!g_nlc) return;
    if (g_enc) rp_end();
    int n = g_nlc; g_nlc = 0;
    typeof(R) save = R;
    for (int gi = 0; gi < n; gi++) { const lc_group *g = &g_lc[gi]; int any = g->clear_d || g->clear_s; for (int i = 0; i < 8; i++) any |= g->clear_c[i];
        if (!any || !g_cb) continue;
        R.rt = g->rt; R.has_rt = 1; memcpy(R.clear_c, g->clear_c, sizeof R.clear_c); memcpy(R.cc, g->cc, sizeof R.cc);
        R.clear_d = g->clear_d; R.clear_s = g->clear_s; R.cd = g->cd; R.cs = g->cs;
        rp_begin_ex(1); rp_end(); g_lc_landed++; }
    R = save;
}
// SetRenderTargets with clears pending and no draw since: the clears wait instead of opening a pass
static void lc_push(void) {
    if (g_nlc == LC_MAX) lc_land();
    lc_group *g = &g_lc[g_nlc];
    g->rt = R.rt; memcpy(g->clear_c, R.clear_c, sizeof g->clear_c); memcpy(g->cc, R.cc, sizeof g->cc); g->clear_d = R.clear_d; g->clear_s = R.clear_s; g->cd = R.cd; g->cs = R.cs;
    for (int gi = 0; gi < g_nlc; gi++) { lc_group *o = &g_lc[gi];   // older clears of the same subresources are overwritten
        for (int i = 0; i < 8; i++) if (o->clear_c[i]) for (int j = 0; j < 8; j++) if (g->clear_c[j]) { uint64_t s1, s2; int l1, l2, x1, x2; rt_sub(&o->rt, i, &s1, &l1, &x1); rt_sub(&g->rt, j, &s2, &l2, &x2);
            if (s1 && s1 == s2 && l1 == l2 && x1 == x2) { o->clear_c[i] = 0; break; } }
        if ((o->clear_d || o->clear_s) && (g->clear_d || g->clear_s)) { uint64_t s1, s2; int l1, l2, x1, x2; rt_sub(&o->rt, 8, &s1, &l1, &x1); rt_sub(&g->rt, 8, &s2, &l2, &x2);
            if (s1 && s1 == s2 && l1 == l2 && x1 == x2) { if (g->clear_d) o->clear_d = 0; if (g->clear_s) o->clear_s = 0; } } }
    g_nlc++;
    memset(R.clear_c, 0, sizeof R.clear_c); R.clear_d = R.clear_s = 0;
}
// Whether the program samples a texture with a waiting clear (a program with bindless or dynamic handles counts as sampling)
static int lc_sampled(NvProg *pr) {
    if (!pr) return 1;
    for (int st = 0; st < 5; st++) { __unsafe_unretained NvShader *sh = pr->sh[st]; if (!sh || sh->stage >= 6) continue;
        if (sh->rf.bindless || sh->rf.dyn) return 1;
        for (int i = 0; i < sh->rf.nb; i++) { const nvn_rbind *b = &sh->rf.b[i]; uint64_t h;
            if (b->kind == NVN_RB_IMG) h = b->reg < 32 ? S.img[sh->stage][b->reg] : 0; else if (b->kind == NVN_RB_TEX || b->kind == NVN_RB_TBUF) h = tex_slot_h(S.tex[sh->stage], S.img[sh->stage], b->reg); else continue;
            uint64_t ser = h ? h_ser(h) : 0; if (!ser) continue;
            for (int gi = 0; gi < g_nlc; gi++) { const lc_group *g = &g_lc[gi];
                for (int k = 0; k < 8; k++) if (g->clear_c[k] && g->rt.color[k] && g->rt.color[k]->serial == ser) return 1;
                if ((g->clear_d || g->clear_s) && g->rt.depth && g->rt.depth->serial == ser) return 1; } } }
    return 0;
}
// The draw about to open a pass: its attachments take the waiting clears of the same subresources (the newest one; the pass's own pending clear is newer still), the rest land first
static void lc_take(NvProg *pr) {
    if (lc_sampled(pr)) { lc_land(); return; }
    for (int gi = g_nlc - 1; gi >= 0; gi--) { lc_group *g = &g_lc[gi];
        for (int i = 0; i < 8; i++) if (g->clear_c[i])
            for (int j = 0; j < R.rt.n && j < 8; j++) if (R.rt.color[j] && !rt_dup_slot(&R.rt, j) && !nvn_fmt_bc(R.rt.color[j]->d.format) && lc_same(&g->rt, i, &R.rt, j)) {
                if (!R.clear_c[j]) { R.clear_c[j] = 1; R.cc[j] = g->cc[i]; }
                g->clear_c[i] = 0; g_lc_taken++; break; }
        if ((g->clear_d || g->clear_s) && R.rt.depth && lc_same(&g->rt, 8, &R.rt, 8)) {
            if (g->clear_d) { if (!R.clear_d) { R.clear_d = 1; R.cd = g->cd; } g->clear_d = 0; }
            if (g->clear_s) { if (!R.clear_s) { R.clear_s = 1; R.cs = g->cs; } g->clear_s = 0; }
            g_lc_taken++; } }
    lc_land();
}
// The operations a waiting clear can wait across: they neither read nor write textures
static int lc_keeps(uint32_t op) {
    switch (op) {
        case OP_SET_RT: case OP_CLEAR_COLOR: case OP_CLEAR_DS: case OP_DRAW: case OP_DRAW_IDX: case OP_BEGIN: case OP_JUMP: case OP_CALL:
        case OP_SCISSOR: case OP_BIND_PROGRAM: case OP_BIND_STATE: case OP_BIND_VATTR: case OP_BIND_VSTREAM: case OP_BIND_VBUF: case OP_BIND_VBUF1:
        case OP_BIND_UBO: case OP_BIND_UBO_SMALL: case OP_BIND_SSBO: case OP_BIND_TEX: case OP_BIND_TEX1: case OP_BIND_SAMP: case OP_BIND_IMG: case OP_STENCIL:
        case OP_POLY_OFFSET: case OP_VIEWPORTS: case OP_DEPTH_RANGE: case OP_DEPTH_CLAMP: case OP_VP_SWIZZLE: case OP_BLEND_COLOR: case OP_ALPHA_REF: case OP_COPY_STRIDE:
        case OP_TF_BIND: case OP_TF_STATE: case OP_RAST_DISCARD: case OP_PRIM_RESTART: case OP_BARRIER: case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL: case OP_RENDER_COND:
        case OP_UPDATE_UB: case OP_COPY_B2B: case OP_CLEAR_BUF: return 1;
        default: return 0;
    }
}

