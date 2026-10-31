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
// On by default on the Mac; on iOS DM_PSO_MTL4=1, or the profile key pso_mtl4 on, turns it on, and with it the hybrid pipelines and the pipeline prebuild. The archive's footprint
// (with the prebuild, about 850 MB more for Breath of the Wild) is not measured against an iPhone's limit yet. The prebuild's bases can also come made offline, bundled with the
// app (DM_PSO_BUNDLE, pso_bundle_open)
static int pso4_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_PSO_MTL4");
#if TARGET_OS_IPHONE
        v = e ? !strcmp(e, "1") : DM_GAME_PSO_MTL4;   // the profile's pso_mtl4
        if (!v) MLOG("render pipelines through the MTLDevice (iOS: DM_PSO_MTL4=1 or the profile key pso_mtl4 on turns on the Metal 4 bases, the pipeline archive and the hybrid pipelines)");
#else
        v = !(e && !strcmp(e, "0"));
        if (!v) MLOG("DM_PSO_MTL4=0: render pipelines are built through the MTLDevice (no bases, no pipeline archive)");
#endif
        // Metal cannot capture pipelines made from bases ("Capturing Render pipeline specialization is not supported"), and its Metal 4 → 3 conversion for the capture layer
        // crashes on the archives: a run that may capture GPU frames (DM_GPU_CAPTURE, MTL_CAPTURE_ENABLED) builds its pipelines through the MTLDevice
        if (v && (getenv("DM_GPU_CAPTURE") || getenv("MTL_CAPTURE_ENABLED"))) { v = 0; MLOG("render pipelines through the MTLDevice in a run that may capture GPU frames (DM_GPU_CAPTURE / MTL_CAPTURE_ENABLED)"); }
    }
    return v;
}
// g_comp4: the compiler of what the archive does not capture (specializations, and every compile without an archive folder); compiles the archive captures go through the current
// flush period's own compiler (ArchEpoch)
static id<MTL4Compiler> g_comp4;
// g_arch4: the files that may hold bases with a vertex layout, g_arch4p: the files that hold none (p<seq>.mtl4a: the prebuild's, and any flush whose bases all read their vertices
// themselves). A base with a layout is looked up in g_arch4 alone: each file a lookup misses in costs about 0.06 ms, and a cold start's misses were for such bases, in the 13 prebuilt files
static NSArray<id<MTL4Archive>> *g_arch4, *g_arch4p; static NSString *g_arch_dir; static int g_arch_seq;
// Compaction: every start writes a file or more (each flush captures only what is new), Breath of the Wild 5-7 per start, and each file a base is not in costs a lookup about 0.06 ms.
// With more than PSO_ARCH_COMPACT files of bases with a vertex layout, the bases the prewarm takes from them are captured again from the original shader functions, so the flush
// after the prewarm writes them into one file; the old files are then removed (open archives stay usable for the rest of the run). Bases the
// prewarm did not take (pso_user.tsv never named them) go with them and are built again when drawn. The prebuild's files (p<seq>) are left as they are
enum { PSO_ARCH_COMPACT = 8 };
static int g_compact_enabled;   // immutable after pso4_init; unlike g_compact_files, never cleared by the archive queue
static NSArray<NSString *> *g_compact_files; static _Atomic int g_compacted, g_compact_taken;   // g_compact_taken: a flush period holds the recaptured bases (arch_flush)
// Keep the crash-recovery guard if a compaction still fails in Metal. Markers name the OS build and capture method: a marker from the old archive-import path must not
// disable the safer original-function capture. A failed flush of this method disables compaction on this OS build until it changes; ordinary archive reads remain available.
static NSString *arch_compact_marker(void) { return [g_arch_dir stringByAppendingPathComponent:@"compacting"]; }
static NSString *arch_os_build(void) { char b[64] = ""; size_t n = sizeof b; sysctlbyname("kern.osversion", b, &n, NULL, 0); return [NSString stringWithFormat:@"os\t%s\n", b]; }
static NSString *arch_compact_stamp(void) { return [arch_os_build() stringByAppendingString:@"capture\toriginal-functions-v1\n"]; }
static int arch_compact_allowed(void) {
    NSString *m = [NSString stringWithContentsOfFile:arch_compact_marker() encoding:NSUTF8StringEncoding error:nil];
    if (!m) return 1;
    if ([m isEqualToString:arch_compact_stamp()]) { MLOG("[RED] pipeline archive: a compaction did not finish on this system build (issue #342): the files are left as they are"); return 0; }
    unlink(arch_compact_marker().fileSystemRepresentation);   // another system build or capture method: try once more
    return 1;
}
static dispatch_queue_t g_arch_q; static _Atomic int g_arch_new, g_arch_new_vd, g_arch_sched;   // bases captured and not written yet, those of them with a vertex layout
// A flush period: a compiler of its own and the serializer it captures into. A compile the archive captures joins the current period (ep_enter) and leaves it when it returns; a
// flush (arch_flush) hands the compiles after it to a new period and writes the old period's serializer once every compile in it has returned (arch_write), and that serializer is
// never used again. In Metal (macOS 27.0) the write (-[_MTLBinaryArchive airntSerializeToURL:options:error:]) runs on the archive's _serializeQueue and then empties it (entries,
// AIR objects, library cache, file mapping) on its _queue, while each function a compile captures adds itself from the compile's completion: a new MTLBinaryEntry from the
// request's AIR script and the function's bitcode, mapped before newArchiverIdWithBinaryKey takes _queue, then added under _queue, before the compile reports back. A compile
// under way across a write came back with both nil and crashed there (dispatch_data_create_map on nil); eight threads compiling into one serializer written every 150 ms
// crashed 3 runs of 3, and still 6 of 6 when the write waited for the compiles under way but let new ones start; a serializer per period crashed in none of 118 runs, every
// pipeline found in the files. The flush used to hold a lock against every such compile instead, and one compile that did not return (Super Mario Odyssey's alRenderMaterialUber
// with the front end at -O2: 33.7 minutes until Metal gave up, GitHub issue #294) held the flush, and the flush every compile and draw that came after it. Now such a compile
// holds back the file of its own period and nothing else
@interface ArchEpoch : NSObject { @public id<MTL4Compiler> comp; id<MTL4PipelineDataSetSerializer> ser; dispatch_group_t busy; _Atomic int active, n, vd; int compacting; } @end
// n, vd: bases captured, those with a vertex layout
@implementation ArchEpoch @end
static ArchEpoch *g_ep; static os_unfair_lock g_ep_lock = OS_UNFAIR_LOCK_INIT;   // the current period
static int g_arch_cap;   // compiles are captured (an archive folder, a serializer): immutable after pso4_init
static dispatch_group_t g_arch_pending;   // periods handed over and not written yet
static _Atomic uint64_t g_pb_built, g_pb_archived, g_pb_reused, g_pb_spec, g_pb_legacy, g_pb_base_ns;
// Time by kind, all threads (the 'pipelines up to frame' line): archive lookups that found the base / found none, specializations, base compiles; predictions (submit thread)
static _Atomic uint64_t g_pt_hit_n, g_pt_hit_ns, g_pt_miss_n, g_pt_miss_ns, g_pt_spec_ns, g_pt_comp_ns, g_pt_pred_n, g_pt_pred_ns, g_pp_done_n;   // bases compiled / taken from the archive / reused, specializations, MTLDevice builds
typedef struct { const void *vf, *ff; uint32_t samples, a2c, rast, topo, amp; uint32_t va[31][3], vl[31][3]; } pbase_key;   // what a base depends on: pso_desc's descriptor less the colour attachments
static nvn_hmap g_pbase = {.klen = sizeof(pbase_key)};
// Only callers for the same base wait on this group; unrelated bases compile in parallel.
// compiled: its owner asked the compiler (a compile 0 owner only looks in the archive, and a waiter that needs the base builds it itself when that found none).
@interface PbaseBuild : NSObject { @public dispatch_group_t done; id<MTLRenderPipelineState> ps; NSError *error; int compiled; } @end
@implementation PbaseBuild @end
static nvn_hmap g_pbase_pending = {.klen = sizeof(pbase_key)};
static os_unfair_lock g_pbase_lock = OS_UNFAIR_LOCK_INIT;
static NSString *pso_user_path(void);
// The pipeline archive made offline (dm/gfx/tools/pso_offline.py, metal-tt) and bundled with the app: DM_PSO_BUNDLE=<folder> (the iOS app sets it to its pso/ folder when it has one;
// off: not used) holds <GPU architecture>.mtl4a per architecture it was built for and prebuilt.tsv, what it covers (pso_prebuild). The file named after this device's architecture is
// opened read only and looked up first among the files of bases without a vertex layout; it is never written or compacted. The device's own archive (pso_archive/) so takes only
// what it lacks: a draw, the prewarm and the prebuild compile a base, and the serializer captures it, only after the bundle missed (it holds no base with a vertex layout). A system
// that cannot open it (one older than the archive's minimum, or an OS update that changed the GPU compiler) or a GPU it was not built for gets an info line, and the device builds
// the bases itself as before (the on-device prebuild is the fallback)
static id<MTL4Archive> g_arch_bundle; static NSString *g_bundle_dir;
// Metal's loader crashes on malformed containers rather than returning an error. Check the GPU image's load commands and referenced file ranges, including every slice of a
// universal archive, before giving it to Metal. This checks the container, not the compiler's private payload: Metal still decides whether this system can use its binaries.
static int arch_macho_ok(int fd, uint64_t off, uint64_t size) {
    struct mach_header_64 h;
    if (size < sizeof h || pread(fd, &h, sizeof h, (off_t)off) != sizeof h || h.magic != MH_MAGIC_64 || h.filetype != MH_GPU_EXECUTE ||
        !h.ncmds || h.sizeofcmds > size - sizeof h || h.ncmds > h.sizeofcmds / sizeof(struct load_command)) return 0;
    uint64_t pos = sizeof h, end = pos + h.sizeofcmds; int pipeline = 0;
    for (uint32_t i = 0; i < h.ncmds; i++) {
        struct load_command lc;
        if (end - pos < sizeof lc || pread(fd, &lc, sizeof lc, (off_t)(off + pos)) != sizeof lc || lc.cmdsize < sizeof lc || lc.cmdsize > end - pos) return 0;
        if (lc.cmd == LC_SEGMENT_64) {
            struct segment_command_64 s;
            if (lc.cmdsize < sizeof s || pread(fd, &s, sizeof s, (off_t)(off + pos)) != sizeof s || s.fileoff > size || s.filesize > size - s.fileoff ||
                s.nsects > (lc.cmdsize - sizeof s) / sizeof(struct section_64)) return 0;
        } else if (lc.cmd == LC_NOTE) {
            struct note_command n;
            if (lc.cmdsize < sizeof n || pread(fd, &n, sizeof n, (off_t)(off + pos)) != sizeof n || n.offset > size || n.size > size - n.offset) return 0;
            if (n.size && !memcmp(n.data_owner, "AIR_PIPELINE", sizeof "AIR_PIPELINE")) pipeline = 1;
        }
        pos += lc.cmdsize;
    }
    return pos == end && pipeline;
}
static int arch_container_ok(int fd, uint64_t size) {
    struct fat_header h;
    if (size < sizeof h || pread(fd, &h, sizeof h, 0) != sizeof h) return 0;
    if (h.magic == MH_MAGIC_64) return arch_macho_ok(fd, 0, size);
    // The serializer's 0xcbfebabe wrapper uses fat_arch entries and includes a Metal library slice beside the GPU images.
    int metal = h.magic == OSSwapBigToHostInt32(0xcbfebabe), wide = h.magic == OSSwapBigToHostInt32(FAT_MAGIC_64);
    if (!metal && !wide && h.magic != OSSwapBigToHostInt32(FAT_MAGIC)) return 0;
    uint32_t n = OSSwapBigToHostInt32(h.nfat_arch); uint64_t ent = wide ? sizeof(struct fat_arch_64) : sizeof(struct fat_arch);
    if (!n || n > (size - sizeof h) / ent) return 0;
    uint64_t table = sizeof h + n * ent; int gpu = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t off, len;
        if (wide) {
            struct fat_arch_64 a; if (pread(fd, &a, sizeof a, (off_t)(sizeof h + i * ent)) != sizeof a) return 0;
            off = OSSwapBigToHostInt64(a.offset); len = OSSwapBigToHostInt64(a.size);
        } else {
            struct fat_arch a; if (pread(fd, &a, sizeof a, (off_t)(sizeof h + i * ent)) != sizeof a) return 0;
            off = OSSwapBigToHostInt32(a.offset); len = OSSwapBigToHostInt32(a.size);
        }
        if (off < table || off > size || len > size - off) return 0;
        uint32_t magic;
        if (len < sizeof magic || pread(fd, &magic, sizeof magic, (off_t)off) != sizeof magic) return 0;
        if (metal && magic == 0x424c544d) {   // MTLB: its header declares the full slice length and four section ranges
            uint64_t lib[11];
            if (len < sizeof lib || pread(fd, lib, sizeof lib, (off_t)off) != sizeof lib || lib[2] != len) return 0;
            for (int j = 3; j < 11; j += 2) if (lib[j] > len || lib[j + 1] > len - lib[j]) return 0;
        } else {
            if (!arch_macho_ok(fd, off, len)) return 0;
            gpu = 1;
        }
    }
    return gpu;
}
static int arch_file_ok(NSString *path) {
    int fd = open(path.fileSystemRepresentation, O_RDONLY); if (fd < 0) return 0;
    struct stat st; int ok = !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size > 0 && arch_container_ok(fd, (uint64_t)st.st_size);
    close(fd); return ok;
}
static void pso_bundle_open(NSMutableArray<id<MTL4Archive>> *archp) {
    const char *e = getenv("DM_PSO_BUNDLE");
    if (!e || !*e || !strcmp(e, "off")) return;
    NSString *dir = @(e), *gpu = [(id)g_dev respondsToSelector:@selector(architecture)] ? g_dev.architecture.name : nil;
    NSString *p = gpu ? [dir stringByAppendingPathComponent:[gpu stringByAppendingPathExtension:@"mtl4a"]] : nil;
    if (!p || ![NSFileManager.defaultManager fileExistsAtPath:p]) {
        NSArray<NSString *> *have = [[NSFileManager.defaultManager contentsOfDirectoryAtPath:dir error:nil] filteredArrayUsingPredicate:[NSPredicate predicateWithFormat:@"self ENDSWITH '.mtl4a'"]];
        MLOG("pipeline archive bundled with the app: none for this GPU (%s; %s has %s): the bases are built on the device", gpu ? gpu.UTF8String : "architecture unknown", e,
             have.count ? [have componentsJoinedByString:@", "].UTF8String : "none");
        return;
    }
    uint64_t t0 = bp_now(); NSError *ae = nil;
    if (!arch_file_ok(p)) { MLOG("[RED] pipeline archive bundled with the app: %s is not a pipeline archive: the bases are built on the device", p.UTF8String); return; }
    id<MTL4Archive> a = [g_dev newArchiveWithURL:[NSURL fileURLWithPath:p] error:&ae];
    if (!a) { MLOG("pipeline archive bundled with the app: this system cannot open %s (%s; made for another system version, or an OS update changed the GPU compiler): the bases are built on the device",
                   p.UTF8String, ae.localizedDescription.UTF8String); return; }
    g_arch_bundle = a; g_bundle_dir = dir; [archp addObject:a];
    MLOG("pipeline archive bundled with the app: %s, %.1f MB, opened in %.1f ms", p.UTF8String, [[NSFileManager.defaultManager attributesOfItemAtPath:p error:nil] fileSize] / 1048576.0, (bp_now() - t0) / 1e6);
}
// gpuarchiver-* folders are Metal's private compiler state. On the Mac their parent is shared by every program: a directory appearing during our capture or flush may belong
// to another compiler, and serialization success does not end its lifetime. Only our output archives are ours to remove. iOS can reclaim previous runs' app-private folders
// at cold start, before this run captures anything; never infer ownership from a live directory delta or remove compiler state at flush/exit.
static NSString *ga_dir(void) {
    static NSString *dir; static dispatch_once_t once;
    dispatch_once(&once, ^{ char b[PATH_MAX]; if (confstr(_CS_DARWIN_USER_CACHE_DIR, b, sizeof b)) dir = [@(b) stringByAppendingPathComponent:@"com.apple.gpuarchiver"]; });
    return dir;
}
static NSSet<NSString *> *ga_list(void) {
    NSArray<NSString *> *a = ga_dir() ? [NSFileManager.defaultManager contentsOfDirectoryAtPath:ga_dir() error:nil] : nil;
    return [NSSet setWithArray:[a filteredArrayUsingPredicate:[NSPredicate predicateWithFormat:@"self BEGINSWITH 'gpuarchiver-'"]] ?: @[]];
}
static ArchEpoch *ep_new(void) {   // a flush period: its compiler and serializer, nil when Metal makes none
    MTL4PipelineDataSetSerializerDescriptor *sd = [MTL4PipelineDataSetSerializerDescriptor new]; sd.configuration = MTL4PipelineDataSetSerializerConfigurationCaptureBinaries;
    ArchEpoch *ep = [ArchEpoch new]; ep->ser = [g_dev newPipelineDataSetSerializerWithDescriptor:sd];
    if (!ep->ser) return nil;
    MTL4CompilerDescriptor *cd = [MTL4CompilerDescriptor new]; cd.pipelineDataSetSerializer = ep->ser;
    ep->comp = [g_dev newCompilerWithDescriptor:cd error:nil];
    if (!ep->comp) return nil;
    ep->busy = dispatch_group_create();
    return ep;
}
static ArchEpoch *ep_enter(void) {   // the current period, joined: a flush hands it to arch_write only once every compile that joined it has left
    os_unfair_lock_lock(&g_ep_lock);
    ArchEpoch *ep = g_ep; atomic_fetch_add(&ep->active, 1); dispatch_group_enter(ep->busy);
    os_unfair_lock_unlock(&g_ep_lock);
    return ep;
}
static void ep_leave(ArchEpoch *ep) { atomic_fetch_sub(&ep->active, 1); dispatch_group_leave(ep->busy); }
// A compile the archive captures, in the current flush period, counted there when it built (vd: the base has a vertex layout). No task options: a captured base never imports
// archive entries (see the compaction crash below)
static id<MTLRenderPipelineState> arch_capture(MTL4RenderPipelineDescriptor *d, NSError **e, int vd) {
    if (!g_arch_cap) return [g_comp4 newRenderPipelineStateWithDescriptor:d compilerTaskOptions:nil error:e];
    ArchEpoch *ep = ep_enter();
    id<MTLRenderPipelineState> ps = [ep->comp newRenderPipelineStateWithDescriptor:d compilerTaskOptions:nil error:e];
    if (ps) { atomic_fetch_add(&ep->n, 1); atomic_fetch_add(&g_arch_new, 1); if (vd) { atomic_fetch_add(&ep->vd, 1); atomic_fetch_add(&g_arch_new_vd, 1); } }
    ep_leave(ep);
    return ps;
}
static void pso4_init(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        if (!pso4_on() || ![(id)g_dev respondsToSelector:@selector(newCompilerWithDescriptor:error:)]) return;   // no Metal 4 compiler (t_pso_pending's fake device): the MTLDevice path
        const char *e = getenv("DM_PSO_ARCHIVE"); NSString *user = pso_user_path();
        g_arch_dir = e && !strcmp(e, "off") ? nil : e && *e ? @(e) : user ? [user.stringByDeletingLastPathComponent stringByAppendingPathComponent:@"pso_archive"] : nil;
        NSMutableArray<id<MTL4Archive>> *arch = [NSMutableArray array], *archp = [NSMutableArray array];
        pso_bundle_open(archp);   // first: it holds every pair's base
        if (g_arch_dir) {
            g_arch_q = dispatch_queue_create("dm.nvn.pso-archive", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0));
            g_arch_pending = dispatch_group_create();
#if TARGET_OS_IPHONE
            NSSet<NSString *> *old = ga_list();   // listed before any capture, so this run's own folder is never among them
            if (old.count) dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
                int n = 0; for (NSString *name in old) n += [NSFileManager.defaultManager removeItemAtPath:[ga_dir() stringByAppendingPathComponent:name] error:nil];
                MLOG("pipeline archive: %d of Metal's build folders left by earlier runs removed", n);
            });
#endif
            uint64_t t0 = bp_now(); unsigned long long bytes = 0;
            NSMutableArray<NSString *> *runf = [NSMutableArray array];
            NSArray<NSString *> *names = [[[NSFileManager.defaultManager contentsOfDirectoryAtPath:g_arch_dir error:nil] filteredArrayUsingPredicate:[NSPredicate predicateWithFormat:@"self ENDSWITH '.mtl4a'"]]
                                          sortedArrayUsingSelector:@selector(compare:)];
            for (NSString *n in names.reverseObjectEnumerator) {   // newest first
                NSString *p = [g_arch_dir stringByAppendingPathComponent:n]; NSError *ae = nil;
                id<MTL4Archive> a = arch_file_ok(p) ? [g_dev newArchiveWithURL:[NSURL fileURLWithPath:p] error:&ae] : nil;
                int pf = [n hasPrefix:@"p"];
                if (a) { [pf ? archp : arch addObject:a]; if (!pf) [runf addObject:p]; bytes += [[NSFileManager.defaultManager attributesOfItemAtPath:p error:nil] fileSize]; }
                else MLOG("[RED] pipeline archive file %s not usable (%s): its bases are built again", p.UTF8String, ae ? ae.localizedDescription.UTF8String : "not a pipeline archive");
                g_arch_seq = MAX(g_arch_seq, [n substringFromIndex:pf].intValue + 1);
            }
            if (runf.count > PSO_ARCH_COMPACT && arch_compact_allowed()) { g_compact_enabled = 1; g_compact_files = runf; }
            MLOG("pipeline archive %s: %lu files (%lu of bases without a vertex layout), %.1f MB, opened in %.1f ms", g_arch_dir.UTF8String, (unsigned long)(arch.count + archp.count - (g_arch_bundle != nil)),
                 (unsigned long)archp.count - (g_arch_bundle != nil), bytes / 1048576.0, (bp_now() - t0) / 1e6);
        }
        g_arch4 = arch; g_arch4p = archp;
        NSError *ce = nil; g_comp4 = [g_dev newCompilerWithDescriptor:[MTL4CompilerDescriptor new] error:&ce];
        if (!g_comp4) MLOG("[RED] no Metal 4 compiler (%s): render pipelines are built through the MTLDevice", ce.localizedDescription.UTF8String);
        else if (g_arch_dir) {
            g_ep = ep_new(); g_arch_cap = g_ep != nil;
            if (!g_ep) MLOG("[RED] pipeline archive: Metal made no pipeline serializer: nothing is captured into %s", g_arch_dir.UTF8String);
        }
    });
}
// Writes one flush period's captures as the next file of the archive, on g_arch_q once every compile in the period has returned; g_arch_flush_ok: the last one wrote its file
static int g_arch_flush_ok;
static void arch_compact_finish(void);
static void arch_write(ArchEpoch *ep) {
    int n = atomic_load(&ep->n), vd = atomic_load(&ep->vd);   // exact: no compile of the period is under way
    if (!n) return;
    g_arch_flush_ok = 0;
    [NSFileManager.defaultManager createDirectoryAtPath:g_arch_dir withIntermediateDirectories:YES attributes:nil error:nil];
    uint64_t t0 = bp_now(); NSError *e = nil;
    NSString *path = [g_arch_dir stringByAppendingPathComponent:[NSString stringWithFormat:vd ? @"%05d.mtl4a" : @"p%05d.mtl4a", g_arch_seq++]], *tmp = [path stringByAppendingString:@".tmp"];
    if (ep->compacting) [arch_compact_stamp() writeToFile:arch_compact_marker() atomically:YES encoding:NSUTF8StringEncoding error:nil];
    BOOL ok = [ep->ser serializeAsArchiveAndFlushToURL:[NSURL fileURLWithPath:tmp] error:&e];
    if (ep->compacting) unlink(arch_compact_marker().fileSystemRepresentation);
    atomic_fetch_sub(&g_arch_new, n); atomic_fetch_sub(&g_arch_new_vd, vd);
    ok = ok && !rename(tmp.fileSystemRepresentation, path.fileSystemRepresentation);
    if (!ok) { MLOG("[RED] pipeline archive: writing %s failed%s%s", path.UTF8String, e ? ": " : "", e ? e.localizedDescription.UTF8String : ""); unlink(tmp.fileSystemRepresentation); }
    else {
        g_arch_flush_ok = 1;
        MLOG("pipeline archive: %d new pipeline bases written to %s (%.1f MB, %.0f ms)", n, path.lastPathComponent.UTF8String,
             [[NSFileManager.defaultManager attributesOfItemAtPath:path error:nil] fileSize] / 1048576.0, (bp_now() - t0) / 1e6);
    }
    if (ep->compacting) arch_compact_finish();
}
// A flush: the compiles from here on capture into a new period, and the current one is written once its compiles have returned. Returns at once, on any thread; a compile that
// does not return holds back only its own period's file
static void arch_flush(void) {
    // Keep all recaptured bases in one serialization, even when a prewarm lasts beyond the timer.
    if (!g_arch_cap || (g_compact_enabled && atomic_load(&g_warm_running))) return;
    os_unfair_lock_lock(&g_ep_lock); int any = atomic_load(&g_ep->n) || atomic_load(&g_ep->active); os_unfair_lock_unlock(&g_ep_lock);
    if (!any) return;   // nothing captured or under way: the period goes on
    ArchEpoch *ne = ep_new();
    if (!ne) { MLOG("[RED] pipeline archive: Metal made no compiler for a new flush period: the captures go on into the current one"); return; }
    os_unfair_lock_lock(&g_ep_lock);
    ArchEpoch *old = g_ep; any = atomic_load(&old->n) || atomic_load(&old->active);   // again: another flush may just have handed it over
    if (any) {
        g_ep = ne;
        old->compacting = g_compact_enabled && atomic_load(&g_compacted) && !atomic_exchange(&g_compact_taken, 1);   // this period holds the recaptured bases (see arch_compact_allowed)
    }
    os_unfair_lock_unlock(&g_ep_lock);
    if (!any) return;
    dispatch_group_enter(g_arch_pending);
    dispatch_group_notify(old->busy, g_arch_q, ^{ @autoreleasepool { arch_write(old); } dispatch_group_leave(g_arch_pending); });
}
// A flush, then every period handed over is written (not on g_arch_q, which writes them)
static void arch_flush_wait(void) { arch_flush(); if (g_arch_pending) dispatch_group_wait(g_arch_pending, DISPATCH_TIME_FOREVER); }
static void arch_note_new(void) {   // a base was compiled: flush 10 s later (one flush for a burst of them)
    if (!g_arch_q || atomic_exchange(&g_arch_sched, 1)) return;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 10 * (int64_t)NSEC_PER_SEC), g_arch_q, ^{ atomic_store(&g_arch_sched, 0); arch_flush(); });
}
static void arch_compact_finish(void) {   // after the period that holds the recaptured bases is written (arch_write): the old files go once their bases are in the new one
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
    // Metal 4 requires a fragment function when rasterization is enabled. Depth-only draws still need rasterization.
    if (d.rasterizationEnabled && !d.fragmentFunction) return nil;
    MTL4FunctionDescriptor *vf = fn4_of(d.vertexFunction), *ff = d.fragmentFunction ? fn4_of(d.fragmentFunction) : nil;
    if (!vf || (d.fragmentFunction && !ff)) return nil;
    MTL4RenderPipelineDescriptor *m = [MTL4RenderPipelineDescriptor new];
    m.vertexFunctionDescriptor = vf; m.fragmentFunctionDescriptor = ff; m.vertexDescriptor = d.vertexDescriptor;
    m.rasterSampleCount = d.rasterSampleCount; m.alphaToCoverageState = d.alphaToCoverageEnabled ? MTL4AlphaToCoverageStateEnabled : MTL4AlphaToCoverageStateDisabled;
    m.rasterizationEnabled = d.rasterizationEnabled; m.inputPrimitiveTopology = d.inputPrimitiveTopology;
    if (d.maxVertexAmplificationCount > 1) m.maxVertexAmplificationCount = d.maxVertexAmplificationCount;
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
    k->samples = (uint32_t)d.rasterSampleCount; k->a2c = d.alphaToCoverageEnabled; k->rast = d.rasterizationEnabled; k->topo = (uint32_t)d.inputPrimitiveTopology; k->amp = (uint32_t)d.maxVertexAmplificationCount;
    MTLVertexDescriptor *vd = d.vertexDescriptor;
    for (NSUInteger i = 0; vd && i < 31; i++) {
        MTLVertexAttributeDescriptor *a = vd.attributes[i]; MTLVertexBufferLayoutDescriptor *l = vd.layouts[i];
        k->va[i][0] = (uint32_t)a.format; k->va[i][1] = (uint32_t)a.offset; k->va[i][2] = (uint32_t)a.bufferIndex;
        k->vl[i][0] = (uint32_t)l.stride; k->vl[i][1] = (uint32_t)l.stepFunction; k->vl[i][2] = (uint32_t)l.stepRate;
    }
}
// The pipeline pso_desc describes: its base (built, from the archive or already made), then specialized with its colour state. Called on several threads.
// compile 0: nil on an archive miss (*e untouched), with no MTLDevice fallback. Prewarm compaction may still refresh a found base for serialization.
static id<MTLRenderPipelineState> pso_build_ex(MTLRenderPipelineDescriptor *d, NSError **e, int compile) {
    pso4_init();
    int rast = d.rasterizationEnabled;
    MTL4RenderPipelineDescriptor *b = g_comp4 ? pso_desc4(d, rast) : nil;   // with rasterization off there is no colour state to leave open: the base is the pipeline
    if (!b) { if (!compile) return nil; g_pb_legacy++; return [g_dev newRenderPipelineStateWithDescriptor:d error:e]; }
    pbase_key bk; pbase_key_of(d, &bk);
    id<MTLRenderPipelineState> base = nil; PbaseBuild *job = nil; int owner = 0;
    for (;;) {
        os_unfair_lock_lock(&g_pbase_lock);
        base = hm_get_id(&g_pbase, &bk); job = nil; owner = 0;
        if (!base) {
            job = hm_get_id(&g_pbase_pending, &bk);
            if (!job) { job = [PbaseBuild new]; job->done = dispatch_group_create(); dispatch_group_enter(job->done); hm_put_id(&g_pbase_pending, &bk, job); owner = 1; }
        }
        os_unfair_lock_unlock(&g_pbase_lock);
        if (!job || owner) break;
        if (!compile) return nil;   // another thread is taking or building it: no wait for the Metal compiler
        dispatch_group_wait(job->done, DISPATCH_TIME_FOREVER); base = job->ps;
        if (base) break;
        if (job->compiled) { if (e) *e = job->error; return nil; }
        // its owner only looked in the archive (compile 0) and found none: build it here
    }
    if (base) g_pb_reused++;
    else {
        uint64_t t0 = bp_now();
        int vd = 0; for (int i = 0; i < 31 && !vd; i++) vd = bk.va[i][0] != 0;   // a vertex layout (MTLVertexFormatInvalid is 0)
        if (!vd) for (id<MTL4Archive> a in g_arch4p) if ((base = [a newRenderPipelineStateWithDescriptor:b error:nil])) break;
        int from_run = 0;
        if (!base) for (id<MTL4Archive> a in g_arch4) if ((base = [a newRenderPipelineStateWithDescriptor:b error:nil])) { from_run = 1; break; }
        uint64_t t1 = bp_now();
        if (base) { g_pb_archived++; g_pt_hit_n++; g_pt_hit_ns += t1 - t0; }
        if (base && from_run && g_compact_enabled && atomic_load(&g_warm_running)) {
            // Metal can crash in materializeAirNTFromOnlineForSlice when serializing archive imports mixed with fresh bases (#342).
            // Work around that framework bug by capturing the original shader functions, without lookupArchives. The normal compiler cache still applies.
            id<MTLRenderPipelineState> cb = arch_capture(b, nil, vd);
            if (cb) { base = cb; g_compacted++; }
        }
        if (!base) {
            if (g_arch4.count + g_arch4p.count) { g_pt_miss_n++; g_pt_miss_ns += t1 - t0; }
            if (compile) {
                NSError *be = nil; base = arch_capture(b, &be, vd); job->error = be; job->compiled = 1;
                g_pt_comp_ns += bp_now() - t1;
                if (base) { g_pb_built++; if (g_arch_cap) arch_note_new(); }
            }
        }
        if (base || compile) g_pb_base_ns += bp_now() - t0;
        os_unfair_lock_lock(&g_pbase_lock);
        job->ps = base;
        if (base) hm_put_id(&g_pbase, &bk, base);
        hm_rel(nvn_hm_del(&g_pbase_pending, &bk)); dispatch_group_leave(job->done);
        os_unfair_lock_unlock(&g_pbase_lock);
        if (!base) { if (compile && e) *e = job->error; return nil; }
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
// Pipeline key: current state S + pass formats (normal pass; a merged pass additionally carries lmap / specialized function, filled in by pso_get)
static void pso_key_fill(pso_key *k, NvShader *vs, NvShader *fs, const uint32_t cfmt[8], uint32_t dfmt, uint32_t sfmt, uint32_t samples, int lmap) {
    memset(k, 0, sizeof *k);
    k->vs = (__bridge void *)vs; k->fs = (__bridge void *)fs;
    memcpy(k->cfmt, cfmt, sizeof k->cfmt); k->dfmt = dfmt; k->sfmt = sfmt; k->samples = samples;
    k->blend_en = S.color.blend_en; k->chmask = S.chmask.mask; k->a2c = S.ms.a2c; k->atest = atest_key(fs, cfmt, lmap ? g_pso_lmap : NULL);
    for (int i = 0; i < 8; i++) if (k->cfmt[i] || lmap) k->blend[i] = S.blend[i];
    k->logic = lop_key(k, fs, lmap);
    const nvn_refl *rf = &vs->rf;
    for (int v = 0; v < rf->nv; v++) { int a = rf->v[v].nvn; if (a < 16 && a < S.nattr) { k->attr[a] = S.attr[a]; int st = S.attr[a].stream; if (st < 16) k->stream[st] = S.stream[st]; } }
    pso_key_norm(k, lmap);
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
// inputs but no pulling library, and integer-output or multisampled bases are left to the draws. An app that bundles these bases made offline (DM_PSO_BUNDLE, pso_bundle_covers)
// builds none of them, only the pairs that archive left to the device (rest_pairs.tsv beside it: functions with constants)
static _Atomic int g_pre_running; static _Atomic int g_pre_next, g_pre_done, g_pre_built, g_pre_archived, g_pre_skipped, g_pre_failed;   // g_pre_next: pairs handed to a worker
// Pairs whose base failed to build, kept with the system build and the stamp they failed under (prebuild_failed.tsv beside prebuilt.tsv): prebuilt.tsv is written with them, and a
// later start with both the same leaves them to the draws instead of building them again, which would fail the same way (Metal reports a compile that its compiler service
// could not finish only after retrying it itself: Super Mario Odyssey's alRenderMaterialUber at -O2, 33.7 minutes, on every start in the background). Another system build (a new
// GPU compiler) or another stamp tries them again. g_pre_known: those lines by pair ("vertex<TAB>pixel"); g_pre_fails: this run's new lines
static NSDictionary<NSString *, NSString *> *g_pre_known; static NSMutableArray<NSString *> *g_pre_fails; static os_unfair_lock g_pre_fail_lock = OS_UNFAIR_LOCK_INIT;
static _Atomic int g_pre_known_n;   // pairs left to the draws because they failed on an earlier start
static NSString *pre_fail_head(NSString *want) { return [arch_os_build() stringByAppendingString:want]; }   // what a failure list must have been written for
// The failure list at path, by pair, when it was written for this system build and stamp (else empty). *other: a list written for another system build or stamp exists
static NSDictionary<NSString *, NSString *> *pre_fail_load(NSString *path, NSString *want, int *other) {
    NSString *txt = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
    *other = 0; if (!txt) return @{};
    NSMutableString *head = [NSMutableString string]; NSMutableDictionary<NSString *, NSString *> *pairs = [NSMutableDictionary dictionary];
    for (NSString *l in [txt componentsSeparatedByString:@"\n"]) {
        if (!l.length || [l hasPrefix:@"#"]) continue;
        NSArray<NSString *> *c = [l componentsSeparatedByString:@"\t"];
        if (c.count >= 3 && [c[0] isEqualToString:@"fail"]) pairs[[NSString stringWithFormat:@"%@\t%@", c[1], c[2]]] = l;
        else [head appendFormat:@"%@\n", l];
    }
    if ([head isEqualToString:pre_fail_head(want)]) return pairs;
    *other = 1; return @{};
}
static MTL4FunctionDescriptor *pre_fn(id<MTLLibrary> lib, NSString *name) {   // as shader_load notes them (fn4_note): constants specialized with an empty set
    id<MTLFunction> f = [lib newFunctionWithName:name]; if (!f) return nil;
    MTL4LibraryFunctionDescriptor *ld = [MTL4LibraryFunctionDescriptor new]; ld.library = lib; ld.name = name;
    if (!f.functionConstantsDictionary.count) return ld;
    MTL4SpecializedFunctionDescriptor *sd = [MTL4SpecializedFunctionDescriptor new]; sd.functionDescriptor = ld; sd.constantValues = [MTLFunctionConstantValues new]; return sd;
}
static NSDictionary *pre_json(NSString *key) { NSData *d = [NSData dataWithContentsOfFile:[NSString stringWithFormat:@"%@/%@_b.json", g_msl_dir, key]]; return d ? [NSJSONSerialization JSONObjectWithData:d options:0 error:nil] : nil; }
// The base of one pair as the draws make it (pso_desc_pull / pso_desc), or nil when the pair is left to the draws. Call inside an autorelease pool
static MTL4RenderPipelineDescriptor *pre_desc(NSString *vk, NSString *fk, int zero) {
    NSDictionary *vj = pre_json(vk), *fj = pre_json(fk);
    if (!vj || !fj || ![vj[@"chain"] isEqual:@DMSH_CHAIN] || ![fj[@"chain"] isEqual:@DMSH_CHAIN]) return nil;
    NSString *ventry = zero && vj[@"entry_z"] ? vj[@"entry_z"] : vj[@"entry"], *fentry = fj[@"entry"] ?: @"main0";
    int pull = [vj[@"pull"] isKindOfClass:[NSDictionary class]];
    if (!pull && [vj[@"vertex_inputs"] count]) return nil;   // stage inputs and no pulling library: its pipelines need the draw's layout
    NSError *e = nil;
    id<MTLLibrary> vl = lib_from_file([NSString stringWithFormat:@"%@/%@%@.metallib", g_msl_dir, vk, pull ? @"_bp" : @"_b"], &e);
    if (!vl || t_lib_layer) return nil;   // writes the layer: its pipelines take an input primitive class (pso_desc), no pulling pipeline
    id<MTLLibrary> fl = lib_from_file([NSString stringWithFormat:@"%@/%@_b.metallib", g_msl_dir, fk], &e);
    MTL4FunctionDescriptor *vf = pre_fn(vl, ventry), *ff = fl ? pre_fn(fl, fentry) : nil;
    if (!vf || !ff) return nil;
    MTL4RenderPipelineDescriptor *m = [MTL4RenderPipelineDescriptor new];
    m.vertexFunctionDescriptor = vf; m.fragmentFunctionDescriptor = ff; m.vertexDescriptor = [MTLVertexDescriptor vertexDescriptor]; m.rasterSampleCount = 1;
    for (int i = 0; i < 8; i++) { MTL4RenderPipelineColorAttachmentDescriptor *a = m.colorAttachments[i];
        a.pixelFormat = MTLPixelFormatUnspecialized; a.writeMask = MTLColorWriteMaskUnspecialized; a.blendingState = MTL4BlendStateUnspecialized;
        a.sourceRGBBlendFactor = a.destinationRGBBlendFactor = a.sourceAlphaBlendFactor = a.destinationAlphaBlendFactor = MTLBlendFactorUnspecialized;
        a.rgbBlendOperation = a.alphaBlendOperation = MTLBlendOperationUnspecialized; }
    return m;
}
static void pre_pair(NSString *vk, NSString *fk, int zero) {
    @autoreleasepool {
        if (g_pre_known.count && g_pre_known[[NSString stringWithFormat:@"%@\t%@", vk, fk]]) { g_pre_skipped++; g_pre_known_n++; return; }
        MTL4RenderPipelineDescriptor *m = pre_desc(vk, fk, zero);
        if (!m) { g_pre_skipped++; return; }
        for (id<MTL4Archive> a in g_arch4p) if ([a newRenderPipelineStateWithDescriptor:m error:nil]) { g_pre_archived++; return; }
        for (id<MTL4Archive> a in g_arch4) if ([a newRenderPipelineStateWithDescriptor:m error:nil]) { g_pre_archived++; return; }
        NSError *e = nil; id ps = arch_capture(m, &e, 0);
        if (ps) g_pre_built++;
        else {
            if (g_pre_failed++ < 4) MLOG("[RED] pipeline prebuild: the base of %s + %s failed: %s", vk.UTF8String, fk.UTF8String, e.localizedDescription.UTF8String);
            NSString *why = [[[e.localizedDescription ?: @"no error" componentsSeparatedByCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet]
                              filteredArrayUsingPredicate:[NSPredicate predicateWithFormat:@"length > 0"]] componentsJoinedByString:@" "];
            os_unfair_lock_lock(&g_pre_fail_lock); [g_pre_fails addObject:[NSString stringWithFormat:@"fail\t%@\t%@\t%@", vk, fk, why]]; os_unfair_lock_unlock(&g_pre_fail_lock);
        }
    }
}
// What a prebuild covers (prebuilt.tsv): the chain version, the depth mode, and the pair lists' length in bytes and SHA-256 (first 16 hex digits). dm/gfx/tools/pso_offline.py writes
// the same for the archive it makes offline, from the same files
static NSString *pre_stamp(NSString *txt, int zero) {
    NSData *d = [txt dataUsingEncoding:NSUTF8StringEncoding]; uint8_t h[CC_SHA256_DIGEST_LENGTH]; CC_SHA256(d.bytes, (CC_LONG)d.length, h);
    return [NSString stringWithFormat:@"chain\t%s\ndepth\t%s\npairs\t%lu\t%02x%02x%02x%02x%02x%02x%02x%02x\n", DMSH_CHAIN, zero ? "near=0" : "near=-w", (unsigned long)d.length,
            h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7]];
}
static NSArray<NSArray<NSString *> *> *pre_parse(NSString *txt) {   // (vertex key, pixel key) per line, without empty lines, comments and repeated lines
    NSMutableArray<NSArray<NSString *> *> *pairs = [NSMutableArray array]; NSMutableSet<NSString *> *seen = [NSMutableSet set];
    for (NSString *l in [txt componentsSeparatedByString:@"\n"]) { if (!l.length || [l hasPrefix:@"#"] || [seen containsObject:l]) continue; [seen addObject:l]; NSArray *c = [l componentsSeparatedByString:@"\t"]; if (c.count >= 2) [pairs addObject:c]; }
    return pairs;
}
// Whether the archive bundled with the app (pso_bundle_open) holds this run's prebuild: its prebuilt.tsv is the stamp the device would write for these pairs and this depth mode,
// and a probe of up to 16 pairs spread over the list (not those it left to the device, rest) finds every base it makes in it (an archive that opens but whose binaries this system
// does not take finds none)
enum { PRE_PROBE = 16 };
static const char *stamp_line(NSString *st) {   // a stamp on one line, for the log
    return [[[st stringByTrimmingCharactersInSet:NSCharacterSet.newlineCharacterSet] stringByReplacingOccurrencesOfString:@"\n" withString:@"; "] stringByReplacingOccurrencesOfString:@"\t" withString:@" "].UTF8String;
}
static int pso_bundle_covers(NSArray<NSArray<NSString *> *> *pairs, NSString *want, int zero, NSString *rest) {
    NSString *have = [NSString stringWithContentsOfFile:[g_bundle_dir stringByAppendingPathComponent:@"prebuilt.tsv"] encoding:NSUTF8StringEncoding error:nil];
    if (![have isEqualToString:want]) {
        MLOG("pipeline archive bundled with the app: made for %s, this run needs %s: the device builds the bases it does not hold%s", have ? stamp_line(have) : "(no prebuilt.tsv)", stamp_line(want),
             have && [have rangeOfString:zero ? @"depth\tnear=-w" : @"depth\tnear=0"].location != NSNotFound ? " (the game's depth mode is not the one built for: the profile key depth_mode)" : "");
        return 0;
    }
    NSMutableSet<NSArray<NSString *> *> *left = [NSMutableSet setWithArray:pre_parse(rest ?: @"")];
    NSMutableArray<NSArray<NSString *> *> *cand = [NSMutableArray array];
    for (NSArray<NSString *> *p in pairs) if (![left containsObject:@[p[0], p[1]]]) [cand addObject:p];
    uint64_t t0 = bp_now(); int probed = 0, found = 0; NSUInteger n = cand.count;
    for (NSUInteger k = 0; k < PRE_PROBE && k < n; k++) @autoreleasepool {
        NSArray<NSString *> *p = cand[k * n / MIN(n, (NSUInteger)PRE_PROBE)];
        MTL4RenderPipelineDescriptor *m = pre_desc(p[0], p[1], zero);
        if (!m) continue;
        probed++; found += [g_arch_bundle newRenderPipelineStateWithDescriptor:m error:nil] != nil;
    }
    int ok = probed > 0 && found == probed;   // skipped samples give no evidence that this system can take a base from the archive
    MLOG("pipeline archive bundled with the app: %s (%d of %d probed bases found, %.1f ms)%s", ok ? "holds the bases of every shader pair" : "this system does not take its bases",
         found, probed, (bp_now() - t0) / 1e6, ok ? "" : ": the device builds them");
    return ok;
}
// threads: workers; wait: run on the calling thread until done (the tool), else in the background. Returns the number of pairs, -1 when there is nothing to do
static int pso_prebuild(int threads, int wait, dispatch_qos_class_t qos) {
    shmap_load(); pso4_init();
    if (!g_comp4 || (!g_arch_bundle && !g_arch_cap) || atomic_exchange(&g_pre_running, 1)) return -1;
    // The pairs: shader_pairs.tsv (BNSH variations and the other readers' pairs) and pso_pairs.tsv (agl BAHS program variations, also read by vs_pad; DM_PSO_PAIRS: another path),
    // both beside the shader map
    NSString *pp = [g_shmap_path.stringByDeletingLastPathComponent stringByAppendingPathComponent:@"shader_pairs.tsv"];
    const char *ev = getenv("DM_PSO_PAIRS");
    NSString *pp2 = ev && *ev ? @(ev) : [g_shmap_path.stringByDeletingLastPathComponent stringByAppendingPathComponent:@"pso_pairs.tsv"];
    NSString *txt = [NSString stringWithContentsOfFile:pp encoding:NSUTF8StringEncoding error:nil], *txt2 = [NSString stringWithContentsOfFile:pp2 encoding:NSUTF8StringEncoding error:nil];
    if (!txt) { MLOG("pipeline prebuild: no %s, nothing to build", pp.UTF8String); g_pre_running = 0; return -1; }
    if (txt2) txt = [txt stringByAppendingString:txt2];
    int zero = atomic_load(&g_depth_zero);
    NSString *want = pre_stamp(txt, zero);
    NSArray<NSArray<NSString *> *> *pairs = pre_parse(txt);
    NSString *rp = g_arch_bundle ? [g_bundle_dir stringByAppendingPathComponent:@"rest_pairs.tsv"] : nil, *rest = rp ? [NSString stringWithContentsOfFile:rp encoding:NSUTF8StringEncoding error:nil] : nil;
    if (g_arch_bundle && pso_bundle_covers(pairs, want, zero, rest)) {   // the archive made offline holds them: only the pairs it left to the device (function constants) are built here
        if (!rest.length) { g_pre_running = 0; return -1; }
        txt = rest; want = pre_stamp(rest, zero); pairs = pre_parse(rest); pp = rp; txt2 = nil;
    }
    if (!g_arch_cap) { g_pre_running = 0; return -1; }
    NSString *stamp = [g_arch_dir stringByAppendingPathComponent:@"prebuilt.tsv"], *failp = [g_arch_dir stringByAppendingPathComponent:@"prebuild_failed.tsv"];
    int other = 0; g_pre_known = pre_fail_load(failp, want, &other); g_pre_fails = [NSMutableArray array]; g_pre_known_n = 0;
    // done, unless its failures were found on another system build: then they are tried again (the rest are taken from the archive)
    if ([[NSString stringWithContentsOfFile:stamp encoding:NSUTF8StringEncoding error:nil] isEqualToString:want] && !other) { g_pre_running = 0; return -1; }
    int n = (int)pairs.count; uint64_t t0 = bp_now();
    MLOG("pipeline prebuild: %d shader pairs of %s%s%s, %d workers (%s)", n, pp.UTF8String, txt2 ? " and " : "", txt2 ? pp2.UTF8String : "", threads, wait ? "tool" : prepare_on() ? "prepare step, before the first frame" : "background");
    atomic_store(&g_pre_next, 0);
    dispatch_group_t g = dispatch_group_create();
    for (int t = 0; t < threads; t++) dispatch_group_async(g, dispatch_get_global_queue(qos, 0), ^{
        for (;;) {
            int i = atomic_fetch_add(&g_pre_next, 1);
            // the first worker to find no pair left hands the period over: it is written once the compiles still under way in it return, not after the slowest of all
            if (i >= n) { if (i == n) arch_flush(); break; }
            pre_pair(pairs[(NSUInteger)i][0], pairs[(NSUInteger)i][1], zero);
            int d = ++g_pre_done;
            if (d % 1000 == 0) {
                MLOG("pipeline prebuild: %d of %d pairs, %d built, %d already archived, %d left to the draws, %d failed, %.0f s", d, n, (int)g_pre_built, (int)g_pre_archived, (int)g_pre_skipped, (int)g_pre_failed, (bp_now() - t0) / 1e9);
                if (atomic_load(&g_arch_new) >= 1000) arch_flush();   // the serializer holds what it captured until written
            }
        }
    });
    dispatch_block_t done = ^{
        arch_flush_wait();
        NSMutableArray<NSString *> *fails = [NSMutableArray arrayWithArray:g_pre_known.allValues]; [fails addObjectsFromArray:g_pre_fails];
        if (fails.count) [[NSString stringWithFormat:@"# pipeline prebuild: pairs whose base failed to build on this system build, left to the draws until it or the stamp changes (nvn_mtl.m, pre_fail_load)\n%@%@\n",
                            pre_fail_head(want), [fails componentsJoinedByString:@"\n"]] writeToFile:failp atomically:YES encoding:NSUTF8StringEncoding error:nil];
        else unlink(failp.fileSystemRepresentation);
        [want writeToFile:stamp atomically:YES encoding:NSUTF8StringEncoding error:nil];
        MLOG("pipeline prebuild done: %d pairs, %d bases built, %d already archived, %d left to the draws, %d failed, %.1f s", n, (int)g_pre_built, (int)g_pre_archived, (int)g_pre_skipped, (int)g_pre_failed, (bp_now() - t0) / 1e9);
        if (fails.count) MLOG("pipeline prebuild: %lu pairs whose base failed are left to the draws (%s; %d of them failed on an earlier start of this system build, not built again); another system build tries them again",
                              (unsigned long)fails.count, failp.UTF8String, (int)g_pre_known_n);
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
    if (pso_prebuild(threads, 1, QOS_CLASS_USER_INITIATED) < 0) MLOG("pipeline prebuild: nothing to do (no archive, no shader_pairs.tsv, prebuilt.tsv says it is done, or the archive bundled with the app holds every base)");
    return 1;
}
static int prepare_on(void);
static void pso_prebuild_bg(void) {   // after the prewarm, in the app without a prepare step (with one, prepare_prebuild builds them before the first frame)
    const char *e = getenv("DM_PSO_PREBUILD"); if (e && !strcmp(e, "0")) return;
    if (!hybrid_on() || prepare_on()) return;
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
// The pipelines of slice-merged passes (g_sm_plan): the draw's pipeline built with maxVertexAmplificationCount 2, keyed with AMP_MARK. Built in the background on first use; until it is
// there the draw gets nil and goes to a pass of its own (sm_plan does not merge, draw splits a merged pass), so no frame waits for the compiler. A failed build is not tried again
enum { SM_AMP_MAX = 64 };
static struct { pso_key k; id<MTLRenderPipelineState> ps; int done; } g_sm_amp[SM_AMP_MAX]; static int g_sm_amp_n; static os_unfair_lock g_sm_amp_lock = OS_UNFAIR_LOCK_INIT;
static __unsafe_unretained id<MTLRenderPipelineState> sm_amp_get(const pso_key *k, NvShader *vs, NvShader *fs) {
    __unsafe_unretained id<MTLRenderPipelineState> c = hm_get_id(&g_pso, k); if (c) return c;
    os_unfair_lock_lock(&g_sm_amp_lock); int i = 0; for (; i < g_sm_amp_n; i++) if (!memcmp(&g_sm_amp[i].k, k, sizeof *k)) break;
    if (i < g_sm_amp_n) { id ps = g_sm_amp[i].done ? g_sm_amp[i].ps : nil; os_unfair_lock_unlock(&g_sm_amp_lock); if (ps) { hm_put_id(&g_pso, k, ps); return hm_get_id(&g_pso, k); } return nil; }
    if (g_sm_amp_n >= SM_AMP_MAX) { os_unfair_lock_unlock(&g_sm_amp_lock); return nil; }
    g_sm_amp[i].k = *k; g_sm_amp[i].ps = nil; g_sm_amp[i].done = 0; g_sm_amp_n++; os_unfair_lock_unlock(&g_sm_amp_lock);
    MTLRenderPipelineDescriptor *d = pso_desc(k, vs, fs, nil, 0);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ NSError *e = nil; id<MTLRenderPipelineState> ps = pso_build(d, &e);
        if (!ps) MLOG("[RED] slice-merged pass: no amplified pipeline for vertex %s fragment %s: %s (those passes stay apart)", vs->key, fs ? fs->key : "-", e.localizedDescription.UTF8String);
        os_unfair_lock_lock(&g_sm_amp_lock); g_sm_amp[i].ps = ps; g_sm_amp[i].done = 1; os_unfair_lock_unlock(&g_sm_amp_lock); });
    return nil;
}
static __unsafe_unretained id<MTLRenderPipelineState> pso_get_(NvProg *pr, MTLRenderPassDescriptor *unused) {
    (void)unused;
    g_pso_pull = 0;   // set again only where a pulling pipeline is returned: a stale 1 made the next draw, whose pipeline came from the cache, bind the layout block instead of its streams
    __unsafe_unretained NvShader *vs = pr->sh[0], *fs = pr->sh[1];
    if (!vs) { RED_ONCE(13, "program has no vertex shader (or it is not mapped)"); return nil; }
    pso_key k; pso_key_fill(&k, vs, fs, g_pass.cfmt, g_pass.dfmt, g_pass.sfmt, g_pass.samples, g_pso_lmap != NULL);
    if (S.rast_discard) {
        k.fsfn = DISCARD_MARK;
        id<MTLRenderPipelineState> cached = hm_get_id(&g_pso, &k);
        if (cached) return cached;
        if (hm_get_id(&g_pso_fail, &k)) return nil;
        NSError *error = nil;
        id<MTLRenderPipelineState> ps = pso_build(pso_desc(&k, vs, nil, nil, 0), &error);
        if (ps) hm_put_id(&g_pso, &k, ps);
        else { MLOG("[RED] rasterizer-discard pipeline (vertex %s): %s", vs->key, error.localizedDescription.UTF8String); hm_put_id(&g_pso_fail, &k, NSNull.null); }
        return ps;
    }
    if (g_pso_lmap) { memcpy(k.lmap, g_pso_lmap, 8); k.fsfn = (__bridge void *)g_pso_fsfn; }
    id<MTLFunction> ms_fn = ms_fs_override(fs); if (ms_fn) k.fsfn = (__bridge void *)ms_fn;   // DM_NVN_MAGSCAN phases 2 / 3
    if (g_pso_amp) { if (k.fsfn || g_pso_lmap) return nil; k.fsfn = AMP_MARK; return sm_amp_get(&k, vs, fs); }   // a slice-merged pass's pipeline: built in the background, nil until then
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
    static int predict = -1; if (predict < 0) { const char *e = getenv("DM_NVN_PREDICT"); predict = !(e && !strcmp(e, "0")); if (!predict) MLOG("DM_NVN_PREDICT=0: no parallel precompile prediction"); }
    if (predict && g_pp_submit_mark != g_sub_serial) {
        g_pp_submit_mark = g_sub_serial; uint64_t scan_t0 = bp_now(); pso_predict_rest();
        g_bp_pso_ns += bp_now() - scan_t0;   // Keep lookahead work in the pipeline-miss timing, as before
    }
    return ps;
}
static NSString *warm_key(const char *s, size_t n) {
    size_t z = strnlen(s, n);
    return [[NSString alloc] initWithBytes:s length:z encoding:NSUTF8StringEncoding];
}
// The prewarm runs the lines of its lists as tasks: a shader (S), a compute pipeline (C), or every render pipeline (R) of one shader pair. The pipelines of a pair share their
// bases, so one worker builds them one after the other and no two workers compile the same base. The tasks keep the lists' order (a pair at its first line).
// Shaders go into one dictionary all workers share (a key two workers load at once is kept once; a shader that failed to load is kept as NSNull and not tried again)
@interface WarmTask : NSObject { @public int kind, stage; NSString *key, *vkey, *fkey; NSMutableArray<NSData *> *rs; } @end
@implementation WarmTask @end
static NSMutableDictionary *g_warm_dict; static os_unfair_lock g_warm_dict_lock = OS_UNFAIR_LOCK_INIT;
static _Atomic long g_warm_done;   // lines done (the prepare screen's count)
// A prewarm list may name shaders of another version of the game: the bundled list of the version the profile was brought up on, packed with another version (the NSP script's
// derived profile: Hollow Knight: Silksong's 1.0.30000 list named 16 shaders its base game 1.0.27776 does not have), or a device's pso_user.tsv kept across a game update.
// A line with a key the shader map does not have is dropped quietly (the keys are counted in the "prewarm done" line); the game never draws with it
static int warm_stale(NSString *key, NSMutableSet<NSString *> *stale) {
    NSString *mk = [key hasSuffix:@"_b"] ? [key substringToIndex:key.length - 2] : key;   // library names are <map key>_b
    if (g_shmap[mk]) return 0;
    if (![stale containsObject:key]) { [stale addObject:key]; g_warm_skip_stale++; }
    return 1;
}
static NvShader *warm_shader(NSString *key, int stage) {
    os_unfair_lock_lock(&g_warm_dict_lock); id sh = g_warm_dict[key]; os_unfair_lock_unlock(&g_warm_dict_lock);
    if (!sh) {
        NvShader *n = shader_load(key, stage, 0);
        if (n && n->stage == 0 && n->has_pull && !n->layer_out && hybrid_on()) pull_load(n);   // its pulling entry too, as shader_prefetch_rest loads it, so a draw that falls back to it loads nothing (no other thread has n yet)
        os_unfair_lock_lock(&g_warm_dict_lock); sh = g_warm_dict[key]; if (!sh) g_warm_dict[key] = sh = n ?: (id)NSNull.null; os_unfair_lock_unlock(&g_warm_dict_lock);
    }
    return sh == NSNull.null ? nil : sh;
}
static void warm_enqueue(WarmItem *item) {
    os_unfair_lock_lock(&g_warm_lock);
    [g_warm_items addObject:item];
    atomic_store_explicit(&g_warm_ready, 1, memory_order_release);
    os_unfair_lock_unlock(&g_warm_lock);
}
// The lists' lines as tasks; *lines: the lines they hold
static NSArray<WarmTask *> *warm_parse(NSArray<NSString *> *paths, long *lines) {
    NSMutableArray<WarmTask *> *tasks = [NSMutableArray array]; NSMutableDictionary<NSString *, WarmTask *> *pairs = [NSMutableDictionary dictionary];
    NSMutableSet<NSString *> *seen = [NSMutableSet set], *stale = [NSMutableSet set]; long n = 0;
    NSMutableSet<NSData *> *seen_k = [NSMutableSet set];   // rows recorded before pso_key_norm that are one pipeline now
    for (NSString *path in paths) {
        NSString *txt = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
        if (!txt) { MLOG("[RED] cannot read prewarm list: %s", path.UTF8String); continue; }
        { unsigned long nl = 0; for (NSString *l in [txt componentsSeparatedByString:@"\n"]) nl += l.length && ![l hasPrefix:@"#"];
          MLOG("pipeline prewarm list %s: %lu lines", path.UTF8String, nl); }   // Diagnostics: how many lines each manifest gave (when a device warms up 0, shows which one was missing)
        for (NSString *line in [txt componentsSeparatedByString:@"\n"]) {
            if (!line.length || [line hasPrefix:@"#"] || [seen containsObject:line]) continue;
            [seen addObject:line];
            NSArray<NSString *> *c = [line componentsSeparatedByString:@"\t"];
            WarmTask *t = nil;
            if (([c[0] isEqualToString:@"S"] && c.count >= 3) || ([c[0] isEqualToString:@"C"] && c.count >= 2)) {
                if (!c[1].length || warm_stale(c[1], stale)) continue;
                t = [WarmTask new]; t->kind = [c[0] isEqualToString:@"S"] ? 0 : 2; t->key = c[1]; t->stage = t->kind ? 5 : c[2].intValue; [tasks addObject:t];
            } else if ([c[0] isEqualToString:@"R"] && c.count >= 2) {
                pso_skey sk; if (pso_skey_parse(c[1].UTF8String, &sk)) continue;
                NSData *kd = [NSData dataWithBytes:&sk length:sizeof sk]; if ([seen_k containsObject:kd]) continue;
                [seen_k addObject:kd];
                NSString *vkey = warm_key(sk.vs, sizeof sk.vs), *fkey = warm_key(sk.fs, sizeof sk.fs);
                if (!vkey.length || warm_stale(vkey, stale) || (fkey.length && warm_stale(fkey, stale))) continue;
                NSString *pk = [NSString stringWithFormat:@"%@|%@", vkey, fkey];
                t = pairs[pk];
                if (!t) { t = [WarmTask new]; t->kind = 1; t->vkey = vkey; t->fkey = fkey; t->rs = [NSMutableArray array]; pairs[pk] = t; [tasks addObject:t]; }
                [t->rs addObject:kd];
            } else continue;
            n++;
        }
    }
    *lines = n; return tasks;
}
static void warm_task(WarmTask *t) {
    @autoreleasepool {
        if (t->kind == 0) {
            NvShader *sh = warm_shader(t->key, t->stage);
            if (sh) { WarmItem *item = [WarmItem new]; item->kind = 0; item->key = t->key; item->shader = sh; warm_enqueue(item); g_warm_shaders++; }
            g_warm_done++; return;
        }
        if (t->kind == 2) {
            NvShader *cs = warm_shader(t->key, 5); NSError *e = nil; id ps = cs ? [g_dev newComputePipelineStateWithFunction:cs->fn error:&e] : nil;
            if (cs && !ps) MLOG("[RED] background compute pipeline creation failed: %s", e.localizedDescription.UTF8String);
            if (ps) { WarmItem *item = [WarmItem new]; item->kind = 2; item->key = t->key; item->shader = cs; item->object = ps; warm_enqueue(item); g_warm_cps++; }
            g_warm_done++; return;
        }
        pso_skey sk; [t->rs[0] getBytes:&sk length:sizeof sk];
        NvShader *vs = warm_shader(t->vkey, sk.vs_stage), *fs = t->fkey.length ? warm_shader(t->fkey, sk.fs_stage) : nil;
        for (NSData *row in t->rs) {
            [row getBytes:&sk length:sizeof sk];
            if (!vs || (t->fkey.length && !fs)) { g_warm_done++; continue; }
            id<MTLFunction> fsfn = nil;
            if (sk.fz) {
                if (sk.fz_desc.kind == 1 && fs) { int fx[32]; for (int i = 0; i < 32; i++) fx[i] = sk.fz_desc.fx[i]; fsfn = bf_fn(fs, fx, sk.fz_desc.fbmask, sk.k.lmap); }
                if (!fsfn) { g_warm_skip_fz++; g_warm_done++; continue; }
            }
            pso_key k = sk.k; k.vs = (__bridge void *)vs; k.fs = (__bridge void *)fs; k.fsfn = (__bridge void *)fsfn;   // pso_skey_parse normalized rows written before the key was
            NSError *e = nil; id ps = pso_build(pso_desc(&k, vs, fs, fsfn, sk.merged), &e);
            if (!ps && !vs->layer_out && [e.localizedDescription containsString:@"render_target_array_index"]) {   // a vertex shader that writes the layer (see pso_get_)
                vs->layer_out = 1; e = nil; ps = pso_build(pso_desc(&k, vs, fs, fsfn, sk.merged), &e); }
            g_warm_done++;
            if (!ps) { MLOG("[RED] background render pipeline creation failed: %s", e.localizedDescription.UTF8String); continue; }
            WarmItem *item = [WarmItem new]; item->kind = 1; item->vs_key = t->vkey; item->fs_key = t->fkey; item->vs = vs; item->fs = fs; item->fsfn = fsfn; item->pso = k; item->object = ps;
            warm_enqueue(item); g_warm_rps++;
        }
    }
}
static void warm_finish(void) {
    MLOG("pipeline prewarm done: shaders %llu / render pipelines %llu (specialized fragment skipped %llu) / compute pipelines %llu, background time %.1f ms; shader keys not in the shader map (another game version) %llu",
         (unsigned long long)g_warm_shaders, (unsigned long long)g_warm_rps, (unsigned long long)g_warm_skip_fz, (unsigned long long)g_warm_cps, (bp_now() - g_warm_t0) / 1e6, (unsigned long long)g_warm_skip_stale);
    if (g_comp4) MLOG("pipeline bases so far: %llu compiled, %llu from the archive, %llu reused (%.1f ms for the bases); %llu specializations, %llu pipelines without a base (MTLDevice)",
                      (unsigned long long)g_pb_built, (unsigned long long)g_pb_archived, (unsigned long long)g_pb_reused, g_pb_base_ns / 1e6, (unsigned long long)g_pb_spec, (unsigned long long)g_pb_legacy);
    atomic_store(&g_warm_running, 0);
    arch_flush();   // the period of the recaptured bases (compaction finishes once it is written), after the prewarm stops recapturing them
    @autoreleasepool { pso_prebuild_bg(); }   // then, once, the bases every shader pair's hybrid draws need
}
// Wait for the background warm-up to finish (at most timeout_ms): returns how many milliseconds were actually waited, 0 if it is not running. On first install the system shader cache is fully cold; if warm-up is slower than the player entering a run,
// pipelines not yet warmed are compiled on the spot on the submit thread = in-run stutter; the loading screen on entering a run blocks the player anyway, so let it load a little longer (called where hle_nvn.c presents).
int mtl_warm_wait(int timeout_ms) {
    if (!atomic_load(&g_warm_running)) return 0;
    uint64_t t0 = bp_now();
    while (atomic_load(&g_warm_running) && (bp_now() - t0) / 1000000ull < (uint64_t)timeout_ms) usleep(2000);
    return (int)((bp_now() - t0) / 1000000ull) + 1;
}
// ---- The prepare step: the prewarm before the game's first frame, with a progress screen ----
// With a window (or DM_PSO_PREPARE=1), the first submission waits until every line of the prewarm lists (the profile's pso_manifest.tsv, then this device's pso_user.tsv) is built:
// the shaders loaded and the pipelines made, on as many workers as the device has cores, at user-initiated QoS, since the game draws nothing meanwhile. It runs at the first
// submission and not before the game starts because the vertex entry every pipeline is built with follows the game's depth mode (nvnDeviceSetDepthMode), which is known only by then.
// Each start runs it: when everything is in the Metal shader cache and the pipeline archive it takes well under a second, and only past 0.5 s (with 0.3 s or more still to go) does the front end show its screen
// ("Preparing shaders", a bar and a count: front_progress). A stamp that says the step ran would skip it exactly when the system dropped the Metal cache (an OS update, storage
// pressure on iOS), which is when it is needed. DM_PSO_PREPARE=0: the prewarm runs in the background as before (as it does without a window); DM_PSO_PREPARE_THREADS=<n> sets the workers
void front_progress(const char *title, long done, long total);
__attribute__((weak)) void front_progress(const char *title, long done, long total) { (void)title; (void)done; (void)total; }   // the front ends' (front.h); none in the unit tests
static int prepare_on(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("DM_PSO_PREPARE");
        if (e && !strcmp(e, "0")) { MLOG("DM_PSO_PREPARE=0: the pipeline prewarm runs in the background, the game does not wait for it"); v = 0; }
        else v = (e && !strcmp(e, "1")) || front_metal_layer() != NULL; }
    return v;
}
static int prepare_workers(void) {
    const char *e = getenv("DM_PSO_PREPARE_THREADS"); int n = e ? atoi(e) : 0;
    if (n <= 0) { size_t sz = sizeof n; if (sysctlbyname("hw.activecpu", &n, &sz, NULL, 0) || n < 2) n = 2; }
    return n;
}
static void warm_prepare(NSArray<WarmTask *> *tasks, long lines) {
    int workers = prepare_workers(); long n = (long)tasks.count;
    uint64_t t0 = bp_now(), built0 = g_pb_built, arch0 = g_pb_archived, reused0 = g_pb_reused, spec0 = g_pb_spec;
    __block _Atomic long next = 0;
    dispatch_group_t g = dispatch_group_create();
    for (int w = 0; w < workers; w++) dispatch_group_async(g, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        for (;;) { long i = atomic_fetch_add(&next, 1); if (i >= n) break; warm_task(tasks[(NSUInteger)i]); }
    });
    int shown = 0;
    while (dispatch_group_wait(g, dispatch_time(DISPATCH_TIME_NOW, 50 * (int64_t)NSEC_PER_MSEC))) {
        // the screen shows past 0.5 s, unless the rate so far says the rest takes less than 0.3 s more (no screen that flashes up and goes)
        uint64_t el = bp_now() - t0; long done = g_warm_done; double rest = done ? (double)el * (double)(lines - done) / (double)done : 1e18;
        if (!shown && el >= 500000000ull && rest >= 3e8) { shown = 1; MLOG("pipeline prepare: %ld of %ld lines done after %.1f s, the progress screen shows", done, lines, el / 1e9); }
        if (shown) front_progress("Preparing shaders", g_warm_done, lines);
    }
    if (shown) front_progress(NULL, lines, lines);
    MLOG("pipeline prepare: %ld lines (%llu shaders, %llu render pipelines, %llu compute pipelines) in %.2f s on %d workers before the first frame, progress screen %s; "
         "bases %llu compiled, %llu from the archive, %llu reused; %llu specializations",
         lines, (unsigned long long)g_warm_shaders, (unsigned long long)g_warm_rps, (unsigned long long)g_warm_cps, (bp_now() - t0) / 1e9, workers, shown ? "shown" : "not shown",
         (unsigned long long)(g_pb_built - built0), (unsigned long long)(g_pb_archived - arch0), (unsigned long long)(g_pb_reused - reused0), (unsigned long long)(g_pb_spec - spec0));
}
// The prebuild in the prepare step: the bases of every shader pair are made before the game's first frame, behind the progress screen ("Preparing pipelines"), once per chain
// version, depth mode and pair list (prebuilt.tsv), so a running game never compiles them meanwhile. Built in the background while playing, they took an iPhone 17 Pro Max's GPU
// from about 13 to 15-18 ms a frame in Breath of the Wild (the compiler's load on the shared power budget); a first start waits for them instead. It waits until every pair is
// handed to a worker: the compiles still under way then (at most one per worker) finish in the background, so one that does not return (a base Apple's compiler cannot build,
// GitHub issue #294) does not hold the game's start
static void prepare_prebuild(void) {
    const char *e = getenv("DM_PSO_PREBUILD"); if (e && !strcmp(e, "0")) return;
    if (!hybrid_on()) return;
    int n = pso_prebuild(prepare_workers(), 0, QOS_CLASS_USER_INITIATED);
    if (n <= 0) return;
    uint64_t t0 = bp_now(); int shown = 0;
    while (atomic_load(&g_pre_running) && atomic_load(&g_pre_next) < n) {
        usleep(50000);
        if (!shown && bp_now() - t0 >= 500000000ull) shown = 1;
        if (shown) front_progress("Preparing pipelines", g_pre_done, n);
    }
    int left = atomic_load(&g_pre_running) ? n - (int)g_pre_done : 0;   // a worker's last compile can still be under way
    if (left > 0) MLOG("pipeline prepare: every shader pair handed out after %.1f s; %d compiles still under way finish in the background", (bp_now() - t0) / 1e9, left);
    if (shown) front_progress(NULL, n, n);
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
    g_warm_dict = [NSMutableDictionary dictionary];
    pso4_init();   // the pipeline archive opens before the prewarm builds from it
    g_warm_t0 = bp_now();
    atomic_store(&g_warm_running, 1);
    if (prepare_on()) {
        long lines = 0; NSArray<WarmTask *> *tasks = warm_parse(paths, &lines);
        if (lines) warm_prepare(tasks, lines);
        else MLOG("pipeline prepare: the prewarm lists hold nothing to build");
        warm_finish();
        @autoreleasepool { prepare_prebuild(); }   // then the bases every shader pair's hybrid draws need, before the first frame too (once)
        return;
    }
    dispatch_queue_attr_t attr = dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INITIATED, 0);   // Not optional background work: it decides whether the pipelines are ready before the player enters a run
    dispatch_queue_t q = dispatch_queue_create("dm.nvn.pso-warm", attr);
    dispatch_async(q, ^{ @autoreleasepool { long lines = 0; for (WarmTask *t in warm_parse(paths, &lines)) warm_task(t); warm_finish(); } });
}
