// nvn_mtl.m, part: tex
// Textures: the NVN texture to MTLTexture conversion, the alias / sharing tables, upgrades, the swizzled and linear upload paths, zero fill, sparse views and release.
// Included by nvn_mtl.m in its original order, as one translation unit: statics are shared with the other parts.
// ================= Render replay =================
#include "../../host/front.h"
static int g_red_once[113];
#define RED_ONCE(k, ...) do { if (!g_red_once[k]++) MLOG("[RED] " __VA_ARGS__); } while (0)
// NVN vertex streams 0–15 → Metal vertex buffer slots: 0–13 go to 16–29 (30 is the constant fallback buffer), 14 and 15 to 14 and 15. Metal has only the 31 slots 0–30; translated shaders use only buffer(0) (the argument buffer),
// and the host's own vertex functions read no vertex buffers. 13 Sentinels uses stream 15, and the old 16 + stream went out of range (Metal assertion: buffer index (31) must be < 31)
#define VB_SLOT(st) ((st) < 14 ? 16 + (st) : (st))

// ---------- Texture → MTLTexture ----------
static int fmt_compressed(int f) { return (f >= 0x41 && f <= 0x50) || (f >= 0x79 && f <= 0x94); }
static int fmt_depth(int f) { return f >= 0x31 && f <= 0x36; }
typedef struct { MTLPixelFormat a, b; } pfv_pair;
static const pfv_pair g_pfv_srgb[] = {
    {MTLPixelFormatRGBA8Unorm, MTLPixelFormatRGBA8Unorm_sRGB},
    {MTLPixelFormatBGRA8Unorm, MTLPixelFormatBGRA8Unorm_sRGB},
    {MTLPixelFormatBC1_RGBA, MTLPixelFormatBC1_RGBA_sRGB},
    {MTLPixelFormatBC2_RGBA, MTLPixelFormatBC2_RGBA_sRGB},
    {MTLPixelFormatBC3_RGBA, MTLPixelFormatBC3_RGBA_sRGB},
    {MTLPixelFormatBC7_RGBAUnorm, MTLPixelFormatBC7_RGBAUnorm_sRGB},
};
static int tex_need_pfv(MTLPixelFormat base, MTLPixelFormat view) {
    if (base == view) return 0;
    for (size_t i = 0; i < sizeof g_pfv_srgb / sizeof *g_pfv_srgb; i++)
        if ((base == g_pfv_srgb[i].a && view == g_pfv_srgb[i].b) || (base == g_pfv_srgb[i].b && view == g_pfv_srgb[i].a)) return 0;
    return 1;
}
// Cache tables on the replay hot path always use nvn_hmap (C hash, keys not boxed); the table stores CFBridgingRetain'ed objects, CFRelease'd on replace / delete
static void hm_rel(void *v) { CFRelease(v); }
static __unsafe_unretained id hm_get_id(const nvn_hmap *m, const void *k) { return (__bridge id)nvn_hm_get(m, k); }
static void hm_put_id(nvn_hmap *m, const void *k, id o) { void *old = nvn_hm_put(m, k, (void *)CFBridgingRetain(o)); if (old) CFRelease(old); }
static void hm_del_id(nvn_hmap *m, const void *k) { void *old = nvn_hm_del(m, k); if (old) CFRelease(old); }
static nvn_hmap g_by_serial = {.klen = 8};     // Serial → MTLTexture / NvPkg / NvProg
static pthread_mutex_t g_smu = PTHREAD_MUTEX_INITIALIZER;
// Serial table generation: +1 when entries are added / removed or a packaged texture is replaced. Downstream caches (texture views, programs) record the generation at fetch time; if it is unchanged they can be used directly without another table lookup
static _Atomic uint64_t g_ser_gen = 1;
// Textures shared by storage (non-packaged): key = memory pool + offset + texture description. An NVN texture object is only a descriptor; an object rebuilt over the same storage with the same description is the same texture;
// SMBW rebuilds its transient render targets on new objects every frame and never Finalizes them, so building one per serial leaked about 13 MB of GPU memory per frame. Textures here are kept resident by the sharing table,
// and when the serial table replaces / deletes them it only drops its reference (defer_obj_unref), never removing them from the residency set. Entries go when their pool is finalized (mtl_alias_drop_pool)
// or another texture takes over their storage (alias_take_over).
typedef struct { uint64_t pool; int64_t off; nvn_texdesc d; } alias_key;
static nvn_hmap g_alias = {.klen = sizeof(alias_key)};
static nvn_hmap g_alias_seen = {.klen = sizeof(alias_key)};   // key → g_fence + 1 at its last lookup or creation (alias_take_over spares entries looked up lately)
static CFMutableSetRef g_alias_set;   // Textures in the sharing table (non-owning; used to ask "is this shared")
static uint64_t g_alias_evicted;   // shared textures evicted on pool Finalize so far (for the memory report)
static int alias_has(id o) { return g_alias_set && o && CFSetContainsValue(g_alias_set, (__bridge const void *)o); }
// On memory pool Finalize, evict the textures built on that pool from the sharing table (collect the keys first, then delete: deletion shifts entries back). The sharing table used not to evict: Mario's pools stay for good, so entries are bounded;
// Engage frees pools on scene changes and rebuilds on new ones, so the old pools' render targets and other private textures stayed in the table forever (about 0.8 GB more GPU memory over 850 s of the title demo), and linear textures built directly on a pool also kept the whole pool buffer alive.
// Also, when a pool object's address is reused, old entries would hand a texture on the old storage to the new pool's texture at the same offset and description. Evicted textures go through deferred release; if the serial table still holds one, that is just one more reference
static void treg_drop_pool_views(const void *pool);
void mtl_alias_drop_pool(const void *pool) {
    if (!pool) return;
    pthread_mutex_lock(&g_smu);
    uint32_t n = 0; alias_key *ks = NULL;
    for (uint32_t i = 0; i < g_alias.cap; i++) {
        if (!g_alias.vals[i]) continue;
        alias_key k; memcpy(&k, g_alias.keys + (size_t)i * g_alias.klen, sizeof k);
        if (k.pool != (uint64_t)(uintptr_t)pool) continue;
        alias_key *nk = realloc(ks, (n + 1) * sizeof *ks); if (!nk) break; ks = nk; ks[n++] = k;
    }
    for (uint32_t j = 0; j < n; j++) {
        void *old = nvn_hm_del(&g_alias, &ks[j]);
        if (!old) continue;
        nvn_hm_del(&g_alias_seen, &ks[j]);
        id o = CFBridgingRelease(old);
        if (g_alias_set) CFSetRemoveValue(g_alias_set, (__bridge const void *)o);
        defer_obj_release(o);
    }
    if (n) { g_ser_gen++; g_alias_evicted += n; }
    pthread_mutex_unlock(&g_smu);
    free(ks);
    treg_drop_pool_views(pool);   // outside g_smu: the texture ID registry lock g_rmu is taken before g_smu
}
// A new shared texture takes over storage: entries of the same pool whose storage overlaps the new texture's under another description go, unless a live texture object still resolves
// to them (an object rebuilt over the same storage with the same description must keep finding it: SMBW's transient render targets) or a lookup found them in the last ALIAS_IDLE submissions
// (Breath of the Wild finalizes its transient render targets before it submits and makes them again the next frame: targets sharing storage would evict each other every frame). On NVN the memory now belongs to the new texture;
// the old private Metal texture only held a copy nothing can name again. STEINS;GATE RE:BOOT decodes every image it shows into one scratch region of a pool that lives for good and
// finalizes the texture before the next one: each image added an entry, 468 of them and 1.58 GB of GPU memory 24,000 frames into a new game. A finalized object resolved through g_ghost
// keeps its own reference, so the entry's reference is only unref'd while one does; otherwise the texture is retired (deferred until the submissions that used it complete).
// Cached texture-ID views follow g_ser_gen. Caller holds g_smu
enum { ALIAS_IDLE = 16 };
static uint64_t g_alias_taken;   // shared textures evicted when another texture took over their storage (for the memory report)
static nvn_hmap g_ghost;   // defined below (finalized texture serials)
static int serial_holds(nvn_hmap *m, id o) { for (uint32_t i = 0; i < m->cap; i++) if (m->vals[i] == (__bridge void *)o) return 1; return 0; }
static void alias_take_over(const alias_key *nk) {
    nvn_texlayout L; nvn_tex_layout(&nk->d, &L);
    if (!L.size) return;
    int64_t lo = nk->off, hi = nk->off + (int64_t)L.size;
    uint32_t n = 0; alias_key ks[16];
    for (uint32_t i = 0; i < g_alias.cap && n < 16; i++) {
        if (!g_alias.vals[i]) continue;
        alias_key k; memcpy(&k, g_alias.keys + (size_t)i * g_alias.klen, sizeof k);
        if (k.pool != nk->pool || !memcmp(&k, nk, sizeof k) || k.off >= hi) continue;
        nvn_texlayout kl; nvn_tex_layout(&k.d, &kl);
        uint64_t seen = (uint64_t)(uintptr_t)nvn_hm_get(&g_alias_seen, &k);
        if (k.off + (int64_t)kl.size <= lo || (seen && g_fence + 1 - seen < ALIAS_IDLE) || serial_holds(&g_by_serial, (__bridge id)g_alias.vals[i])) continue;
        ks[n++] = k;
    }
    for (uint32_t j = 0; j < n; j++) {
        void *old = nvn_hm_del(&g_alias, &ks[j]);
        if (!old) continue;
        nvn_hm_del(&g_alias_seen, &ks[j]);
        id o = CFBridgingRelease(old);
        if (g_alias_set) CFSetRemoveValue(g_alias_set, (__bridge const void *)o);
        if (serial_holds(&g_ghost, o)) defer_obj_unref(o); else defer_obj_release(o);
    }
    if (n) { g_ser_gen++; g_alias_taken += n; }
}
static __unsafe_unretained id serial_get(uint64_t k) { pthread_mutex_lock(&g_smu); __unsafe_unretained id o = hm_get_id(&g_by_serial, &k); pthread_mutex_unlock(&g_smu); return o; }
static void serial_put(uint64_t k, id o) { pthread_mutex_lock(&g_smu); id old = hm_get_id(&g_by_serial, &k); if (old) { if (alias_has(old)) defer_obj_unref(old); else defer_obj_release(old); } hm_put_id(&g_by_serial, &k, o); g_ser_gen++; pthread_mutex_unlock(&g_smu); }
static void serial_del(uint64_t k) { pthread_mutex_lock(&g_smu); id old = hm_get_id(&g_by_serial, &k); if (old) { if (alias_has(old)) defer_obj_unref(old); else defer_obj_release(old); hm_del_id(&g_by_serial, &k); g_ser_gen++; } pthread_mutex_unlock(&g_smu); }   // nothing to drop: the texture ID caches stay valid
// Texture serials owned by a live NVN texture object (TextureInitialize until Finalize or re-initialization, hle_nvn.c). Command records and texture pool registrations snapshot the
// texture object (tex_snap in hle_nvn.c, mtl_reg_tex), so replay can resolve a texture the game has already finalized: Breath of the Wild records each frame's transient render targets
// (the HUD captures, the 640x360 half-resolution buffers) and finalizes their objects before it submits. Those serials went back into g_by_serial and stayed there for good, as no second
// Finalize comes: about 10 a frame, 200,000 after an hour in the first shrine. A dead serial resolves through g_ghost instead, a small table whose oldest entry goes when a new one comes,
// so a record replayed again soon still finds its texture
enum { NGHOST = 512 };
static nvn_hmap g_tex_live = {.klen = 8};   // serial → (void *)1, under g_smu
static nvn_hmap g_ghost = {.klen = 8}; static uint64_t g_ghost_ring[NGHOST]; static unsigned g_ghost_next; static uint64_t g_ghost_puts;
void mtl_tex_init(const tex_t *t) { if (!t->serial) return; pthread_mutex_lock(&g_smu); nvn_hm_put(&g_tex_live, &t->serial, (void *)1); pthread_mutex_unlock(&g_smu); }
static void obj_drop(id o) { if (alias_has(o)) defer_obj_unref(o); else defer_obj_release(o); }   // caller holds g_smu
static __unsafe_unretained id tex_get(uint64_t k) {
    pthread_mutex_lock(&g_smu); __unsafe_unretained id o = hm_get_id(&g_by_serial, &k); if (!o && g_ghost.n) o = hm_get_id(&g_ghost, &k); pthread_mutex_unlock(&g_smu); return o;
}
static void tex_put(uint64_t k, id o) {   // serials from 2^62 up are external textures (mtl_ext_tex), never initialized by the game
    pthread_mutex_lock(&g_smu);
    nvn_hmap *m = k >= (1ull << 62) || nvn_hm_get(&g_tex_live, &k) ? &g_by_serial : &g_ghost;
    id old = hm_get_id(m, &k);
    if (old) obj_drop(old);
    else if (m == &g_ghost) {
        uint64_t *slot = &g_ghost_ring[g_ghost_next++ % NGHOST];
        if (*slot) { id ev = hm_get_id(&g_ghost, slot); if (ev) obj_drop(ev); hm_del_id(&g_ghost, slot); }
        *slot = k; g_ghost_puts++;
    }
    hm_put_id(m, &k, o); g_ser_gen++;
    pthread_mutex_unlock(&g_smu);
}
// Host record of a packaged texture: the current MTLTexture, the upload stamp, and the global write generation at the last check
// spare: the texture the last re-upload replaced, kept resident and written by the next re-upload once the GPU has passed spare_fence (a texture the CPU rewrites every frame, a movie frame,
// alternates between two textures instead of allocating one per frame: Link's Awakening's title movie made and freed an 8 MB texture a frame)
@interface NvPkg : NSObject { @public id<MTLTexture> tex, spare, view; uint64_t stamp, seen, hash, spare_fence, vgen, vseen; const void *span; int sparse, vbase; uint8_t *edge[2]; uint32_t edge_n[2]; } @end   // hash: full hash of the uploaded content under DM_NVN_DIRTYCHECK; span: its tracked range (nvn_dirty_span, NULL = untracked);
// sparse: in a sparse virtual pool, read through its page table (hle_nvn.c nvn_sparse_*), vgen the page table's generation at the upload and vseen at the last look; view: what it is bound as (sparse_view)
// edge: the uploaded bytes of the storage's first and last host pages where other data shares them (pkg_edges_take)
@implementation NvPkg
- (void)dealloc { res_remove(tex); if (spare) res_remove(spare); free(edge[0]); free(edge[1]); }
@end
static uint64_t g_pkg_uploads, g_pkg_reuploads, g_pkg_miss, g_pkg_edge_skips;
static uint64_t g_pfv_upgrades;
// DM_NVN_DIRTYCHECK=1 (diagnostic, compared against ground truth): fully hash the pool content on every use; changed but not detected by dirty-page tracking = a missed detection
static int dirtycheck(void) { static int v = -1; if (v < 0) v = getenv("DM_NVN_DIRTYCHECK") != NULL; return v; }
static uint64_t full_hash(const uint8_t *m, uint64_t n) { uint64_t h = 1469598103934665603ull; for (uint64_t i = 0; i < n; i++) h = (h ^ m[i]) * 1099511628211ull; return h; }
// Writes beside a texture. The tracker marks whole host pages (16 KB) dirty, and guest memory pools are only 4 KB aligned: the first and last pages of a texture's storage are shared with
// whatever lies beside it. Pokémon Sword keeps each texture of its GFPAK models in a pool of its own, with the pool's object 4 KB before the storage, in the same host page, and writes there
// every frame: 73,232 re-uploads in 70 s of play from a save, every one checked of unchanged content, 12 GB of upload staging in 8 minutes. So the bytes of the storage in those edge pages
// are kept at the upload (pkg_edges_take), and a texture whose only written pages are its edges, with those bytes unchanged, is protected again instead of re-uploaded (pkg_edges_clean).
// The edges are the whole storage when it lies within two pages (no page wholly inside)
static void pkg_edge_ranges(const tex_t *t, uintptr_t r[2][2], uintptr_t *ia, uintptr_t *ie) {
    uintptr_t pg = (uintptr_t)getpagesize(), a = (uintptr_t)t->mem, e = a + t->L.size;
    *ia = (a + pg - 1) & ~(pg - 1); *ie = e & ~(pg - 1);
    if (*ie <= *ia) { r[0][0] = a; r[0][1] = e; r[1][0] = r[1][1] = 0; *ia = *ie = 0; return; }
    r[0][0] = a; r[0][1] = *ia; r[1][0] = *ie; r[1][1] = e;
}
static void pkg_edges_take(NvPkg *pk, const tex_t *t) {   // right after an upload, with its stamp in pk->stamp
    for (int i = 0; i < 2; i++) { free(pk->edge[i]); pk->edge[i] = NULL; pk->edge_n[i] = 0; }
    if (pk->sparse || !pk->span || !t->mem) return;
    uintptr_t r[2][2], ia, ie; pkg_edge_ranges(t, r, &ia, &ie);
    for (int i = 0; i < 2; i++) if (r[i][1] > r[i][0]) { pk->edge_n[i] = (uint32_t)(r[i][1] - r[i][0]); pk->edge[i] = malloc(pk->edge_n[i]); memcpy(pk->edge[i], (const void *)r[i][0], pk->edge_n[i]); }
    // A write after the upload's stamp may have reached the copy but not the upload: then there is no copy to trust, and the next write re-uploads as before
    if (nvn_dirty_since_span(pk->span, t->mem, t->L.size, pk->stamp)) for (int i = 0; i < 2; i++) { free(pk->edge[i]); pk->edge[i] = NULL; pk->edge_n[i] = 0; }
}
static int pkg_edges_clean(NvPkg *pk, const tex_t *t) {   // the covered pages were written since the upload: 1 = only edge pages, and the texture's bytes there are as uploaded
    if (!pk->edge_n[0] && !pk->edge_n[1]) return 0;
    uintptr_t r[2][2], ia, ie; pkg_edge_ranges(t, r, &ia, &ie);
    nvn_dirty_protect(t->mem, t->L.size); uint64_t st = nvn_dirty_now();   // protect, take the stamp, then read (as pkg_upload): a later write is seen at the next check
    if (ie > ia && nvn_dirty_since_span(pk->span, (const void *)ia, ie - ia, pk->stamp)) return 0;
    for (int i = 0; i < 2; i++) if (pk->edge_n[i] && memcmp(pk->edge[i], (const void *)r[i][0], pk->edge_n[i])) return 0;
    pk->stamp = st; g_pkg_edge_skips++;
    return 1;
}
static MTLTextureDescriptor *tex_desc(const tex_t *t, int packaged) {
    uint32_t pf = 0; uint8_t swz[4];
    if (!nvn_fmt_to_mtl(t->d.format, &pf, swz) || !pf) { RED_ONCE(0, "texture format %#x has no Metal equivalent (first case)", t->d.format); return nil; }
    // Metal's limits: 16384 in 2D, 2048 in 3D, 2048 array layers; beyond them Metal aborts the process on an assertion, so they are caught here with a red line
    if (t->d.width > 16384 || t->d.height > 16384 || t->d.depth > 2048 || t->d.levels > 15) { RED_ONCE(71, "texture size exceeds Metal limits: %u×%u×%u levels %u format %#x target %d (first case ser=%llu)", t->d.width, t->d.height, t->d.depth, t->d.levels, t->d.format, t->d.target, (unsigned long long)t->serial); return nil; }
    MTLTextureDescriptor *d = [MTLTextureDescriptor new];
    d.pixelFormat = (MTLPixelFormat)pf;
    d.width = t->d.width ? t->d.width : 1; d.height = t->d.height ? t->d.height : 1; d.depth = 1; d.arrayLength = 1;
    d.mipmapLevelCount = t->d.levels ? t->d.levels : 1;
    switch (t->d.target) {
        // 1D / 1D array are created as one-row 2D / 2D array textures: translated shaders never declare texture1d (0 in all five games' libraries) and always sample them as 2D; a Metal 1D texture bound to a texture2d argument is a type mismatch with undefined reads.
        // Super Mario 3D World + Bowser's Fury's material shaders sample 64x1 R32F 1D lookup tables heavily every frame (on Maxwell a 2D sample of a 1D texture reads that one row)
        case 0: d.textureType = MTLTextureType2D; d.height = 1; break;
        case 2: d.textureType = MTLTextureType3D; d.depth = t->d.depth; break;
        case 3: d.textureType = MTLTextureType2DArray; d.arrayLength = t->d.height; d.height = 1; break;
        case 4: d.textureType = MTLTextureType2DArray; d.arrayLength = t->d.depth; break;
        case 5: d.textureType = MTLTextureType2DMultisample; d.sampleCount = t->d.samples; d.mipmapLevelCount = 1; break;
        case 6: d.textureType = MTLTextureType2DMultisampleArray; d.sampleCount = t->d.samples; d.arrayLength = t->d.depth; d.mipmapLevelCount = 1; break;
        // Cube textures are created as 6-layer 2D arrays and sampled through cube / cube array views made by tex_fit: Metal only allows cube array views of a 2D array base texture (not of a cube base).
        // The Legend of Zelda: Breath of the Wild has 1512 shaders that sample the environment light probes as a cube array while a single cube texture is bound (texture ID 1064 onwards): with mismatched types the reads are undefined, and every lit surface came out black
        case 8: d.textureType = MTLTextureType2DArray; d.arrayLength = 6; break;
        // Cube arrays likewise, as a 2D array of all their faces: NVN gives a cube array's depth in layer-faces (a multiple of 6), where a Metal cube array's arrayLength counts cubes.
        // Created as a cube array with arrayLength = depth, DRAGON QUEST XI S's 510-face one (85 cubes, made when its prologue movie ends) asked for 510 cubes, past Metal's 341, and Metal aborted the process
        case 9: d.textureType = MTLTextureType2DArray; d.arrayLength = t->d.depth; break;
        case 10: RED_ONCE(1, "buffer textures (TARGET_BUFFER) not implemented"); return nil;
        default: d.textureType = MTLTextureType2D; break;
    }
    d.storageMode = MTLStorageModePrivate;   // Render targets and assets both go in GPU private storage (lossless compression available)
    d.usage = MTLTextureUsageShaderRead | (fmt_compressed(t->d.format) || packaged ? 0 : MTLTextureUsageRenderTarget)
            | (t->d.flags & NVN_TEXTURE_FLAGS_IMAGE ? MTLTextureUsageShaderWrite : 0);   // Image usage: shader-writable (compute kernels generating the 3D color lookup table, etc.
    // A depth-stencil texture has the format-view usage from the start: its stencil is read through an X32_Stencil8 view (stencil-mode views and textures, the depth-stencil copy), which Metal's
    // contract allows only with that usage. Apple GPUs read the view correctly without it, but the API validation layer aborts the process at the first such view (PR #106, found on Luigi's
    // Mansion 3), and upgrading later would recreate a depth buffer in use. Unlike colour formats it costs nothing measured: on an M4 Pro a 2560x1440 one has the same allocated size and the same
    // GPU time over eight load / store passes with and without it
    if (d.pixelFormat == MTLPixelFormatDepth32Float_Stencil8) d.usage |= MTLTextureUsagePixelFormatView;
    return d;
}
// In-place upgrade: when the original texture was not created with PixelFormatView (off by default, to keep Apple GPU lossless compression) but a different-format view is needed, create a new one with the original spec + PixelFormatView, copy over every slice × level, and replace the registration.
// Count: across the 4 screens character select / camp / wilderness / combat, 17473 textures needed a different-format view 0 times, so this is a fallback and does not rely on predicting which textures need it
// The same upgrade adds render-target usage for a whole-level clear (clear_tex) of a texture made without it: Monster Hunter Rise clears a 32x80 packaged one
static id<MTLTexture> tex_upgrade(const tex_t *t, id<MTLTexture> base, MTLTextureUsage add, MTLPixelFormat view_pf) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor new];
    d.textureType = base.textureType; d.pixelFormat = base.pixelFormat; d.width = base.width; d.height = base.height; d.depth = base.depth;   // Keep the original format, only add the format-view usage
    d.arrayLength = base.arrayLength; d.mipmapLevelCount = base.mipmapLevelCount; d.sampleCount = base.sampleCount;
    d.storageMode = base.storageMode; d.cpuCacheMode = base.cpuCacheMode; d.hazardTrackingMode = base.hazardTrackingMode;
    d.resourceOptions = base.resourceOptions; d.usage = base.usage | add;
    // A linear texture aliases the guest pool. Adding usage must keep that backing: a new standalone texture copies the old pixels once, but later GPU writes never reach the CPU map.
    id<MTLBuffer> backing = base.buffer;
    id<MTLTexture> upgraded = backing ? [backing newTextureWithDescriptor:d offset:base.bufferOffset bytesPerRow:base.bufferBytesPerRow] : [g_dev newTextureWithDescriptor:d];
    if (!upgraded) { RED_ONCE(45, "texture upgrade failed ser=%llu format %lu → %lu usage %#lx", t->serial, (unsigned long)base.pixelFormat, (unsigned long)view_pf, (unsigned long)d.usage); return base; }
    res_add(upgraded);
    id<MTL4ComputeCommandEncoder> b = backing ? nil : blit();   // shared backing already contains the pixels; copying it onto itself would overlap
    NSUInteger slices = base.textureType == MTLTextureType3D ? 1 : base.arrayLength * (base.textureType == MTLTextureTypeCube || base.textureType == MTLTextureTypeCubeArray ? 6 : 1);
    for (NSUInteger l = 0; !backing && l < base.mipmapLevelCount; l++) {
        NSUInteger w = MAX(base.width >> l, 1u), h = MAX(base.height >> l, 1u), z = base.textureType == MTLTextureType3D ? MAX(base.depth >> l, 1u) : 1;
        if (base.textureType == MTLTextureType3D)
            enc_b_copy_texture(b, base, 0, l, MTLOriginMake(0, 0, 0), MTLSizeMake(w, h, z), upgraded, 0, l, MTLOriginMake(0, 0, 0));
        else for (NSUInteger s = 0; s < slices; s++)
            enc_b_copy_texture(b, base, s, l, MTLOriginMake(0, 0, 0), MTLSizeMake(w, h, 1), upgraded, s, l, MTLOriginMake(0, 0, 0));
    }
    id holder = tex_get(t->serial);
    if ([holder isKindOfClass:[NvPkg class]]) { id<MTLTexture> old = ((NvPkg *)holder)->tex; ((NvPkg *)holder)->tex = upgraded; defer_obj_release(old); g_ser_gen++;
        if (((NvPkg *)holder)->view) { defer_obj_unref(((NvPkg *)holder)->view); ((NvPkg *)holder)->view = nil; ((NvPkg *)holder)->vbase = 0; } }   // a sparse texture's view of the old one: bound whole until its next upload
    else {
        // Every name of the old texture moves to the upgraded one: other serials sharing its storage (live in g_by_serial, finalized in g_ghost) and the sharing table. Only the asking serial moved before,
        // so the others kept reading the old copy while writes went to the new one, and the old texture stayed in g_alias_set. Slots are replaced in place (no insert, so no table resize during the walk);
        // the old texture is retired once the submissions that used it complete, the view caches follow g_ser_gen (PR #107)
        pthread_mutex_lock(&g_smu);
        nvn_hmap *tables[] = {&g_by_serial, &g_ghost}; int named = 0, shared = 0;
        for (unsigned j = 0; j < 2; j++) for (uint32_t i = 0; i < tables[j]->cap; i++) {
            if (tables[j]->vals[i] != (__bridge void *)base) continue;
            uint64_t serial; memcpy(&serial, tables[j]->keys + (size_t)i * tables[j]->klen, sizeof serial);
            void *old = tables[j]->vals[i]; tables[j]->vals[i] = (void *)CFBridgingRetain(upgraded); CFRelease(old); named |= serial == t->serial;
        }
        for (uint32_t i = 0; i < g_alias.cap; i++) {
            if (g_alias.vals[i] != (__bridge void *)base) continue;
            void *old = g_alias.vals[i]; g_alias.vals[i] = (void *)CFBridgingRetain(upgraded); CFRelease(old); shared = 1;
        }
        if (shared) { CFSetRemoveValue(g_alias_set, (__bridge const void *)base); CFSetAddValue(g_alias_set, (__bridge const void *)upgraded); }
        g_ser_gen++;
        pthread_mutex_unlock(&g_smu);
        if (!named) tex_put(t->serial, upgraded);
        defer_obj_release(base);
    }
    MLOG("texture upgraded to allow %s ser=%llu base format %lu → view format %lu (upgrades=%llu)", add & MTLTextureUsageRenderTarget ? "render-target use" : "format views", t->serial, (unsigned long)base.pixelFormat, (unsigned long)view_pf, ++g_pfv_upgrades);
    return upgraded;
}
static id<MTLTexture> tex_upgrade_pfv(const tex_t *t, id<MTLTexture> base, MTLPixelFormat view_pf) { return tex_upgrade(t, base, MTLTextureUsagePixelFormatView, view_pf); }
// Packaged texture upload: first set read-only, then take the stamp, then read the pool memory and deswizzle into a staging buffer, and blit into a new private texture.
// Batching: uploads within one replay submission share one upload command buffer (compute encoder deswizzle + a unified blit), flushed before the replay command buffer is committed;
// the same queue executes in commit order ⇒ uploads are always finished before the first sample. Uploads outside a submission are flushed on the spot. The staging area is cut from 32 MB private blocks, held by the command buffer until completion.
static _Atomic uint64_t g_upload_ns;   // Cumulative time of deswizzle + upload encoding on the submit thread (timing instrumentation)
static _Atomic uint64_t g_upload_gpu_ns;
static id<MTL4CommandBuffer> g_up_cb;
static id<MTL4ComputeCommandEncoder> g_up_ce;
static _Atomic int g_up_alloc_busy[3];
// Waiting for upload batches to finish (g_up_pending, g_up_alloc_busy): the batches' completion handler broadcasts after it updates them, so waiters sleep until then (they used to poll every 0.2–1 ms;
// Three Houses' loader calls QueueFinish after each resource, each one waited up to a millisecond late). The 10 ms timeout is only a safety net
static pthread_mutex_t g_up_wmu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_up_wcv = PTHREAD_COND_INITIALIZER;
static void up_wake(void) { pthread_mutex_lock(&g_up_wmu); pthread_cond_broadcast(&g_up_wcv); pthread_mutex_unlock(&g_up_wmu); }
static void up_wait_zero(_Atomic int *v) {
    if (!atomic_load(v)) return;
    pthread_mutex_lock(&g_up_wmu);
    while (atomic_load(v)) { struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl); dl.tv_nsec += 10000000; if (dl.tv_nsec >= 1000000000) { dl.tv_sec++; dl.tv_nsec -= 1000000000; } pthread_cond_timedwait(&g_up_wcv, &g_up_wmu, &dl); }
    pthread_mutex_unlock(&g_up_wmu);
}
static unsigned g_up_seq;
#define UP_BLOCK_SIZE (32u * 1024u * 1024u)
// Each of the three upload slots keeps one staging block of UP_SLOT_BLOCK for good (resident, reused once that slot's previous batch has finished: up_begin waits for it). Every batch used to make a fresh
// 32 MB private block, put it in the residency set and retire it after the GPU was done: Breath of the Wild re-uploads a few CPU-written textures each frame (about 40 KB each), so that was about 1.7 blocks
// a frame, about 3 GB/s of GPU memory allocated, made resident and freed, and 2.4 residency commits a frame. A slot's block grows to fit the first texture of a batch (up to UP_BLOCK_SIZE); what does
// not fit after that continues in fresh blocks of UP_BLOCK_SIZE as before
#define UP_SLOT_BLOCK (4u * 1024u * 1024u)
static __strong id<MTLBuffer> g_up_slot_blk[3]; static unsigned g_up_cur_slot;
typedef struct {
    __unsafe_unretained id<MTLBuffer> src;
    size_t off, row, image;
    MTLSize size;
    __unsafe_unretained id<MTLTexture> dst;   // Strongly referenced by g_up_texs until the flush is encoded
    NSUInteger slice, level;
} up_blit;
static up_blit *g_up_blits; static int g_nup_blits, g_up_blitcap;
static NSMutableArray *g_up_texs;   // Target textures in the batch: before the flush, textures may be replaced / released within the same submission
static id<MTLBuffer> g_up_block;
static size_t g_up_block_off, g_up_block_size;
static NSMutableArray *g_up_keep;


// The upload command buffer was discarded by the system (dropped along during GPU error recovery; the first frame on entering Travincal drops one every run): the objects of this batch of textures are built and the timestamps updated, but the content was never written in,
// and if the pool memory on the game side no longer changes it will never be re-uploaded. Record this batch of target textures, and the next time any of them is fetched treat it as dirty and re-upload once.
// DM_TEST_UP_LOST=<batch index> (for testing): treat that batch of uploads as dropped, to verify the re-upload path.
static NSHashTable *g_up_lost; static os_unfair_lock g_up_lost_lk = OS_UNFAIR_LOCK_INIT; static _Atomic int g_up_lost_n; static _Atomic uint64_t g_up_lost_redo;   // Weak-reference table: if a texture in the record is never fetched again and is released normally, this table does not keep it alive
static int up_lost_take(id tex) {   // This texture's upload was dropped: take it out of the record and return 1
    if (!tex) return 0; os_unfair_lock_lock(&g_up_lost_lk); int hit = [g_up_lost containsObject:tex]; if (hit) { [g_up_lost removeObject:tex]; atomic_store(&g_up_lost_n, (int)g_up_lost.allObjects.count); atomic_fetch_add(&g_up_lost_redo, 1); } os_unfair_lock_unlock(&g_up_lost_lk); return hit;
}
static void up_diag(void);
static void up_flush(void) {
    up_owner_check("up_flush");
    if (!g_up_cb) return;
    if (g_up_ce) {
        m4_end(g_up_ce); g_up_ce = nil;
    }
    id<MTL4ComputeCommandEncoder> b = g_nup_blits ? g_up_ce = [g_up_cb computeCommandEncoder] : nil;
    if (b) [b barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
    for (int i = 0; b && i < g_nup_blits; i++) {
        up_blit *x = &g_up_blits[i];
        enc_b_copy_buffer_texture(b, x->src, x->off, x->row, x->image, x->size, x->dst, x->slice, x->level, MTLOriginMake(0, 0, 0));
    }
    if (b) { m4_end(b); g_up_ce = nil; }
    id<MTL4CommandBuffer> cb = g_up_cb;
    unsigned slot = (g_up_seq - 1) % 3;
    NSMutableArray *dsts = [NSMutableArray arrayWithCapacity:(NSUInteger)g_nup_blits]; for (int i = 0; i < g_nup_blits; i++) if (g_up_blits[i].dst) [dsts addObject:g_up_blits[i].dst];
    static int lost_at = -2; static uint64_t batch_n; if (lost_at == -2) { const char *e = getenv("DM_TEST_UP_LOST"); lost_at = e ? atoi(e) : -1; } int fake_lost = dsts.count && lost_at > 0 && (int)++batch_n == lost_at;
    MTL4CommitOptions *o = [MTL4CommitOptions new];
    atomic_fetch_add(&g_up_pending, 1);
    [o addFeedbackHandler:^(id<MTL4CommitFeedback> f) {
        if (f.error) MLOG("[RED] upload command buffer execution error: %s", f.error.description.UTF8String);
        if (f.error || fake_lost) {
            os_unfair_lock_lock(&g_up_lost_lk); if (!g_up_lost) g_up_lost = [NSHashTable weakObjectsHashTable]; for (id d in dsts) [g_up_lost addObject:d]; int n = (int)g_up_lost.allObjects.count; atomic_store(&g_up_lost_n, n); os_unfair_lock_unlock(&g_up_lost_lk);
            pthread_mutex_lock(&g_smu); g_ser_gen++; pthread_mutex_unlock(&g_smu);   // Make the view cache of every texture ID take the slow path once (on a cache hit tex_mtl is not passed through and this record is not seen)
            MLOG("%supload command buffer dropped: the %lu textures of this batch are marked unwritten and re-uploaded on next use (%d awaiting re-upload)", fake_lost ? "DM_TEST_UP_LOST: " : "", (unsigned long)dsts.count, n);
        }
        double d = f.GPUEndTime - f.GPUStartTime;
        if (d > 0) atomic_fetch_add(&g_upload_gpu_ns, (uint64_t)(d * 1e9));
        atomic_store(&g_up_alloc_busy[slot], 0);
        atomic_fetch_sub(&g_up_pending, 1);
        up_wake();
    }];
    res_commit();
    [cb endCommandBuffer];
    rec_commit_upload(cb, o);
    g_up_cb = nil; if (g_up_block) { if (g_up_block != g_up_slot_blk[slot]) defer_obj_release(g_up_block); g_up_block = nil; } g_up_block_off = g_up_block_size = 0;   // the slot's own block stays (UP_SLOT_BLOCK)
    for (id obj in g_up_keep) defer_obj_release(obj);
    [g_up_keep removeAllObjects];
    up_diag();
    g_nup_blits = 0;
    [g_up_texs removeAllObjects];
}

static void up_begin(void) {
    up_owner_check("up_begin");
    if (g_up_cb) return;
    unsigned slot = g_up_seq++ % 3;
    // The slot's last batch may still be only a commit record in the encode thread's current block (rec_commit_upload), which is handed over when it fills or the submission drains:
    // waiting without handing it over waited for a batch never committed. Little Nightmares II's first render passes zero-filled enough new targets to come round to a slot within one
    // block, and its RHI thread waited for good inside nvnQueueSubmitCommands (q_cb drains before it waits for the same reason)
    if (atomic_load(&g_up_alloc_busy[slot])) rec_publish();
    up_wait_zero(&g_up_alloc_busy[slot]);
    atomic_store(&g_up_alloc_busy[slot], 1); g_up_cur_slot = slot;
    [g_up_alloc4[slot] reset];
    g_up_cb = [g_dev newCommandBuffer];
    [g_up_cb beginCommandBufferWithAllocator:g_up_alloc4[slot]];
    g_up_ce = [g_up_cb computeCommandEncoder];
    [g_up_ce barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
    [g_up_ce setArgumentTable:g_up_ct];
    if (!g_up_texs) g_up_texs = [NSMutableArray array];
    if (!g_up_keep) g_up_keep = [NSMutableArray array];
}

// Diagnostic readout (low frequency; printed only when the block switch count changed and ≥ 1 second since the last line, at most 200 lines per game): how many times an upload batch ran out of staging block and switched blocks midway; GPU allocated, residency set object count, deferred-release queue length.
// If the old block is not retired on a switch it stays in the residency set and can never be freed, and "GPU allocated" climbs one block at a time with the switch count
static uint64_t g_up_blocks_new, g_up_swaps, g_up_swap_bytes; static NSHashTable *g_up_old;   // g_up_old: old blocks switched out (weak references); the diagnostic line counts how many are still alive
static void up_diag(void) {
    // Print a line when the block switch count changes; then one more line 5 seconds later (that batch's commit is long done and deferred retirement drained, so "GPU allocated" is the settled number)
    static uint64_t said_swaps = ~0ull, last_ns; static int lines, follow; uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC);
    int chg = g_up_swaps != said_swaps, fol = follow && now - last_ns >= 5000000000ull; if ((!chg && !fol) || lines >= 200) return;
    if (chg && last_ns && now - last_ns < 1000000000ull) return; last_ns = now; said_swaps = g_up_swaps; lines++; follow = chg;
    unsigned long alive = 0, inres = 0; uint64_t abytes = 0; NSArray *olds = g_up_old.allObjects;
    pthread_mutex_lock(&g_res_mu); unsigned long nres = (unsigned long)g_res_objects.allObjects.count; for (id<MTLBuffer> o in olds) { alive++; abytes += o.length; inres += [g_res_objects containsObject:o]; } pthread_mutex_unlock(&g_res_mu);
    pthread_mutex_lock(&g_deferred_mu); unsigned long ndef = (unsigned long)g_deferred.count; pthread_mutex_unlock(&g_deferred_mu);
    MLOG("upload staging blocks%s: %llu created in total, swapped within a batch %llu times (%llu MB of old blocks swapped out); swapped-out blocks still alive now %lu, %llu MB, still in the residency set %lu; GPU allocated %llu MB, residency set %lu objects, deferred-release queue %lu", fol && !chg ? " (settled)" : "",
         (unsigned long long)g_up_blocks_new, (unsigned long long)g_up_swaps, (unsigned long long)(g_up_swap_bytes >> 20), alive, (unsigned long long)(abytes >> 20), inres, (unsigned long long)(g_dev.currentAllocatedSize >> 20), nres, ndef);
}
static id<MTLBuffer> up_alloc(size_t n, size_t *off) {
    size_t need = (n + 255u) & ~((size_t)255u);
    if (!g_up_block && need <= UP_BLOCK_SIZE) {   // first allocation of the batch: the slot's own block, grown to the next power of two that holds it (a movie frame re-uploaded every frame, 8 MB in Link's Awakening)
        __strong id<MTLBuffer> *sb = &g_up_slot_blk[g_up_cur_slot];
        size_t want = UP_SLOT_BLOCK; while (want < need) want <<= 1;
        if (*sb && (*sb).length < want) { defer_obj_release(*sb); *sb = nil; }   // the slot's previous batch has finished (up_begin waited for it)
        if (!*sb) { *sb = [g_dev newBufferWithLength:want options:MTLResourceStorageModePrivate]; if (!*sb) MDIE("failed to create the swizzled-upload staging block %#zx", want); res_add(*sb); g_up_blocks_new++; }
        g_up_block = *sb; g_up_block_size = (*sb).length; g_up_block_off = 0;
    }
    if (!g_up_block || g_up_block_off + need > g_up_block_size) {
        // Block switch: the old block is still referenced by copy / decompress commands already recorded in this batch and cannot be freed on the spot; hand it to g_up_keep, to be retired later with the other staging after the flush commits (removed from the residency set on retire).
        // Previously g_up_block was simply replaced with the new block here: nobody retired the old block, yet it stayed in the residency set, so each extra block switch in an upload batch leaked one block (usually 32 MB) of GPU memory.
        // The old block and the "current block" retired at flush are two objects, so nothing is retired twice
        g_up_blocks_new++; if (g_up_block) { g_up_swaps++; g_up_swap_bytes += g_up_block_size; if (!g_up_old) g_up_old = [NSHashTable weakObjectsHashTable]; [g_up_old addObject:g_up_block]; static int leak = -1; if (leak < 0) leak = getenv("DM_TEST_UP_LEAK") != NULL;   // test switch DM_TEST_UP_LEAK=1: do not retire the old block, as before the fix (for comparison, to measure the leak)
            if (!leak && g_up_block != g_up_slot_blk[g_up_cur_slot]) { if (!g_up_keep) g_up_keep = [NSMutableArray array]; [g_up_keep addObject:g_up_block]; } }   // the slot's block is not retired: it stays for the slot's later batches
        g_up_block_size = n > UP_BLOCK_SIZE ? need : UP_BLOCK_SIZE;
        g_up_block = [g_dev newBufferWithLength:g_up_block_size options:MTLResourceStorageModePrivate];
        if (!g_up_block) MDIE("failed to create the swizzled-upload staging block %#zx", g_up_block_size);
        res_add(g_up_block);
        g_up_block_off = 0;
    }
    *off = g_up_block_off;
    g_up_block_off += need;
    return g_up_block;
}

// Deswizzle parameter blocks: 64 bytes per cell, cut sequentially from a 64 KB shared-storage block; a full block is replaced by a new one, and the old block is released lazily by fence
static id<MTLBuffer> g_up_pblock; static size_t g_up_pblock_off;
static id<MTLBuffer> up_param_alloc(size_t *off) {
    if (!g_up_pblock || g_up_pblock_off + 64 > g_up_pblock.length) {
        if (g_up_pblock) defer_obj_release(g_up_pblock);
        g_up_pblock = [g_dev newBufferWithLength:64 << 10 options:MTLResourceStorageModeShared];
        if (!g_up_pblock) MDIE("failed to create the deswizzle parameter block");
        res_add(g_up_pblock); g_up_pblock_off = 0;
    }
    *off = g_up_pblock_off; g_up_pblock_off += 64;
    return g_up_pblock;
}
static void up_blit_add(id<MTLBuffer> src, size_t off, size_t row, size_t image, MTLSize size,
                        id<MTLTexture> dst, NSUInteger slice, NSUInteger level) {
    if (g_nup_blits == g_up_blitcap) {
        g_up_blitcap = g_up_blitcap ? g_up_blitcap * 2 : 256;
        g_up_blits = realloc(g_up_blits, g_up_blitcap * sizeof *g_up_blits);
    }
    g_up_blits[g_nup_blits++] = (up_blit){src, off, row, image, size, dst, slice, level};
}

static id<MTLTexture> pkg_upload_(const tex_t *t, uint64_t *stamp, id<MTLTexture> reuse);
int nvn_host_readable(uint64_t a, uint64_t n);   // hle_nvn.c: guest memory and sparse pools
int nvn_sparse_pool(const void *pool); uint64_t nvn_sparse_gen(void); uint64_t nvn_sparse_copy(const void *pool, void *dst, uint64_t a, uint64_t n);
int nvn_sparse_since(const void *pool, uint64_t a, uint64_t n, uint64_t gen, uint64_t stamp); void nvn_sparse_protect(const void *pool, uint64_t a, uint64_t n); int nvn_sparse_mapped(const void *pool, uint64_t a, uint64_t n);
static id<MTLTexture> pkg_upload(const tex_t *t, uint64_t *stamp, id<MTLTexture> reuse) {   // reuse: a texture of the same description, idle on the GPU, to upload into instead of a new one
    struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
    id<MTLTexture> r = pkg_upload_(t, stamp, reuse);
    clock_gettime(CLOCK_MONOTONIC, &b); uint64_t d_ = (uint64_t)((b.tv_sec - a.tv_sec) * 1000000000ll + b.tv_nsec - a.tv_nsec);
    atomic_fetch_add(&g_upload_ns, d_); g_up_total_ns += d_;
    return r;
}
uint64_t mtl_upload_ns_take(void) { return atomic_exchange(&g_upload_ns, 0); }
uint64_t mtl_upload_gpu_ns_take(void) { return atomic_exchange(&g_upload_gpu_ns, 0); }
void mtl_create_stats(uint64_t *lib, uint64_t *pso) { *lib = g_bp_lib_n; *pso = g_bp_pso_n; }
void mtl_create_ns(uint64_t *lib_ns, uint64_t *pso_ns) { *lib_ns = g_bp_lib_ns; *pso_ns = g_bp_pso_ns; }   // Cumulative time spent synchronously creating shader libraries / pipelines on the spot (the long-frame log takes the per-frame difference)
uint64_t mtl_upload_bytes(void) { return g_bp_up_bytes; }   // Cumulative texture upload bytes
// Diagnostic DM_NVN_ZBLOCK=1: all-zero 16-byte blocks of each ASTC texture uploaded on the CPU, per level (an all-zero ASTC block decodes as the error colour, magenta). tex_mtl_ logs
// a texture whose bound levels hold any (zb_report), with each level's zero blocks, its total and whether all its pages are mapped (sparse pools)
static int zblock_on(void) { static int v = -1; if (v < 0) { v = getenv("DM_NVN_ZBLOCK") != NULL; if (v) MLOG("DM_NVN_ZBLOCK: counting all-zero blocks of ASTC textures at upload (diagnostic)"); } return v; }
static uint64_t g_zb_ser; static uint32_t g_zb[16], g_zb_tot[16]; static int g_zb_n;
static void zb_report(const tex_t *t, unsigned base, const char *why) {
    if (!zblock_on() || g_zb_ser != t->serial) return;
    int bad = 0; for (int l = (int)base; l < g_zb_n; l++) bad |= g_zb[l] != 0;
    g_zb_ser = 0; if (!bad) return;
    static int nl; if (nl++ >= 400) return;
    char b[512]; size_t o = 0; int sp = t->pool && nvn_sparse_pool(t->pool);
    for (int l = 0; l < g_zb_n && o < sizeof b - 48; l++) { uint64_t lo = nvn_tex_level_offset(&t->d, (unsigned)l), hi = l + 1 < (int)t->d.levels ? nvn_tex_level_offset(&t->d, (unsigned)l + 1) : t->L.size;
        o += (size_t)snprintf(b + o, sizeof b - o, " L%d %u/%u%s", l, g_zb[l], g_zb_tot[l], sp ? (nvn_sparse_mapped(t->pool, (uint64_t)(uintptr_t)t->mem + lo, hi - lo) ? "" : " unmapped") : ""); }
    MLOG("[zb] %s ser=%llu format %#x %ux%u levels %u flags %#x mem=%p size %#llx sparse %d bound from level %u frame %llu:%s", why, (unsigned long long)t->serial, t->d.format, t->d.width, t->d.height, t->d.levels,
         t->d.flags, t->mem, (unsigned long long)t->L.size, sp, base, (unsigned long long)atomic_load(&g_cur_frame), b);
    const char *dir = getenv("DM_NVN_ZBLOCK"); static int nd; static uint64_t seen[16];   // DM_NVN_ZBLOCK=<folder>: also the raw storage of the first such texture of each format and size (up to 16), zb_<serial>.bin (unmapped pages as 0)
    uint64_t kind = (uint64_t)t->d.format << 48 | (uint64_t)t->d.width << 24 | t->d.height; int dup = 0; for (int i = 0; i < nd; i++) dup |= seen[i] == kind;
    if (dir && dir[0] == '/' && nd < 16 && !dup) { seen[nd] = kind; uint64_t n = sp ? MAX(t->L.size, 2u << 20) : t->L.size; uint8_t *m = malloc(n); char fp[1024]; FILE *f;   // a sparse texture: 2 MB, and zbm_<serial>.bin a byte per 4 KB page (1 mapped)
        if (m) { if (sp) nvn_sparse_copy(t->pool, m, (uint64_t)(uintptr_t)t->mem, n); else memcpy(m, t->mem, n);
            snprintf(fp, sizeof fp, "%s/zb_%llu.bin", dir, (unsigned long long)t->serial); if ((f = fopen(fp, "wb"))) { fwrite(m, 1, n, f); fclose(f); nd++; }
            if (sp) { for (uint64_t o = 0; o < n; o += 0x1000) m[o >> 12] = (uint8_t)nvn_sparse_mapped(t->pool, (uint64_t)(uintptr_t)t->mem + o, 0x1000);
                snprintf(fp, sizeof fp, "%s/zbm_%llu.bin", dir, (unsigned long long)t->serial); if ((f = fopen(fp, "wb"))) { fwrite(m, 1, n >> 12, f); fclose(f); } }
            free(m); }
        MLOG("[zb] ser=%llu: block height log2 %d (packaged %d), stride %u, depth %u, target %d, samples %d", (unsigned long long)t->serial, (int)t->L.tileH, t->d.pkg_block_h, t->d.stride, t->d.depth, t->d.target, t->d.samples); }
}
static id<MTLTexture> pkg_upload_(const tex_t *t, uint64_t *stamp, id<MTLTexture> reuse) {
    MTLTextureDescriptor *d = tex_desc(t, 1);
    if (!d) return nil;
    id<MTLTexture> mt = reuse && reuse.pixelFormat == d.pixelFormat && reuse.width == d.width && reuse.height == d.height && reuse.mipmapLevelCount == d.mipmapLevelCount && reuse.textureType == d.textureType
                        ? reuse : [g_dev newTextureWithDescriptor:d];
    if (!mt) { RED_ONCE(2, "failed to create MTLTexture %ux%u format %#x target %d", t->d.width, t->d.height, t->d.format, t->d.target); return nil; }
    if (g_allocprof > 0) { static int n; if (n++ < 30 || n % 500 == 0) MLOG("new texture (%s) #%d ser=%llu %ux%ux%u format %#x target %d levels %u storage %p", __func__, n, (unsigned long long)t->serial, t->d.width, t->d.height, t->d.depth, t->d.format, t->d.target, t->d.levels, (void *)t->mem); }
    res_add(mt);   // Both first upload and re-upload register residency here (Metal 4 does not make resources resident automatically; a missed registration = GPU address page fault)
    int sparse = t->pool && nvn_sparse_pool(t->pool);
    if (stamp) {   // no stamp: a one-time upload of content the CPU will not change (tex_init_fill), with no write tracking
        if (sparse) nvn_sparse_protect(t->pool, (uint64_t)(uintptr_t)t->mem, t->L.size); else nvn_dirty_protect(t->mem, t->L.size);
        *stamp = nvn_dirty_now(); }
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    unsigned layers = (t->d.target == 4 || t->d.target == 9) ? t->d.depth : t->d.target == 8 ? 6 : t->d.target == 3 ? t->d.height : 1;   // a cube array's depth counts layer-faces (tex_desc)
    size_t total = 0;
    for (unsigned l = 0; l < d.mipmapLevelCount; l++) {
        uint32_t w = MAX(t->d.width >> l, 1u), h = MAX(t->d.height >> l, 1u), z = t->d.target == 2 ? MAX(t->d.depth >> l, 1u) : 1;
        total += (size_t)((w + bw - 1) / bw) * bpb * ((h + bh - 1) / bh) * z * layers;
    }
    g_bp_up_bytes += total;
    void *mb; uint64_t mo;
    int use_gpu = !sparse && g_nvn_deswz_gpu && mtl_addr((uint64_t)t->mem, t->L.size, &mb, &mo);   // a sparse pool's texture is gathered through its page table on the CPU
    if (g_nvn_deswz_gpu && !use_gpu && !sparse) RED_ONCE(40, "swizzled texture storage is not in a memory pool; falling back to the CPU");
    if (use_gpu) {
        // The GPU reads the memory pool at execution time, which is closer to a real Switch than a snapshot at CPU upload time; the dirty-page stamp is still taken before encoding, and later writes still trigger a re-upload.
        up_begin();
        size_t stg_off;
        id<MTLBuffer> stg = up_alloc(total ? total : 16, &stg_off);
        size_t off = 0; int enc_ok = g_up_ce != nil, nb0 = g_nup_blits;
        for (unsigned l = 0; enc_ok && l < d.mipmapLevelCount; l++) {
            uint32_t w = MAX(t->d.width >> l, 1u), h = MAX(t->d.height >> l, 1u), z = t->d.target == 2 ? MAX(t->d.depth >> l, 1u) : 1;
            size_t pitch = (size_t)((w + bw - 1) / bw) * bpb, rows = (h + bh - 1) / bh;
            for (unsigned y = 0; y < layers; y++) {
                size_t poff; id<MTLBuffer> params = up_param_alloc(&poff);   // Metal 4 has no setBytes: the parameter block of each deswizzle is cut from a shared block (no new GPU buffer each time)
                if (nvn_deswz_gpu_encode_m4(g_up_ce, g_up_ct, params, poff, (__bridge id<MTLBuffer>)mb, mo, &t->d, l, y, stg, stg_off + off, pitch)) { enc_ok = 0; break; }
                up_blit_add(stg, stg_off + off, pitch, pitch * rows, MTLSizeMake(w, h, z), mt, y, l);
                off += pitch * rows * z;
            }
        }
        if (enc_ok) {
            [g_up_texs addObject:mt];
            if (!g_cb) up_flush();
            return mt;
        }
        g_nup_blits = nb0;   // Roll back the copies already recorded for this texture: the already-encoded deswizzle only writes the staging area and is harmless; this texture takes the CPU path below instead
        if (!g_cb) up_flush();
        RED_ONCE(41, "GPU deswizzle does not support format %#x target %d; falling back to the CPU", t->d.format, t->d.target);
    }

    // A sparse pool's texture is read through a copy gathered from its page table, unmapped pages as 0 (a mip tail at a 4 KB offset has no host alias; reading the alias stopped the host with SIGBUS)
    const uint8_t *src = t->mem; uint8_t *part = NULL;
    if (sparse) {
        if (!(part = malloc(t->L.size ? t->L.size : 1))) MDIE("sparse texture copy of %llu bytes failed", (unsigned long long)t->L.size);
        nvn_sparse_copy(t->pool, part, (uint64_t)(uintptr_t)t->mem, t->L.size); src = part;
    }
    up_begin();
    id<MTLBuffer> stg = [g_dev newBufferWithLength:total ? total : 16 options:MTLResourceStorageModeShared];
    res_add(stg); [g_up_keep addObject:stg];
    size_t off = 0;
    int zb = zblock_on() && t->d.format >= 0x79 && t->d.format <= 0x94; if (zb) { g_zb_ser = t->serial; g_zb_n = (int)MIN(d.mipmapLevelCount, 16u); memset(g_zb, 0, sizeof g_zb); memset(g_zb_tot, 0, sizeof g_zb_tot); }
    for (unsigned l = 0; l < d.mipmapLevelCount; l++) {
        uint32_t w = MAX(t->d.width >> l, 1u), h = MAX(t->d.height >> l, 1u), z = t->d.target == 2 ? MAX(t->d.depth >> l, 1u) : 1;
        size_t pitch = (size_t)((w + bw - 1) / bw) * bpb, rows = (h + bh - 1) / bh;
        for (unsigned y = 0; y < layers; y++) {
            if (nvn_deswizzle(&t->d, l, y, src, (uint8_t *)stg.contents + off, pitch)) { RED_ONCE(9, "deswizzle failed format %#x target %d", t->d.format, t->d.target); break; }
            if (zb && l < 16) { const uint64_t *q = (const uint64_t *)((const uint8_t *)stg.contents + off); size_t nb = pitch * rows * z / 16;
                for (size_t i = 0; i < nb; i++) g_zb[l] += !(q[2 * i] | q[2 * i + 1]);
                g_zb_tot[l] += (uint32_t)nb; }
            up_blit_add(stg, off, pitch, pitch * rows, MTLSizeMake(w, h, z), mt, y, l);
            off += pitch * rows * z;
        }
    }
    free(part);
    up_flush();
    return mt;
}
// Zero newly created non-packaged textures: a new Metal texture's content is undefined, while a freshly mapped memory pool on the Switch reads as 0. Games depend on it: SMBW's fountain water surface uses a 512×1 RGBA16F wave texture
// that every frame is "read last frame → render into a render target → copy back" and is never cleared; the undefined initial value keeps swinging through the feedback and pushes the water vertices to the top of the screen (the tall panels above the fountain).
// Copied level by level / slice by slice from one shared all-zero buffer, executed with the upload batch (ordered before this replay). Multisample, depth / stencil and block-compressed formats are not cleared (games always clear them before use, and they cannot be copied into anyway).
static size_t mtl_pf_bytes(MTLPixelFormat pf) {
    switch ((int)pf) {
        case 10: case 11: case 12: case 13: case 14: return 1;                                   // R8
        case 20: case 22: case 23: case 24: case 25: case 30: case 31: case 32: case 33: case 34: return 2;   // R16 / RG8
        case 40: case 41: case 42: case 43: return 2;                                            // packed 16-bit colour
        case 53: case 54: case 55: case 60: case 62: case 63: case 64: case 65:
        case 70: case 71: case 72: case 73: case 74: case 80: case 81:
        case 90: case 91: case 92: case 93: case 94: return 4;                                   // R32 / RG16 / RGBA8 / BGRA8 / 10-10-10-2 / 11-11-10 / 9-9-9-5
        case 103: case 104: case 105: case 110: case 112: case 113: case 114: case 115: return 8;   // RG32 / RGBA16
        case 123: case 124: case 125: return 16;                                                 // RGBA32
    }
    return 0;
}
// Shared all-zero buffer (for zeroing new textures and scissored zero clears): replaced by a bigger one when too small; the old one is released after in-flight submissions finish
static id<MTLBuffer> zero_buf(size_t need) {
    static id<MTLBuffer> zb; static size_t zn;
    if (need > zn) {
        size_t n = MAX(need, (size_t)4 << 20);
        id<MTLBuffer> b = [g_dev newBufferWithLength:n options:MTLResourceStorageModeShared];
        if (!b) return nil;
        memset(b.contents, 0, n); res_add(b);
        if (zb) defer_obj_release(zb);
        zb = b; zn = n;
    }
    return zb;
}
static uint64_t g_tex_zeroed;
static void tex_zero_fill(id<MTLTexture> mt) {
    static int warned;
    if (!atomic_load(&g_sub_thread_set) || !on_sub_thread()) return;   // the upload batch belongs to the submit thread only
    MTLTextureType tt = mt.textureType;
    if (tt == MTLTextureType2DMultisample || tt == MTLTextureType2DMultisampleArray || tt == MTLTextureTypeTextureBuffer) return;
    size_t bpp = mtl_pf_bytes(mt.pixelFormat);
    if (!bpp) { if (!warned++) MLOG("new-texture zeroing: pixel format %lu not cleared (depth / stencil / compressed etc.)", (unsigned long)mt.pixelFormat); return; }
    NSUInteger slices = mt.arrayLength * (tt == MTLTextureTypeCube || tt == MTLTextureTypeCubeArray ? 6 : 1);
    size_t need = (size_t)mt.width * bpp * mt.height * mt.depth;   // level 0, one slice is the largest
    if (need > (64u << 20)) { static int big; if (!big++) MLOG("new-texture zeroing: %lux%lux%lu exceeds 64 MB, not cleared", (unsigned long)mt.width, (unsigned long)mt.height, (unsigned long)mt.depth); return; }
    id<MTLBuffer> zb = zero_buf(need); if (!zb) return;
    up_begin();
    for (NSUInteger l = 0; l < mt.mipmapLevelCount; l++) {
        NSUInteger w = MAX(mt.width >> l, 1u), h = MAX(mt.height >> l, 1u), z = tt == MTLTextureType3D ? MAX(mt.depth >> l, 1u) : 1;
        for (NSUInteger y = 0; y < slices; y++) up_blit_add(zb, 0, w * bpp, w * bpp * h, MTLSizeMake(w, h, z), mt, y, l);
    }
    [g_up_texs addObject:mt];
    if (!g_cb) up_flush();
    if (!g_tex_zeroed++) MLOG("new-texture zeroing: on (new Metal textures are undefined; new Switch memory reads 0)");
}

// Initial content: what a memory pool's storage holds at nvnMemoryPoolInitialize is the pool's content, also in a pool the CPU cannot access afterwards (CPU_NO_ACCESS), where the
// game never writes it again. Rhythm Heaven Groove reads each texture file into memory and makes a CPU_NO_ACCESS | GPU_CACHED pool on it (245 by its first screens), with BC7 textures
// that have no packaged layout; as GPU textures of zeros its sprite shader discarded every fragment (alpha 0) and the screen stayed black. A new texture in such a pool whose storage
// holds data is filled from it once, as a GPU texture that can still be rendered into (no re-upload on page writes, unlike packaged / CPU-written ones). Left out: storage of zeros
// (the zero fill does the same), COMPRESSIBLE textures (their memory is meaningless without the compression tags), multisample, depth / stencil and sparse ones.
// In a pool the CPU can write, the same holds for storage the game flushed (nvnMemoryPoolFlushMappedRange) before the texture's first use, when the GPU has not written it yet
// (nvn_flushed in hle_nvn.c: Breath of the Wild's sky scattering tables, copied into their pools and flushed; without them the sky was brownish grey). DM_NVN_FLUSH_FILL=0 leaves
// those out, as before
int nvn_flushed(const void *p, uint64_t n);   // hle_nvn.c
static int mem_any(const uint8_t *p, uint64_t n) {   // whether any byte is non-zero (stops at the first)
    uint64_t i = 0, v;
    for (; i + 8 <= n; i += 8) { memcpy(&v, p + i, 8); if (v) return 1; }
    for (; i < n; i++) if (p[i]) return 1;
    return 0;
}
static int tex_init_fill(const tex_t *t, id<MTLTexture> mt) {
    if (!t->mem || !t->pool || (t->pool->flags & NVN_MEMORY_POOL_FLAGS_VIRTUAL) || t->has_layout) return 0;
    if (t->d.samples > 1 || (t->d.flags & NVN_TEXTURE_FLAGS_COMPRESSIBLE) || ((int)mt.pixelFormat >= 250 && (int)mt.pixelFormat <= 262)) return 0;   // 250..262: Depth16Unorm … X24_Stencil8 (some names are macOS-only)
    int no_cpu = (t->pool->flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS) != 0;
    static int flush_fill = -1; if (flush_fill < 0) { const char *e = getenv("DM_NVN_FLUSH_FILL"); flush_fill = !e || atoi(e) != 0; }
    if (!no_cpu && (!flush_fill || !nvn_flushed(t->mem, t->L.size))) return 0;
    if (!atomic_load(&g_sub_thread_set) || !on_sub_thread() || !mem_any(t->mem, t->L.size)) return 0;   // the upload batch belongs to the submit thread
    if (pkg_upload(t, NULL, mt) != mt) return 0;
    static uint64_t n[2]; if (n[no_cpu]++ < 4) MLOG("initial content: ser=%llu %ux%ux%u format %#x filled from its storage, %s (%llu so far)", (unsigned long long)t->serial, t->d.width, t->d.height, t->d.depth, t->d.format,
                                                    no_cpu ? "a CPU_NO_ACCESS pool's" : "flushed by the CPU before its first use", (unsigned long long)n[no_cpu]);
    return 1;
}

static int clear_rect_zero(int i);
// Linear textures (LINEAR) in a CPU-visible pool: the game writes the pool memory directly from the CPU (SMBW's glyph cache: 2048×8192 R8, IMAGE|LINEAR, pool CPU_UNCACHED|GPU_CACHED,
// the font engine rasterizes glyphs into it on the CPU). A GPU-private texture never sees those writes → all menu / dialog text is blank. On unified memory, create a linear texture directly on the pool buffer (zero copy): the GPU reads the current content at execution time, same as NVN.
static id<MTLTexture> tex_linear(const tex_t *t) {
    // LINEAR_RENDER_TARGET is a linear layout too (SMBW's sky colour ramps: four 64×1 RGBA16F, written by the CPU every frame). Also in a pool the CPU cannot access, whose buffer holds what
    //   its storage held at creation (see tex_init_fill): Rhythm Heaven Groove's 228 linear R8 textures (sprite masks), read from files into CPU_NO_ACCESS pools, were private textures of zeros
    if (!(t->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)) || !t->mem || !t->pool) return nil;
    if (t->d.target != NVN_TEXTURE_TARGET_2D || t->d.levels > 1 || t->d.samples > 1) return nil;
    uint32_t pf = 0; uint8_t swz[4]; int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    if (!nvn_fmt_to_mtl(t->d.format, &pf, swz) || !pf || !bpb || bw != 1 || bh != 1) return nil;
    size_t row = t->d.stride ? t->d.stride : (size_t)t->d.width * bpb;
    void *mb; uint64_t mo;
    if (!mtl_addr((uint64_t)t->mem, (uint64_t)row * (t->d.height ? t->d.height : 1), &mb, &mo)) { RED_ONCE(64, "linear texture storage is not in a memory pool (%ux%u format %#x)", t->d.width, t->d.height, t->d.format); return nil; }
    id<MTLBuffer> b = (__bridge id<MTLBuffer>)mb;
    NSUInteger al = [g_dev minimumLinearTextureAlignmentForPixelFormat:(MTLPixelFormat)pf];
    if (mo % al || row % al) { RED_ONCE(65, "linear texture offset %#llx / row pitch %zu not aligned to %lu bytes; created as a private texture (CPU writes invisible)", (unsigned long long)mo, row, (unsigned long)al); return nil; }
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:(MTLPixelFormat)pf width:t->d.width ? t->d.width : 1 height:t->d.height ? t->d.height : 1 mipmapped:NO];
    d.resourceOptions = b.resourceOptions;
    d.usage = MTLTextureUsageShaderRead | (t->d.flags & NVN_TEXTURE_FLAGS_IMAGE ? MTLTextureUsageShaderWrite : 0) | (t->d.flags & NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET ? MTLTextureUsageRenderTarget : 0);
    return [b newTextureWithDescriptor:d offset:mo bytesPerRow:row];
}
// CPU-written block-linear textures (WriteTexels): the game gave no packaged layout and changes the content from the CPU each frame (Engage's movie frames: a Y plane R8 + a UV plane RG8, WriteTexels every frame).
// Only packaged textures used to be uploaded from guest memory, so such a texture became a GPU-only texture whose content stayed 0. Their serials are recorded and they resolve through the packaged path (upload + re-upload on dirty pages)
static pthread_mutex_t g_cpu_tex_mu = PTHREAD_MUTEX_INITIALIZER; static CFMutableSetRef g_cpu_tex;
// The GPU owns the texture's content: its host texture was made to be rendered into or written by shaders (nvn_ft_poll in hle_nvn.c never re-uploads such a texture from guest memory)
int mtl_tex_gpu_owned(uint64_t serial) {
    __unsafe_unretained id o = tex_get(serial);
    if (!o || ![o conformsToProtocol:@protocol(MTLTexture)]) return 0;
    return (((id<MTLTexture>)o).usage & (MTLTextureUsageRenderTarget | MTLTextureUsageShaderWrite)) != 0;
}
void mtl_tex_cpu_written(const tex_t *t) {
    uint64_t serial = t->serial;
    if (!serial) return;
    pthread_mutex_lock(&g_cpu_tex_mu);
    if (!g_cpu_tex) g_cpu_tex = CFSetCreateMutable(NULL, 0, NULL);
    int added = 0;
    if (!CFSetContainsValue(g_cpu_tex, (const void *)(uintptr_t)serial)) {
        CFSetAddValue(g_cpu_tex, (const void *)(uintptr_t)serial); added = 1;
        static int n; if (n++ < 16) MLOG("CPU-written texture ser=%llu %ux%ux%u format %#x target %d levels %u: uploaded / re-uploaded like a swizzled texture", (unsigned long long)serial, t->d.width, t->d.height, t->d.depth, t->d.format, t->d.target, t->d.levels);
    }
    pthread_mutex_unlock(&g_cpu_tex_mu);
    if (added) { pthread_mutex_lock(&g_smu); g_ser_gen++; pthread_mutex_unlock(&g_smu); }   // the texture IDs that resolve to it take the slow path, which now treats it as packaged (tex_by_id view_pkg)
}
static int tex_cpu_written(uint64_t serial) {
    pthread_mutex_lock(&g_cpu_tex_mu); int r = g_cpu_tex && CFSetContainsValue(g_cpu_tex, (const void *)(uintptr_t)serial); pthread_mutex_unlock(&g_cpu_tex_mu);
    return r;
}
static id<MTLTexture> tex_mtl_(const tex_t *ct);
static id<MTLTexture> tex_mtl(const tex_t *ct) { AP_B; id<MTLTexture> r = tex_mtl_(ct); AP_E(4); return r; }
// A sparse texture is bound from its first level whose pages, and those of every smaller level, are all mapped: a view of levels [r, n) samples as with a minimum LOD of r.
// The translator drops TEX's LOD clamp and residency predicate, so Monster Hunter Rise's streamed textures were sampled at levels it had not mapped yet; those read 0, which ASTC
// decodes as its error colour (magenta clothes on the title screen). Single-layer 2D textures only; the view follows each upload, as the mappings change only with one
static void sparse_view(NvPkg *pk, const tex_t *t) {
    unsigned n = (unsigned)pk->tex.mipmapLevelCount, r = n;
    if (t->d.target == NVN_TEXTURE_TARGET_2D && n > 1)
        while (r > 0) { uint64_t lo = nvn_tex_level_offset(&t->d, r - 1), hi = r < n ? nvn_tex_level_offset(&t->d, r) : t->L.size;
            if (!nvn_sparse_mapped(t->pool, (uint64_t)(uintptr_t)t->mem + lo, hi - lo)) break;
            r--; }
    else r = 0;
    if (r == n) r = n - 1;   // nothing mapped: the smallest level (reads 0)
    if (pk->view) defer_obj_unref(pk->view);   // a view shares its texture's allocation: drop only the reference, the residency set keeps the texture
    pk->view = r ? [pk->tex newTextureViewWithPixelFormat:pk->tex.pixelFormat textureType:pk->tex.textureType levels:NSMakeRange(r, n - r) slices:NSMakeRange(0, 1)] : nil;
    if (r != (unsigned)pk->vbase) { static int nl; if (nl++ < 6) MLOG("sparse texture ser=%llu %ux%u: sampled from level %u of %u (finer levels not mapped)", (unsigned long long)t->serial, t->d.width, t->d.height, r, n); }
    pk->vbase = (int)r;
}
static uint64_t g_texinfo_ser[8]; static int g_texinfo_n;
static id<MTLTexture> tex_mtl_(const tex_t *ct) {
    tex_t *t = (tex_t *)ct;
    if (!t || !t->serial) return nil;
    if (t->magic != MAGIC('T', 'E', 'X', 0)) { RED_ONCE(70, "texture object no longer valid (magic %#x, first case ser=%llu): no MTLTexture created", t->magic, (unsigned long long)t->serial); return nil; }
    // A sampled texture in a sparse virtual pool holds what the CPU wrote into the physical pages mapped there (Monster Hunter Rise: the hunter's innerwear, ASTC 6x6 with no packaged
    // layout, came out magenta, and the face black): it is read through the page table like a packaged one. Render targets, images, linear and buffer textures stay GPU textures (as ft_add)
    int packaged = (t->has_layout || tex_cpu_written(t->serial) || (t->pool && (t->pool->flags & NVN_MEMORY_POOL_FLAGS_VIRTUAL) && t->d.target != NVN_TEXTURE_TARGET_BUFFER
                    && !(t->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET | NVN_TEXTURE_FLAGS_COMPRESSIBLE | NVN_TEXTURE_FLAGS_IMAGE)) && nvn_sparse_pool(t->pool))) && t->mem;
    {   // Diagnostic DM_NVN_TEXINFO=WxH: log storage and description of textures of that size on first resolution (to tell whether two textures share storage and why they were not merged)
        static int ti = -1; static uint32_t tw, th; static uint64_t last[64]; static int nl;
        if (ti > 0 && t->d.width == tw && t->d.height == th && g_texinfo_n < 8) { int dup = 0; for (int i = 0; i < g_texinfo_n; i++) dup |= g_texinfo_ser[i] == t->serial; if (!dup && !(t->d.flags & 8)) g_texinfo_ser[g_texinfo_n++] = t->serial; }   // remember the non-render-target ones (no COMPRESSIBLE) for DM_NVN_TEXRAW=info
        if (ti < 0) { const char *e = getenv("DM_NVN_TEXINFO"); ti = e && sscanf(e, "%ux%u", &tw, &th) == 2; }
        if (ti && t->d.width == tw && t->d.height == th) {
            int seen = 0; for (int i = 0; i < nl; i++) if (last[i] == t->serial) seen = 1;
            static uint64_t n; if (!seen && (n++ < 40 || n % 600 == 0)) { if (nl < 64) last[nl++] = t->serial; else last[n % 64] = t->serial;
                uint64_t nz = 0; if (t->mem && t->L.size <= (64u << 20)) { uint8_t *m = t->mem, *part = NULL;   // non-zero bytes in guest memory: content the CPU wrote (a sparse pool's texture through its page table)
                    if (t->pool && nvn_sparse_pool(t->pool) && (part = malloc(t->L.size))) { nvn_sparse_copy(t->pool, part, (uint64_t)(uintptr_t)t->mem, t->L.size); m = part; }
                    for (uint64_t i = 0; i < t->L.size; i++) nz += m[i] != 0;
                    free(part); }
                MLOG("TEXINFO ser=%llu swizzled=%d has_layout=%d mem=%p pool=%p(%#x) off=%#llx target=%d format=%#x flags=%#x samples=%d %ux%ux%u levels=%u stride=%u block_h=%d storage %llu bytes non-zero %llu",
                     (unsigned long long)t->serial, packaged, t->has_layout, t->mem, t->pool, t->pool ? t->pool->flags : 0, (unsigned long long)t->off, t->d.target, t->d.format, t->d.flags, t->d.samples,
                     t->d.width, t->d.height, t->d.depth, t->d.levels, t->d.stride, t->d.pkg_block_h, (unsigned long long)t->L.size, (unsigned long long)nz); }
        }
    }
    id have = tex_get(t->serial);
    if (!packaged && !have && DM_GAME_TEX_DIRTY_UPLOAD && t->mem && on_sub_thread()) { int nvn_ft_written(const tex_t *); packaged = nvn_ft_written(t); }   // first use of a texture the CPU wrote since init without a flush (hle_nvn.c)
    if (packaged) {
        NvPkg *pk = [have isKindOfClass:[NvPkg class]] ? have : nil;   // resolved as GPU-only before the CPU wrote it: dropped and rebuilt as packaged
        if (pk) {   // Already uploaded: check the covered pages only if there has been a guest write since the last check
            uint64_t now = nvn_dirty_now();
            int tracked = pk->span != NULL;   // kept from the upload: a texture's pool stays registered while the texture lives
            int clean = tracked && (now == pk->seen || !nvn_dirty_since_span(pk->span, t->mem, t->L.size, pk->stamp));
            if (pk->sparse) { uint64_t g = nvn_sparse_gen(); clean = (now == pk->seen && g == pk->vseen) || !nvn_sparse_since(t->pool, (uint64_t)(uintptr_t)t->mem, t->L.size, pk->vgen, pk->stamp); pk->vseen = g; }
            pk->seen = now;
            if (clean && atomic_load_explicit(&g_up_lost_n, memory_order_relaxed) && up_lost_take(pk->tex)) { clean = 0; uint64_t k = atomic_load(&g_up_lost_redo); if (k <= 3 || !atomic_load(&g_up_lost_n)) MLOG("re-uploading a texture whose upload was dropped ser=%llu (%llu in total, %d left in the record)", (unsigned long long)t->serial, (unsigned long long)k, atomic_load(&g_up_lost_n)); }
            if (clean) {
                if (dirtycheck() && !pk->sparse && full_hash(t->mem, t->L.size) != pk->hash && !g_pkg_miss++)
                    MLOG("[RED] dirty-page miss: texture ser=%llu pool content changed but was not detected", t->serial);
                return pk->view ? pk->view : pk->tex;
            }
            if (!pk->sparse && pkg_edges_clean(pk, t)) return pk->view ? pk->view : pk->tex;   // only the pages it shares with other data were written
            // Pool content changed (streamed assets written into the same storage, a movie frame): switch to another texture, as in-flight draws still read the old one. That is the spare
            // when the GPU is done with it (see NvPkg), else a new one; the old one becomes the spare (pkg_upload_ made nt resident)
            id<MTLTexture> reuse = nil;
            if (pk->spare) { pthread_mutex_lock(&g_mu); uint64_t done = g_done; pthread_mutex_unlock(&g_mu); if (pk->spare_fence <= done && pk->spare.usage == pk->tex.usage) reuse = pk->spare; }
            uint64_t st, vg = nvn_sparse_gen(); id<MTLTexture> nt = pkg_upload(t, &st, reuse);
            if (nt) { id<MTLTexture> old = pk->tex; pk->tex = nt;
                if (pk->spare && pk->spare != nt) defer_obj_release(pk->spare);   // still in use, or of another shape: retired as before
                if (pk->sparse) sparse_view(pk, t);
                zb_report(t, pk->sparse ? (unsigned)pk->vbase : 0, "re-upload");
                pk->spare = old; pk->spare_fence = g_fence + 1; g_ser_gen++; pk->stamp = st; pk->vgen = pk->vseen = vg; pk->span = nvn_dirty_span(t->mem, t->L.size); pk->seen = nvn_dirty_now(); g_pkg_reuploads++; if (dirtycheck() && !pk->sparse) pk->hash = full_hash(t->mem, t->L.size); pkg_edges_take(pk, t); }
            return pk->view ? pk->view : pk->tex;
        }
        pk = [NvPkg new];
        pk->sparse = t->pool && nvn_sparse_pool(t->pool); pk->vgen = pk->vseen = nvn_sparse_gen();
        pk->tex = pkg_upload(t, &pk->stamp, nil);
        if (!pk->tex) return nil;
        pk->span = nvn_dirty_span(t->mem, t->L.size);
        pk->seen = nvn_dirty_now(); g_pkg_uploads++; if (dirtycheck() && !pk->sparse) pk->hash = full_hash(t->mem, t->L.size);
        pkg_edges_take(pk, t);
        if (pk->sparse) sparse_view(pk, t);
        zb_report(t, pk->sparse ? (unsigned)pk->vbase : 0, "upload");
        tex_put(t->serial, pk);
        return pk->view ? pk->view : pk->tex;
    }
    if (have) return have;
    if (t->d.target == 10) {   // Buffer texture (TARGET_BUFFER): create the texture-buffer view directly on the memory pool buffer (zero copy; the GPU reads the pool's current content at execution time, same as NVN semantics)
        uint32_t pf = 0; uint8_t swz[4]; int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
        void *mb; uint64_t mo;
        if (!nvn_fmt_to_mtl(t->d.format, &pf, swz) || !pf || !bpb) { RED_ONCE(36, "buffer texture format %#x not mapped", t->d.format); return nil; }
        if (!t->mem || !mtl_addr((uint64_t)t->mem, (uint64_t)t->d.width * bpb, &mb, &mo)) { RED_ONCE(37, "buffer texture storage is not in a memory pool"); return nil; }
        id<MTLBuffer> b = (__bridge id<MTLBuffer>)mb;
        NSUInteger al = [g_dev minimumLinearTextureAlignmentForPixelFormat:(MTLPixelFormat)pf];
        // Metal reports a 16-byte alignment for texture buffer views (minimumTextureBufferAlignmentForPixelFormat), but on an Apple GPU a view at any offset that is a multiple of the element
        // size reads the right elements (measured on an M6: R32Float views at offsets 4, 8 and 12, all 64 elements right). Unreal puts some buffer textures at 8-byte offsets in its pools
        // (Octopath Traveler 0: offset 0x168), which used to be left as dummies. Below the element size the view stays a dummy
        if (mo % al) {
            if (mo % (uint64_t)bpb) { RED_ONCE(38, "buffer texture offset %#llx not aligned to its %d-byte elements", (unsigned long long)mo, bpb); return nil; }
            static int once; if (!once++) MLOG("buffer texture at pool offset %#llx, below Metal's reported %lu-byte alignment: created there (element-aligned views read correctly on Apple GPUs)", (unsigned long long)mo, (unsigned long)al);
        }
        MTLTextureDescriptor *d = [MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:(MTLPixelFormat)pf width:t->d.width resourceOptions:b.resourceOptions
                                                                                         usage:MTLTextureUsageShaderRead | (t->d.flags & NVN_TEXTURE_FLAGS_IMAGE ? MTLTextureUsageShaderWrite : 0)];
        size_t row = ((size_t)t->d.width * bpb + al - 1) / al * al;
        id<MTLTexture> mt = [b newTextureWithDescriptor:d offset:mo bytesPerRow:row];
        if (!mt) { RED_ONCE(39, "cannot create buffer texture: %u elements format %#x", t->d.width, t->d.format); return nil; }
        res_add(mt);
        tex_put(t->serial, mt);
        return mt;
    }
    alias_key ak; memset(&ak, 0, sizeof ak); ak.pool = (uint64_t)(uintptr_t)t->pool; ak.off = t->off; ak.d = t->d;
    pthread_mutex_lock(&g_smu); __unsafe_unretained id shared = hm_get_id(&g_alias, &ak); if (shared) nvn_hm_put(&g_alias_seen, &ak, (void *)(uintptr_t)(g_fence + 1)); pthread_mutex_unlock(&g_smu);
    if (shared) { tex_put(t->serial, shared); return shared; }
    id<MTLTexture> mt = tex_linear(t);
    if (!mt) {
        MTLTextureDescriptor *d = tex_desc(t, 0);
        if (!d) return nil;
        mt = [g_dev newTextureWithDescriptor:d];
        if (!mt) { RED_ONCE(2, "failed to create MTLTexture %ux%u format %#x target %d", t->d.width, t->d.height, t->d.format, t->d.target); return nil; }
        res_add(mt); if (!tex_init_fill(t, mt)) tex_zero_fill(mt);   // register residency before filling (the copy destination must be resident too)
    }
    if (g_allocprof > 0) { static int n; if (n++ < 30 || n % 500 == 0) MLOG("new texture (%s) #%d ser=%llu %ux%ux%u format %#x target %d levels %u storage %p", __func__, n, (unsigned long long)t->serial, t->d.width, t->d.height, t->d.depth, t->d.format, t->d.target, t->d.levels, (void *)t->mem); }
    if (getenv("DM_NVN_TEXCPU") && t->pool && !(t->pool->flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS)) MLOG("non-swizzled texture on a CPU-visible pool ser=%llu %ux%ux%u format %#x target %d levels %u texture flags %#x pool flags %#x storage %p", (unsigned long long)t->serial, t->d.width, t->d.height, t->d.depth, t->d.format, t->d.target, t->d.levels, t->d.flags, t->pool->flags, (void *)t->mem);
    res_add(mt);
    pthread_mutex_lock(&g_smu);
    if (!g_alias_set) g_alias_set = CFSetCreateMutable(NULL, 0, NULL);
    alias_take_over(&ak);
    hm_put_id(&g_alias, &ak, mt); CFSetAddValue(g_alias_set, (__bridge const void *)mt); nvn_hm_put(&g_alias_seen, &ak, (void *)(uintptr_t)(g_fence + 1));
    pthread_mutex_unlock(&g_smu);
    tex_put(t->serial, mt);
    return mt;
}
// Memory readout line (always on, low frequency, capped): resident memory, GPU allocated, deferred-release queue, residency set object count, registered memory pools / peak / tracking table capacity, cumulative texture re-uploads.
// Purpose: pin down "crashes with no log". The system gives no notice before killing a process for memory, so only readouts left beforehand can show which number kept climbing.
// Called by the frame hook every 300 frames; printed only when resident or GPU allocated changed by ≥ 64 MB since the last line, otherwise one line every 3000 frames; capped at 400 lines per game. All integers (the frame hook runs on the guest thread's small stack, so no floating-point formatting)
void mtl_mem_stat(unsigned long long frame, unsigned long long rss_mb) {
    static unsigned long long last_rss, last_gpu, last_frame; static int lines; unsigned long long gpu = (unsigned long long)(g_dev.currentAllocatedSize >> 20);
    unsigned long long dr = rss_mb > last_rss ? rss_mb - last_rss : last_rss - rss_mb, dg = gpu > last_gpu ? gpu - last_gpu : last_gpu - gpu;
    if (lines >= 400 || (lines && dr < 64 && dg < 64 && frame - last_frame < 3000)) return; lines++; last_rss = rss_mb; last_gpu = gpu; last_frame = frame;
    pthread_mutex_lock(&g_deferred_mu); unsigned long nd = (unsigned long)g_deferred.count; pthread_mutex_unlock(&g_deferred_mu);
    pthread_mutex_lock(&g_res_mu); unsigned long nr = (unsigned long)g_res_objects.count; pthread_mutex_unlock(&g_res_mu);
    int live = 0, peak = 0, cap = 0; nvn_dirty_stat(&live, &peak, &cap);
    pthread_mutex_lock(&g_smu); unsigned ns = g_by_serial.n, ashared = g_alias.n, nghost = g_ghost.n; uint64_t ghost_puts = g_ghost_puts;
    unsigned aorph = 0;   // shared textures no live texture object or finalized serial resolves to: kept only until their storage is taken over or their pool goes (alias_take_over)
    { CFMutableSetRef held = CFSetCreateMutable(NULL, 0, NULL);
      for (uint32_t i = 0; i < g_by_serial.cap; i++) if (g_by_serial.vals[i]) CFSetAddValue(held, g_by_serial.vals[i]);
      for (uint32_t i = 0; i < g_ghost.cap; i++) if (g_ghost.vals[i]) CFSetAddValue(held, g_ghost.vals[i]);
      for (uint32_t i = 0; i < g_alias.cap; i++) if (g_alias.vals[i] && !CFSetContainsValue(held, g_alias.vals[i])) aorph++;
      CFRelease(held); }
    pthread_mutex_unlock(&g_smu);
    unsigned long dead_n = 0; unsigned long long dead_mb = 0;
    // Objects read from a weak table come back retained + autoreleased: the frame hook runs on a guest thread with no autorelease pool that drains, so without this wrapper every buffer counted would be pinned for good (the readout creating the leak itself)
    int pres = 0; @autoreleasepool { pthread_mutex_lock(&g_mu); pres = g_pool_res_n; for (id<MTLBuffer> b in g_dead_pools) { dead_n++; dead_mb += b.length; } pthread_mutex_unlock(&g_mu); } dead_mb >>= 20;   // host objects held by serial (textures / packaged textures / programs): if it climbs along with GPU allocated, textures are not being released
    // os_proc_available_memory: how far this process is from being terminated by the system for memory, iOS only; the Mac writes "none". On a phone, a crash with no log is most likely this number reaching 0
    char avail[32] = "none";
#if TARGET_OS_IPHONE
    snprintf(avail, sizeof avail, "%llu MB", (unsigned long long)(os_proc_available_memory() >> 20));
#endif
    MLOG("memory up to frame %llu: resident %llu MB, GPU allocated %llu MB, deferred-release queue %lu, residency set %lu, serial objects %u, memory pools %d (resident %d; peak %d / tracking table capacity %d; freed but alive %lu, %llu MB), shared textures %u (%u of them unreferenced; evicted with pools %llu, on storage takeover %llu in total), texture re-uploads %llu in total, per-frame arena blocks created %llu in total, finalized textures still referenced %u (small table, %llu put in total); headroom to the system memory limit %s", frame, rss_mb, gpu, nd, nr, ns, live, pres, peak, cap, dead_n, dead_mb, ashared, aorph, (unsigned long long)g_alias_evicted, (unsigned long long)g_alias_taken, (unsigned long long)g_pkg_reuploads, (unsigned long long)g_arena_made, nghost, (unsigned long long)ghost_puts, avail);
    { void mtl_vc_stats(unsigned *, uint64_t *, uint64_t *), mtl_tb2_stats(uint64_t *, uint64_t *, uint64_t *); uint64_t mtl_tex_mis_count(void); unsigned vn; uint64_t vh, vm, tb, tm, tf;
      mtl_vc_stats(&vn, &vh, &vm); mtl_tb2_stats(&tb, &tm, &tf);
      MLOG("texture views by texture (vc_get): %u textures hold cached views; %llu views reused, %llu made in total; texture buffers bound as 2D rows for wrapped 1D fetches %llu times, %llu made, %llu failed "
           "(tbuf_as_2d); texture type mismatches no view bridges %llu in total (tex_mis_note)",
           vn, (unsigned long long)vh, (unsigned long long)vm, (unsigned long long)tb, (unsigned long long)tm, (unsigned long long)tf, (unsigned long long)mtl_tex_mis_count()); }
    if (g_allocprof > 0) { char ab[256]; int o = 0; for (int i = 0; i < g_alloc_n && o < 200; i++) o += snprintf(ab + o, sizeof ab - o, " #%d %llu MB", i, (unsigned long long)(g_alloc4[i].allocatedSize >> 20));
        for (int i = 0; i < 3 && o < 240; i++) o += snprintf(ab + o, sizeof ab - o, " up%d %llu pres%d %llu", i, (unsigned long long)(g_up_alloc4[i].allocatedSize >> 20), i, (unsigned long long)(g_present_alloc4[i].allocatedSize >> 20));
        MLOG("command allocator usage:%s", ab);
        MLOG("GPU allocation growth by function: pass end %llu MB, pass begin %llu MB, pipeline fetch %llu MB, argument buffers %llu MB, texture fetch %llu MB", g_ap_fn[0] >> 20, g_ap_fn[1] >> 20, g_ap_fn[2] >> 20, g_ap_fn[3] >> 20, g_ap_fn[4] >> 20); }
    if (g_allocprof > 0) { int top[4] = {-1, -1, -1, -1}; for (int o = 0; o < 64; o++) for (int k = 0; k < 4; k++) if (top[k] < 0 || g_ap_bytes[o] > g_ap_bytes[top[k]]) { for (int j = 3; j > k; j--) top[j] = top[j - 1]; top[k] = o; break; }
        MLOG("GPU allocation growth by op: op%d %llu MB, op%d %llu MB, op%d %llu MB, op%d %llu MB", top[0], g_ap_bytes[top[0]] >> 20, top[1], g_ap_bytes[top[1]] >> 20, top[2], g_ap_bytes[top[2]] >> 20, top[3], g_ap_bytes[top[3]] >> 20); }
}
void mtl_pkg_stats(uint64_t *uploads, uint64_t *reuploads) { *uploads = g_pkg_uploads; *reuploads = g_pkg_reuploads; }
uint64_t mtl_pkg_edge_skips(void) { return g_pkg_edge_skips; }
uint64_t mtl_pkg_miss(void) { return g_pkg_miss; }
void mtl_vc_drop(uint64_t ser);
void mtl_tex_release(tex_t *t) {
    if (!t->serial) return;
    pthread_mutex_lock(&g_smu); nvn_hm_del(&g_tex_live, &t->serial); pthread_mutex_unlock(&g_smu);
    serial_del(t->serial);
    mtl_vc_drop(t->serial);
}

