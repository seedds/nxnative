// nvn_mtl.m, part: shader
// Shaders and descriptors: the shader mapping table, c1 banks, pipeline recording files, shader and program loading, vertex-conversion sets, texture and sampler registration (the descriptor pool), tex_by_id / samp_by_id and the argument buffer arenas.
// Included by nvn_mtl.m in its original order, as one translation unit: statics are shared with the other parts.
// ================= Shaders / descriptor pool / pipelines / draws =================
static pthread_mutex_t g_rmu = PTHREAD_MUTEX_INITIALIZER;
// ---------- Shader mapping table: Switch control section hash → PC metallib (produced by dm/gfx/tools/shader_map.py) ----------
static NSMutableDictionary<NSString *, NSArray *> *g_shmap;
static NSString *g_shmap_path, *g_msl_dir;
static NSString *exe_rel(const char *rel) {
    char buf[4096]; uint32_t n = sizeof buf;
    if (_NSGetExecutablePath(buf, &n)) return nil;
    char *dir = dirname(buf);
    return [NSString stringWithFormat:@"%s/%s", dir, rel];
}
// Shader mapping table (output of tools/shader_map.py): at runtime only 'Switch control section key → stage' is used; shaders are always translations of the Switch originals (tools/dmsh_build.py produces <key>_b.metallib + reflection offline)
static void shmap_load(void) {
    if (g_shmap) return;
    const char *e = getenv("DM_NVN_SHMAP");
    NSString *path = e ? @(e) : exe_rel("../games/" DM_GAME_ID "/shader_map.tsv");   // default: the current game profile folder (Mac: dm/games/<id>; on iOS main.m sets DM_NVN_SHMAP)
    NSString *txt = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
    if (!txt) MDIE("cannot read the shader map %s (DM_NVN_SHMAP can name another)", path.UTF8String);
    g_shmap = [NSMutableDictionary dictionary];
    for (NSString *ln in [txt componentsSeparatedByString:@"\n"]) {
        if (!ln.length || [ln hasPrefix:@"#"] || [ln hasPrefix:@"package"]) continue;
        NSArray *c = [ln componentsSeparatedByString:@"\t"];
        if (c.count > 3) g_shmap[c[3]] = c[2];
    }
    g_shmap_path = path;
    const char *m = getenv("DM_MSL_DIR");
    // Library directory = <DM_MSL_DIR or default cache>/b-<translation chain version>: the version comes from dm/gfx/nvn/dmsh_chain.h (tools/dmsh_chain.py), one for every game; a host built from an old source tree only accepts libraries of the old version
    g_msl_dir = [NSString stringWithFormat:@"%@/b-%s", m ? @(m) : [NSString stringWithFormat:@"%s%s/build/cache/metal_shader_cache", getenv("DM_ROOT") ?: getenv("HOME"), getenv("DM_ROOT") ? "" : "/dm-work"], DMSH_CHAIN];
    if (![NSFileManager.defaultManager fileExistsAtPath:g_msl_dir]) MLOG("[RED] shader library directory %s does not exist: translation chain version %s of this source tree has not been built yet (dm/gfx/tools/dmsh_build.py)", g_msl_dir.UTF8String, DMSH_CHAIN);
    // The library's stamp (inputs.tsv, written by a full dmsh_build.py run): the game and the digest of the profile inputs it was built from. The chain version is the same for every
    // game and every state of a profile's tables, so the stamp is what ties c1_full.tsv / cb_extent.tsv to these libraries: each names the inputs of the library it was made from
    NSString *stamp = [NSString stringWithContentsOfFile:[g_msl_dir stringByAppendingPathComponent:@"inputs.tsv"] encoding:NSUTF8StringEncoding error:nil], *st_game = nil, *st_inputs = nil;
    for (NSString *ln in [stamp componentsSeparatedByString:@"\n"]) {
        NSArray *c = [ln componentsSeparatedByString:@"\t"];
        if (c.count == 2 && [c[0] isEqual:@"game"]) st_game = c[1]; else if (c.count == 2 && [c[0] isEqual:@"inputs"]) st_inputs = c[1];
    }
    if (!st_inputs) MLOG("[RED] shader library %s has no stamp (inputs.tsv): no full dmsh_build.py run made it as it is, so c1_full.tsv and cb_extent.tsv are not taken", g_msl_dir.UTF8String);
    else if (st_game && ![st_game isEqual:@DM_GAME_ID]) MLOG("[RED] shader library %s was built for game %s, not " DM_GAME_ID, g_msl_dir.UTF8String, st_game.UTF8String);
    else MLOG("shader library stamp: game %s, inputs %s", st_game.UTF8String, st_inputs.UTF8String);
    NSString *cp = [[path stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"c1_full.tsv"];   // Same directory as the mapping table (Mac: dm/games/<id>; iOS: dm/ inside the App, copied along by build_ipa.sh)
    NSString *ct = cp ? [NSString stringWithContentsOfFile:cp encoding:NSUTF8StringEncoding error:nil] : nil;
    NSString *head = ct ? [[ct componentsSeparatedByString:@"\n"] firstObject] : nil;
    NSString *want = [NSString stringWithFormat:@"# chain b-%s inputs %@ ", DMSH_CHAIN, st_inputs ?: @"-"];   // a library without a stamp matches no table
    if (!head || ![head hasPrefix:want]) {
        MLOG("[RED] c1_full.tsv is stale or missing (its header must name this library's chain and inputs; a full dmsh_build.py run makes it); every c1 gets its own 64 KiB");
    } else {
        g_c1_full_valid = 1; g_c1_full = [NSMutableSet set];
        for (NSString *line in [ct componentsSeparatedByString:@"\n"]) if (line.length && ![line hasPrefix:@"#"]) [g_c1_full addObject:[line stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet]];
    }
    {   // cb_extent.tsv (tools/cb_extent.py), beside c1_full.tsv: without it a uniform block updated in part inside a pass is copied whole, up to 64 KB (ubp_get)
        NSString *xp = [[path stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"cb_extent.tsv"];
        NSString *xt = [NSString stringWithContentsOfFile:xp encoding:NSUTF8StringEncoding error:nil];
        NSString *xh = xt ? [[xt componentsSeparatedByString:@"\n"] firstObject] : nil;
        if (!xh || ![xh hasPrefix:want]) MLOG("cb_extent.tsv is stale or missing: uniform reads are taken to reach the whole binding");
        else { NSMutableDictionary *d = [NSMutableDictionary dictionary];
            for (NSString *line in [xt componentsSeparatedByString:@"\n"]) { if (!line.length || [line hasPrefix:@"#"]) continue;
                NSRange t = [line rangeOfString:@"\t"]; if (t.location != NSNotFound) d[[line substringToIndex:t.location]] = [line substringFromIndex:t.location + 1]; }
            g_cb_ext = d; MLOG("uniform read extents: %lu shaders (cb_extent.tsv)", (unsigned long)d.count); }
    }
    MLOG("shader route B (translated Switch originals): shader map %lu entries (%s), metallib directory %s", (unsigned long)g_shmap.count, path.UTF8String, g_msl_dir.UTF8String);
    warm_start();
}

@interface NvShader : NSObject { @public id<MTLFunction> fn, tf_fn, fn_u, fn_m, geo_fn; uint8_t mask_m;   // fn_u: the integer-output entry point (dmsh_build.py fs_uint), for pipelines whose colour attachments are all unsigned integer (pso_desc); fn_m: the mixed one, its outputs in mask_m unsigned integer
    uint16_t tf_stride[4]; uint32_t cb_ext[16], sb_ro; nvn_refl rf; id<MTLArgumentEncoder> ae; NSUInteger arg_len; int stage, range_ro, afr, layer_out; const char *builtin; uint64_t logged; char key[24]; id<MTLBuffer> c1buf; uint64_t c1_gpu; size_t c1off;
    uint32_t geo_vertices, geo_invocations, geo_output, geo_stride, geo_index_stride;
    id<MTLComputePipelineState> geo_pso;
    int gs_loc, lay_loc; char gs_comp, lay_comp;   // a slice-writing geometry shader (no function: the layer comes from input gs_loc.gs_comp) / a vertex shader with layer entry points writing output lay_loc.lay_comp (dmsh_build.py gs_slice, vs_layer_entry); -1 = none
    // The argument buffer encoded last time
    id<MTLBuffer> ab_buf; uint64_t ab_gpu; size_t ab_off; uint64_t ab_pass; uint32_t ab_binst;   // ab_pass: serial of the render pass that encoded this argument block - reused only within the same pass (the arena holding a block from another pass / another submission may already have been reclaimed and rewritten)
    id<MTLLibrary> bflib; int bflib_tried; NSMutableDictionary<NSString *, id<MTLFunction>> *bffn;   // bflib: readable-tile version <key>_bf (merge B step 2)
    uint64_t mis[4];   // mis: texture slots 0-255 already reported for a type mismatch (tex_mis_note)
    NSString *ventry; id<MTLFunction> vfn; int vfn_state;
    id<MTLFunction> pfn; int pfn_state, has_pull; } @end   // pfn: the same entry of the pulling library <key>_bp (pull_fn; has_pull: the reflection names one, dmsh_pull.py)   // vertex: the entry point loaded (by the depth mode); vfn: the same entry of the shadow-input library <key>_bv (vin_fn; state 0 untried, 1 loaded, 2 failed)
@implementation NvShader
- (void)dealloc { free(rf.c1); }
@end
#include "nvn_pso_skey.h"
#include "nvn_geometry.h"
@interface NvProg : NSObject { @public NvShader *sh[6]; char gs_miss[17]; NvProg *geo_raster; } @end   // geo_raster: GS output reader paired with this program's fragment stage
@implementation NvProg @end
// Warm-up entry: built in the background, waiting to be taken by the submit thread. vs / fs are strong references (in pso they are only raw pointers, and the background dictionary is released when the task ends)
@interface WarmItem : NSObject { @public int kind; NSString *key, *vs_key, *fs_key; NvShader *shader, *vs, *fs; pso_key pso; id<MTLFunction> fsfn; id object; } @end
@implementation WarmItem @end
static NSMutableDictionary<NSString *, NvShader *> *g_shcache;

static void c1_store(NvShader *o) {
    size_t len = o->rf.c1_len;
    NSString *guardKey = [NSString stringWithUTF8String:o->key];
    if ([guardKey hasSuffix:@"_b"]) guardKey = [guardKey substringToIndex:guardKey.length - 2];
    os_unfair_lock_lock(&g_c1_lock);   // Both the warm-up thread and the submit thread load shaders
    if (o->geo_vertices || !g_c1_full_valid || [g_c1_full containsObject:guardKey]) {   // Geometry topology variants have independent control flow; only the base shader is in the guard table.
        o->c1buf = [g_dev newBufferWithLength:C1_DECL_SIZE options:MTLResourceStorageModeShared]; o->c1_gpu = o->c1buf.gpuAddress;
        if (!o->c1buf) { os_unfair_lock_unlock(&g_c1_lock); MDIE("c1 dedicated buffer allocation failed"); }
        res_add(o->c1buf);
        memset(o->c1buf.contents, 0, C1_DECL_SIZE); memcpy(o->c1buf.contents, o->rf.c1, len);
        g_c1_alloc_bytes += C1_DECL_SIZE; g_c1_data_bytes += len; g_c1_count++; g_c1_exclusive++;
        os_unfair_lock_unlock(&g_c1_lock);
        return;
    }
    size_t need = (len + 255u) & ~((size_t)255u);
    if (!g_c1_banks) g_c1_banks = [NSMutableArray array];
    if (!g_c1_banks.count || g_c1_bank_off + need + 256u + C1_DECL_SIZE > C1_BANK_SIZE) {
        id<MTLBuffer> b = [g_dev newBufferWithLength:C1_BANK_SIZE options:MTLResourceStorageModeShared];
        if (!b) { os_unfair_lock_unlock(&g_c1_lock); MDIE("c1 buffer block allocation failed"); }
        res_add(b);
        memset(b.contents, 0, C1_BANK_SIZE);
        [g_c1_banks addObject:b];
        g_c1_bank_off = 0;
        g_c1_alloc_bytes += C1_BANK_SIZE;
    }
    id<MTLBuffer> b = g_c1_banks.lastObject;
    size_t off = g_c1_bank_off;
    memcpy((uint8_t *)b.contents + off, o->rf.c1, len);
    g_c1_bank_off += need + 256u;
    g_c1_data_bytes += len;
    g_c1_count++;
    o->c1buf = b;
    o->c1_gpu = b.gpuAddress;
    o->c1off = off;
    os_unfair_lock_unlock(&g_c1_lock);
}

static NSMutableArray<WarmItem *> *g_warm_items;
static os_unfair_lock g_warm_lock = OS_UNFAIR_LOCK_INIT;
static _Atomic int g_warm_ready;
static int g_warm_started;
static uint64_t g_warm_t0;
static _Atomic int g_warm_running;   // Background warm-up has not finished yet (the loading screen on entering a run waits a little longer based on this, see mtl_warm_wait)
static uint64_t g_warm_shaders, g_warm_rps, g_warm_cps, g_warm_skip_fz, g_warm_skip_stale;
static uint64_t g_warm_claim_shaders, g_warm_claim_rps, g_warm_claim_cps;
static id<MTLFunction> bf_fn(NvShader *fs, const int *fx, uint8_t fbmask, const uint8_t *lmap);
static int rt_range_bindings_ro(NSDictionary *j);
static FILE *g_pso_record; static int g_pso_record_checked;
static FILE *pso_record_file(void) {
    if (g_pso_record_checked) return g_pso_record;
    g_pso_record_checked = 1;
    const char *path = getenv("DM_PSO_RECORD");
    if (path && *path) g_pso_record = fopen(path, "a");
    return g_pso_record;
}
static void pso_record(const char *fmt, ...) {
    FILE *f = pso_record_file();
    if (!f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap); fflush(f);
}
// User manifest: shaders / pipelines created synchronously on the submit thread while running on this machine, warmed up at the next launch together with the seed manifest.
// Path = DM_PSO_USER, or pso_user.tsv under the parent of the save directory (DM_SAVE, Documents/save on iOS); tests use a fresh save copy per run, so they do not pollute each other
static NSString *pso_user_path(void) {
    const char *u = getenv("DM_PSO_USER"), *save = getenv("DM_SAVE");
    if (u && *u) return @(u);
    return save && *save ? [[@(save) stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"pso_user.tsv"] : nil;
}
static FILE *g_pso_user; static int g_pso_user_checked;
static NSMutableSet<NSString *> *g_pso_user_seen;
static FILE *pso_user_file(void) {
    if (g_pso_user_checked) return g_pso_user;
    g_pso_user_checked = 1;
    NSString *path = pso_user_path();
    if (path) g_pso_user = fopen(path.fileSystemRepresentation, "a");
    return g_pso_user;
}
static void pso_user_record(const char *fmt, ...) {
    FILE *f = pso_user_file();
    if (!f) return;
    char line[8192]; va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    if (!g_pso_user_seen) {
        g_pso_user_seen = [NSMutableSet set];
        NSString *seed = g_shmap_path ? [g_shmap_path.stringByDeletingLastPathComponent stringByAppendingPathComponent:@"pso_manifest.tsv"] : nil;
        NSMutableArray<NSString *> *paths = [NSMutableArray array]; if (seed) [paths addObject:seed]; NSString *user = pso_user_path(); if (user) [paths addObject:user];
        for (NSString *path in paths) { NSString *txt = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
            for (NSString *old in [txt componentsSeparatedByString:@"\n"]) if (old.length && ![old hasPrefix:@"#"]) [g_pso_user_seen addObject:old]; }
    }
    size_t ll = strlen(line); NSString *s = [[NSString alloc] initWithBytes:line length:ll && line[ll - 1] == '\n' ? ll - 1 : ll encoding:NSUTF8StringEncoding];   // Compared with the manifest file line by line (without the newline)
    if (!s || [g_pso_user_seen containsObject:s]) return;
    [g_pso_user_seen addObject:s]; fputs(line, f); fflush(f);
}
// Build the library from a .metallib file without permanently occupying a file descriptor: newLibraryWithURL keeps that file open for as long as the library object lives (one per game shader - of the 258 descriptors the iOS app permanently holds in a run, 213 are these,
// while the default soft limit of an iOS process is 256; new shaders created in other acts raise it further). Here the file is mapped into memory and closed immediately, and the library is built from the in-memory data; the mapping is released when the library's data object is freed
static __thread int t_lib_layer;
static id<MTLLibrary> lib_from_file(NSString *path, NSError **err) {
    int fd = open(path.fileSystemRepresentation, O_RDONLY); struct stat st;
    if (fd < 0 || fstat(fd, &st) || st.st_size <= 0) { if (fd >= 0) close(fd); if (err) *err = [NSError errorWithDomain:NSPOSIXErrorDomain code:errno userInfo:@{NSLocalizedDescriptionKey: [NSString stringWithFormat:@"cannot open %@", path]}]; return nil; }
    size_t len = (size_t)st.st_size; void *m = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0); close(fd);
    t_lib_layer = m != MAP_FAILED && memmem(m, len, "render_target_array_index", 25) != NULL;   // the library's functions use the layer: for a vertex library, its output (shader_load)
    if (m == MAP_FAILED) { if (err) *err = [NSError errorWithDomain:NSPOSIXErrorDomain code:errno userInfo:@{NSLocalizedDescriptionKey: [NSString stringWithFormat:@"mmap failed %@", path]}]; return nil; }
    dispatch_data_t d = dispatch_data_create(m, len, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ munmap(m, len); });
    return [g_dev newLibraryWithData:d error:err];
}
// NVN depth mode (nvnDeviceSetDepthMode): every vertex library has two entry points (dmsh_build.py vs_depth): main0 rewrites z for the GL convention (near = -w, NVN's default),
// main0_z leaves z as is (near = 0, which Link's Awakening, 13 Sentinels and Engage set). Loading picks by the mode the game set, so no game needs a setting for it
static _Atomic int g_depth_zero, g_vs_loaded;
void mtl_set_depth_zero(int on) {
    if (atomic_exchange(&g_depth_zero, on) == on) return;
    MLOG("depth mode: %s", on ? "near=0, vertex shaders use the main0_z entry" : "near=-w (GL), vertex shaders use the main0 entry");
    int n = atomic_load(&g_vs_loaded);
    if (n) MLOG("[RED] depth mode changed after %d vertex shaders were loaded: they keep the old convention", n);
}
// The Metal 4 function descriptor of each function pipelines are built from (pso_build): MTL4Compiler takes a library, an entry name and the constant values a function was
// specialized with, not an MTLFunction, so every function made from a library is noted here as it is made. A function with no note (the diagnostic fragment overrides
// DM_NVN_HILITE and DM_NVN_MAGSCAN) keeps the MTLDevice path
static os_unfair_lock g_fn4_lock = OS_UNFAIR_LOCK_INIT;
static NSMapTable *g_fn4;   // id<MTLFunction> → MTL4FunctionDescriptor
static id<MTLFunction> fn4_note(id<MTLFunction> f, id<MTLLibrary> lib, NSString *name, MTLFunctionConstantValues *cv) {
    if (!f || !lib || !name) return f;
    MTL4LibraryFunctionDescriptor *ld = [MTL4LibraryFunctionDescriptor new]; ld.library = lib; ld.name = name;
    MTL4FunctionDescriptor *d = ld;
    if (cv) { MTL4SpecializedFunctionDescriptor *sd = [MTL4SpecializedFunctionDescriptor new]; sd.functionDescriptor = ld; sd.constantValues = cv; d = sd; }
    os_unfair_lock_lock(&g_fn4_lock);
    if (!g_fn4) g_fn4 = [[NSMapTable alloc] initWithKeyOptions:NSPointerFunctionsStrongMemory | NSPointerFunctionsObjectPointerPersonality valueOptions:NSPointerFunctionsStrongMemory capacity:0];   // by identity: a function made again from the same library and name (other constants: bf_fn) is another function
    [g_fn4 setObject:d forKey:f];
    os_unfair_lock_unlock(&g_fn4_lock);
    return f;
}
static MTL4FunctionDescriptor *fn4_of(id<MTLFunction> f) {
    if (!f) return nil;
    os_unfair_lock_lock(&g_fn4_lock); MTL4FunctionDescriptor *d = [g_fn4 objectForKey:f]; os_unfair_lock_unlock(&g_fn4_lock);
    return d;
}
// layer: a vertex shader's layer entry points (entry_layer / entry_layer_z, which also write the layer) instead of main0 / main0_z, for a program whose geometry shader is a slice writer (prog_fold_gs)
static NvShader *shader_load(NSString *pc, int stage, int layer) {
    NSError *e = nil;
    NSData *jd = [NSData dataWithContentsOfFile:[NSString stringWithFormat:@"%@/%@.json", g_msl_dir, pc]];
    NSDictionary *j = jd ? [NSJSONSerialization JSONObjectWithData:jd options:0 error:nil] : nil;
    if (j && stage == 2 && [j[@"gs_slice"] isKindOfClass:[NSDictionary class]] && [j[@"chain"] isEqual:@DMSH_CHAIN]) {   // a slice-writing geometry shader has no library: a stub prog_fold_gs replaces
        NvShader *o = [NvShader new]; o->stage = 2; o->lay_loc = -1; strlcpy(o->key, pc.UTF8String, sizeof o->key);
        o->gs_loc = [j[@"gs_slice"][@"loc"] intValue]; o->gs_comp = [[j[@"gs_slice"][@"comp"] description] UTF8String][0];
        return o;
    }
    if (stage == 2 && !j) return nil;   // missing geometry translation: the caller's red line says so
    NSString *lp = [NSString stringWithFormat:@"%@/%@.metallib", g_msl_dir, pc];
    id<MTLLibrary> lib = lib_from_file(lp, &e);
    if (!lib || !j) { MLOG("[RED] failed to load shader %s: %s", pc.UTF8String, e ? e.localizedDescription.UTF8String : "no reflection json"); return nil; }
    if (![j[@"chain"] isEqual:@DMSH_CHAIN]) { MLOG("[RED] shader %s has translation chain version %s, but this host expects %s", pc.UTF8String, [[j[@"chain"] description] UTF8String], DMSH_CHAIN); return nil; }
    if (layer && !j[@"entry_layer"]) { MLOG("[RED] vertex shader %s has no layer entry points", pc.UTF8String); return nil; }
    NvShader *o = [NvShader new]; o->gs_loc = o->lay_loc = -1;
    if (stage == 0 && [j[@"layer_from"] isKindOfClass:[NSDictionary class]]) { o->lay_loc = [j[@"layer_from"][@"loc"] intValue]; o->lay_comp = [[j[@"layer_from"][@"comp"] description] UTF8String][0]; }
    char err[160];
    if (nvn_refl_from_json(j, &o->rf, err, sizeof err)) { MLOG("[RED] shader %s reflection conversion failed: %s", pc.UTF8String, err); return nil; }
    if (o->rf.dyn && !DM_GAME_TEX_DYN_SLOTS) {   // dynamic texture handles (TEXD / SAMPD) only where the profile says the game picks them from c2's tables, as before otherwise
        // (rf.c1 is freed by dealloc when o goes: freeing it here too was a double free, which aborted FINAL FANTASY RESONANCE at its first such shader)
        MLOG("[RED] shader %s reflection conversion failed: a texture handle the translator could not trace to one slot (profile key tex_dyn_slots off)", pc.UTF8String); return nil; }
    if (rt_range_on()) o->range_ro=rt_range_bindings_ro(j);
    NSString *entry = j[@"entry"];
    if (stage == 0) {
        atomic_fetch_add(&g_vs_loaded, 1);
        if (atomic_load(&g_depth_zero)) { if (j[@"entry_z"]) entry = j[@"entry_z"]; else RED_ONCE(85, "vertex shader %s has no near=0 entry (entry_z): depth stays in the GL convention", pc.UTF8String); }
        if (layer) entry = atomic_load(&g_depth_zero) ? j[@"entry_layer_z"] : j[@"entry_layer"];
    }
    if (j[@"entry_geometry_capture"]) {
        NSError *ge = nil; MTLFunctionConstantValues *gcv = [MTLFunctionConstantValues new];
        o->geo_fn = fn4_note([lib newFunctionWithName:j[@"entry_geometry_capture"] constantValues:gcv error:&ge], lib, j[@"entry_geometry_capture"], gcv);
        if (!o->geo_fn) MLOG("[RED] vertex shader %s: geometry capture entry failed: %s", pc.UTF8String, ge.localizedDescription.UTF8String);
    }
    NSDictionary *geo = j[@"geometry"];
    if (j[@"geometry_reader"]) o->geo_output = [j[@"geometry_output"] unsignedIntValue];
    if (geo) { o->geo_vertices = [geo[@"vertices"] unsignedIntValue]; o->geo_invocations = [geo[@"invocations"] unsignedIntValue];
        o->geo_output = [geo[@"output"] unsignedIntValue]; o->geo_stride = [geo[@"stride"] unsignedIntValue]; o->geo_index_stride = [geo[@"index_stride"] unsignedIntValue]; }
    o->fn = fn4_note([lib newFunctionWithName:entry], lib, entry, nil); o->stage = stage; strlcpy(o->key, pc.UTF8String, sizeof o->key); o->ventry = entry;
    // writes the layer (render_target_array_index, named in the library's metadata): its passes are layered and its pipelines get an input class (pso_desc). A library with layer entry points
    // names it for those alone: its main0 / main0_z write no layer
    if (stage == 0) o->layer_out = layer || (t_lib_layer && !j[@"entry_layer"]);
    if (stage == 0 && !layer) o->has_pull = [j[@"pull"] isKindOfClass:[NSDictionary class]];
    // A function that declares function constants cannot go into a pipeline until it is specialized, even when every constant is optional. SPIRV-Cross declares one for shaders that read a
    // texel buffer (spvLinearTextureAlignmentOverride, used only through is_function_constant_defined; one of Pokémon Legends: Z-A's imgui pixel shaders, 2bd5037003980716): an empty set of
    // values keeps its default. Without it the first pipeline with that shader failed Metal's validation (`fragmentFunction main0 cannot be used to build a pipeline state`)
    if (j[@"entry_tf"]) {   // transform feedback capture entry point (dmsh_build.py vs_tf) and the bytes per vertex of each feedback buffer
        o->tf_fn = fn4_note([lib newFunctionWithName:j[@"entry_tf"]], lib, j[@"entry_tf"], nil); NSArray *ts = j[@"tf_strides"];
        for (NSUInteger b = 0; b < 4 && b < ts.count; b++) o->tf_stride[b] = (uint16_t)[ts[b] unsignedIntValue];
        if (!o->tf_fn) MLOG("[RED] shader %s: no transform feedback entry point %s in its library", pc.UTF8String, [j[@"entry_tf"] UTF8String]);
    }
    if (stage == 1 && j[@"entry_u"]) { o->fn_u = fn4_note([lib newFunctionWithName:j[@"entry_u"]], lib, j[@"entry_u"], nil); if (!o->fn_u) MLOG("[RED] shader %s: no integer-output entry point %s in its library", pc.UTF8String, [j[@"entry_u"] UTF8String]); }
    if (stage == 1 && j[@"entry_m"]) { o->fn_m = fn4_note([lib newFunctionWithName:j[@"entry_m"]], lib, j[@"entry_m"], nil); o->mask_m = (uint8_t)[j[@"uint_mask_m"] unsignedIntValue];
        if (!o->fn_m) MLOG("[RED] shader %s: no mixed integer-output entry point %s in its library", pc.UTF8String, [j[@"entry_m"] UTF8String]); }
    if (o->fn.functionConstantsDictionary.count) {
        NSError *fe = nil; MTLFunctionConstantValues *cv = [MTLFunctionConstantValues new]; id<MTLFunction> f = fn4_note([lib newFunctionWithName:entry constantValues:cv error:&fe], lib, entry, cv);
        if (f) o->fn = f; else MLOG("[RED] shader %s: specializing its function constants failed: %s", pc.UTF8String, fe.localizedDescription.UTF8String);
        if (o->fn_u) { f = fn4_note([lib newFunctionWithName:j[@"entry_u"] constantValues:cv error:&fe], lib, j[@"entry_u"], cv); o->fn_u = f; }
        if (o->fn_m) { f = fn4_note([lib newFunctionWithName:j[@"entry_m"] constantValues:cv error:&fe], lib, j[@"entry_m"], cv); o->fn_m = f; }
    }
    if (o->rf.nb) {
        o->ae = [o->fn newArgumentEncoderWithBufferIndex:o->rf.abi];
        int max_aid = -1;
        for (int i = 0; i < o->rf.nb; i++) max_aid = MAX(max_aid, o->rf.b[i].aid + (o->rf.b[i].n ? o->rf.b[i].n - 1 : 0));   // an array takes n entries
        o->arg_len = o->ae.encodedLength;
        if (o->arg_len != (NSUInteger)(8 * (max_aid + 1)) || (o->arg_len & 7) || max_aid >= NVN_REFL_MAXARG)
            MDIE("shader %s has an unexpected argument buffer layout: encodedLength=%lu maxAid=%d", o->key, (unsigned long)o->arg_len, max_aid);
    }
    if (o->rf.has_c1) c1_store(o);
    NSString *ext = g_cb_ext[[pc hasSuffix:@"_b"] ? [pc substringToIndex:pc.length - 2] : pc];   // bytes each uniform slot can read (cb_len), storage slots never written (sb_ro)
    for (NSString *f in [ext componentsSeparatedByString:@" "]) { int r = 0, n = 0;
        if (sscanf(f.UTF8String, "%d:%d", &r, &n) == 2 && r >= 0 && r < 16 && n > 0) o->cb_ext[r] = (uint32_t)n;
        else if (sscanf(f.UTF8String, "s%d", &r) == 1 && r >= 0 && r < 32) o->sb_ro |= 1u << r; }
    return o;
}
// The part of uniform slot reg's binding the shader can read: its first cb_ext bytes when cb_extent.tsv bounds them, else the whole binding
static inline uint64_t cb_len(NvShader *sh, int reg, uint64_t size) { uint32_t e = sh && reg >= 0 && reg < 16 ? sh->cb_ext[reg] : 0; return e && e < size ? e : size; }

static int stage_of(NSString *stage) {
    if ([stage isEqualToString:@"vertex"]) return 0; if ([stage isEqualToString:@"pixel"]) return 1; if ([stage isEqualToString:@"compute"]) return 5;
    if ([stage isEqualToString:@"hull"]) return 3; if ([stage isEqualToString:@"domain"]) return 4; if ([stage isEqualToString:@"geometry"]) return 2; return -1;
}
#define NVN_RT_RANGE_RO_IMPL 1
// Private continuation accepts only B reflection that is explicitly RO; a missing rw on sbuf does not let the default 0 count as proof; does not change the existing reflection / translation chain.
static int rt_range_bindings_ro(NSDictionary *j) {
    if (![j[@"route"] isEqual:@"B"] || ![j[@"bindings"] isKindOfClass:[NSArray class]] ||
        ![j[@"drv_reads"] isKindOfClass:[NSDictionary class]] || [j[@"drv_reads"] count]) return 0;
    for (NSDictionary *b in j[@"bindings"]) {
        if (![b isKindOfClass:[NSDictionary class]] || ![b[@"kind"] isKindOfClass:[NSString class]] || ![b[@"msl_type"] isKindOfClass:[NSString class]]) return 0;
        NSString *k=b[@"kind"], *t=b[@"msl_type"];
        if (b[@"rw"] && (![b[@"rw"] isKindOfClass:[NSNumber class]] || [b[@"rw"] boolValue])) return 0;
        if ([k isEqual:@"cbuf"] || [k isEqual:@"c1"]) { if (![t hasPrefix:@"constant "]) return 0; }
        else if ([k isEqual:@"sbuf"]) { if (![b[@"rw"] isKindOfClass:[NSNumber class]] || [b[@"rw"] boolValue] || ![t hasPrefix:@"const device "]) return 0; }
        else if ([k isEqual:@"samp"] || [k isEqual:@"sampt"]) { if (![t isEqual:@"sampler"]) return 0; }
        else if ([k isEqual:@"tex"] || [k isEqual:@"tbuf"]) {
            if (![t hasPrefix:@"texture"] || ([t containsString:@"access::"] && ![t containsString:@"access::sample>"] && ![t containsString:@"access::read>"])) return 0; }
        else return 0;
    }
    return 1;
}
static int rt_range_pr_ro(NvProg *p) {
    if (!p || !p->sh[0] || !p->sh[1]) return 0;
    for (int k=0;k<6;k++) if (p->sh[k] && (k>1 || p->sh[k]->stage != k || !p->sh[k]->range_ro || p->sh[k]->rf.drv_read[0] || p->sh[k]->rf.drv_read[2])) return 0;
    return 1;
}
static NvShader *shader_get(NSString *pc, int stage) {
    if (!g_shcache) g_shcache = [NSMutableDictionary dictionary];
    NvShader *o = g_shcache[pc];
    if (o) return o;
    uint64_t bt = bp_now(); o = shader_load(pc, stage, 0);
    if (!o) return nil;
    g_shcache[pc] = o;
    pso_record("S\t%s\t%d\n", pc.UTF8String, stage);
    pso_user_record("S\t%s\t%d\n", pc.UTF8String, stage);
    g_bp_lib_n++; g_bp_lib_ns += bp_now() - bt;
    return o;
}
// The layer entry points of vertex shader vs as a shader of their own (cached beside it as <key>#l)
static NvShader *shader_get_layer(NvShader *vs) {
    NSString *pc = @(vs->key), *ck = [pc stringByAppendingString:@"#l"];
    NvShader *o = g_shcache[ck];
    if (!o && (o = shader_load(pc, 0, 1))) g_shcache[ck] = o;
    return o;
}
// A program whose geometry shader is a slice writer (a stub with no function, shader_load) draws with its vertex shader's layer entry points instead, which write the layer the geometry
// shader would have picked: the pass is layered (layer_out) and the geometry stage goes. Metal has no geometry stage. Unreal Engine binds each stage from a program of its own (DRAGON QUEST XI S:
// stage masks 1, 2, 4, 8, 16, an empty program for the stages a draw does not use), so a vertex shader is often combined with the previous draw's geometry shader for a moment before that
// stage is rebound: a stub that does not fit its vertex shader stays in the program, and only a draw with it gets a red line (draw_gs_check)
static void prog_fold_gs(NvProg *o) {
    NvShader *g = o->sh[2];
    if (!g || g->fn || !o->sh[0]) return;
    NvShader *v = o->sh[0], *l = v->lay_loc == g->gs_loc && v->lay_comp == g->gs_comp ? shader_get_layer(v) : nil;
    if (!l) return;
    o->sh[0] = l; o->sh[2] = nil;
    static int once; if (!once++) MLOG("geometry shader %s writes only the layer: vertex shader %s draws with its layer entry points instead (layered pass)", g->key, v->key);
}
// A draw whose geometry stage has no function (a slice writer that does not fit the vertex shader, prog_fold_gs) runs without it, reaching only the first layer, as before
static uint64_t g_gs_miss_draws;
static void draw_gs_check(NvProg *pr) {
    if (pr->gs_miss[0]) { g_gs_miss_draws++; static int m; if (m++ < 8) MLOG("[RED] draw with geometry shader %s, which has no library (vertex %s fragment %s)", pr->gs_miss, pr->sh[0] ? pr->sh[0]->key : "-", pr->sh[1] ? pr->sh[1]->key : "-");
        PD("G %llu draw with geometry shader %s, which has no library", g_stat_pass, pr->gs_miss); }
    NvShader *g = pr->sh[2]; static int n;
    if (g && !g->fn && n++ < 8) MLOG("[RED] geometry shader %s picks the layer from input %d.%c, but vertex shader %s has no layer entry points for it (layer_from %d.%c): the draw reaches only the first layer",
                                    g->key, g->gs_loc, g->gs_comp, pr->sh[0] ? pr->sh[0]->key : "-", pr->sh[0] ? pr->sh[0]->lay_loc : -1, pr->sh[0] && pr->sh[0]->lay_loc >= 0 ? pr->sh[0]->lay_comp : '-');
}
static id<MTLLibrary> g_builtin_lib;
static NSMutableDictionary<NSString *, NvShader *> *g_builtin;
static NvShader *builtin_get(const char *ctl) {
    for (unsigned i = 0; i < sizeof k_nvn_builtin_map / sizeof *k_nvn_builtin_map; i++) {
        if (strcmp(ctl, k_nvn_builtin_map[i].ctl)) continue;
        if (!g_builtin) g_builtin = [NSMutableDictionary dictionary];
        NvShader *o = g_builtin[@(ctl)];
        if (o) return o;
        if (!g_builtin_lib) {
            NSError *e = nil; g_builtin_lib = [g_dev newLibraryWithSource:@(k_nvn_builtin_msl) options:nil error:&e];
            if (!g_builtin_lib) MDIE("embedded compute kernel MSL compile failed: %s", e.localizedDescription.UTF8String);
        }
        o = [NvShader new]; o->fn = [g_builtin_lib newFunctionWithName:@(k_nvn_builtin_map[i].fn)]; o->stage = 5; o->builtin = k_nvn_builtin_map[i].fn;
        g_builtin[@(ctl)] = o;
        return o;
    }
    return nil;
}
static __unsafe_unretained NvProg *prog_get(const prog_t *cp) {   // Called only on the submit thread
    prog_t *p = (prog_t *)cp;
    static const prog_t *lp; static uint64_t lser, lgen; static NvProg *lpr;   // Last result: same object, same serial and unchanged serial table means it is used directly
    uint64_t gen = g_ser_gen;
    if (p == lp && p->serial == lser && gen == lgen && lpr) return lpr;
    __unsafe_unretained NvProg *have = p->serial ? serial_get(p->serial) : nil;
    if (have) { lp = p; lser = p->serial; lgen = gen; lpr = have; return have; }
    shmap_load();
    NvProg *o = [NvProg new];
    for (unsigned i = 0; i < p->nstage; i++) {
        // Key = sha256(first 0x880 bytes of the control section + code area): many of Mario's shaders share a control section but differ in code (dm/gfx/tools/shader_split.py). It was computed at SetShaders (hle_nvn.c): by now the game may have freed the control section
        uint8_t md[8]; prog_key(p, i, md);
        NSString *k = [NSString stringWithFormat:@"%02x%02x%02x%02x%02x%02x%02x%02x", md[0], md[1], md[2], md[3], md[4], md[5], md[6], md[7]];
        NSString *stage = g_shmap[k];
        if (!stage) {   // Not in the package: look up the compute kernels embedded in the main body (nvn_builtin.h)
            NvShader *b = builtin_get(k.UTF8String);
            if (b) { o->sh[5] = b; continue; }
            MLOG("[RED] shader %s is not in the shader map (DM_NVN_SHDUMP=directory saves its control section and code area)", k.UTF8String);
            // DM_NVN_SHDUMP=directory: shaders missing from the map are saved as <key>.ctl (0x880-byte control section) and <key>.code (code area, length from control +0x704), to trace their source / add them to the map offline
            static const char *shd; static int shd_init; if (!shd_init) { shd = getenv("DM_NVN_SHDUMP"); shd_init = 1; }
            if (shd) { char path[1024]; FILE *f; uint32_t ncode; memcpy(&ncode, (const uint8_t *)p->sd[i].control + 0x704, 4);   // with SHDUMP set the control section is the host's copy
                snprintf(path, sizeof path, "%s/%s.ctl", shd, k.UTF8String); if ((f = fopen(path, "wb"))) { fwrite(p->sd[i].control, 1, 0x880, f); fclose(f); }
                snprintf(path, sizeof path, "%s/%s.code", shd, k.UTF8String);
                if (p->sd[i].data && ncode && ncode < (1u << 24) && (f = fopen(path, "wb"))) { fwrite((const void *)(uintptr_t)p->sd[i].data, 1, ncode, f); fclose(f); } }
            continue;
        }
        int st = stage_of(stage);
        NvShader *sh = st >= 0 ? shader_get([k stringByAppendingString:@"_b"], st) : nil;
        if (sh) o->sh[st] = sh; else MLOG("[RED] shader %s (%s) failed to load: build the shaders with dm/gfx/tools/dmsh_build.py first", k.UTF8String, stage.UTF8String);
        if (!sh && st == 2) strlcpy(o->gs_miss, k.UTF8String, sizeof o->gs_miss);
    }
    prog_fold_gs(o);   // a slice-writing geometry shader: the vertex shader's layer entry points instead
    if (p->serial) serial_put(p->serial, o);
    return o;
}
void mtl_prog_release(prog_t *p) { if (p->serial) serial_del(p->serial); }

// ---------- Descriptor pool: texture ID / sampler ID → object ----------
// At registration the whole texture object is snapshotted (NVN semantics: the descriptor has been written into the pool); view: the MTLTexture for that ID (a view or the base texture, held via CFBridgingRetain) and the serial-table / guest-write generations at the time it was obtained
typedef struct { tex_t t; int ok; view_t v; int has_v; void *view; uint64_t view_id, view_rid, view_ser, view_dirty; int view_pkg;   // view_pkg: CPU-written (packaged); only those depend on guest writes (view_dirty)
                 void *alt, *alt_of; uint64_t alt_rid; uint8_t alt_type;   // alt: compatible view used when the shader's declared type differs from view (tex_fit), alt_of = the view it was built from
                 void *ar, *ar_of; uint64_t ar_rid;                         // ar: the alpha-from-red view for the profile's alpha_from_red shaders (tex_alpha_red), ar_of = the view it was built from
                 void *im, *im_of; uint64_t im_rid; } treg_t;              // im: the view an image slot reads in the shader's pixel class (img_fit), im_of = the view it was built from
static treg_t *g_treg; static int g_ntreg;
// Lock-free copy of texture ID → serial (the replay hot path only needs the serial: store lookahead index, binding set of the clear-only hoist): texture IDs are 20 bits, fixed 1M entries (BSS, only touched pages are used),
// release write at registration, relaxed atomic read on the reader side, without entering g_rmu (the former h_ser locked and unlocked on every lookup, thousands of times per frame in the camp)
static _Atomic uint64_t g_tser[1 << 20];
static samp_par *g_sreg; static uint8_t *g_sreg_ok; static void **g_sview; static uint64_t *g_srid; static int g_nsreg;   // g_sview: sampler ID → MTLSamplerState (retained)
// Memory pool Finalize: for texture IDs whose registered texture lives on this pool, drop the cached MTLTexture (view or base texture). The cache used to be released only when the ID was registered again,
// so a linear texture built directly on a pool kept the whole pool buffer alive (6 to 17 finalized 16 MB pool buffers stayed alive in Engage's title demo). If the ID is still used, the next lookup rebuilds it on the slow path
static void treg_drop_ar(treg_t *r) { if (r->ar) { defer_obj_unref((__bridge id)r->ar); CFRelease(r->ar); r->ar = r->ar_of = NULL; r->ar_rid = 0; }
                                     if (r->im) { defer_obj_unref((__bridge id)r->im); CFRelease(r->im); r->im = r->im_of = NULL; r->im_rid = 0; } }   // and the image class view (img_fit)
// Texture views by texture serial and view parameters, beside the per-ID view: KTGL (Age of Calamity) registers a texture into the next ID of a ring before every bind (about 21,000
// registrations a frame in battle after the first cutscene, 5 in 6 of them a different texture than the ID held), so the per-ID view was rebuilt for nearly every bind: half the
// registrations ended in a new Metal texture view, 45% of the replay's time. A hit is checked against the current base texture (re-uploads and format-view upgrades replace it).
// Only live textures are cached; entries go when the texture is finalized (mtl_tex_release) or its pool is (treg_drop_pool_views), and all of them past VC_MAX textures. Under g_rmu
typedef struct { uint32_t pf, l0, nl, s0, ns; uint8_t tt, sw[4], pad[3]; } vc_key;
enum { VC_WAYS = 4, VC_MAX = 16384 };
typedef struct { const void *pool; unsigned next; vc_key k[VC_WAYS]; void *view[VC_WAYS];   // view: retained
                 void *rows, *rows_of; uint64_t rows_rid, rows_n; } vc_set;   // rows: a texture buffer as 2D rows (tbuf_as_2d; retained, resident), rows_of / rows_n: the buffer texture it was made from and its element count
static nvn_hmap g_vc = {.klen = 8};   // texture serial → vc_set
static uint64_t g_vc_hit, g_vc_new;
static void vc_rows_drop(vc_set *s) { if (s->rows) { defer_obj_release((__bridge id)s->rows); CFRelease(s->rows); s->rows = s->rows_of = NULL; s->rows_rid = s->rows_n = 0; } }
static void vc_set_free(void *p) {
    vc_set *s = p;
    for (int i = 0; i < VC_WAYS; i++) if (s->view[i]) { defer_obj_unref((__bridge id)s->view[i]); CFRelease(s->view[i]); }
    vc_rows_drop(s);
    free(s);
}
static id<MTLTexture> vc_get(uint64_t ser, const vc_key *k, id<MTLTexture> base) {
    vc_set *s = nvn_hm_get(&g_vc, &ser); if (!s) return nil;
    for (int i = 0; i < VC_WAYS; i++) if (s->view[i] && !memcmp(&s->k[i], k, sizeof *k)) {
        id<MTLTexture> v = (__bridge id<MTLTexture>)s->view[i];
        if (v.parentTexture == base) { g_vc_hit++; return v; }
        defer_obj_unref(v); CFRelease(s->view[i]); s->view[i] = NULL; return nil;   // made from a base texture since replaced
    }
    return nil;
}
static vc_set *vc_set_for(uint64_t ser, const void *pool) {   // the texture's entry, made if needed; NULL for a texture already finalized (resolved through g_ghost): nothing would drop the entry
    pthread_mutex_lock(&g_smu); int live = nvn_hm_get(&g_tex_live, &ser) != NULL; pthread_mutex_unlock(&g_smu);
    if (!live) return NULL;
    vc_set *s = nvn_hm_get(&g_vc, &ser);
    if (!s) {
        if (g_vc.n >= VC_MAX) nvn_hm_clear(&g_vc, vc_set_free);
        if (!(s = calloc(1, sizeof *s))) return NULL;
        s->pool = pool; nvn_hm_put(&g_vc, &ser, s);
    }
    return s;
}
static void vc_put(uint64_t ser, const void *pool, const vc_key *k, id<MTLTexture> v) {
    vc_set *s = vc_set_for(ser, pool); if (!s) return;
    unsigned i = s->next++ % VC_WAYS;
    if (s->view[i]) { defer_obj_unref((__bridge id)s->view[i]); CFRelease(s->view[i]); }
    s->k[i] = *k; s->view[i] = (void *)CFBridgingRetain(v); g_vc_new++;
}
void mtl_vc_drop(uint64_t ser) { pthread_mutex_lock(&g_rmu); vc_set *s = nvn_hm_del(&g_vc, &ser); if (s) vc_set_free(s); pthread_mutex_unlock(&g_rmu); }
void mtl_vc_stats(unsigned *n, uint64_t *hit, uint64_t *made) { pthread_mutex_lock(&g_rmu); *n = g_vc.n; *hit = g_vc_hit; *made = g_vc_new; pthread_mutex_unlock(&g_rmu); }
static void vc_drop_pool(const void *pool) {   // caller holds g_rmu
    for (uint32_t i = 0; i < g_vc.cap;) {   // a deletion shifts later entries back into slot i: look at it again
        vc_set *s = g_vc.vals[i];
        if (s && s->pool == pool) { uint64_t ser; memcpy(&ser, g_vc.keys + (size_t)i * 8, 8); nvn_hm_del(&g_vc, &ser); vc_set_free(s); }
        else i++;
    }
}
// A texture buffer bound to a slot whose 1D integer fetches the translator made 2D reads of rows of NVN_TBUF_ROW texels (nvn_refl f1d; Program.cs RecordTexelFetch1D): a linear 2D texture of
// such rows over the same pool memory, so the shader reads the buffer. Maxwell reads 1D textures and texture buffers with the same instruction and the descriptor decides; without a type
// answer the translator read them as 2D, and Age of Calamity's light and decal lists (and Three Houses' lighting) read the dummy texture, zeros. Kept with the texture's views (vc_set),
// checked against the buffer texture it was made from. Caller holds g_rmu
static uint64_t g_tb2_binds, g_tb2_made, g_tb2_fail;
static id<MTLTexture> tbuf_as_2d(int tid, id<MTLTexture> t, uint64_t *rid) {
    if (tid <= 0 || tid >= g_ntreg || !g_treg[tid].ok) return nil;
    const tex_t *tt = &g_treg[tid].t; uint64_t ser = tt->serial;
    vc_set *s = nvn_hm_get(&g_vc, &ser);
    if (s && s->rows && s->rows_of == (__bridge void *)t) {   // rows_of is not retained: a texture made since at the same address must also match the buffer the rows hold, their offset, format and element count
        id<MTLTexture> rv = (__bridge id<MTLTexture>)s->rows;
        if (rv.buffer == t.buffer && rv.bufferOffset == t.bufferOffset && rv.pixelFormat == t.pixelFormat && s->rows_n == t.width) { g_tb2_binds++; *rid = s->rows_rid; return rv; }
    }
    id<MTLBuffer> buf = t.buffer; int bw, bh; uint32_t bpb = (uint32_t)nvn_fmt_bpb(tt->d.format, &bw, &bh), w, h, row;
    NSUInteger al = [g_dev minimumLinearTextureAlignmentForPixelFormat:t.pixelFormat];
    if (!buf || bw != 1 || bh != 1 || t.bufferOffset % al || !nvn_tbuf_rows(t.width, bpb, (uint32_t)al, &w, &h, &row)) {
        if (!g_tb2_fail++) MLOG("[RED] texture buffer (texture ID %d, %lu elements of format %#x at pool offset %lu) has no 2D-row form for a wrapped 1D fetch: the slot reads 0", tid, (unsigned long)t.width, tt->d.format, (unsigned long)t.bufferOffset);
        return nil;
    }
    while (h > 1 && t.bufferOffset + (uint64_t)row * h > buf.length) h--;   // a last row running past the pool buffer: reads beyond it are undefined anyway
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:t.pixelFormat width:w height:h mipmapped:NO];
    d.resourceOptions = buf.resourceOptions; d.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> v = [buf newTextureWithDescriptor:d offset:t.bufferOffset bytesPerRow:row];
    if (!v) { if (!g_tb2_fail++) MLOG("[RED] cannot create the 2D-row form of a texture buffer: %ux%u format %lu row %u bytes at pool offset %lu", w, h, (unsigned long)t.pixelFormat, row, (unsigned long)t.bufferOffset); return nil; }
    if (!g_tb2_made++) MLOG("texture buffer read by wrapped 1D fetches: bound as %ux%u rows of %u bytes (texture ID %d, %lu elements; first case)", w, h, row, tid, (unsigned long)t.width);
    res_add(v);
    *rid = v.gpuResourceID._impl; g_tb2_binds++;
    if ((s = vc_set_for(ser, tt->pool))) { vc_rows_drop(s); s->rows = (void *)CFBridgingRetain(v); s->rows_of = (__bridge void *)t; s->rows_rid = *rid; s->rows_n = t.width; }
    else defer_obj_release(v);   // a finalized texture's last reads: released once this submission is done
    return v;
}
void mtl_tb2_stats(uint64_t *binds, uint64_t *made, uint64_t *fail) { *binds = g_tb2_binds; *made = g_tb2_made; *fail = g_tb2_fail; }
static void treg_drop_pool_views(const void *pool) {
    pthread_mutex_lock(&g_rmu);
    vc_drop_pool(pool);
    for (int i = 0; i < g_ntreg; i++)
        if (g_treg[i].view && g_treg[i].ok && (const void *)g_treg[i].t.pool == pool) {
            defer_obj_unref((__bridge id)g_treg[i].view); CFRelease(g_treg[i].view); g_treg[i].view = NULL; g_treg[i].view_id = g_treg[i].view_rid = 0; treg_drop_ar(&g_treg[i]); }
    pthread_mutex_unlock(&g_rmu);
}
void mtl_reg_tex(int tid, const tex_t *t, const view_t *v) {
    int id_ = tid; if (id_ < 0) return;
    pthread_mutex_lock(&g_rmu);
    if (id_ >= g_ntreg) { int n = id_ + 1024; g_treg = realloc(g_treg, n * sizeof *g_treg); memset(g_treg + g_ntreg, 0, (n - g_ntreg) * sizeof *g_treg); g_ntreg = n; }
    // Registering what the ID already holds changes nothing: its view and the registration generation stay (the lookup still checks the base texture). About 1 in 6 of the
    // registrations KTGL (Age of Calamity) makes before every bind repeat the ID's texture; the rest cycle textures through its IDs (the view cache below, vc_get)
    treg_t *rr = &g_treg[id_];
    int same = t && rr->ok && rr->has_v == (v != NULL) && !memcmp(&rr->t, t, sizeof *t) && (!v || !memcmp(&rr->v, v, sizeof *v));
    if (!same) {
        g_reg_gen++;
        if (rr->view) { defer_obj_unref((__bridge id)rr->view); CFRelease(rr->view); rr->view = NULL; rr->view_id = rr->view_rid = 0; }
        if (rr->alt) { defer_obj_unref((__bridge id)rr->alt); CFRelease(rr->alt); rr->alt = rr->alt_of = NULL; rr->alt_rid = 0; }
        treg_drop_ar(rr);
        rr->ok = t != NULL; if (t) rr->t = *t; rr->has_v = v != NULL; if (v) rr->v = *v;
        if (id_ < (1 << 20)) atomic_store_explicit(&g_tser[id_], t ? t->serial : 0, memory_order_release);
    }
    pthread_mutex_unlock(&g_rmu);
    {   // DM_NVN_TREG_ID=n logs every registration of texture ID n: what it holds from then on, and when (the frame being recorded)
        static int want = -2; if (want == -2) { const char *e = getenv("DM_NVN_TREG_ID"); want = e ? atoi(e) : -1; }
        if (want == tid) MLOG("TREG id %d frame %llu %s ser=%llu %ux%u format=%#x target %d view %d", tid, (unsigned long long)atomic_load(&g_cur_frame), same ? "(the same)" : "→",
                              t ? (unsigned long long)t->serial : 0ull, t ? t->d.width : 0, t ? t->d.height : 0, t ? t->d.format : 0, t ? (int)t->d.target : -1, v != NULL);
    }
    {   // DM_NVN_TEXINFO=WxH also logs registrations of textures of that size: the channel swizzle from the builder and from the view
        static int ti = -1; static uint32_t tw, th; static uint64_t n;
        if (ti < 0) { const char *e = getenv("DM_NVN_TEXINFO"); ti = e && sscanf(e, "%ux%u", &tw, &th) == 2; }
        if (ti && t && t->d.width == tw && t->d.height == th && (n++ < 40 || n % 600 == 0))
            MLOG("TEXINFO register id %d ser=%llu format=%#x creation swizzle %d:%d%d%d%d view %d view swizzle %d:%d%d%d%d view format %d:%#x", tid, (unsigned long long)t->serial, t->d.format,
                 t->set_swz, t->swz & 7, t->swz >> 3 & 7, t->swz >> 6 & 7, t->swz >> 9 & 7, v != NULL, v ? v->set_swizzle : 0, v ? v->swz[0] : 0, v ? v->swz[1] : 0, v ? v->swz[2] : 0, v ? v->swz[3] : 0,
                 v ? v->set_format : 0, v ? v->format : 0);
    }
}
void mtl_reg_samp(int sid, const samp_par *sp) {
    int id_ = sid; if (id_ < 0) return;
    pthread_mutex_lock(&g_rmu);
    if (id_ >= g_nsreg) { int n = id_ + 256; g_sreg = realloc(g_sreg, n * sizeof *g_sreg); g_sreg_ok = realloc(g_sreg_ok, n); g_sview = realloc(g_sview, n * sizeof *g_sview); g_srid = realloc(g_srid, n * sizeof *g_srid);
        memset(g_sreg_ok + g_nsreg, 0, n - g_nsreg); memset(g_sview + g_nsreg, 0, (n - g_nsreg) * sizeof *g_sview); memset(g_srid + g_nsreg, 0, (n - g_nsreg) * sizeof *g_srid); g_nsreg = n; }
    if (g_sview[id_]) { CFRelease(g_sview[id_]); g_sview[id_] = NULL; g_srid[id_] = 0; }
    g_reg_gen++;
    g_sreg[id_] = *sp; g_sreg_ok[id_] = 1;
    pthread_mutex_unlock(&g_rmu);
}
// Handles of deferred binds (BindTexturesDeferred) are read at execution: the record keeps the address of the handles (see hle_nvn.c); every reader of a bind record refreshes it first. Command memory is writable, so it is updated in place
static inline void bh_fresh(const p_bind_handles *b) {
    if (b->defer && b->src && b->n > 0 && b->n <= 32) memcpy((void *)b->h, (const void *)(uintptr_t)b->src, 8 * (size_t)b->n);
}
static MTLTextureType ttype(int target, int fallback) {
    switch (target) { case 0: return MTLTextureType2D; case 1: case 7: return MTLTextureType2D; case 2: return MTLTextureType3D;   // 1D as one-row 2D (see where textures are created)
        case 3: return MTLTextureType2DArray; case 4: return MTLTextureType2DArray; case 5: return MTLTextureType2DMultisample;
        case 6: return MTLTextureType2DMultisampleArray; case 8: return MTLTextureTypeCube; case 9: return MTLTextureTypeCubeArray; }
    return (MTLTextureType)fallback;
}
// Out-of-pool texture ID = empty slot: the game itself uses GetSeparateTextureHandle(0xFFFFF) to get a handle and bind it to unused slots (measured call site main+0x1746820,
// texture pool capacity 15256) and never registers that ID. Read 0 per D3D / empty-slot semantics (the same all-zero dummy textures as unbound), no failure; unregistered IDs inside the pool are still reported a failure
static int g_tpool_n; static uint64_t g_stat_tex_oob;
// NVN window origin. In lower-left mode (GL convention, NVN's default) NDC y = -1 lands on a render target's first memory row and window y grows with the row; Metal puts NDC y = +1 on the first row.
// Viewport and scissor rectangles, fragment coordinates, copies and texture coordinates all address the same memory rows in both conventions, so only the direction inside the viewport differs:
// lower-left draws get a viewport of negative height (and the opposite front-face winding), and the window image, stored bottom-up, is flipped at present (and in raw screenshots).
// Link's Awakening keeps the default: read as upper-left, every texture it renders and samples came out consistently flipped and cancelled out, except where the game compensates for the
// convention itself (text rendered into textures for the title and the menus came out upside down, and a MIRROR_Y copy of the frame)
static _Atomic int g_origin_ll;
void mtl_set_origin_lower_left(int on) { if (atomic_exchange(&g_origin_ll, on) != on) MLOG("window origin: %s", on ? "lower left (GL: viewport flipped, winding reversed, present flipped)" : "upper left"); }
void mtl_set_tex_pool_n(int n) { g_tpool_n = n; }
static id<MTLTexture> tex_by_id(int tid) {   // Caller holds g_rmu
    if (g_tpool_n && tid >= g_tpool_n) { if (!g_stat_tex_oob++) MLOG("texture ID %d outside the pool (capacity %d) treated as an empty slot: reads 0", tid, g_tpool_n); return nil; }
    if (tid < 0 || tid >= g_ntreg || !g_treg[tid].ok) { RED_ONCE(10, "texture ID %d not registered", tid); return nil; }
    treg_t *rp = &g_treg[tid];
    uint64_t sg = g_ser_gen, dg = nvn_dirty_now();   // Take the generations first and then look up: if a change happens in between, the next time must take the slow path
    if (rp->view && rp->view_ser == sg && (!rp->view_pkg || rp->view_dirty == dg)) return (__bridge id<MTLTexture>)rp->view;
    id<MTLTexture> v = (__bridge id<MTLTexture>)rp->view;
    // Guest writes (nvn_dirty_now) matter only to CPU-written textures: a texture that becomes CPU-written later bumps g_ser_gen once (mtl_tex_cpu_written), which sends its ID through here again.
    // Before, every guest write to any protected page sent every texture ID, render targets included, down this path
    rp->view_pkg = (rp->t.has_layout || tex_cpu_written(rp->t.serial)) && rp->t.mem;
    if (v) {   // The serial table or guest memory has changed: check whether the base texture was re-uploaded and replaced; if so the view is invalidated
        id<MTLTexture> cur = tex_mtl(&rp->t);
        if (cur == v || v.parentTexture == cur) { rp->view_ser = sg; rp->view_dirty = dg; return v; }
        defer_obj_unref(v); CFRelease(rp->view); rp->view = NULL; rp->view_rid = 0; v = nil;
    }
    treg_t r = *rp;
    id<MTLTexture> base = tex_mtl(&rp->t);
    if (!base) return nil;
    v = base;
    // Channel swizzle: the view's if set, otherwise the one from texture building; identity builds no view (SMBW's font cache is (1 1 1 R): without the swizzle glyphs become white blocks).
    // Its channels name components of the NVN format, which the format's own mapping finds in Metal's (nvn_fmt_to_mtl: alpha 1 for RGBX and the like, red and blue swapped for the RGB565 family),
    // the view format's if Metal has it, else the texture's (GitHub PR #66: The Legend of Zelda: Echoes of Wisdom's hearts are an RGB565 mask, drawn as red rectangles without the swap).
    // The swap is for bits in the guest's order, so only for textures uploaded from guest memory or read from it in place: what Metal rendered or cleared into one has red where Metal puts it
    uint8_t sw[4] = {MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue, MTLTextureSwizzleAlpha}, fsw[4]; int need_sw = 0; uint32_t fpf = 0;
    if (!(r.has_v && r.v.set_format && nvn_fmt_to_mtl(r.v.format, &fpf, fsw) && fpf)) nvn_fmt_to_mtl(r.t.d.format, NULL, fsw);
    if (fsw[0] != MTLTextureSwizzleRed && !base.buffer && ![tex_get(r.t.serial) isKindOfClass:[NvPkg class]]) { fsw[0] = MTLTextureSwizzleRed; fsw[2] = MTLTextureSwizzleBlue; }
    if (r.has_v && r.v.set_swizzle) memcpy(sw, r.v.swz, 4);
    else if (r.t.set_swz) for (int c = 0; c < 4; c++) sw[c] = (uint8_t)(r.t.swz >> (3 * c) & 7);
    for (int c = 0; c < 4; c++) {
        if (sw[c] > MTLTextureSwizzleAlpha) sw[c] = (uint8_t)(MTLTextureSwizzleRed + c);
        if (sw[c] >= MTLTextureSwizzleRed) sw[c] = fsw[sw[c] - MTLTextureSwizzleRed];
        if (sw[c] != MTLTextureSwizzleRed + c) need_sw = 1;
    }
    // Stencil mode (nvnTextureViewSetDepthStencilMode STENCIL): the view reads the stencil of a depth-stencil texture, through Metal's stencil-only format. The shader declares a float texture and
    // takes the raw bits (the translator has no integer texture type): a read of the integer view gives the stencil value in those bits. Pokémon Legends: Z-A's deferred lighting tests stencil bit 6
    // this way and discards; reading depth instead, the bit followed the depth value and the whole lit picture was striped
    // A texture built in stencil mode (nvnTextureBuilderSetDepthStencilMode, Unreal Engine 4.27's stencil shader resources) reads stencil the same way unless a view sets the mode
    int want_st = r.has_v && r.v.set_dsmode ? r.v.dsmode == NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL : r.t.stencil;
    int stv = want_st && base.pixelFormat == MTLPixelFormatDepth32Float_Stencil8;
    if (want_st && !stv)
        RED_ONCE(104, "stencil-mode texture view of Metal format %lu (texture ID %d): no stencil-only view, depth is read", (unsigned long)base.pixelFormat, tid);
    if ((r.has_v && (r.v.set_format || r.v.set_levels || r.v.set_layers || r.v.set_target)) || need_sw || stv) {
        if (!r.has_v) memset(&r.v, 0, sizeof r.v);
        uint32_t pf = (uint32_t)base.pixelFormat; uint8_t swz[4];
        if (r.v.set_format && !(nvn_fmt_to_mtl(r.v.format, &pf, swz) && pf)) pf = (uint32_t)base.pixelFormat;
        if (stv) pf = MTLPixelFormatX32_Stencil8;
        if (r.v.set_format && nvn_fmt_bc(rp->t.d.format) && !nvn_fmt_bc(r.v.format)) {   // the raw blocks of a BC texture through an uncompressed view (see rt_view_fmt): Metal has no such view
            RED_ONCE(86, "Metal cannot create the raw-block view of a compressed texture (NVN format %#x → view %#x): treated as an empty slot", rp->t.d.format, r.v.format);
            return nil;
        }
        if (r.v.set_format && tex_need_pfv(base.pixelFormat, (MTLPixelFormat)pf) && !(base.usage & MTLTextureUsagePixelFormatView)) {
            base = tex_upgrade_pfv(&rp->t, base, (MTLPixelFormat)pf);
            sg = g_ser_gen;
        }
        NSUInteger l0 = r.v.set_levels ? r.v.base_level : 0, nl = r.v.set_levels && r.v.nlevels ? r.v.nlevels : base.mipmapLevelCount - l0;
        if (l0 >= base.mipmapLevelCount) { RED_ONCE(109, "texture view id %d starts at mip %lu beyond %lu levels", tid, (unsigned long)l0, (unsigned long)base.mipmapLevelCount); return nil; }
        // A level count past the texture's levels is cut to those it has, as deko3d's Tegra descriptor writer clips the maximum level (Echoes of Wisdom: levels 1 + 7 of a seven-level texture,
        // which Metal's validation refuses)
        if (nl > base.mipmapLevelCount - l0) nl = base.mipmapLevelCount - l0;
        NSUInteger s0 = r.v.set_layers ? r.v.min_layer : 0;
        NSUInteger ns = base.textureType == MTLTextureType3D ? 1 : (r.v.set_layers && r.v.nlayers ? r.v.nlayers : base.arrayLength * (base.textureType == MTLTextureTypeCube || base.textureType == MTLTextureTypeCubeArray ? 6 : 1) - s0);
        MTLTextureType tt = r.v.set_target ? ttype(r.v.target, base.textureType) : base.textureType;
        if (tt == MTLTextureType2D && ns > 1) ns = 1;
        // A cube view whose layer range is not whole cubes of faces counts its layers in cubes, as the hardware's texture header does for cube maps (Pokémon Sword's environment maps:
        // 6-layer 2D array textures sampled through views with target CUBEMAP and layers 0 + 1, as the outdoors loads). Metal counts faces, and its validation aborts on a cube view of 1 slice
        if ((tt == MTLTextureTypeCube || tt == MTLTextureTypeCubeArray) && ns % 6) {
            NSUInteger total = base.textureType == MTLTextureType3D ? 1 : base.arrayLength * (base.textureType == MTLTextureTypeCube || base.textureType == MTLTextureTypeCubeArray ? 6 : 1);
            int fits = (s0 + ns) * 6 <= total;
            static int noted; if (fits && !noted++) MLOG("[nvn-mtl] cube texture view of %u + %u layers (texture ID %d, %lu faces): counted in cubes, as the hardware does", (unsigned)s0, (unsigned)ns, tid, (unsigned long)total);
            if (!fits) RED_ONCE(110, "cube texture view of %u + %u layers past the %lu faces of texture ID %d: read as a 2D array", (unsigned)s0, (unsigned)ns, (unsigned long)total, tid);
            if (fits) { s0 *= 6; ns *= 6; } else tt = MTLTextureType2DArray;
        }
        vc_key k; memset(&k, 0, sizeof k); k.pf = pf; k.tt = (uint8_t)tt; k.l0 = (uint32_t)l0; k.nl = (uint32_t)nl; k.s0 = (uint32_t)s0; k.ns = (uint32_t)ns; memcpy(k.sw, sw, 4);
        if (!(v = vc_get(rp->t.serial, &k, base))) {
            v = need_sw ? [base newTextureViewWithPixelFormat:(MTLPixelFormat)pf textureType:tt levels:NSMakeRange(l0, nl) slices:NSMakeRange(s0, ns) swizzle:MTLTextureSwizzleChannelsMake(sw[0], sw[1], sw[2], sw[3])]
                        : [base newTextureViewWithPixelFormat:(MTLPixelFormat)pf textureType:tt levels:NSMakeRange(l0, nl) slices:NSMakeRange(s0, ns)];
            if (!v) { RED_ONCE(11, "cannot create texture view id %d", tid); v = base; }
            else vc_put(rp->t.serial, rp->t.pool, &k, v);
        }
    }
    rp->view = (void *)CFBridgingRetain(v); rp->view_rid = v.gpuResourceID._impl; rp->view_ser = sg; rp->view_dirty = dg;
    return v;
}
// Hot-path object fetch: if the table entry is valid, return the table-held pointer directly (without ARC retain / autorelease), otherwise take the slow path to build / validate. Caller holds g_rmu.
static inline __attribute__((always_inline)) void *tex_fast(int tid) {
    if (tid > 0 && tid < g_ntreg && g_treg[tid].ok && g_treg[tid].view && g_treg[tid].view_ser == g_ser_gen && (!g_treg[tid].view_pkg || g_treg[tid].view_dirty == nvn_dirty_now())) return g_treg[tid].view;
    return (__bridge void *)tex_by_id(tid);
}
// The texture type the shader declares differs from the one the descriptor gives: use a compatible view. Maxwell sample instructions carry their own dimension, and a non-array instruction reading a 2D array texture reads layer 0;
// on Metal a 2D array bound to a texture2d argument is a type mismatch with undefined reads (measured: one fixed value). Super Mario 3D World + Bowser's Fury creates its pre-lighting buffer as a one-layer 2D array
// and the deferred lighting shader samples it as 2D: the ambient light of the whole picture became one colour, washed out white. Supports 2D array <-> 2D and 2D array -> cube / cube array; any other mismatch is reported once. Caller holds g_rmu
static id<MTLTexture> dummy_tex(int dtt, int dpf); static uint64_t g_dummy_tex_rid[8][4];
static id<MTLTexture> tex_fit(int tid, id<MTLTexture> t, MTLTextureType want, uint64_t *rid) {
    MTLTextureType have = t.textureType; NSUInteger ns = 0;
    if (want == MTLTextureType2D && have == MTLTextureType2DArray) ns = 1;
    else if (want == MTLTextureType2DArray && have == MTLTextureType2D) ns = 1;
    else if (want == MTLTextureTypeCube && have == MTLTextureType2DArray && t.arrayLength >= 6) ns = 6;
    else if (want == MTLTextureTypeCubeArray && have == MTLTextureType2DArray && t.arrayLength >= 6) ns = t.arrayLength / 6 * 6;
    else if (want == MTLTextureTypeCube && have == MTLTextureTypeCubeArray) ns = 6;   // a cube array read as a cube: the first cube
    // A one-layer 2D texture in a cube slot has no cube view (Pokémon Legends: Z-A binds one to a cube sampler on its character creation screen, a placeholder for an environment map): the zero cube,
    // a defined read, instead of a type mismatch whose reads are undefined. The same for a 2D array of fewer than 6 layers, which is how the host holds some 2D textures (Mario Kart 8 Deluxe binds
    // an NVN 2D texture, format 0x44, at slot 10 of two pixel shaders that declare a cube there, as a race loads)
    if (!ns && (want == MTLTextureTypeCube || want == MTLTextureTypeCubeArray) && (have == MTLTextureType2D || (have == MTLTextureType2DArray && t.arrayLength < 6))) {
        RED_ONCE(103, "texture type mismatch: shader declares a cube, texture is 2D (texture ID %d); the zero cube is bound", tid);
        id<MTLTexture> z = dummy_tex((int)want, NVN_DPF_FLOAT); *rid = g_dummy_tex_rid[want & 7][NVN_DPF_FLOAT]; return z; }
    if (!ns || tid <= 0 || tid >= g_ntreg) return nil;   // no view bridges it: the caller reports it (tex_mis_note)
    treg_t *r = &g_treg[tid];
    if (r->alt && r->alt_of == (__bridge void *)t && r->alt_type == (uint8_t)want) { *rid = r->alt_rid; return (__bridge id<MTLTexture>)r->alt; }
    if (r->alt) { defer_obj_unref((__bridge id)r->alt); CFRelease(r->alt); r->alt = r->alt_of = NULL; r->alt_rid = 0; }
    id<MTLTexture> v = [t newTextureViewWithPixelFormat:t.pixelFormat textureType:want levels:NSMakeRange(0, t.mipmapLevelCount) slices:NSMakeRange(0, ns) swizzle:t.swizzle];
    if (!v) { RED_ONCE(77, "cannot create a type-compatible texture view: %lu → %lu (texture ID %d)", (unsigned long)have, (unsigned long)want, tid); return nil; }
    r->alt = (void *)CFBridgingRetain(v); r->alt_of = (__bridge void *)t; r->alt_type = (uint8_t)want; r->alt_rid = v.gpuResourceID._impl;
    *rid = r->alt_rid; return v;
}
// tex_fit for an element of a handle array (TEXD, TEXU arrays): every element is declared as the type of the one the shader picks, and the others hold whatever the slots or handles do.
// A 2D texture in a cube array is one the shader does not pick there (Paper Mario: The Thousand-Year Door's cube samples pick slot 13 or 28, beside 2D textures in the others): nil, so the
// caller binds the zero cube as tex_fit would, without the type mismatch report
static id<MTLTexture> tex_fit_elem(int tid, id<MTLTexture> t, MTLTextureType want, uint64_t *rid) {
    if ((want == MTLTextureTypeCube || want == MTLTextureTypeCubeArray) && (t.textureType == MTLTextureType2D || (t.textureType == MTLTextureType2DArray && t.arrayLength < 6))) return nil;
    return tex_fit(tid, t, want, rid);
}
// A texture type mismatch no view bridges, reported once per shader and texture slot with what is bound: one report per run used to hide that Hyrule Warriors: Age of Calamity has
// thousands of shaders reading its light and decal lists, buffer textures, through slots translated as 2D. For a buffer texture: its element count and how far its offset in the
// pool buffer is from the alignment a linear texture there needs. The total goes on the memory stats line. Caller holds g_rmu
static uint64_t g_tex_mis_n, g_tex_mis_shown;
uint64_t mtl_tex_mis_count(void) { return g_tex_mis_n; }
static void tex_mis_note(NvShader *sh, int stage, int slot, int tid, id<MTLTexture> t, MTLTextureType want, const char *outcome) {
    g_tex_mis_n++;
    if (slot < 0 || slot >= 256 || (sh->mis[slot >> 6] >> (slot & 63) & 1)) return;
    sh->mis[slot >> 6] |= 1ull << (slot & 63);
    if (g_tex_mis_shown++ >= 64) return;
    const tex_t *tt = tid > 0 && tid < g_ntreg && g_treg[tid].ok ? &g_treg[tid].t : NULL;
    char bt[96] = "";
    if (t.textureType == MTLTextureTypeTextureBuffer) snprintf(bt, sizeof bt, ", %lu elements at pool offset %lu, %lu past the linear-texture alignment", (unsigned long)t.width, (unsigned long)t.bufferOffset,
                                                                (unsigned long)(t.bufferOffset % [g_dev minimumLinearTextureAlignmentForPixelFormat:t.pixelFormat]));
    MLOG("[RED] texture type mismatch: shader %s (stage %d) declares slot %d as Metal type %lu, texture ID %d is type %lu (NVN target %d, format %#x, Metal format %lu%s); %s", sh->key, stage, slot,
         (unsigned long)want, tid, (unsigned long)t.textureType, tt ? (int)tt->d.target : -1, tt ? (unsigned)tt->d.format : 0u, (unsigned long)t.pixelFormat, bt, outcome);
}
// Alpha from red: the fragment shaders the game profile's alpha_from_red lists read a single-channel render target whose channel mapping is exactly (R, 0, 0, 1) as (R, 0, 0, R).
// A workaround for NintendoWare layout captures (BotW, Odyssey): the game rebuilds each capture on the same storage with its own mapping ((1, 1, 1, R) for an A8 capture), which was lost while
// nvnTextureCompare ignored the mapping and left the creation-time (R, 0, 0, 1), alpha always 1 (hard-edged prompt boxes, white coin cards). Since that fix it no longer fires (docs/agents/04-graphics.md).
// Only render targets qualify (captures always are; asset and CPU-written textures are not). DM_NVN_ALPHA_RED=0 turns it off, <keys,...> replaces the list. Caller holds g_rmu
static char g_afr_keys[48][20]; static int g_afr_nk = -1;
static int afr_want(NvShader *sh) {
    if (!sh->afr) {
        if (g_afr_nk < 0) { g_afr_nk = 0; const char *e = getenv("DM_NVN_ALPHA_RED"), *src = e && *e ? e : DM_GAME_ALPHA_FROM_RED;
            if (strcmp(src, "0")) for (const char *q = src; *q && g_afr_nk < 48;) { size_t n = strcspn(q, ", "); if (n) { if (n > 16) n = 16; memcpy(g_afr_keys[g_afr_nk], q, n); g_afr_keys[g_afr_nk++][n] = 0; } q += n; q += strspn(q, ", "); }
            if (g_afr_nk) MLOG("single-channel texture alpha from red: %s%s", src, e && *e ? " (DM_NVN_ALPHA_RED)" : " (profile key alpha_from_red)"); }
        sh->afr = 1; for (int i = 0; i < g_afr_nk; i++) if (!strncmp(sh->key, g_afr_keys[i], strlen(g_afr_keys[i]))) sh->afr = 2;
    }
    return sh->afr == 2;
}
static id<MTLTexture> tex_alpha_red(int tid, id<MTLTexture> t, uint64_t *rid) {
    MTLPixelFormat pf = t.pixelFormat; MTLTextureSwizzleChannels sw = t.swizzle;
    if (pf != MTLPixelFormatR8Unorm && pf != MTLPixelFormatR16Unorm && pf != MTLPixelFormatR16Float && pf != MTLPixelFormatR32Float) return nil;
    if (sw.red != MTLTextureSwizzleRed || sw.green != MTLTextureSwizzleZero || sw.blue != MTLTextureSwizzleZero || sw.alpha != MTLTextureSwizzleOne || tid <= 0 || tid >= g_ntreg) return nil;
    id<MTLTexture> base = t.parentTexture ? t.parentTexture : t;
    if (!(base.usage & MTLTextureUsageRenderTarget)) return nil;   // packaged and CPU-written textures are built without render-target usage
    treg_t *r = &g_treg[tid];
    if (r->ar && r->ar_of == (__bridge void *)t) { *rid = r->ar_rid; return (__bridge id<MTLTexture>)r->ar; }
    treg_drop_ar(r);
    NSUInteger l0 = t.parentTexture ? t.parentRelativeLevel : 0, s0 = t.parentTexture ? t.parentRelativeSlice : 0;
    NSUInteger ns = t.textureType == MTLTextureType3D ? 1 : t.arrayLength * (t.textureType == MTLTextureTypeCube || t.textureType == MTLTextureTypeCubeArray ? 6 : 1);
    id<MTLTexture> v = [base newTextureViewWithPixelFormat:pf textureType:t.textureType levels:NSMakeRange(l0, t.mipmapLevelCount) slices:NSMakeRange(s0, ns)
                                                   swizzle:MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed, MTLTextureSwizzleZero, MTLTextureSwizzleZero, MTLTextureSwizzleRed)];
    if (!v) { RED_ONCE(82, "cannot create the alpha-from-red view (texture ID %d format %lu)", tid, (unsigned long)pf); return nil; }
    { static int once; if (!once++) MLOG("single-channel texture alpha from red: first case texture ID %d %lux%lu", tid, (unsigned long)t.width, (unsigned long)t.height); }
    r->ar = (void *)CFBridgingRetain(v); r->ar_of = (__bridge void *)t; r->ar_rid = v.gpuResourceID._impl;
    *rid = r->ar_rid; return v;
}
// Pixel class of a Metal format as a shader's texture type sees it (NVN_DPF_*): the Uint / Sint formats, everything else reads as float
static int mtl_pf_dpf(MTLPixelFormat pf) {
    switch ((int)pf) {
        case 13: case 23: case 33: case 53: case 63: case 73: case 91: case 103: case 113: case 123: return NVN_DPF_UINT;
        case 14: case 24: case 34: case 54: case 64: case 74: case 104: case 114: case 124: return NVN_DPF_SINT;
    }
    return NVN_DPF_FLOAT;
}
// An image slot the shader declares as uint / int holding a texture of another class: on the Switch the shader's image format decides how the texel's bits are read. FINAL FANTASY TACTICS -
// The Ivalice Chronicles' UI composite blends into the RGBA8 window texture in software, loading it as r32ui and unpacking the four bytes (uirender.tec, fragment 4cf1975ed1e36590); bound as
// it was, an RGBA8Unorm texture in a texture2d<uint> argument read undefined values and translucent UI came out yellow and white. The slot gets a view of the same texel size in the
// shader's class; a texture made without the format-view usage is upgraded before the next draw pass (img_fit_upgrade), three textures in that game. Caller holds g_rmu
static int g_imfit_pend[16];
static void img_fit_upgrade(void) {
    pthread_mutex_lock(&g_rmu);
    for (int i = 0; i < g_imfit_npend; i++) { int tid = g_imfit_pend[i];
        if (tid <= 0 || tid >= g_ntreg || !g_treg[tid].ok) continue;
        id<MTLTexture> b = tex_mtl(&g_treg[tid].t); if (b.parentTexture) b = b.parentTexture;
        if (b && !(b.usage & MTLTextureUsagePixelFormatView)) tex_upgrade_pfv(&g_treg[tid].t, b, MTLPixelFormatR32Uint); }
    g_imfit_npend = 0;
    pthread_mutex_unlock(&g_rmu);
}
// The same view, in the texture's own class, for a swizzled view (the channel mapping of tex_by_id): Metal gives a swizzled view only the read usage (a base of usage 0x7, a view of 0x1),
// so an RGBX8 or RGB565 image was bound as the dummy and its writes were lost. An image takes no channel mapping, as a Vulkan storage image view must have the identity swizzle
static id<MTLTexture> img_fit(int tid, id<MTLTexture> t, int dpf, uint64_t *rid) {
    size_t n = mtl_pf_bytes(t.pixelFormat); int u = dpf == NVN_DPF_UINT, cls = dpf != NVN_DPF_FLOAT && mtl_pf_dpf(t.pixelFormat) != dpf;
    MTLPixelFormat pf = !cls ? t.pixelFormat : n == 1 ? (u ? MTLPixelFormatR8Uint : MTLPixelFormatR8Sint) : n == 2 ? (u ? MTLPixelFormatR16Uint : MTLPixelFormatR16Sint) : n == 4 ? (u ? MTLPixelFormatR32Uint : MTLPixelFormatR32Sint)
                      : n == 8 ? (u ? MTLPixelFormatRG32Uint : MTLPixelFormatRG32Sint) : n == 16 ? (u ? MTLPixelFormatRGBA32Uint : MTLPixelFormatRGBA32Sint) : MTLPixelFormatInvalid;
    id<MTLTexture> base = t.parentTexture ? t.parentTexture : t;
    if (pf == MTLPixelFormatInvalid || tid <= 0 || tid >= g_ntreg) {
        RED_ONCE(106, "image read as %s in the shader, texture of Metal format %lu (texture ID %d): no view of that class, bound as is (reads undefined)", u ? "uint" : "int", (unsigned long)t.pixelFormat, tid); return nil; }
    if (cls && !(base.usage & MTLTextureUsagePixelFormatView)) {   // made without the format-view usage (it costs lossless compression, so textures get it only when needed): upgraded before the next
        int seen = 0; for (int i = 0; i < g_imfit_npend; i++) seen |= g_imfit_pend[i] == tid;   // draw pass opens, as an upgrade copies the texture and so cannot run inside this draw's pass;
        if (!seen && g_imfit_npend < 16) g_imfit_pend[g_imfit_npend++] = tid;                  // until then the slot is bound as is
        return nil; }
    treg_t *r = &g_treg[tid];
    if (r->im && r->im_of == (__bridge void *)t && ((__bridge id<MTLTexture>)r->im).pixelFormat == pf) { *rid = r->im_rid; return (__bridge id<MTLTexture>)r->im; }
    if (r->im) { defer_obj_unref((__bridge id)r->im); CFRelease(r->im); r->im = r->im_of = NULL; r->im_rid = 0; }
    NSUInteger l0 = t.parentTexture ? t.parentRelativeLevel : 0, s0 = t.parentTexture ? t.parentRelativeSlice : 0;
    NSUInteger ns = t.textureType == MTLTextureType3D ? 1 : t.arrayLength * (t.textureType == MTLTextureTypeCube || t.textureType == MTLTextureTypeCubeArray ? 6 : 1);
    id<MTLTexture> v = [base newTextureViewWithPixelFormat:pf textureType:t.textureType levels:NSMakeRange(l0, t.mipmapLevelCount) slices:NSMakeRange(s0, ns)];
    if (!v) { RED_ONCE(107, "cannot create the image view of class %s (texture ID %d, Metal format %lu)", u ? "uint" : "int", tid, (unsigned long)t.pixelFormat); return nil; }
    { static int once; if (cls && !once++) MLOG("image read in another class than its texture: first case texture ID %d Metal format %lu → %lu", tid, (unsigned long)t.pixelFormat, (unsigned long)pf); }
    r->im = (void *)CFBridgingRetain(v); r->im_of = (__bridge void *)t; r->im_rid = v.gpuResourceID._impl;
    *rid = r->im_rid; return v;
}
static inline __attribute__((always_inline)) uint64_t tex_fast_rid(int tid, void **out) {
    if (tid > 0 && tid < g_ntreg && g_treg[tid].ok && g_treg[tid].view && g_treg[tid].view_ser == g_ser_gen && (!g_treg[tid].view_pkg || g_treg[tid].view_dirty == nvn_dirty_now())) { if (out) *out = g_treg[tid].view; return g_treg[tid].view_rid; }
    id<MTLTexture> t = tex_by_id(tid); if (out) *out = (__bridge void *)t; return t ? t.gpuResourceID._impl : 0;
}
static MTLSamplerAddressMode wrapm(int w) {
    switch (w) { case 0: case 7: return MTLSamplerAddressModeClampToEdge; case 1: return MTLSamplerAddressModeRepeat;
        case 3: case 2: return MTLSamplerAddressModeMirrorClampToEdge; case 5: case 4: return MTLSamplerAddressModeClampToBorderColor;
        case 6: return MTLSamplerAddressModeMirrorRepeat; }
    return MTLSamplerAddressModeRepeat;
}
static id<MTLSamplerState> samp_by_id(int sid) {   // Caller holds g_rmu
    if (sid < 0 || sid >= g_nsreg || !g_sreg_ok[sid]) { RED_ONCE(12, "sampler ID %d not registered", sid); return nil; }
    if (g_sview[sid]) return (__bridge id<MTLSamplerState>)g_sview[sid];
    samp_par p = g_sreg[sid];
    MTLSamplerDescriptor *d = [MTLSamplerDescriptor new];
    d.magFilter = p.magf ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    d.minFilter = (p.minf == 1 || p.minf == 3 || p.minf == 5) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    d.mipFilter = p.minf <= 1 ? MTLSamplerMipFilterNotMipmapped : (p.minf == 2 || p.minf == 3) ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterLinear;
    d.sAddressMode = wrapm(p.ws); d.tAddressMode = wrapm(p.wt); d.rAddressMode = wrapm(p.wr);
    d.lodMinClamp = p.lod_min < 0 ? 0 : p.lod_min; d.lodMaxClamp = p.lod_max;
    d.maxAnisotropy = p.aniso < 1 ? 1 : p.aniso > 16 ? 16 : (NSUInteger)p.aniso;
    if (p.cmp_mode) d.compareFunction = (MTLCompareFunction)(p.cmp_func - 1);
    // Reduction (NVN 1 min / 2 max): the minimum or maximum of the texels a filtered sample covers, per channel (Metal 26). Pokémon Legends: Z-A builds its depth pyramid with a min sampler
    // (151607c7a92386cc reads the level above at an explicit LOD) and tests occlusion boxes against it; averaged, the pyramid is nearer than the scene at depth edges. Metal ignores the reduction
    // with a nearest or no mip filter, so such samplers filter mips linearly: at the integer LODs reductions are used with that gives the same level, and a sampler without mips keeps to the base
    // level through the LOD clamp
    if (p.reduction == 1 || p.reduction == 2) {
        d.reductionMode = p.reduction == 1 ? MTLSamplerReductionModeMinimum : MTLSamplerReductionModeMaximum;
        if (d.mipFilter == MTLSamplerMipFilterNotMipmapped) { d.lodMinClamp = 0; d.lodMaxClamp = 0; }
        d.mipFilter = MTLSamplerMipFilterLinear;
    }
    d.borderColor = p.border[3] > 0.5f ? (p.border[0] > 0.5f ? MTLSamplerBorderColorOpaqueWhite : MTLSamplerBorderColorOpaqueBlack) : MTLSamplerBorderColorTransparentBlack;
    d.supportArgumentBuffers = YES;
    id<MTLSamplerState> s = [g_dev newSamplerStateWithDescriptor:d];
    if (s) { g_sview[sid] = (void *)CFBridgingRetain(s); g_srid[sid] = s.gpuResourceID._impl; }
    return s;
}
static inline __attribute__((always_inline)) void *samp_fast(int sid) {
    if (sid > 0 && sid < g_nsreg && g_sreg_ok[sid] && g_sview[sid]) return g_sview[sid];
    return (__bridge void *)samp_by_id(sid);
}

// ---------- Argument buffer blocks: one block per submission, reclaimed after the GPU completes ----------
@interface NvArena : NSObject { @public id<MTLBuffer> buf; uint64_t gpu, fence; size_t used; } @end
@implementation NvArena @end
static NSMutableArray<NvArena *> *g_arenas; static NvArena *g_cur_arena; static NSMutableArray<NvArena *> *g_sub_arenas;
static NvArena *arena_new(size_t minimum) {
    // The for-in loop variable does not retain the object: take a strong reference first and then remove it from the array, otherwise what is returned after the array releases it is a dangling pointer
    for (NvArena *it in g_arenas) if (it->fence && it->fence <= g_done && it->buf.length >= minimum) { NvArena *r = it; [g_arenas removeObject:r]; r->fence = 0; r->used = 0; return r; }
    NvArena *a = [NvArena new]; a->buf = [g_dev newBufferWithLength:MAX(minimum, 4u << 20) options:MTLResourceStorageModeShared];
    if (!a->buf) MDIE("cannot allocate %zu bytes of frame scratch", minimum);
    a->gpu = a->buf.gpuAddress; res_add(a->buf); g_arena_made++; return a;
}
void arena_begin(void) { if (!g_arenas) { g_arenas = [NSMutableArray array]; g_sub_arenas = [NSMutableArray array]; } g_cur_arena = nil; }
void arena_commit(uint64_t fence) { for (NvArena *a in g_sub_arenas) { a->fence = fence; [g_arenas addObject:a]; } [g_sub_arenas removeAllObjects]; g_cur_arena = nil; }
static size_t arena_alloc(size_t n, id<MTLBuffer> *buf) {
    n = (n + 255) & ~(size_t)255;
    if (!g_cur_arena || g_cur_arena->used + n > g_cur_arena->buf.length) {
        pthread_mutex_lock(&g_mu); g_cur_arena = arena_new(n); pthread_mutex_unlock(&g_mu);
        [g_sub_arenas addObject:g_cur_arena];
    }
    size_t off = g_cur_arena->used; g_cur_arena->used += n; *buf = g_cur_arena->buf; return off;
}

