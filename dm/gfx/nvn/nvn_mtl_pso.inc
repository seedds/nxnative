// nvn_mtl.m, part: pso
// Pipelines: the Metal 4 base pipelines and the per-game archive, parallel precompile, hybrid pipelines, the prebuild, pso_get and the warm-up worker.
// Included by nvn_mtl.m in its original order, as one translation unit: statics are shared with the other parts.
// ---- Metal 4 pipelines: a base per pipeline with its colour state unspecialized, a per-game archive of the bases ----
// pso_desc stays the description of a pipeline. pso_build turns it into an MTL4RenderPipelineDescriptor whose colour attachments (format, blending, factors, operations, write mask)
// are unspecialized, builds that base once (MTL4Compiler, or takes it from the game's pipeline archive) and makes each pipeline from it by specialization: about 4 µs on an M2 Max,
// where building a pipeline whose colour state alone differs took 3-5 ms. Breath of the Wild's 4,829 recorded pipelines came from 1,818 such bases. Metal 4 pipelines name no
// depth / stencil format, so a base serves every depth / stencil attachment.
// The archive keeps the bases beside pso_user.tsv, in pso_archive/: files written by an MTL4PipelineDataSetSerializer that captures binaries only (with the descriptor capture as
// well it writes no archive and reports no error). Each serialization writes what was captured since the one before, so the folder holds one file per flush; all of them are opened
// at start, and a base found in one is taken from it directly (MTL4Archive), not through the compiler's lookupArchives, which would capture it again into the next file. A base
// taken from an archive costs about 0.06 ms, against 10-80 ms to build one.
// DM_PSO_MTL4=0: the MTLDevice path as before (no bases, no archive). DM_PSO_ARCHIVE=off: no archive; DM_PSO_ARCHIVE=<folder>: the archive in that folder.
// On by default on the Mac only; on iOS DM_PSO_MTL4=1 turns it on, and with it the hybrid pipelines and the pipeline prebuild. The archive's footprint (with the prebuild, about
// 850 MB more for Breath of the Wild) is not measured against an iPhone's limit yet, and an iPhone cannot be given an archive made offline (metal-tt rejects the Metal 4 serializer's
// pipeline script)
static int pso4_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_PSO_MTL4");
#if TARGET_OS_IPHONE
        v = e && !strcmp(e, "1");
        if (!v) MLOG("render pipelines through the MTLDevice (iOS: DM_PSO_MTL4=1 turns on the Metal 4 bases, the pipeline archive and the hybrid pipelines)");
#else
        v = !(e && !strcmp(e, "0"));
        if (!v) MLOG("DM_PSO_MTL4=0: render pipelines are built through the MTLDevice (no bases, no pipeline archive)");
#endif
    }
    return v;
}
static id<MTL4Compiler> g_comp4; static id<MTL4PipelineDataSetSerializer> g_ser4;
// g_arch4: the files that may hold bases with a vertex layout, g_arch4p: the files that hold none (p<seq>.mtl4a: the prebuild's, and any flush whose bases all read their vertices
// themselves). A base with a layout is looked up in g_arch4 alone: each file a lookup misses in costs about 0.06 ms, and a cold start's misses were for such bases, in the 13 prebuilt files
static NSArray<id<MTL4Archive>> *g_arch4, *g_arch4p; static NSString *g_arch_dir; static int g_arch_seq;
// Compaction: every start writes a file or more (each flush captures only what is new), Breath of the Wild 5-7 per start, and each file a base is not in costs a lookup about 0.06 ms.
// With more than PSO_ARCH_COMPACT files of bases with a vertex layout, the bases the prewarm takes from them are taken through the compiler with those files as its lookup archives,
// which captures them again, so the flush after the prewarm writes them into one file; the old files are then removed (open archives stay usable for the rest of the run). Bases the
// prewarm did not take (pso_user.tsv never named them) go with them and are built again when drawn. The prebuild's files (p<seq>) are left as they are
enum { PSO_ARCH_COMPACT = 8 };
static MTL4CompilerTaskOptions *g_compact_opts; static NSArray<NSString *> *g_compact_files; static _Atomic int g_compacted;
static _Atomic int g_arch_new_vd;   // bases with a vertex layout captured since the last flush (counted under g_arch_rw's read side, read under its write side)
static dispatch_queue_t g_arch_q; static _Atomic int g_arch_new, g_arch_sched;
// Compiles with the serializer attached hold this shared, a flush holds it alone: a compile that finishes during serializeAsArchiveAndFlushToURL crashed in Metal
// (-[_MTLBinaryArchive newArchiverIdWithBinaryKey:...], the prebuild's first flush). Specializations and archive lookups do not take it (they capture nothing)
static pthread_rwlock_t g_arch_rw = PTHREAD_RWLOCK_INITIALIZER;
static _Atomic uint64_t g_pb_built, g_pb_archived, g_pb_reused, g_pb_spec, g_pb_legacy, g_pb_base_ns;
// Time by kind, all threads (the 'pipelines up to frame' line): archive lookups that found the base / found none, specializations, base compiles; predictions (submit thread)
static _Atomic uint64_t g_pt_hit_n, g_pt_hit_ns, g_pt_miss_n, g_pt_miss_ns, g_pt_spec_ns, g_pt_comp_ns, g_pt_pred_n, g_pt_pred_ns, g_pp_done_n;   // bases compiled / taken from the archive / reused, specializations, MTLDevice builds
typedef struct { const void *vf, *ff; uint32_t samples, a2c, rast, topo; uint32_t va[31][3], vl[31][3]; } pbase_key;   // what a base depends on: pso_desc's descriptor less the colour attachments
static nvn_hmap g_pbase = {.klen = sizeof(pbase_key)};
static os_unfair_lock g_pbase_lock = OS_UNFAIR_LOCK_INIT;
static NSString *pso_user_path(void);
static void pso4_init(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        if (!pso4_on() || ![(id)g_dev respondsToSelector:@selector(newCompilerWithDescriptor:error:)]) return;   // no Metal 4 compiler (t_pso_pending's fake device): the MTLDevice path
        const char *e = getenv("DM_PSO_ARCHIVE"); NSString *user = pso_user_path();
        g_arch_dir = e && !strcmp(e, "off") ? nil : e && *e ? @(e) : user ? [user.stringByDeletingLastPathComponent stringByAppendingPathComponent:@"pso_archive"] : nil;
        id<MTL4PipelineDataSetSerializer> ser = nil;
        if (g_arch_dir) {
            g_arch_q = dispatch_queue_create("dm.nvn.pso-archive", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0));
            MTL4PipelineDataSetSerializerDescriptor *sd = [MTL4PipelineDataSetSerializerDescriptor new]; sd.configuration = MTL4PipelineDataSetSerializerConfigurationCaptureBinaries;
            ser = [g_dev newPipelineDataSetSerializerWithDescriptor:sd];
            NSMutableArray<id<MTL4Archive>> *arch = [NSMutableArray array], *archp = [NSMutableArray array]; uint64_t t0 = bp_now(); unsigned long long bytes = 0;
            NSMutableArray<NSString *> *runf = [NSMutableArray array];
            NSArray<NSString *> *names = [[[NSFileManager.defaultManager contentsOfDirectoryAtPath:g_arch_dir error:nil] filteredArrayUsingPredicate:[NSPredicate predicateWithFormat:@"self ENDSWITH '.mtl4a'"]]
                                          sortedArrayUsingSelector:@selector(compare:)];
            for (NSString *n in names.reverseObjectEnumerator) {   // newest first
                NSString *p = [g_arch_dir stringByAppendingPathComponent:n]; NSError *ae = nil;
                id<MTL4Archive> a = [g_dev newArchiveWithURL:[NSURL fileURLWithPath:p] error:&ae];
                int pf = [n hasPrefix:@"p"];
                if (a) { [pf ? archp : arch addObject:a]; if (!pf) [runf addObject:p]; bytes += [[NSFileManager.defaultManager attributesOfItemAtPath:p error:nil] fileSize]; }
                else MLOG("[RED] pipeline archive file %s not usable (%s): its bases are built again", p.UTF8String, ae.localizedDescription.UTF8String);
                g_arch_seq = MAX(g_arch_seq, [n substringFromIndex:pf].intValue + 1);
            }
            g_arch4 = arch; g_arch4p = archp;
            if (runf.count > PSO_ARCH_COMPACT) { g_compact_opts = [MTL4CompilerTaskOptions new]; g_compact_opts.lookupArchives = arch; g_compact_files = runf; }
            MLOG("pipeline archive %s: %lu files (%lu of bases without a vertex layout), %.1f MB, opened in %.1f ms", g_arch_dir.UTF8String, (unsigned long)(arch.count + archp.count),
                 (unsigned long)archp.count, bytes / 1048576.0, (bp_now() - t0) / 1e6);
        }
        MTL4CompilerDescriptor *cd = [MTL4CompilerDescriptor new]; cd.pipelineDataSetSerializer = ser;
        NSError *ce = nil; g_comp4 = [g_dev newCompilerWithDescriptor:cd error:&ce];
        if (!g_comp4) MLOG("[RED] no Metal 4 compiler (%s): render pipelines are built through the MTLDevice", ce.localizedDescription.UTF8String);
        else g_ser4 = ser;
    });
}
// Writes what the serializer captured since the last flush as the next file of the archive (on g_arch_q); g_arch_flush_ok: the last one wrote its file
static int g_arch_flush_ok;
static void arch_flush(void) {
    g_arch_flush_ok = 0;
    int n = atomic_exchange(&g_arch_new, 0);
    if (!n || !g_ser4 || !g_arch_dir) return;
    [NSFileManager.defaultManager createDirectoryAtPath:g_arch_dir withIntermediateDirectories:YES attributes:nil error:nil];
    uint64_t t0 = bp_now(); NSError *e = nil;
    pthread_rwlock_wrlock(&g_arch_rw);
    int vd = atomic_exchange(&g_arch_new_vd, 0);   // exact: no compile is between capture and count while the write side is held
    NSString *path = [g_arch_dir stringByAppendingPathComponent:[NSString stringWithFormat:vd ? @"%05d.mtl4a" : @"p%05d.mtl4a", g_arch_seq++]], *tmp = [path stringByAppendingString:@".tmp"];
    BOOL ok = [g_ser4 serializeAsArchiveAndFlushToURL:[NSURL fileURLWithPath:tmp] error:&e]; pthread_rwlock_unlock(&g_arch_rw);
    ok = ok && !rename(tmp.fileSystemRepresentation, path.fileSystemRepresentation);
    if (!ok) { MLOG("[RED] pipeline archive: writing %s failed%s%s", path.UTF8String, e ? ": " : "", e ? e.localizedDescription.UTF8String : ""); unlink(tmp.fileSystemRepresentation); return; }
    g_arch_flush_ok = 1;
    MLOG("pipeline archive: %d new pipeline bases written to %s (%.1f MB, %.0f ms)", n, path.lastPathComponent.UTF8String,
         [[NSFileManager.defaultManager attributesOfItemAtPath:path error:nil] fileSize] / 1048576.0, (bp_now() - t0) / 1e6);
}
static void arch_note_new(void) {   // a base was compiled: flush 10 s later (one flush for a burst of them)
    atomic_fetch_add(&g_arch_new, 1);
    if (!g_arch_q || atomic_exchange(&g_arch_sched, 1)) return;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 10 * (int64_t)NSEC_PER_SEC), g_arch_q, ^{ atomic_store(&g_arch_sched, 0); arch_flush(); });
}
static void arch_flush_now(void) { if (g_arch_q) dispatch_async(g_arch_q, ^{ arch_flush(); }); }
static void arch_compact_finish(void) {   // after the flush that follows the prewarm: the old files go once their bases are in the new one
    if (!g_compact_files || !g_compacted) return;
    if (!g_arch_flush_ok) { MLOG("[RED] pipeline archive: compaction left the old files (the flush failed)"); return; }
    for (NSString *f in g_compact_files) unlink(f.fileSystemRepresentation);
    MLOG("pipeline archive compacted: %lu files of bases with a vertex layout gave %d bases to the file just written and were removed", (unsigned long)g_compact_files.count, (int)g_compacted);
    g_compact_files = nil;
}
static void pso_col4(MTL4RenderPipelineColorAttachmentDescriptor *a, MTLRenderPipelineColorAttachmentDescriptor *c) {
    a.pixelFormat = c.pixelFormat; a.writeMask = c.writeMask; a.blendingState = c.blendingEnabled ? MTL4BlendStateEnabled : MTL4BlendStateDisabled;
    a.sourceRGBBlendFactor = c.sourceRGBBlendFactor; a.destinationRGBBlendFactor = c.destinationRGBBlendFactor; a.rgbBlendOperation = c.rgbBlendOperation;
    a.sourceAlphaBlendFactor = c.sourceAlphaBlendFactor; a.destinationAlphaBlendFactor = c.destinationAlphaBlendFactor; a.alphaBlendOperation = c.alphaBlendOperation;
}
// pso_desc's descriptor as a Metal 4 one: unspec leaves every colour attachment unspecialized (the base), else copies them. nil when a function has no Metal 4 descriptor (fn4_of)
static MTL4RenderPipelineDescriptor *pso_desc4(MTLRenderPipelineDescriptor *d, int unspec) {
    MTL4FunctionDescriptor *vf = fn4_of(d.vertexFunction), *ff = d.fragmentFunction ? fn4_of(d.fragmentFunction) : nil;
    if (!vf || (d.fragmentFunction && !ff)) return nil;
    MTL4RenderPipelineDescriptor *m = [MTL4RenderPipelineDescriptor new];
    m.vertexFunctionDescriptor = vf; m.fragmentFunctionDescriptor = ff; m.vertexDescriptor = d.vertexDescriptor;
    m.rasterSampleCount = d.rasterSampleCount; m.alphaToCoverageState = d.alphaToCoverageEnabled ? MTL4AlphaToCoverageStateEnabled : MTL4AlphaToCoverageStateDisabled;
    m.rasterizationEnabled = d.rasterizationEnabled; m.inputPrimitiveTopology = d.inputPrimitiveTopology;
    for (int i = 0; i < 8; i++) {
        MTL4RenderPipelineColorAttachmentDescriptor *a = m.colorAttachments[i];
        if (!unspec) { pso_col4(a, d.colorAttachments[i]); continue; }
        a.pixelFormat = MTLPixelFormatUnspecialized; a.writeMask = MTLColorWriteMaskUnspecialized; a.blendingState = MTL4BlendStateUnspecialized;
        a.sourceRGBBlendFactor = a.destinationRGBBlendFactor = a.sourceAlphaBlendFactor = a.destinationAlphaBlendFactor = MTLBlendFactorUnspecialized;
        a.rgbBlendOperation = a.alphaBlendOperation = MTLBlendOperationUnspecialized;
    }
    return m;
}
static void pbase_key_of(MTLRenderPipelineDescriptor *d, pbase_key *k) {
    memset(k, 0, sizeof *k);
    k->vf = (__bridge const void *)d.vertexFunction; k->ff = (__bridge const void *)d.fragmentFunction;
    k->samples = (uint32_t)d.rasterSampleCount; k->a2c = d.alphaToCoverageEnabled; k->rast = d.rasterizationEnabled; k->topo = (uint32_t)d.inputPrimitiveTopology;
    MTLVertexDescriptor *vd = d.vertexDescriptor;
    for (NSUInteger i = 0; vd && i < 31; i++) {
        MTLVertexAttributeDescriptor *a = vd.attributes[i]; MTLVertexBufferLayoutDescriptor *l = vd.layouts[i];
        k->va[i][0] = (uint32_t)a.format; k->va[i][1] = (uint32_t)a.offset; k->va[i][2] = (uint32_t)a.bufferIndex;
        k->vl[i][0] = (uint32_t)l.stride; k->vl[i][1] = (uint32_t)l.stepFunction; k->vl[i][2] = (uint32_t)l.stepRate;
    }
}
// The pipeline pso_desc describes: its base (built, from the archive or already made), then specialized with its colour state. Called on several threads.
// compile 0: only when the base is made or in the archive (no wait for the Metal compiler: nil otherwise, *e untouched); the MTLDevice fallback then gives nil too
static id<MTLRenderPipelineState> pso_build_ex(MTLRenderPipelineDescriptor *d, NSError **e, int compile) {
    pso4_init();
    int rast = d.rasterizationEnabled;
    MTL4RenderPipelineDescriptor *b = g_comp4 ? pso_desc4(d, rast) : nil;   // with rasterization off there is no colour state to leave open: the base is the pipeline
    if (!b) { if (!compile) return nil; g_pb_legacy++; return [g_dev newRenderPipelineStateWithDescriptor:d error:e]; }
    pbase_key bk; pbase_key_of(d, &bk);
    os_unfair_lock_lock(&g_pbase_lock); id<MTLRenderPipelineState> base = hm_get_id(&g_pbase, &bk); os_unfair_lock_unlock(&g_pbase_lock);
    if (base) g_pb_reused++;
    else {
        uint64_t t0 = bp_now();
        int vd = 0; for (int i = 0; i < 31 && !vd; i++) vd = bk.va[i][0] != 0;   // a vertex layout (MTLVertexFormatInvalid is 0)
        if (!vd) for (id<MTL4Archive> a in g_arch4p) if ((base = [a newRenderPipelineStateWithDescriptor:b error:nil])) break;
        int from_run = 0;
        if (!base) for (id<MTL4Archive> a in g_arch4) if ((base = [a newRenderPipelineStateWithDescriptor:b error:nil])) { from_run = 1; break; }
        uint64_t t1 = bp_now();
        if (base) { g_pb_archived++; g_pt_hit_n++; g_pt_hit_ns += t1 - t0; }
        if (base && from_run && g_compact_opts && atomic_load(&g_warm_running)) {   // compaction (see g_compact_opts): taken again through the compiler, which captures it
            pthread_rwlock_rdlock(&g_arch_rw);
            id<MTLRenderPipelineState> cb = [g_comp4 newRenderPipelineStateWithDescriptor:b compilerTaskOptions:g_compact_opts error:nil];
            if (cb) { base = cb; atomic_fetch_add(&g_arch_new, 1); g_compacted++; if (vd) g_arch_new_vd++; }
            pthread_rwlock_unlock(&g_arch_rw);
        }
        if (!base) {
            if (g_arch4.count + g_arch4p.count) { g_pt_miss_n++; g_pt_miss_ns += t1 - t0; }
            if (!compile) return nil;
            pthread_rwlock_rdlock(&g_arch_rw); base = [g_comp4 newRenderPipelineStateWithDescriptor:b compilerTaskOptions:nil error:e]; if (base && vd) g_arch_new_vd++; pthread_rwlock_unlock(&g_arch_rw);
            g_pt_comp_ns += bp_now() - t1;
            if (!base) return nil;
            g_pb_built++; if (g_ser4) arch_note_new();
        }
        g_pb_base_ns += bp_now() - t0;
        os_unfair_lock_lock(&g_pbase_lock); id old = hm_get_id(&g_pbase, &bk); if (old) base = old; else hm_put_id(&g_pbase, &bk, base); os_unfair_lock_unlock(&g_pbase_lock);
    }
    if (!rast) return base;
    uint64_t ts = bp_now();
    MTL4RenderPipelineDescriptor *s = (MTL4RenderPipelineDescriptor *)[base newRenderPipelineDescriptorForSpecialization];
    for (int i = 0; i < 8; i++) pso_col4(s.colorAttachments[i], d.colorAttachments[i]);
    id<MTLRenderPipelineState> ps = [g_comp4 newRenderPipelineStateBySpecializationWithDescriptor:s pipeline:base error:e];
    if (ps) g_pb_spec++;
    g_pt_spec_ns += bp_now() - ts;
    return ps;
}
static id<MTLRenderPipelineState> pso_build(MTLRenderPipelineDescriptor *d, NSError **e) { return pso_build_ex(d, e, 1); }
// What no pipeline depends on leaves the key: the blend state of targets whose blending is off (pso_desc reads blend[l] only where blend_en has bit l). Breath of the Wild recorded
// 4,829 keys for 1,982 different pipelines, 2,845 of them told apart by such blend state alone. The depth / stencil formats stay: a Metal 4 base names none (pbase_key leaves them
// out, so keys that differ only there cost one specialization each), but a pipeline built through the MTLDevice must name the pass's
static void pso_key_norm(pso_key *k) {
    for (int i = 0; i < 8; i++) if (!(k->blend_en >> i & 1)) memset(&k->blend[i], 0, sizeof k->blend[i]);
}
// Pipeline key: current state S + pass formats (normal pass; a merged pass additionally carries lmap / specialized function, filled in by pso_get)
static void pso_key_fill(pso_key *k, NvShader *vs, NvShader *fs, const uint32_t cfmt[8], uint32_t dfmt, uint32_t sfmt, uint32_t samples, int lmap) {
    memset(k, 0, sizeof *k);
    k->vs = (__bridge void *)vs; k->fs = (__bridge void *)fs;
    memcpy(k->cfmt, cfmt, sizeof k->cfmt); k->dfmt = dfmt; k->sfmt = sfmt; k->samples = samples;
    k->blend_en = S.color.blend_en; k->chmask = S.chmask.mask; k->a2c = S.ms.a2c;
    for (int i = 0; i < 8; i++) if (k->cfmt[i] || lmap) k->blend[i] = S.blend[i];
    const nvn_refl *rf = &vs->rf;
    for (int v = 0; v < rf->nv; v++) { int a = rf->v[v].nvn; if (a < 16 && a < S.nattr) { k->attr[a] = S.attr[a]; int st = S.attr[a].stream; if (st < 16) k->stream[st] = S.stream[st]; } }
    pso_key_norm(k);
}
// ---- Parallel precompile: pipelines met for the first time used to be compiled synchronously one by one on the submit thread; 9 misses in a row in one frame = 284 ms (heavy scene on a real device).
// After satisfying the first pipeline miss in a submission, scan ahead once along the remaining commands (replaying state only, no drawing) and kick a bounded number of normal-pass pipelines to the background;
// when the submit thread reaches that draw it only waits for 'the remaining time not yet compiled'. The compiled pipelines are identical to synchronously compiled ones (same descriptor). Pipeline keys of merged passes / specialized fragments depend on the planning result at replay time, are not predicted, and are compiled synchronously as before.
#define PSO_PEND_MAX 384
enum { PSO_PEND_ACTIVE = 2, PSO_PREDICT_BUDGET = 2 };
// order: admission order (the oldest finished, untaken entry is reclaimed first); demand: a build a draw asked for while it draws with its pulling stand-in (hybrid pipelines),
// outside the speculative limit; run: 0 queued, 1 taken by its worker; d: the descriptor while queued (a draw that would wait for a queued build makes the pipeline itself:
// pso_pend_take); gen tells a slot's fills apart, so a worker whose slot was taken that way and filled again does nothing
static struct { pso_key k; id<MTLRenderPipelineState> ps; MTLRenderPipelineDescriptor *d; int used, done, run, demand; uint32_t gen; uint64_t order; } g_pp[PSO_PEND_MAX];
static pthread_mutex_t g_pp_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_pp_cv = PTHREAD_COND_INITIALIZER;
static uint64_t g_pp_submit_mark, g_pp_kicked, g_pp_taken, g_pp_stolen;   // Whether this submission has already been predicted (by submission serial); cumulative kicked / taken counts
static void pso_predict_rest(void);
static int pso_pend_find(const pso_key *k) { for (int i = 0; i < PSO_PEND_MAX; i++) if (g_pp[i].used && !memcmp(&g_pp[i].k, k, sizeof *k)) return i; return -1; }
static int hybrid_on(void);
// The queue of the builds hybrid draws asked for (the draw has a pulling stand-in, so nothing waits for them): serial queues at utility QoS, one per two performance cores, round
// robin. At user-initiated QoS on the global queue, a cold start's burst (777 builds on entering Zora's Domain, Breath of the Wild) ran on every core, beside the game's own threads,
// for the seconds it took. Speculative builds (pso_pend_kick: at most PSO_PEND_ACTIVE at a time) keep the global queue, so a demanded build never waits behind speculation
static dispatch_queue_t pp_bg_queue(void) {
    static dispatch_queue_t q[8]; static int n; static _Atomic unsigned rr; static dispatch_once_t once;
    dispatch_once(&once, ^{ int pc = 0; size_t sz = sizeof pc; if (sysctlbyname("hw.perflevel0.physicalcpu", &pc, &sz, NULL, 0) || pc < 2) pc = 2;
        n = pc / 2 > 8 ? 8 : pc / 2;
        for (int i = 0; i < n; i++) q[i] = dispatch_queue_create("dm.nvn.pso-bg", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0)); });
    return q[atomic_fetch_add(&rr, 1) % (unsigned)n];
}
// Draws that waited for their pipeline on the submit thread, by why no stand-in served them: 0 hybrid pipelines off, 1 merged pass or readable-tile fragment, 2 diagnostic fragment,
// 3 no pulling entry (layer-writing or transform feedback vertex shader, or no pulling library), 4 pulling base not made yet; count and time (prediction walks left out)
static uint64_t g_pw_n[5], g_pw_ns[5];
static const char *const g_pw_why[5] = {"hybrid off", "merged pass or readable tile", "diagnostic fragment", "no pulling entry", "pulling base not made yet"};
// Takes a slot for k (with g_pp_mu held): a free one, else the oldest finished entry no draw took (its worker no longer touches it). spec: a speculative build, refused while
// PSO_PEND_ACTIVE speculative builds run. Returns the slot, -1 when refused or k is in the table already
static int pso_pend_slot(const pso_key *k, int spec, uint32_t *gen) {
    int slot = -1, reclaim = -1, active = 0;
    if (pso_pend_find(k) >= 0) return -1;
    for (int i = 0; i < PSO_PEND_MAX; i++) {
        if (!g_pp[i].used) { if (slot < 0) slot = i; }
        else if (!g_pp[i].done) active += !g_pp[i].demand;
        else if (reclaim < 0 || g_pp[i].order < g_pp[reclaim].order) reclaim = i;
    }
    if (spec && active >= PSO_PEND_ACTIVE) return -1;
    if (slot < 0) slot = reclaim;
    if (slot < 0) return -1;
    g_pp[slot].k = *k; g_pp[slot].ps = nil; g_pp[slot].d = nil; g_pp[slot].done = g_pp[slot].run = 0; g_pp[slot].used = 1; g_pp[slot].demand = !spec;
    g_pp[slot].order = ++g_pp_kicked; *gen = ++g_pp[slot].gen;
    return slot;
}
static void pso_pend_start(int slot, uint32_t gen, MTLRenderPipelineDescriptor *d, dispatch_queue_t q) {
    pthread_mutex_lock(&g_pp_mu); if (g_pp[slot].gen == gen) g_pp[slot].d = d; pthread_mutex_unlock(&g_pp_mu);   // only the submit thread takes a queued build, so none has yet
    dispatch_async(q, ^{
        pthread_mutex_lock(&g_pp_mu);
        int go = g_pp[slot].used && g_pp[slot].gen == gen && !g_pp[slot].run; if (go) { g_pp[slot].run = 1; g_pp[slot].d = nil; }
        pthread_mutex_unlock(&g_pp_mu);
        if (!go) return;   // the draw made it itself
        NSError *e = nil; id<MTLRenderPipelineState> ps = pso_build(d, &e);
        pthread_mutex_lock(&g_pp_mu); if (g_pp[slot].gen == gen) { g_pp[slot].ps = ps; g_pp[slot].done = 1; } pthread_cond_broadcast(&g_pp_cv); pthread_mutex_unlock(&g_pp_mu); g_pp_done_n++;
    });
}
static int pso_pend_kick(const pso_key *k, NvShader *vs, NvShader *fs) {   // Nonblocking admission of a speculative build: no speculative backlog in front of a demanded compile
    if (hm_get_id(&g_pso, k)) return 0;
    pthread_mutex_lock(&g_pp_mu); uint32_t gen = 0; int slot = pso_pend_slot(k, 1, &gen); pthread_mutex_unlock(&g_pp_mu);
    if (slot < 0) return 0;
    pso_pend_start(slot, gen, pso_desc(k, vs, fs, nil, 0), dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0));
    return 1;
}
// The build a hybrid draw asked for while it draws with its stand-in: outside the speculative limit, on pp_bg_queue
static int pso_pend_demand(const pso_key *k, NvShader *vs, NvShader *fs) {
    if (hm_get_id(&g_pso, k)) return 0;
    pthread_mutex_lock(&g_pp_mu); uint32_t gen = 0; int slot = pso_pend_slot(k, 0, &gen); pthread_mutex_unlock(&g_pp_mu);
    if (slot < 0) return 0;
    pso_pend_start(slot, gen, pso_desc(k, vs, fs, nil, 0), pp_bg_queue());
    return 1;
}
// Called by the submit thread: a build in the table is taken (waited for while its worker runs it; made here at once while it is still queued, so a draw that waits waits for its
// own pipeline alone, not for the builds queued before it: Breath of the Wild's cold start waited 0.5-0.9 s for one pipeline behind a burst); nil when it is not in the table
static id<MTLRenderPipelineState> pso_pend_take(const pso_key *k) {
    pthread_mutex_lock(&g_pp_mu);
    int at = pso_pend_find(k); id<MTLRenderPipelineState> ps = nil;
    if (at >= 0 && !g_pp[at].done && !g_pp[at].run && g_pp[at].d) {
        MTLRenderPipelineDescriptor *d = g_pp[at].d; g_pp[at].d = nil; g_pp[at].used = 0;
        pthread_mutex_unlock(&g_pp_mu);
        g_pp_stolen++; NSError *e = nil; return pso_build(d, &e);
    }
    if (at >= 0) { while (!g_pp[at].done) pthread_cond_wait(&g_pp_cv, &g_pp_mu); ps = g_pp[at].ps; g_pp[at].ps = nil; g_pp[at].used = 0; }
    pthread_mutex_unlock(&g_pp_mu);
    return ps;
}
// Without waiting: 0 = not in the table; 1 = in it, still being built; 2 = built and taken into *out (nil when the build failed)
static int pso_pend_try(const pso_key *k, id<MTLRenderPipelineState> *out) {
    pthread_mutex_lock(&g_pp_mu);
    int at = pso_pend_find(k), st = at < 0 ? 0 : g_pp[at].done ? 2 : 1;
    *out = nil;
    if (st == 2) { *out = g_pp[at].ps; g_pp[at].ps = nil; g_pp[at].d = nil; g_pp[at].used = 0; }
    pthread_mutex_unlock(&g_pp_mu);
    return st;
}
// ---- Hybrid pipelines: a draw never waits for the Metal compiler ----
// On a pipeline miss the draw first takes its pipeline without compiling (the base made already, or in the pipeline archive: pso_build_ex(..., 0)). Failing that it draws with the
// pair's pulling pipeline (<key>_bp, dmsh_pull.py: no vertex descriptor; the vertex streams are read through a per-draw layout block, pull_bind), specialized with the draw's colour
// state, while the pipeline for its vertex layout is built in the background (pso_pend_demand); a later draw takes that one when it is ready (pso_pend_try). The pulling pipeline costs
// vertex time (the run-time format decode: about 1 ms per million vertices on an M2 Max), so it only bridges those frames. Both compute the same positions: [[position, invariant]],
// built with preserved invariance, from bit-identical inputs (t_pull.m, t_pull_inv.m). Only a pulling base already made or in the archive is used (the prebuild makes them); otherwise
// the draw waits as before and the pulling base is built in the background for the pair's next miss. Not for merged passes, readable-tile or diagnostic fragments, layer-writing or
// transform feedback vertex shaders. DM_PSO_HYBRID=0 turns it off
static int hybrid_on(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("DM_PSO_HYBRID"); pso4_init(); v = g_comp4 && !(e && !strcmp(e, "0")); if (!v) MLOG("hybrid pipelines off: a draw whose pipeline is not built yet waits for it"); }
    return v;
}
static nvn_hmap g_pso_hy = {.klen = sizeof(pso_key)};   // pulling pipelines by the key they stand in for (submit thread)
static int g_pso_pull;   // the pipeline pso_get_ returned reads its vertex streams through the layout block
static _Atomic uint64_t g_hy_draws, g_hy_made, g_hy_none, g_hy_kicked, g_hy_ns;   // draws with a pulling pipeline, pulling pipelines made, misses with no pulling base ready, pulling bases built for later, time
static NSMutableSet<NSData *> *g_hy_kick_set; static os_unfair_lock g_hy_lock = OS_UNFAIR_LOCK_INIT;
static MTLRenderPipelineDescriptor *pso_desc_pull(const pso_key *k, NvShader *vs, NvShader *fs) {   // pso_desc's pipeline with the pulling entry point and no vertex descriptor
    id<MTLFunction> pf = pull_fn(vs); if (!pf) return nil;
    MTLRenderPipelineDescriptor *d = pso_desc(k, vs, fs, nil, 0);
    d.vertexFunction = pf; d.vertexDescriptor = [MTLVertexDescriptor vertexDescriptor];
    return d;
}
static __unsafe_unretained id<MTLRenderPipelineState> hybrid_get(const pso_key *k, NvShader *vs, NvShader *fs) {
    __unsafe_unretained id<MTLRenderPipelineState> h = hm_get_id(&g_pso_hy, k);
    if (h) return h;
    MTLRenderPipelineDescriptor *d = vs->has_pull ? pso_desc_pull(k, vs, fs) : nil;
    if (!d) return nil;
    NSError *e = nil; id<MTLRenderPipelineState> ps = pso_build_ex(d, &e, 0);
    if (!ps) {   // the pulling base is neither made nor archived: build it in the background for the pair's next miss
        pbase_key bk; pbase_key_of(d, &bk); NSData *kd = [NSData dataWithBytes:&bk length:sizeof bk];
        os_unfair_lock_lock(&g_hy_lock); if (!g_hy_kick_set) g_hy_kick_set = [NSMutableSet set];
        int fresh = ![g_hy_kick_set containsObject:kd]; if (fresh) [g_hy_kick_set addObject:kd]; os_unfair_lock_unlock(&g_hy_lock);
        if (fresh) { g_hy_kicked++; dispatch_async(pp_bg_queue(), ^{ NSError *be = nil; (void)pso_build(d, &be); }); }
        g_hy_none++; return nil;
    }
    g_hy_made++; hm_put_id(&g_pso_hy, k, ps);
    return ps;
}
void mtl_hybrid_stats(uint64_t *draws, uint64_t *made, uint64_t *none) { *draws = g_hy_draws; *made = g_hy_made; *none = g_hy_none; }
// The pipeline time line of the timing summary (hle_nvn.c), the difference since the last call
void mtl_pso_line(char *o, size_t n) {
    static uint64_t p[18]; uint64_t v[18] = {g_bp_pso_ns, g_bp_lib_ns, g_pt_hit_n, g_pt_hit_ns, g_pt_miss_n, g_pt_miss_ns, g_pb_spec, g_pt_spec_ns, g_pb_built, g_pt_comp_ns, g_pt_pred_n, g_pt_pred_ns, g_hy_draws, g_hy_made, g_pp_kicked, g_pp_done_n, g_pp_stolen, g_hy_none};
    uint64_t d[18]; for (int i = 0; i < 18; i++) d[i] = v[i] - p[i];
    snprintf(o, n, "on the submit thread pipelines %.1f ms, shader libraries %.1f ms, predictions %llu / %.1f ms; all threads: archive lookups found %llu / %.1f ms, found none %llu / %.1f ms, "
             "specializations %llu / %.1f ms, bases compiled %llu / %.1f ms; draws with a hybrid pipeline %llu (%llu pulling pipelines made, %llu misses with no pulling base ready); background builds kicked %llu, done %llu, made by the draw %llu, in flight %llu",
             d[0] / 1e6, d[1] / 1e6, (unsigned long long)d[10], d[11] / 1e6, (unsigned long long)d[2], d[3] / 1e6, (unsigned long long)d[4], d[5] / 1e6, (unsigned long long)d[6], d[7] / 1e6,
             (unsigned long long)d[8], d[9] / 1e6, (unsigned long long)d[12], (unsigned long long)d[13], (unsigned long long)d[17], (unsigned long long)d[14], (unsigned long long)d[15], (unsigned long long)d[16], (unsigned long long)(v[14] - v[15] - v[16]));
    memcpy(p, v, sizeof p);
    static uint64_t pn[5], pns[5]; size_t o2 = strlen(o);
    o2 += snprintf(o + o2, n > o2 ? n - o2 : 0, "; draws that waited:");
    for (int i = 0; i < 5; i++) if (g_pw_n[i] > pn[i]) o2 += snprintf(o + o2, n > o2 ? n - o2 : 0, " %s %llu / %.1f ms,", g_pw_why[i], (unsigned long long)(g_pw_n[i] - pn[i]), (g_pw_ns[i] - pns[i]) / 1e6);
    memcpy(pn, g_pw_n, sizeof pn); memcpy(pns, g_pw_ns, sizeof pns);
}

// ---- Prebuild: the base of every shader pair of the game, into the pipeline archive ----
// For each pair in the profile's shader_pairs.tsv, the base a hybrid draw needs: the pulling vertex entry (<key>_bp; a vertex shader without stage inputs: its own entry), the pixel
// shader, no vertex descriptor, one sample, no alpha-to-coverage, the colour state unspecialized: the descriptor pso_desc_pull / pso_desc make at run time, so the archive gives it back
// there (the archive matches libraries by content). Bases already in the archive are skipped; what is built goes into the archive, flushed every 1,000 bases (the serializer holds
// what it captured until then). Runs once per chain version, depth mode and pair list (pso_archive/prebuilt.tsv): in the app after the prewarm, on a background queue at utility QoS
// (DM_PSO_PREBUILD=0: not at all), or as a tool on the Mac (DM_PSO_PREBUILD=tool: build, write, exit). Vertex shaders that write the layer, pairs whose vertex shader has stage
// inputs but no pulling library, and integer-output or multisampled bases are left to the draws
static _Atomic int g_pre_running; static _Atomic int g_pre_done, g_pre_built, g_pre_archived, g_pre_skipped, g_pre_failed;
static MTL4FunctionDescriptor *pre_fn(id<MTLLibrary> lib, NSString *name) {   // as shader_load notes them (fn4_note): constants specialized with an empty set
    id<MTLFunction> f = [lib newFunctionWithName:name]; if (!f) return nil;
    MTL4LibraryFunctionDescriptor *ld = [MTL4LibraryFunctionDescriptor new]; ld.library = lib; ld.name = name;
    if (!f.functionConstantsDictionary.count) return ld;
    MTL4SpecializedFunctionDescriptor *sd = [MTL4SpecializedFunctionDescriptor new]; sd.functionDescriptor = ld; sd.constantValues = [MTLFunctionConstantValues new]; return sd;
}
static NSDictionary *pre_json(NSString *key) { NSData *d = [NSData dataWithContentsOfFile:[NSString stringWithFormat:@"%@/%@_b.json", g_msl_dir, key]]; return d ? [NSJSONSerialization JSONObjectWithData:d options:0 error:nil] : nil; }
static void pre_pair(NSString *vk, NSString *fk, int zero) {
    @autoreleasepool {
        NSDictionary *vj = pre_json(vk), *fj = pre_json(fk);
        if (!vj || !fj || ![vj[@"chain"] isEqual:@DMSH_CHAIN] || ![fj[@"chain"] isEqual:@DMSH_CHAIN]) { g_pre_skipped++; return; }
        NSString *ventry = zero && vj[@"entry_z"] ? vj[@"entry_z"] : vj[@"entry"], *fentry = fj[@"entry"] ?: @"main0";
        int pull = [vj[@"pull"] isKindOfClass:[NSDictionary class]];
        if (!pull && [vj[@"vertex_inputs"] count]) { g_pre_skipped++; return; }   // stage inputs and no pulling library: its pipelines need the draw's layout
        NSError *e = nil;
        id<MTLLibrary> vl = lib_from_file([NSString stringWithFormat:@"%@/%@%@.metallib", g_msl_dir, vk, pull ? @"_bp" : @"_b"], &e);
        if (!vl || t_lib_layer) { g_pre_skipped++; return; }   // writes the layer: its pipelines take an input primitive class (pso_desc), no pulling pipeline
        id<MTLLibrary> fl = lib_from_file([NSString stringWithFormat:@"%@/%@_b.metallib", g_msl_dir, fk], &e);
        MTL4FunctionDescriptor *vf = pre_fn(vl, ventry), *ff = fl ? pre_fn(fl, fentry) : nil;
        if (!vf || !ff) { g_pre_skipped++; return; }
        MTL4RenderPipelineDescriptor *m = [MTL4RenderPipelineDescriptor new];
        m.vertexFunctionDescriptor = vf; m.fragmentFunctionDescriptor = ff; m.vertexDescriptor = [MTLVertexDescriptor vertexDescriptor]; m.rasterSampleCount = 1;
        for (int i = 0; i < 8; i++) { MTL4RenderPipelineColorAttachmentDescriptor *a = m.colorAttachments[i];
            a.pixelFormat = MTLPixelFormatUnspecialized; a.writeMask = MTLColorWriteMaskUnspecialized; a.blendingState = MTL4BlendStateUnspecialized;
            a.sourceRGBBlendFactor = a.destinationRGBBlendFactor = a.sourceAlphaBlendFactor = a.destinationAlphaBlendFactor = MTLBlendFactorUnspecialized;
            a.rgbBlendOperation = a.alphaBlendOperation = MTLBlendOperationUnspecialized; }
        for (id<MTL4Archive> a in g_arch4p) if ([a newRenderPipelineStateWithDescriptor:m error:nil]) { g_pre_archived++; return; }
        for (id<MTL4Archive> a in g_arch4) if ([a newRenderPipelineStateWithDescriptor:m error:nil]) { g_pre_archived++; return; }
        pthread_rwlock_rdlock(&g_arch_rw); id ps = [g_comp4 newRenderPipelineStateWithDescriptor:m compilerTaskOptions:nil error:&e]; pthread_rwlock_unlock(&g_arch_rw);
        if (ps) { g_pre_built++; atomic_fetch_add(&g_arch_new, 1); }
        else { if (g_pre_failed++ < 4) MLOG("[RED] pipeline prebuild: the base of %s + %s failed: %s", vk.UTF8String, fk.UTF8String, e.localizedDescription.UTF8String); }
    }
}
// threads: workers; wait: run on the calling thread until done (the tool), else in the background. Returns the number of pairs, -1 when there is nothing to do
static int pso_prebuild(int threads, int wait, dispatch_qos_class_t qos) {
    shmap_load(); pso4_init();
    if (!g_comp4 || !g_ser4 || !g_arch_dir || atomic_exchange(&g_pre_running, 1)) return -1;
    // The pairs: shader_pairs.tsv (BNSH variations and the other readers' pairs) and pso_pairs.tsv (agl BAHS program variations, which vs_pad does not read; DM_PSO_PAIRS: another path),
    // both beside the shader map
    NSString *pp = [g_shmap_path.stringByDeletingLastPathComponent stringByAppendingPathComponent:@"shader_pairs.tsv"];
    const char *ev = getenv("DM_PSO_PAIRS");
    NSString *pp2 = ev && *ev ? @(ev) : [g_shmap_path.stringByDeletingLastPathComponent stringByAppendingPathComponent:@"pso_pairs.tsv"];
    NSString *txt = [NSString stringWithContentsOfFile:pp encoding:NSUTF8StringEncoding error:nil], *txt2 = [NSString stringWithContentsOfFile:pp2 encoding:NSUTF8StringEncoding error:nil];
    if (!txt) { MLOG("pipeline prebuild: no %s, nothing to build", pp.UTF8String); g_pre_running = 0; return -1; }
    if (txt2) txt = [txt stringByAppendingString:txt2];
    int zero = atomic_load(&g_depth_zero);
    NSString *stamp = [g_arch_dir stringByAppendingPathComponent:@"prebuilt.tsv"];
    NSString *want = [NSString stringWithFormat:@"chain\t%s\ndepth\t%s\npairs\t%lu\t%lx\n", DMSH_CHAIN, zero ? "near=0" : "near=-w", (unsigned long)txt.length, (unsigned long)txt.hash];
    if ([[NSString stringWithContentsOfFile:stamp encoding:NSUTF8StringEncoding error:nil] isEqualToString:want]) { g_pre_running = 0; return -1; }
    NSMutableArray<NSArray<NSString *> *> *pairs = [NSMutableArray array];
    NSMutableSet<NSString *> *seen = [NSMutableSet set];
    for (NSString *l in [txt componentsSeparatedByString:@"\n"]) { if (!l.length || [l hasPrefix:@"#"] || [seen containsObject:l]) continue; [seen addObject:l]; NSArray *c = [l componentsSeparatedByString:@"\t"]; if (c.count >= 2) [pairs addObject:c]; }
    int n = (int)pairs.count; uint64_t t0 = bp_now();
    MLOG("pipeline prebuild: %d shader pairs of %s%s%s, %d workers (%s)", n, pp.UTF8String, txt2 ? " and " : "", txt2 ? pp2.UTF8String : "", threads, wait ? "tool" : "background");
    __block _Atomic int next = 0;
    dispatch_group_t g = dispatch_group_create();
    for (int t = 0; t < threads; t++) dispatch_group_async(g, dispatch_get_global_queue(qos, 0), ^{
        for (;;) {
            int i = atomic_fetch_add(&next, 1); if (i >= n) break;
            pre_pair(pairs[(NSUInteger)i][0], pairs[(NSUInteger)i][1], zero);
            int d = ++g_pre_done;
            if (d % 1000 == 0) {
                MLOG("pipeline prebuild: %d of %d pairs, %d built, %d already archived, %d left to the draws, %d failed, %.0f s", d, n, (int)g_pre_built, (int)g_pre_archived, (int)g_pre_skipped, (int)g_pre_failed, (bp_now() - t0) / 1e9);
                if (atomic_load(&g_arch_new) >= 1000) dispatch_sync(g_arch_q, ^{ arch_flush(); });   // the serializer holds what it captured until written
            }
        }
    });
    dispatch_block_t done = ^{
        dispatch_sync(g_arch_q, ^{ arch_flush(); });
        if (!g_pre_failed) [want writeToFile:stamp atomically:YES encoding:NSUTF8StringEncoding error:nil];
        MLOG("pipeline prebuild done: %d pairs, %d bases built, %d already archived, %d left to the draws, %d failed, %.1f s", n, (int)g_pre_built, (int)g_pre_archived, (int)g_pre_skipped, (int)g_pre_failed, (bp_now() - t0) / 1e9);
        g_pre_running = 0;
    };
    if (wait) { dispatch_group_wait(g, DISPATCH_TIME_FOREVER); done(); }
    else dispatch_group_notify(g, dispatch_get_global_queue(qos, 0), done);
    return n;
}
// DM_PSO_PREBUILD=tool: the prebuild alone (the host's other environment as for a run: DM_NVN_SHMAP, DM_MSL_DIR, DM_SAVE or DM_PSO_ARCHIVE), then exit; DM_PSO_PREBUILD_DEPTH=zero for
// a game that sets the near=0 depth mode, DM_PSO_PREBUILD_THREADS=<n> workers (default: the performance cores less one). Returns 0 when not asked for
int mtl_pso_prebuild_tool(void) {
    const char *e = getenv("DM_PSO_PREBUILD"); if (!e || strcmp(e, "tool")) return 0;
    setenv("DM_PSO_MANIFEST", "off", 1);   // no prewarm (loading the shader map starts it)
    mtl_init(); const char *z = getenv("DM_PSO_PREBUILD_DEPTH"); if (z && !strcmp(z, "zero")) atomic_store(&g_depth_zero, 1);
    int threads = getenv("DM_PSO_PREBUILD_THREADS") ? atoi(getenv("DM_PSO_PREBUILD_THREADS")) : 0;
    if (threads <= 0) { int pc = 0; size_t sz = sizeof pc; if (sysctlbyname("hw.perflevel0.physicalcpu", &pc, &sz, NULL, 0) || pc <= 1) pc = 4; threads = pc - 1; }
    if (pso_prebuild(threads, 1, QOS_CLASS_USER_INITIATED) < 0) MLOG("pipeline prebuild: nothing to do (no archive, no shader_pairs.tsv, or prebuilt.tsv says it is done)");
    return 1;
}
static void pso_prebuild_bg(void) {   // after the prewarm, in the app
    const char *e = getenv("DM_PSO_PREBUILD"); if (e && !strcmp(e, "0")) return;
    if (!hybrid_on()) return;
    int pc = 0; size_t sz = sizeof pc; if (sysctlbyname("hw.perflevel0.physicalcpu", &pc, &sz, NULL, 0) || pc <= 1) pc = 4;
    pso_prebuild(pc > 4 ? pc / 2 : 2, 0, QOS_CLASS_UTILITY);
}
static __unsafe_unretained id<MTLRenderPipelineState> pso_get_(NvProg *pr, MTLRenderPassDescriptor *unused);
static id<MTLFunction> ms_fs_override(NvShader *fs);   // DM_NVN_MAGSCAN (defined before draw)
static int pf_int_kind(MTLPixelFormat f);
// A pipeline Metal refused: when a colour the fragment shader writes has an integer attachment, its output was most likely translated as float (a game without PC reflection has no output types,
// Tears of the Kingdom's BC4 block encoder and Ultrahand mask were the first cases). Writes the facts row that declares the outputs, for the profile's shader_facts_manual.tsv (merged by shader_split.py)
static const char *pso_fout_hint(NvShader *fs, const uint32_t cfmt[8], char *b, size_t n) {
    if (!fs || strlen(fs->key) < 16) return "";
    int o = snprintf(b, n, "; if its outputs are integers, add to shader_facts_manual.tsv: %.16s\t{\"fout\":{", fs->key), any = 0;
    for (int c = 0; c < 8; c++) { int kind = !fs->rf.color_known || fs->rf.color_mask >> c & 1 ? pf_int_kind((MTLPixelFormat)cfmt[c]) : 0;
        if (kind && o < (int)n) o += snprintf(b + o, n - o, "%s\"%d\":\"%s\"", any++ ? "," : "", c, kind == 1 ? "uint" : "sint"); }
    if (!any) return "";
    if (o < (int)n) snprintf(b + o, n - o, "}}");
    return b;
}
static __unsafe_unretained id<MTLRenderPipelineState> pso_get(NvProg *pr, MTLRenderPassDescriptor *unused) { AP_B; __unsafe_unretained id<MTLRenderPipelineState> r = pso_get_(pr, unused); AP_E(2); return r; }
static __unsafe_unretained id<MTLRenderPipelineState> pso_get_(NvProg *pr, MTLRenderPassDescriptor *unused) {
    (void)unused;
    g_pso_pull = 0;   // set again only where a pulling pipeline is returned: a stale 1 made the next draw, whose pipeline came from the cache, bind the layout block instead of its streams
    __unsafe_unretained NvShader *vs = pr->sh[0], *fs = pr->sh[1];
    if (!vs) { RED_ONCE(13, "program has no vertex shader (or it is not mapped)"); return nil; }
    pso_key k; pso_key_fill(&k, vs, fs, g_pass.cfmt, g_pass.dfmt, g_pass.sfmt, g_pass.samples, g_pso_lmap != NULL);
    if (g_pso_lmap) { memcpy(k.lmap, g_pso_lmap, 8); k.fsfn = (__bridge void *)g_pso_fsfn; }
    id<MTLFunction> ms_fn = ms_fs_override(fs); if (ms_fn) k.fsfn = (__bridge void *)ms_fn;   // DM_NVN_MAGSCAN phases 2 / 3
    // DM_PSO_PULL_ALL=1 (diagnostic): every draw that has a pulling pipeline draws with it, never with the pipeline for its layout
    { static int all = -1; if (all < 0) { const char *ev = getenv("DM_PSO_PULL_ALL"); all = ev && !strcmp(ev, "1"); if (all) MLOG("DM_PSO_PULL_ALL=1: every draw with a pulling pipeline draws with it (diagnostic)"); }
      if (all && !g_pso_lmap && !ms_fn && hybrid_on()) { __unsafe_unretained id<MTLRenderPipelineState> h = hybrid_get(&k, vs, fs); if (h) { g_pso_pull = 1; return h; } } }
    __unsafe_unretained id<MTLRenderPipelineState> cached = hm_get_id(&g_pso, &k);
    if (cached) return cached;
    if (hm_get_id(&g_pso_fail, &k)) return nil;
    id<MTLRenderPipelineState> ps = nil; int took = 0;
    NSError *e = nil; uint64_t bt = bp_now(), pred0 = g_pt_pred_ns;
    g_pso_pull = 0; int why = g_pso_lmap ? 1 : ms_fn ? 2 : 0;
    if (!took && !g_pso_lmap && !ms_fn && hybrid_on()) {
        why = !vs->has_pull || vs->layer_out ? 3 : 4;
        int st = pso_pend_try(&k, &ps);
        if (st == 2) took = 1;   // built in the background (nil: it failed, and is built again below for its message)
        else {
            if (st == 0) ps = pso_build_ex(pso_desc(&k, vs, fs, nil, 0), &e, 0);   // its base made or archived: no compile
            if (!ps) {
                // Demand first (main's order): the draw's own pipeline is queued before the prediction admits speculative builds; with a stand-in nothing waits for it
                __unsafe_unretained id<MTLRenderPipelineState> h = hybrid_get(&k, vs, fs);
                if (h) {
                    if (st == 0) pso_pend_demand(&k, vs, fs);
                    g_pso_pull = 1; g_hy_ns += bp_now() - bt;
                    if (g_pp_submit_mark != g_sub_serial) { g_pp_submit_mark = g_sub_serial; uint64_t t0 = bp_now(); pso_predict_rest(); g_bp_pso_ns += bp_now() - t0; }
                    return h;
                }
            }
        }
    }
    if (!ps && !took) ps = g_pso_lmap || ms_fn ? nil : pso_pend_take(&k);
    if (ps) g_pp_taken++;
    else {
        MTLRenderPipelineDescriptor *d = pso_desc(&k, vs, fs, ms_fn ? ms_fn : g_pso_fsfn, g_pso_lmap != NULL);
        ps = pso_build(d, &e);
        // A vertex shader that writes the layer (render_target_array_index) needs the pipeline's input primitive class, which the reflection does not record and the pipeline key does not hold.
        // Pokémon Legends: Z-A has two (b29f08619f379a83, 57246f9a04af8529, layered full-screen passes); Metal names the reason, the shader is marked and every later pipeline with it gets the
        // triangle class (layered draws are triangle lists; a line or point draw with such a shader would fail validation)
        if (!ps && !vs->layer_out && [e.localizedDescription containsString:@"render_target_array_index"]) {
            vs->layer_out = 1; MLOG("vertex shader %s writes the layer: its pipelines get the triangle input primitive class", vs->key);
            e = nil; d = pso_desc(&k, vs, fs, ms_fn ? ms_fn : g_pso_fsfn, g_pso_lmap != NULL); ps = pso_build(d, &e);
        }
    }
    if (!took) {   // the draw waited (built here, made from its queued build, or waited for its running build)
        uint64_t w = bp_now() - bt - (g_pt_pred_ns - pred0); g_pw_n[why]++; g_pw_ns[why] += w;
        static int lg = -1; if (lg < 0) { const char *ev = getenv("DM_PSO_WAITLOG"); lg = ev && !strcmp(ev, "1") ? 40 : 0; }
        if (lg > 0 && w > 20000000) { lg--; MLOG("pipeline wait %.1f ms (%s): vertex %s fragment %s, %u samples", w / 1e6, g_pw_why[why], vs->key, fs ? fs->key : "-", k.samples); }
    }
    g_bp_pso_n++; g_bp_pso_ns += bp_now() - bt;
    if (!ps) { char hint[200]; MLOG("[RED] failed to create render pipeline (vertex %s fragment %s): %s%s", vs->key, fs ? fs->key : "-", e.localizedDescription.UTF8String, pso_fout_hint(fs, g_pass.cfmt, hint, sizeof hint));
               hm_put_id(&g_pso_fail, &k, NSNull.null); return nil; }
    if (!ms_fn) { pso_skey sk; char shex[sizeof sk * 2 + 1]; pso_skey_from(&k, vs, fs, g_pso_lmap != NULL, g_pso_lmap ? &g_pso_fz : NULL, &sk); pso_skey_hex(&sk, shex);
        pso_record("R\t%s\n", shex);
        pso_user_record("R\t%s\n", shex); }
    hm_put_id(&g_pso, &k, ps);
    // Metal may serialize requests internally. Submit the pipeline needed now before speculative work, and preserve record-on-use above.
    if (g_pp_submit_mark != g_sub_serial) {
        g_pp_submit_mark = g_sub_serial; uint64_t scan_t0 = bp_now(); pso_predict_rest();
        g_bp_pso_ns += bp_now() - scan_t0;   // Keep lookahead work in the pipeline-miss timing, as before
    }
    return ps;
}
static NSString *warm_key(const char *s, size_t n) {
    size_t z = strnlen(s, n);
    return [[NSString alloc] initWithBytes:s length:z encoding:NSUTF8StringEncoding];
}
// A prewarm list may name shaders of another version of the game: the bundled list of the version the profile was brought up on, packed with another version (the NSP script's
// derived profile: Hollow Knight: Silksong's 1.0.30000 list named 16 shaders its base game 1.0.27776 does not have), or a device's pso_user.tsv kept across a game update.
// A key the shader map does not have is skipped quietly (counted in the "prewarm done" line); the game never draws with it
static NvShader *warm_shader(NSMutableDictionary<NSString *, NvShader *> *dict, NSString *key, int stage) {
    NvShader *sh = dict[key];
    if (!sh) {
        NSString *mk = [key hasSuffix:@"_b"] ? [key substringToIndex:key.length - 2] : key;   // library names are <map key>_b
        static NSMutableSet<NSString *> *stale; if (!stale) stale = [NSMutableSet set];   // only the warm-up thread calls this
        if (!g_shmap[mk]) { if (![stale containsObject:key]) { [stale addObject:key]; g_warm_skip_stale++; } return nil; }
        sh = shader_load(key, stage, 0); if (sh) dict[key] = sh;
    }
    return sh;
}
static void warm_enqueue(WarmItem *item) {
    os_unfair_lock_lock(&g_warm_lock);
    [g_warm_items addObject:item];
    atomic_store_explicit(&g_warm_ready, 1, memory_order_release);
    os_unfair_lock_unlock(&g_warm_lock);
}
static void warm_worker(NSArray<NSString *> *paths) {
    @autoreleasepool {
        NSMutableDictionary<NSString *, NvShader *> *dict = [NSMutableDictionary dictionary];
        NSMutableSet<NSString *> *seen = [NSMutableSet set];
        for (NSString *path in paths) {
            NSString *txt = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
            if (!txt) { MLOG("[RED] cannot read prewarm list: %s", path.UTF8String); continue; }
            { unsigned long nl = 0; for (NSString *l in [txt componentsSeparatedByString:@"\n"]) nl += l.length && ![l hasPrefix:@"#"];
              MLOG("pipeline prewarm list %s: %lu lines", path.UTF8String, nl); }   // Diagnostics: how many lines each manifest gave (when a device warms up 0, shows which one was missing)
            for (NSString *line in [txt componentsSeparatedByString:@"\n"]) {
                if (!line.length || [line hasPrefix:@"#"] || [seen containsObject:line]) continue;
                [seen addObject:line];
                NSArray<NSString *> *c = [line componentsSeparatedByString:@"\t"];
                if (!c.count) continue;
                if ([c[0] isEqualToString:@"S"] && c.count >= 3) {
                    NSString *key = c[1]; NvShader *sh = warm_shader(dict, key, c[2].intValue);
                    if (!sh) continue;
                    WarmItem *item = [WarmItem new]; item->kind = 0; item->key = key; item->shader = sh;
                    warm_enqueue(item); g_warm_shaders++;
                } else if ([c[0] isEqualToString:@"R"] && c.count >= 2) {
                    pso_skey sk; if (pso_skey_parse(c[1].UTF8String, &sk)) continue;
                    NSString *vkey = warm_key(sk.vs, sizeof sk.vs), *fkey = warm_key(sk.fs, sizeof sk.fs);
                    NvShader *vs = vkey.length ? warm_shader(dict, vkey, sk.vs_stage) : nil;
                    NvShader *fs = fkey.length ? warm_shader(dict, fkey, sk.fs_stage) : nil;
                    if (!vs || (fkey.length && !fs)) continue;
                    id<MTLFunction> fsfn = nil;
                    if (sk.fz) {
                        if (sk.fz_desc.kind == 1 && fs) { int fx[32]; for (int i = 0; i < 32; i++) fx[i] = sk.fz_desc.fx[i]; fsfn = bf_fn(fs, fx, sk.fz_desc.fbmask, sk.k.lmap); }
                                                if (!fsfn) { g_warm_skip_fz++; continue; }
                    }
                    pso_key k = sk.k; k.vs = (__bridge void *)vs; k.fs = (__bridge void *)fs; k.fsfn = (__bridge void *)fsfn; pso_key_norm(&k);   // rows written before the key was normalized
                    NSError *e = nil; id ps = pso_build(pso_desc(&k, vs, fs, fsfn, sk.merged), &e);
                    if (!ps && !vs->layer_out && [e.localizedDescription containsString:@"render_target_array_index"]) {   // a vertex shader that writes the layer (see pso_get_)
                        vs->layer_out = 1; e = nil; ps = pso_build(pso_desc(&k, vs, fs, fsfn, sk.merged), &e); }
                    if (!ps) { MLOG("[RED] background render pipeline creation failed: %s", e.localizedDescription.UTF8String); continue; }
                    WarmItem *item = [WarmItem new]; item->kind = 1; item->vs_key = vkey; item->fs_key = fkey; item->vs = vs; item->fs = fs; item->fsfn = fsfn; item->pso = k; item->object = ps;
                    warm_enqueue(item); g_warm_rps++;
                } else if ([c[0] isEqualToString:@"C"] && c.count >= 2) {
                    NSString *key = c[1]; if (!key.length) continue;
                    NvShader *cs = warm_shader(dict, key, 5);
                    if (!cs) continue;
                    NSError *e = nil; id ps = [g_dev newComputePipelineStateWithFunction:cs->fn error:&e];
                    if (!ps) { MLOG("[RED] background compute pipeline creation failed: %s", e.localizedDescription.UTF8String); continue; }
                    WarmItem *item = [WarmItem new]; item->kind = 2; item->key = key; item->shader = cs; item->object = ps;
                    warm_enqueue(item); g_warm_cps++;
                }
            }
        }
        MLOG("pipeline prewarm done: shaders %llu / render pipelines %llu (specialized fragment skipped %llu) / compute pipelines %llu, background time %.1f ms; shader keys not in the shader map (another game version) %llu",
             g_warm_shaders, g_warm_rps, g_warm_skip_fz, g_warm_cps, (bp_now() - g_warm_t0) / 1e6, g_warm_skip_stale);
        if (g_comp4) MLOG("pipeline bases so far: %llu compiled, %llu from the archive, %llu reused (%.1f ms for the bases); %llu specializations, %llu pipelines without a base (MTLDevice)",
                          (unsigned long long)g_pb_built, (unsigned long long)g_pb_archived, (unsigned long long)g_pb_reused, g_pb_base_ns / 1e6, (unsigned long long)g_pb_spec, (unsigned long long)g_pb_legacy);
        arch_flush_now();   // the bases the prewarm compiled go into the archive now, not 10 s after the last one
        if (g_compact_files && g_warm_rps && g_arch_q) dispatch_async(g_arch_q, ^{ arch_compact_finish(); });   // after that flush (the same serial queue)
        pso_prebuild_bg();   // then, once, the bases every shader pair's hybrid draws need
    }
    atomic_store(&g_warm_running, 0);
}
// Wait for the background warm-up to finish (at most timeout_ms): returns how many milliseconds were actually waited, 0 if it is not running. On first install the system shader cache is fully cold; if warm-up is slower than the player entering a run,
// pipelines not yet warmed are compiled on the spot on the submit thread = in-run stutter; the loading screen on entering a run blocks the player anyway, so let it load a little longer (called where hle_nvn.c presents).
int mtl_warm_wait(int timeout_ms) {
    if (!atomic_load(&g_warm_running)) return 0;
    uint64_t t0 = bp_now();
    while (atomic_load(&g_warm_running) && (bp_now() - t0) / 1000000ull < (uint64_t)timeout_ms) usleep(2000);
    return (int)((bp_now() - t0) / 1000000ull) + 1;
}
static void warm_start(void) {
    if (g_warm_started++) return;
    const char *e = getenv("DM_PSO_MANIFEST");
    if (e && !strcmp(e, "off")) return;
    NSArray<NSString *> *paths;
    if (e && *e) {
        paths = @[@(e)];
    } else {
        NSString *seed = [g_shmap_path.stringByDeletingLastPathComponent stringByAppendingPathComponent:@"pso_manifest.tsv"];
        NSMutableArray<NSString *> *all = [NSMutableArray arrayWithObject:seed];
        NSString *user = pso_user_path();
        if (user && [NSFileManager.defaultManager fileExistsAtPath:user]) [all addObject:user];
        else MLOG("pipeline prewarm: this device's user list %s%s", user ? "does not exist yet: " : "has no path (neither DM_PSO_USER nor DM_SAVE is set)", user ? user.UTF8String : "");
        if (![NSFileManager.defaultManager fileExistsAtPath:seed]) { MLOG("[RED] pipeline prewarm: seed list does not exist: %s", seed.UTF8String); return; }
        paths = all;
    }
    if (!g_warm_items) g_warm_items = [NSMutableArray array];
    pso4_init();   // the pipeline archive opens before the prewarm builds from it
    g_warm_t0 = bp_now();
    dispatch_queue_attr_t attr = dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INITIATED, 0);   // Not optional background work: it decides whether the pipelines are ready before the player enters a run
    dispatch_queue_t q = dispatch_queue_create("dm.nvn.pso-warm", attr);
    atomic_store(&g_warm_running, 1);
    dispatch_async(q, ^{ warm_worker(paths); });
}
