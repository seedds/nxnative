// Dispatch capture (DM_NVN_DCAP / DM_CTL "dcap", 2026-11-06): one compute dispatch recorded with everything needed to run it again outside the game
// (sw2_tools/bench/bench.m, "side shader bench"). Included by nvn_mtl.m before encode_args_.
//
// Arm: DM_NVN_DCAP=<key prefix>[,<prefix>...]:<out dir>[:<n>]   or the DM_CTL line   dcap <key prefix> <out dir> [n] [pool MB]
//   The next n (default 1) dispatches of each key whose name starts with the prefix (8 to 16 hex digits) are captured into <out dir>/<key>_<frame>_<k>/.
//   Up to 8 prefixes armed at once; a new dcap line for the same prefix re-arms it.
// What is recorded, on the GPU timeline (copies encoded in the dispatch's own encoder, right before and right after the dispatch, behind barriers; so the
// bytes are what the dispatch itself read, after every earlier GPU writer of the frame and after the indirect-count and c0 copies):
//   args.bin       the argument block (8 bytes per arg_id: GPU addresses, resource IDs) as the GPU read it (pool-table words patched at commit included); args_cpu.bin at encode
//   r<i>.bin       each "region": a range of one Metal buffer that a binding's address falls in, or a memory pool row of the raw pool table (gmem_map), merged per buffer;
//                  r<i>.after.bin the same range after the dispatch (regions a writable binding, an image or a raw store can reach)
//   t<i>.bin       each bound texture / image (every level and slice that fits the budget, tightly packed per level); t<i>.after.bin for images
//   grid.bin       an indirect dispatch's three group counts as the GPU read them (after DM_NVN_DISPATCH_CLAMP)
//   lcap.bin / lcap.after.bin  the loop guard's buffer (DM_NVN_LOOPCAP) around the dispatch (shared by every dispatch of the process: notes of others may be in it)
//   manifest.json  key, library (base or DM_MSL_OVERRIDE) and its path, frame, fence, grid, threadgroup, the bindings (kind, aid, reg, rw, slot values), regions, textures, pool rows
// Budgets (MB): DM_NVN_DCAP_BUF_MB (per buffer binding, 16), DM_NVN_DCAP_POOL_MB (raw pool rows, 256), DM_NVN_DCAP_ROW_MB (one row, 64; a longer row is captured from its start),
// DM_NVN_DCAP_TEX_MB (textures, 192). Constant banks take at most 64 KB. Rows that do not fit are listed with captured 0 (the bench empties them: a lookup there misses).
// Pool rows are taken in this order: rows holding a bound buffer, rows holding a pointer-looking word of the constant banks (read at encode), then the smallest first.
// The files are written by a helper thread once the GPU has completed the submission (fence); a capture whose fence does not complete in 30 s is written with "complete": 0.
// Off (nothing armed), the cost is one atomic load per dispatch and one flag test per binding.

@interface DcapReg : NSObject { @public __strong id<MTLBuffer> src, b, a; uint64_t off, len; int after; char why[96]; } @end
@implementation DcapReg @end
@interface DcapTex : NSObject { @public __strong id<MTLTexture> t; __strong id<MTLBuffer> b, a; uint64_t rid; int after, nl, bw, bh, bpb; size_t bytes;
    struct { uint32_t w, h, d, slices; size_t row, img, off; } lv[16]; char note[128]; } @end
@implementation DcapTex @end
@interface DcapJob : NSObject { @public char key[24], dir[1100], lib[1100]; int over; uint64_t frame, fence, seq, born_ns; int ind, nb, abi; uint32_t grid[3]; uint16_t tg[3]; uint32_t tg_spill, loopguard;
    __strong id<MTLBuffer> args_b, grid_b, lcap_b, lcap_a; size_t arg_len; uint8_t slots_cpu[8 * NVN_REFL_MAXARG];
    nvn_rbind bind[NVN_REFL_MAXB]; __strong NSMutableArray *regs, *texs; __strong NSMutableData *rows; int nrows_skipped; uint64_t pool_bytes, tex_bytes; char note[256]; } @end
@implementation DcapJob @end
typedef struct { uint64_t i, lo, hi, delta; int captured; uint64_t cap_len; } dcap_row;

typedef struct { char pfx[17]; char dir[1024]; int left, k; long pool_mb; } dcap_arm;
static dcap_arm g_dcap_arms[8]; static _Atomic int g_dcap_n; static pthread_mutex_t g_dcap_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_dcap_rec; static __strong NSMutableArray *g_dcap_used;   // resources encode_args_ used (its USE macro), while g_dcap_rec
static __strong NSMutableArray *g_dcap_pending; static pthread_cond_t g_dcap_cv = PTHREAD_COND_INITIALIZER; static int g_dcap_thread;

static inline void dcap_use(id r) { if (r && g_dcap_used && ![g_dcap_used containsObject:r]) [g_dcap_used addObject:r]; }
static long dcap_env_mb(const char *n, long def) { const char *e = getenv(n); long v = e && *e ? atol(e) : def; return v < 0 ? 0 : v; }

static void *dcap_writer(void *arg);
static size_t mtl_pf_block_bytes(MTLPixelFormat pf);
void dm_gfx_dcap_arm(const char *pfx, const char *dir, int n, long pool_mb) {
    if (!pfx || strlen(pfx) < 8 || !dir || !*dir) { MLOG("[RED] dcap: usage dcap <key prefix of 8-16 hex digits> <out dir> [n] [pool MB]"); return; }
    if (n <= 0) n = 1;
    pthread_mutex_lock(&g_dcap_mu);
    int i = 0; for (; i < 8; i++) if (g_dcap_arms[i].left > 0 && !strncmp(g_dcap_arms[i].pfx, pfx, 16)) break;
    if (i == 8) for (i = 0; i < 8; i++) if (g_dcap_arms[i].left <= 0) break;
    if (i == 8) { pthread_mutex_unlock(&g_dcap_mu); MLOG("[RED] dcap: 8 prefixes armed already; %s not armed", pfx); return; }
    dcap_arm *a = &g_dcap_arms[i]; memset(a, 0, sizeof *a); strlcpy(a->pfx, pfx, sizeof a->pfx); strlcpy(a->dir, dir, sizeof a->dir); a->left = n; a->pool_mb = pool_mb;
    int c = 0; for (int k = 0; k < 8; k++) c += g_dcap_arms[k].left > 0; atomic_store(&g_dcap_n, c);
    if (!g_dcap_thread) { g_dcap_thread = 1; g_dcap_pending = [NSMutableArray array]; pthread_t th; pthread_create(&th, NULL, dcap_writer, NULL); pthread_detach(th); }
    pthread_mutex_unlock(&g_dcap_mu);
    [NSFileManager.defaultManager createDirectoryAtPath:@(dir) withIntermediateDirectories:YES attributes:nil error:nil];
    MLOG("DM_NVN_DCAP: the next %d dispatch(es) of %s are captured into %s (pool budget %ld MB)", n, pfx, dir, pool_mb > 0 ? pool_mb : dcap_env_mb("DM_NVN_DCAP_POOL_MB", 256));
}
static void dcap_env_init(void) {
    static int done; if (done) return; done = 1;
    const char *e = getenv("DM_NVN_DCAP"); if (!e || !*e) return;
    char tmp[2048]; strlcpy(tmp, e, sizeof tmp); char *c1 = strchr(tmp, ':'); if (!c1) { MLOG("[RED] DM_NVN_DCAP=<key>[,<key>]:<dir>[:<n>]: no directory"); return; }
    *c1++ = 0; int n = 1; char *c2 = strrchr(c1, ':'); if (c2 && c2[1] && strspn(c2 + 1, "0123456789") == strlen(c2 + 1)) { *c2 = 0; n = atoi(c2 + 1); }
    for (char *k = strtok(tmp, ","); k; k = strtok(NULL, ",")) dm_gfx_dcap_arm(k, c1, n, 0);
}
// Watch mode (DM_CTL "dcapw <key prefix> <out dir> <aid> [n] [pool MB]", 2026-11-07): every dispatch of the key is checked after its arguments are encoded;
// when argument slot <aid> (a GPU address / resource ID) differs from the previous dispatch's, that dispatch and the next n-1 of the key are captured
// (a reallocated buffer: PRAGMATA's surfel colour buffer at the flare onset). One watch at a time; it stays on (re-arms) after each capture run.
typedef struct { char pfx[17]; char dir[1024]; int aid, n, on; long pool_mb; uint64_t last; } dcap_watch;
static dcap_watch g_dcapw; static _Atomic int g_dcapw_on;
void dm_gfx_dcap_watch(const char *pfx, const char *dir, int aid, int n, long pool_mb) {
    if (!pfx || strlen(pfx) < 8 || !dir || !*dir || aid < 0) { MLOG("[RED] dcapw: usage dcapw <key prefix of 8-16 hex digits> <out dir> <aid> [n] [pool MB]"); return; }
    pthread_mutex_lock(&g_dcap_mu); memset(&g_dcapw, 0, sizeof g_dcapw); strlcpy(g_dcapw.pfx, pfx, sizeof g_dcapw.pfx); strlcpy(g_dcapw.dir, dir, sizeof g_dcapw.dir);
    g_dcapw.aid = aid; g_dcapw.n = n > 0 ? n : 1; g_dcapw.pool_mb = pool_mb; g_dcapw.on = 1; pthread_mutex_unlock(&g_dcap_mu); atomic_store(&g_dcapw_on, 1);
    MLOG("DM_NVN_DCAP watch: %s is captured (%d dispatches) whenever its argument slot %d changes, into %s", pfx, g_dcapw.n, aid, dir);
}
// after encode_args: 1 when the watch fires for this dispatch (it then arms a normal capture of n dispatches of the key, which takes this one)
static int dcap_watch_check(const NvShader *cs) {
    if (!atomic_load_explicit(&g_dcapw_on, memory_order_relaxed) || strncmp(cs->key, g_dcapw.pfx, strlen(g_dcapw.pfx))) return 0;
    uint32_t al = cs->arg_len ? cs->arg_len : (uint32_t)cs->ae.encodedLength; if ((uint32_t)(8 * g_dcapw.aid + 8) > al) return 0;
    uint64_t v; memcpy(&v, (const uint8_t *)cs->ab_buf.contents + cs->ab_off + 8 * g_dcapw.aid, 8);
    pthread_mutex_lock(&g_dcap_mu); uint64_t last = g_dcapw.last; g_dcapw.last = v; char pfx[17], dir[1024]; strlcpy(pfx, g_dcapw.pfx, sizeof pfx); strlcpy(dir, g_dcapw.dir, sizeof dir);
    int n = g_dcapw.n; long mb = g_dcapw.pool_mb; pthread_mutex_unlock(&g_dcap_mu);
    if (!last || last == v) return 0;
    MLOG("DM_NVN_DCAP watch: %s argument slot %d changed %#llx -> %#llx at frame %llu: capturing %d dispatches", cs->key, g_dcapw.aid, (unsigned long long)last, (unsigned long long)v, (unsigned long long)atomic_load(&g_cur_frame), n);
    dm_gfx_dcap_arm(pfx, dir, n, mb);
    return 1;
}
// the arm this dispatch takes (its index), or -1; counts it down
static int dcap_match(const NvShader *cs) {
    pthread_mutex_lock(&g_dcap_mu); int r = -1;
    for (int i = 0; i < 8; i++) { dcap_arm *a = &g_dcap_arms[i]; if (a->left <= 0) continue;
        if (!strncmp(cs->key, a->pfx, strlen(a->pfx))) { a->left--; a->k++; r = i; break; } }
    if (r >= 0) { int c = 0; for (int k = 0; k < 8; k++) c += g_dcap_arms[k].left > 0; atomic_store(&g_dcap_n, c); }
    pthread_mutex_unlock(&g_dcap_mu);
    return r;
}
static const char *dcap_kind_name(int k) {
    static const char *n[] = {"?", "cbuf", "samp", "tex", "img", "tbuf", "sbufp", "support", "c1", "drv0", "drv2", "sampt", "ubop", "texu", "sampu", "sampi", "sbufu", "texd", "sampd",
                              "texa", "sampa", "geometry", "texp", "sampp", "texh", "samph", "border", "push", "gmem", "gmeml", "imgu", "tbufu"};
    return k >= 0 && k < (int)(sizeof n / sizeof n[0]) ? n[k] : "?";
}
static int dcap_kind_sampler(int k) { return k == NVN_RB_SAMP || k == NVN_RB_SAMPT || k == NVN_RB_SAMPU || k == NVN_RB_SAMPI || k == NVN_RB_SAMPD || k == NVN_RB_SAMPA; }
static int dcap_kind_texture(int k) { return k == NVN_RB_TEX || k == NVN_RB_IMG || k == NVN_RB_TBUF || k == NVN_RB_TEXU || k == NVN_RB_TEXD || k == NVN_RB_TEXA || k == NVN_RB_IMGU || k == NVN_RB_TBUFU; }
static int dcap_kind_image(int k) { return k == NVN_RB_IMG || k == NVN_RB_IMGU; }
static int dcap_kind_const(int k) { return k == NVN_RB_CBUF || k == NVN_RB_SUPPORT || k == NVN_RB_C1 || k == NVN_RB_DRV0 || k == NVN_RB_DRV2 || k == NVN_RB_PUSH || k == NVN_RB_BORDER; }
// the Metal buffer that holds a GPU address: one encode_args_ used, else a memory pool's (or a large pool's segment)
static id<MTLBuffer> dcap_buf_at(uint64_t gpu, uint64_t *off) {
    for (id o in g_dcap_used) if ([o conformsToProtocol:@protocol(MTLBuffer)]) { id<MTLBuffer> b = o; if (gpu >= b.gpuAddress && gpu < b.gpuAddress + b.length) { *off = gpu - b.gpuAddress; return b; } }
    for (int i = 0; i < g_npool; i++) { id<MTLBuffer> b = g_pools[i].buf; if (b && gpu >= b.gpuAddress && gpu < b.gpuAddress + b.length) { *off = gpu - b.gpuAddress; return b; } }
    for (int k = 0; k < g_nbig; k++) for (id sb in g_big[k].seg) { if (sb == (id)[NSNull null]) continue; id<MTLBuffer> b = sb;
        if (gpu >= b.gpuAddress && gpu < b.gpuAddress + b.length) { *off = gpu - b.gpuAddress; return b; } }
    if (g_gmem_buf && gpu >= g_gmem_buf.gpuAddress && gpu < g_gmem_buf.gpuAddress + g_gmem_buf.length) { *off = gpu - g_gmem_buf.gpuAddress; return g_gmem_buf; }
    return nil;
}
static void dcap_want(NSMutableArray *w, id<MTLBuffer> b, uint64_t off, uint64_t len, int after, const char *why) {
    if (!b || off >= b.length || !len) return;
    if (len > b.length - off) len = b.length - off;
    DcapReg *r = [DcapReg new]; r->src = b; r->off = off & ~15ull; r->len = ((off + len + 15) & ~15ull) - r->off; if (r->off + r->len > b.length) r->len = b.length - r->off;
    r->after = after; strlcpy(r->why, why, sizeof r->why); [w addObject:r];
}
// merge the wanted ranges per buffer (overlapping or within 1 MB of each other)
static NSMutableArray *dcap_merge(NSMutableArray *w) {
    [w sortUsingComparator:^NSComparisonResult(DcapReg *x, DcapReg *y) {
        if (x->src != y->src) return (uintptr_t)(__bridge void *)x->src < (uintptr_t)(__bridge void *)y->src ? NSOrderedAscending : NSOrderedDescending;
        return x->off < y->off ? NSOrderedAscending : x->off > y->off ? NSOrderedDescending : NSOrderedSame; }];
    NSMutableArray *o = [NSMutableArray array]; DcapReg *cur = nil;
    for (DcapReg *r in w) {
        if (cur && cur->src == r->src && r->off <= cur->off + cur->len + (1u << 20)) {
            uint64_t e = MAX(cur->off + cur->len, r->off + r->len); cur->len = e - cur->off; cur->after |= r->after;
            if (strlen(cur->why) + strlen(r->why) + 2 < sizeof cur->why && !strstr(cur->why, r->why)) { strlcat(cur->why, ",", sizeof cur->why); strlcat(cur->why, r->why, sizeof cur->why); }
            continue; }
        cur = r; [o addObject:r];
    }
    return o;
}
static int dcap_tex_layout(DcapTex *x, long *budget) {
    id<MTLTexture> t = x->t; MTLPixelFormat pf = t.pixelFormat;
    if (t.sampleCount > 1) { snprintf(x->note, sizeof x->note, "multisample: not captured"); return 0; }
    if (t.storageMode == MTLStorageModeMemoryless) { snprintf(x->note, sizeof x->note, "memoryless: not captured"); return 0; }
    if (pf == MTLPixelFormatDepth32Float_Stencil8 || pf == MTLPixelFormatX32_Stencil8 || pf == MTLPixelFormatStencil8 || pf == MTLPixelFormatDepth24Unorm_Stencil8) { snprintf(x->note, sizeof x->note, "depth-stencil: not captured"); return 0; }
    if (pf >= MTLPixelFormatASTC_4x4_sRGB && pf <= MTLPixelFormatASTC_12x12_HDR) { snprintf(x->note, sizeof x->note, "ASTC: not captured"); return 0; }
    int bb = bc_block_bytes(pf), etc = pf >= MTLPixelFormatEAC_R11Unorm && pf <= MTLPixelFormatETC2_RGB8A1_sRGB;
    size_t bpb = pf == MTLPixelFormatDepth32Float ? 4 : pf == MTLPixelFormatDepth16Unorm ? 2 : mtl_pf_block_bytes(pf);
    if (!bpb) { snprintf(x->note, sizeof x->note, "pixel format %lu of unknown size: not captured", (unsigned long)pf); return 0; }
    x->bw = x->bh = bb || etc ? 4 : 1; x->bpb = (int)bpb;
    int cube = t.textureType == MTLTextureTypeCube || t.textureType == MTLTextureTypeCubeArray, d3 = t.textureType == MTLTextureType3D;
    uint32_t slices = (uint32_t)(MAX(t.arrayLength, 1u) * (cube ? 6 : 1));
    size_t off = 0; int nl = (int)MIN(t.mipmapLevelCount, (NSUInteger)16);
    for (int l = 0; l < nl; l++) {
        uint32_t w = (uint32_t)MAX(t.width >> l, 1u), h = (uint32_t)MAX(t.height >> l, 1u), d = d3 ? (uint32_t)MAX(t.depth >> l, 1u) : 1;
        if (t.textureType == MTLTextureType1D || t.textureType == MTLTextureType1DArray) h = 1;
        size_t row = (size_t)((w + x->bw - 1) / x->bw) * bpb, img = row * ((h + x->bh - 1) / x->bh), sz = img * d * (d3 ? 1 : slices);
        if ((long)((off + sz) >> 20) > *budget && l > 0) break;
        if ((long)((off + sz) >> 20) > *budget) { snprintf(x->note, sizeof x->note, "level 0 (%zu MB) past the texture budget: not captured", sz >> 20); return 0; }
        x->lv[l].w = w; x->lv[l].h = h; x->lv[l].d = d; x->lv[l].slices = d3 ? 1 : slices; x->lv[l].row = row; x->lv[l].img = img; x->lv[l].off = off; off += (sz + 255) & ~255ull; x->nl = l + 1;
    }
    if (x->nl < nl) snprintf(x->note, sizeof x->note, "levels %d of %d captured (budget)", x->nl, nl);
    x->bytes = off; *budget -= (long)(off >> 20); return x->nl > 0;
}
static void dcap_tex_copy(id ce, DcapTex *x, id<MTLBuffer> dst) {
    id<MTLTexture> t = x->t; int d3 = t.textureType == MTLTextureType3D;
    for (int l = 0; l < x->nl; l++) for (uint32_t s = 0; s < x->lv[l].slices; s++)
        [(id<MTL4ComputeCommandEncoder>)ce copyFromTexture:t sourceSlice:s sourceLevel:l sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(x->lv[l].w, x->lv[l].h, d3 ? x->lv[l].d : 1)
                                                  toBuffer:dst destinationOffset:x->lv[l].off + (size_t)s * x->lv[l].img destinationBytesPerRow:x->lv[l].row destinationBytesPerImage:x->lv[l].img];
}
static id<MTLBuffer> dcap_stage(size_t n) { id<MTLBuffer> b = [g_dev newBufferWithLength:MAX(n, (size_t)16) options:MTLResourceStorageModeShared]; if (b) res_add(b); return b; }
static int dcap_has_ptr_word(const uint8_t *p, size_t n, uint64_t lo, uint64_t hi) {
    for (size_t i = 0; i + 8 <= n; i += 4) { uint64_t v; memcpy(&v, p + i, 8); if (v >= lo && v < hi) return 1; }
    return 0;
}
// encoded right before the dispatch, in its encoder (ce), after encode_args and the indirect-count copies
static DcapJob *dcap_before(id ce, NvShader *cs, const p_dispatch *dp, void *ib, uint64_t io, int arm) { @autoreleasepool {
    dcap_arm A; pthread_mutex_lock(&g_dcap_mu); A = g_dcap_arms[arm]; pthread_mutex_unlock(&g_dcap_mu);
    DcapJob *J = [DcapJob new]; strlcpy(J->key, cs->key, sizeof J->key); J->frame = atomic_load(&g_cur_frame); J->fence = atomic_load(&g_fence) + 1; J->seq = (uint64_t)A.k; J->born_ns = now_ns();
    snprintf(J->dir, sizeof J->dir, "%s/%s_f%llu_%d", A.dir, cs->key, (unsigned long long)J->frame, A.k);
    { const char *od = getenv("DM_MSL_OVERRIDE"); NSString *op = od && *od ? [NSString stringWithFormat:@"%s/%s.metallib", od, cs->key] : nil;
      if (op && [NSFileManager.defaultManager fileExistsAtPath:op]) { J->over = 1; strlcpy(J->lib, op.UTF8String, sizeof J->lib); }
      else { J->over = 0; snprintf(J->lib, sizeof J->lib, "%s/%s.metallib", g_msl_dir.UTF8String, cs->key); } }   // cs->key is "<hex>_b"
    J->ind = dp->ind != 0; J->grid[0] = (uint32_t)dp->x; J->grid[1] = (uint32_t)dp->y; J->grid[2] = (uint32_t)dp->z; memcpy(J->tg, cs->rf.tg, sizeof J->tg); J->tg_spill = cs->rf.tg_spill; J->loopguard = cs->rf.loopguard;
    J->nb = cs->rf.nb; J->abi = cs->rf.abi; memcpy(J->bind, cs->rf.b, sizeof(nvn_rbind) * (size_t)MIN(cs->rf.nb, NVN_REFL_MAXB));
    J->arg_len = cs->arg_len ? cs->arg_len : cs->ae.encodedLength; if (J->arg_len > sizeof J->slots_cpu) J->arg_len = sizeof J->slots_cpu;
    const uint8_t *slots = (const uint8_t *)cs->ab_buf.contents + cs->ab_off; memcpy(J->slots_cpu, slots, J->arg_len);
    long buf_mb = dcap_env_mb("DM_NVN_DCAP_BUF_MB", 16), pool_mb = A.pool_mb > 0 ? A.pool_mb : dcap_env_mb("DM_NVN_DCAP_POOL_MB", 256), row_mb = dcap_env_mb("DM_NVN_DCAP_ROW_MB", 64), tex_mb = dcap_env_mb("DM_NVN_DCAP_TEX_MB", 192);
    pthread_mutex_lock(&g_pool_upd_mu); pthread_mutex_lock(&g_mu);   // the pool list (dcap_buf_at), as gmem_build takes it
    NSMutableArray *want = [NSMutableArray array]; J->texs = [NSMutableArray array]; J->rows = [NSMutableData data];
    int rw_gmem = 0; uint64_t gmem_slot = 0; NSMutableData *cdata = [NSMutableData data];   // the constant banks' bytes (encode time), searched for pointer-looking words
    for (int i = 0; i < J->nb; i++) {
        const nvn_rbind *b = &J->bind[i]; int n = b->n ? b->n : 1;
        if (b->kind == NVN_RB_GMEML) rw_gmem = 1;
        if (b->kind == NVN_RB_GMEM) { rw_gmem |= b->rw; }
        for (int k = 0; k < n && b->aid + k < (int)(J->arg_len / 8); k++) {
            uint64_t v; memcpy(&v, slots + 8 * (b->aid + k), 8); if (!v) continue;
            if (dcap_kind_sampler(b->kind)) continue;
            if (dcap_kind_texture(b->kind)) {
                id<MTLTexture> t = nil; for (id o in g_dcap_used) if ([o conformsToProtocol:@protocol(MTLTexture)] && ((id<MTLTexture>)o).gpuResourceID._impl == v) { t = o; break; }
                if (!t) continue;
                int dup = 0; for (DcapTex *x in J->texs) if (x->rid == v) { x->after |= dcap_kind_image(b->kind); dup = 1; }
                if (dup) continue;
                DcapTex *x = [DcapTex new]; x->t = t; x->rid = v; x->after = dcap_kind_image(b->kind);
                if (t.buffer) {   // a texture over a buffer: the buffer range is the capture (a region); the bench makes the view again
                    size_t len = t.bufferBytesPerRow * MAX(t.height, 1u) * MAX(t.depth, 1u); if (t.textureType == MTLTextureTypeTextureBuffer) len = t.width * MAX(mtl_pf_block_bytes(t.pixelFormat), (size_t)1);
                    dcap_want(want, t.buffer, t.bufferOffset, len, x->after, "texture buffer"); snprintf(x->note, sizeof x->note, "buffer-backed: bytes in the region of buffer offset %lu", (unsigned long)t.bufferOffset);
                } else dcap_tex_layout(x, &tex_mb);
                [J->texs addObject:x]; continue;
            }
            uint64_t off; id<MTLBuffer> mb = dcap_buf_at(v, &off); if (!mb) continue;
            if (b->kind == NVN_RB_GMEM) { gmem_slot = v; dcap_want(want, mb, off, DMSH_GMEM_MAP_ENTRIES * DMSH_GMEM_MAP_ENTRY + 65536, 0, "gmem table"); continue; }
            uint64_t len = dcap_kind_const(b->kind) ? 65536 : b->kind == NVN_RB_TEXP || b->kind == NVN_RB_SAMPP || b->kind == NVN_RB_TEXH || b->kind == NVN_RB_SAMPH ? 8 + 8 * 4096 : (uint64_t)buf_mb << 20;
            char why[48]; snprintf(why, sizeof why, "%s%d", dcap_kind_name(b->kind), b->aid);
            dcap_want(want, mb, off, len, b->rw || b->kind == NVN_RB_GMEML, why);
            if (dcap_kind_const(b->kind) && mb.contents) { uint64_t l = MIN(len, mb.length - off); [cdata appendBytes:(const uint8_t *)mb.contents + off length:l]; }
        }
    }
    // the raw pool table's rows: their guest ranges' bytes, within the pool budget
    if (gmem_slot) {
        uint64_t toff; id<MTLBuffer> tb = dcap_buf_at(gmem_slot, &toff);
        const uint8_t *t = tb && tb.contents ? (const uint8_t *)tb.contents + toff : NULL; int n = 0;
        dcap_row *rows = calloc(DMSH_GMEM_MAP_ENTRIES, sizeof *rows); int *pri = calloc(DMSH_GMEM_MAP_ENTRIES, sizeof *pri), *idx = calloc(DMSH_GMEM_MAP_ENTRIES, sizeof *idx);
        for (int k = 1; t && k < DMSH_GMEM_MAP_ENTRIES; k++) { const uint64_t *e = (const uint64_t *)(t + (size_t)k * DMSH_GMEM_MAP_ENTRY); if (e[0] == ~0ull || e[1] <= e[0]) continue;
            rows[n] = (dcap_row){(uint64_t)k, e[0], e[1], e[2], 0, 0}; uint64_t g = e[0] + e[2]; pri[n] = 2;
            for (DcapReg *r in want) { uint64_t rg = r->src.gpuAddress + r->off; if (rg < g + (e[1] - e[0]) && g < rg + r->len) { pri[n] = 0; break; } }
            if (pri[n] && dcap_has_ptr_word(cdata.bytes, cdata.length, e[0], e[1])) pri[n] = 1;
            n++; }
        long left = pool_mb << 20;
        for (int p = 0; p < 3; p++) {
            int m = 0; for (int k = 0; k < n; k++) if (pri[k] == p) idx[m++] = k;
            if (p == 2) qsort_b(idx, (size_t)m, sizeof idx[0], ^int(const void *a, const void *b) { uint64_t x = rows[*(const int *)a].hi - rows[*(const int *)a].lo, y = rows[*(const int *)b].hi - rows[*(const int *)b].lo; return x < y ? -1 : x > y; });
            for (int q = 0; q < m; q++) { dcap_row *r = &rows[idx[q]]; uint64_t len = MIN(r->hi - r->lo, (uint64_t)row_mb << 20);
                if ((long)len > left) continue;
                uint64_t off; id<MTLBuffer> mb = dcap_buf_at(r->lo + r->delta, &off); if (!mb) continue;
                if (len > mb.length - off) len = mb.length - off;
                char why[48]; snprintf(why, sizeof why, "row%llu", (unsigned long long)r->i); dcap_want(want, mb, off, len, rw_gmem, why);
                r->captured = 1; r->cap_len = len; left -= (long)len; J->pool_bytes += len; }
        }
        for (int k = 0; k < n; k++) { [J->rows appendBytes:&rows[k] length:sizeof rows[k]]; J->nrows_skipped += !rows[k].captured; }
        free(rows); free(pri); free(idx);
    }
    pthread_mutex_unlock(&g_mu); pthread_mutex_unlock(&g_pool_upd_mu);
    J->regs = dcap_merge(want);
    // staging, resident before the copies are encoded
    J->args_b = dcap_stage(J->arg_len);
    for (DcapReg *r in J->regs) { r->b = dcap_stage(r->len); if (r->after) r->a = dcap_stage(r->len); }
    for (DcapTex *x in J->texs) if (x->nl) { x->b = dcap_stage(x->bytes); if (x->after) x->a = dcap_stage(x->bytes); J->tex_bytes += x->bytes; }
    if (J->ind && ib) J->grid_b = dcap_stage(16);
    if (g_lcap_buf) { J->lcap_b = dcap_stage(g_lcap_buf.length); J->lcap_a = dcap_stage(g_lcap_buf.length); }
    res_commit();
    id<MTL4ComputeCommandEncoder> e = ce;
    [e barrierAfterEncoderStages:MTLStageDispatch | MTLStageBlit beforeEncoderStages:MTLStageBlit visibilityOptions:blit_vis()];
    [e copyFromBuffer:cs->ab_buf sourceOffset:cs->ab_off toBuffer:J->args_b destinationOffset:0 size:J->arg_len];
    for (DcapReg *r in J->regs) if (r->b) [e copyFromBuffer:r->src sourceOffset:r->off toBuffer:r->b destinationOffset:0 size:r->len];
    for (DcapTex *x in J->texs) if (x->b) dcap_tex_copy(e, x, x->b);
    if (J->grid_b) [e copyFromBuffer:(__bridge id<MTLBuffer>)ib sourceOffset:io toBuffer:J->grid_b destinationOffset:0 size:12];
    if (J->lcap_b) [e copyFromBuffer:g_lcap_buf sourceOffset:0 toBuffer:J->lcap_b destinationOffset:0 size:g_lcap_buf.length];
    [e barrierAfterEncoderStages:MTLStageBlit beforeEncoderStages:MTLStageDispatch visibilityOptions:blit_vis()];
    return J;
} }
static void dcap_after(id ce, DcapJob *J) {
    id<MTL4ComputeCommandEncoder> e = ce;
    [e barrierAfterEncoderStages:MTLStageDispatch beforeEncoderStages:MTLStageBlit visibilityOptions:blit_vis()];
    for (DcapReg *r in J->regs) if (r->a) [e copyFromBuffer:r->src sourceOffset:r->off toBuffer:r->a destinationOffset:0 size:r->len];
    for (DcapTex *x in J->texs) if (x->a) dcap_tex_copy(e, x, x->a);
    if (J->lcap_a) [e copyFromBuffer:g_lcap_buf sourceOffset:0 toBuffer:J->lcap_a destinationOffset:0 size:g_lcap_buf.length];
    [e barrierAfterEncoderStages:MTLStageBlit beforeEncoderStages:MTLStageDispatch | MTLStageBlit visibilityOptions:blit_vis()];
    pthread_mutex_lock(&g_dcap_mu); [g_dcap_pending addObject:J]; pthread_cond_signal(&g_dcap_cv); pthread_mutex_unlock(&g_dcap_mu);
    MLOG("DM_NVN_DCAP: %s frame %llu fence %llu: capture encoded (%lu regions, %lu textures, %.1f MB of pool rows, %.1f MB of textures, %d rows not captured) → %s",
         J->key, (unsigned long long)J->frame, (unsigned long long)J->fence, (unsigned long)J->regs.count, (unsigned long)J->texs.count, J->pool_bytes / 1048576.0, J->tex_bytes / 1048576.0, J->nrows_skipped, J->dir);
}
static void dcap_wfile(const char *dir, const char *name, id<MTLBuffer> b, size_t n) {
    if (!b) return; char p[1300]; snprintf(p, sizeof p, "%s/%s", dir, name); FILE *f = fopen(p, "wb"); if (!f) { MLOG("[RED] dcap: cannot write %s", p); return; }
    fwrite(b.contents, 1, n, f); fclose(f);
}
static void dcap_write(DcapJob *J, int complete) { @autoreleasepool {
    [NSFileManager.defaultManager createDirectoryAtPath:@(J->dir) withIntermediateDirectories:YES attributes:nil error:nil];
    dcap_wfile(J->dir, "args.bin", J->args_b, J->arg_len);
    { char p[1300]; snprintf(p, sizeof p, "%s/args_cpu.bin", J->dir); FILE *f = fopen(p, "wb"); if (f) { fwrite(J->slots_cpu, 1, J->arg_len, f); fclose(f); } }
    if (J->grid_b) dcap_wfile(J->dir, "grid.bin", J->grid_b, 12);
    if (J->lcap_b) { dcap_wfile(J->dir, "lcap.bin", J->lcap_b, J->lcap_b.length); dcap_wfile(J->dir, "lcap.after.bin", J->lcap_a, J->lcap_a.length); }
    int i = 0; for (DcapReg *r in J->regs) { char n[64]; snprintf(n, sizeof n, "r%d.bin", i); dcap_wfile(J->dir, n, r->b, r->len); if (r->a) { snprintf(n, sizeof n, "r%d.after.bin", i); dcap_wfile(J->dir, n, r->a, r->len); } i++; }
    i = 0; for (DcapTex *x in J->texs) { char n[64]; if (x->b) { snprintf(n, sizeof n, "t%d.bin", i); dcap_wfile(J->dir, n, x->b, x->bytes); } if (x->a) { snprintf(n, sizeof n, "t%d.after.bin", i); dcap_wfile(J->dir, n, x->a, x->bytes); } i++; }
    char p[1300]; snprintf(p, sizeof p, "%s/manifest.json", J->dir); FILE *f = fopen(p, "w"); if (!f) { MLOG("[RED] dcap: cannot write %s", p); return; }
    uint32_t g[3] = {J->grid[0], J->grid[1], J->grid[2]}; if (J->grid_b) memcpy(g, J->grid_b.contents, 12);
    fprintf(f, "{\n \"format\": \"dcap1\", \"key\": \"%s\", \"library\": \"%s\", \"metallib\": \"%s\", \"chain\": \"%s\", \"frame\": %llu, \"fence\": %llu, \"seq\": %llu, \"complete\": %d,\n",
            J->key, J->over ? "override" : "base", J->lib, DMSH_CHAIN, (unsigned long long)J->frame, (unsigned long long)J->fence, (unsigned long long)J->seq, complete);
    fprintf(f, " \"indirect\": %d, \"grid\": [%u, %u, %u], \"grid_encoded\": [%u, %u, %u], \"threadgroup\": [%u, %u, %u], \"tg_spill\": %u, \"loopguard_sites\": %u, \"abi\": %d, \"arg_len\": %zu,\n",
            J->ind, g[0], g[1], g[2], J->grid[0], J->grid[1], J->grid[2], J->tg[0], J->tg[1], J->tg[2], J->tg_spill, J->loopguard, J->abi, J->arg_len);
    fprintf(f, " \"lcap\": %s, \"lcap_slot\": 28, \"tg_slot\": %d, \"gmem_entries\": %d, \"gmem_entry_bytes\": %d,\n", J->lcap_b ? "\"lcap.bin\"" : "null", DMSH_TG_SLOT, DMSH_GMEM_MAP_ENTRIES, DMSH_GMEM_MAP_ENTRY);
    fprintf(f, " \"bindings\": [\n");
    for (int k = 0; k < J->nb; k++) { const nvn_rbind *b = &J->bind[k]; int n = b->n ? b->n : 1;
        fprintf(f, "  {\"kind\": \"%s\", \"aid\": %u, \"n\": %d, \"reg\": %u, \"off\": %u, \"rw\": %u, \"dtt\": %u, \"dpf\": %u, \"atom\": %u, \"slots\": [", dcap_kind_name(b->kind), b->aid, n, b->reg, b->off, b->rw, b->dtt, b->dpf, b->atom);
        for (int q = 0; q < n && q < 8 && b->aid + q < (int)(J->arg_len / 8); q++) { uint64_t v; memcpy(&v, (const uint8_t *)J->args_b.contents + 8 * (b->aid + q), 8); fprintf(f, "%s\"%#llx\"", q ? ", " : "", (unsigned long long)v); }
        fprintf(f, "]}%s\n", k + 1 < J->nb ? "," : ""); }
    fprintf(f, " ],\n \"regions\": [\n");
    i = 0; for (DcapReg *r in J->regs) { fprintf(f, "  {\"file\": \"r%d.bin\", \"after\": %s, \"gpu\": \"%#llx\", \"len\": %llu, \"buf_len\": %lu, \"buf_off\": %llu, \"shared\": %d, \"why\": \"%s\"}%s\n", i,
        r->a ? [NSString stringWithFormat:@"\"r%d.after.bin\"", i].UTF8String : "null", (unsigned long long)(r->src.gpuAddress + r->off), (unsigned long long)r->len, (unsigned long)r->src.length,
        (unsigned long long)r->off, r->src.storageMode == MTLStorageModeShared, r->why, i + 1 < (int)J->regs.count ? "," : ""); i++; }
    fprintf(f, " ],\n \"textures\": [\n");
    i = 0; for (DcapTex *x in J->texs) { id<MTLTexture> t = x->t;
        fprintf(f, "  {\"file\": %s, \"after\": %s, \"rid\": \"%#llx\", \"type\": %lu, \"format\": %lu, \"width\": %lu, \"height\": %lu, \"depth\": %lu, \"array\": %lu, \"levels\": %lu, \"usage\": %lu, \"swizzle\": %u, "
                   "\"buffer_gpu\": %s, \"buffer_row\": %lu, \"bw\": %d, \"bh\": %d, \"bpb\": %d, \"bytes\": %zu, \"note\": \"%s\", \"lv\": [",
            x->b ? [NSString stringWithFormat:@"\"t%d.bin\"", i].UTF8String : "null", x->a ? [NSString stringWithFormat:@"\"t%d.after.bin\"", i].UTF8String : "null", (unsigned long long)x->rid,
            (unsigned long)t.textureType, (unsigned long)t.pixelFormat, (unsigned long)t.width, (unsigned long)t.height, (unsigned long)t.depth, (unsigned long)t.arrayLength, (unsigned long)t.mipmapLevelCount,
            (unsigned long)t.usage, (unsigned)(t.swizzle.red | t.swizzle.green << 8 | t.swizzle.blue << 16 | t.swizzle.alpha << 24),
            t.buffer ? [NSString stringWithFormat:@"\"%#llx\"", (unsigned long long)(t.buffer.gpuAddress + t.bufferOffset)].UTF8String : "null", (unsigned long)(t.buffer ? t.bufferBytesPerRow : 0), x->bw, x->bh, x->bpb, x->bytes, x->note);
        for (int l = 0; l < x->nl; l++) fprintf(f, "%s[%u, %u, %u, %u, %zu, %zu, %zu]", l ? ", " : "", x->lv[l].w, x->lv[l].h, x->lv[l].d, x->lv[l].slices, x->lv[l].row, x->lv[l].img, x->lv[l].off);
        fprintf(f, "]}%s\n", i + 1 < (int)J->texs.count ? "," : ""); i++; }
    fprintf(f, " ],\n \"rows\": [\n");
    const dcap_row *rw = J->rows.bytes; int nr = (int)(J->rows.length / sizeof *rw);
    for (int k = 0; k < nr; k++) fprintf(f, "  [%llu, \"%#llx\", \"%#llx\", \"%#llx\", %d, %llu]%s\n", (unsigned long long)rw[k].i, (unsigned long long)rw[k].lo, (unsigned long long)rw[k].hi, (unsigned long long)rw[k].delta,
                                         rw[k].captured, (unsigned long long)rw[k].cap_len, k + 1 < nr ? "," : "");
    fprintf(f, " ],\n \"rows_fields\": [\"index\", \"guest lo\", \"guest hi\", \"delta (gpu - guest)\", \"captured\", \"captured bytes from lo\"],\n \"rows_not_captured\": %d, \"pool_bytes\": %llu, \"tex_bytes\": %llu\n}\n",
            J->nrows_skipped, (unsigned long long)J->pool_bytes, (unsigned long long)J->tex_bytes);
    fclose(f);
    MLOG("DM_NVN_DCAP: %s frame %llu written to %s%s", J->key, (unsigned long long)J->frame, J->dir, complete ? "" : " ([RED] the submission did not complete in 30 s: contents not valid)");
    // the staging goes out of the residency set (the encode thread's next res_commit applies it)
    res_remove(J->args_b); res_remove(J->grid_b); res_remove(J->lcap_b); res_remove(J->lcap_a);
    for (DcapReg *r in J->regs) { res_remove(r->b); res_remove(r->a); }
    for (DcapTex *x in J->texs) { res_remove(x->b); res_remove(x->a); }
} }
static void *dcap_writer(void *arg) {
    (void)arg; pthread_setname_np("dm-dcap");
    for (;;) {
        DcapJob *J = nil;
        pthread_mutex_lock(&g_dcap_mu);
        while (!g_dcap_pending.count) pthread_cond_wait(&g_dcap_cv, &g_dcap_mu);
        J = g_dcap_pending[0]; [g_dcap_pending removeObjectAtIndex:0];
        pthread_mutex_unlock(&g_dcap_mu);
        uint64_t t0 = now_ns(); while (atomic_load(&g_done) < J->fence && now_ns() - t0 < 30000000000ull) usleep(20000);
        dcap_write(J, atomic_load(&g_done) >= J->fence);
    }
    return NULL;
}
