// DLSS's MetalFX scalers outlive the submissions that encoded them (dlss_scaler_for): a Metal 4 command buffer does not retain what is encoded into it, so a scaler dropped before the
// submission completes is freed under its encoders, and the commit crashes (Fast Fusion, at its 8th network: "-[AGXG17XFamilyMLContext executable]: unrecognized selector" in
// -[_MTL4MachineLearningCommandEncoder encodeToCommandQueue:]). A network keeps a scaler for each input box it draws at (dynamic resolution steps between a few), and one going back
// to a kept scaler resets its history. A scaler whose slot another network or size takes, or whose network is finalized (nvnDlssNetworkFinalize), is held until the next fence
// completes; a newcomer past the slots takes the slot used least recently, not slot 0 (the network the game is drawing with). A network is named by the serial
// nvnDlssNetworkInitialize gives it, so one made again in the same memory takes a scaler of its own, with a new history. A bias mask's format is part of the scaler's key (PR #1904):
// a mask makes a scaler with a reactive mask
#include "../nvn/nvn_mtl.m"
uint64_t g_t0_ns;
int dm_guest_as_contains(uintptr_t address) { (void)address; return 0; }
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
static void *nvn_entry(const char *name) {   // hle_nvn.c's entry points, from the section its NVN_* macros fill
    unsigned long sz; const struct { const char *name; void *fn; } *im = (void *)getsectiondata(&_mh_execute_header, "__DATA", "swnvn", &sz);
    for (unsigned long i = 0; im && i < sz / sizeof *im; i++) if (!strcmp(im[i].name, name)) return im[i].fn;
    return NULL;
}
static int checks, fails;
static void check(int ok, const char *what) { checks++; fails += !ok; printf("%s %s\n", ok ? "ok  " : "FAIL", what); }

static id<MTLTexture> tex(MTLPixelFormat f, NSUInteger w, NSUInteger h) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:w height:h mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget; d.storageMode = MTLStorageModePrivate;
    return [g_dev newTextureWithDescriptor:d];
}
static int held(id o) { int r = 0; pthread_mutex_lock(&g_deferred_mu); for (NvDeferredRelease *d in g_deferred) r |= d->object == o; pthread_mutex_unlock(&g_deferred_mu); return r; }

int main(void) { @autoreleasepool {
    mtl_init();
    if (![MTLFXTemporalScalerDescriptor supportsMetal4FX:g_dev]) { printf("t_dlss_scaler: no Metal 4 MetalFX on this GPU, skipped\n"); return 0; }
    id<MTLTexture> col = tex(MTLPixelFormatRGBA16Float, 960, 540), dep = tex(MTLPixelFormatDepth32Float, 960, 540), mot = tex(MTLPixelFormatRG16Float, 960, 540);
    id<MTLTexture> out = tex(MTLPixelFormatRGBA16Float, 1920, 1080), out2 = tex(MTLPixelFormatRGBA16Float, 1920, 972);
    enum { N = DLSS_KEEP + 1 };
    p_dlss c[N + 1]; memset(c, 0, sizeof c); for (int i = 1; i <= N; i++) c[i].net = 0x1000 * i;
    id<MTL4FXTemporalScaler> sc[N + 1] = {nil};
    int all = 1;
    for (int i = 1; i < N; i++) { dlss_scaler *e = dlss_scaler_for(&c[i], col, dep, mot, out, nil, 960, 540); sc[i] = e ? e->sc : nil; all &= sc[i] != nil; }
    check(all, "each network takes a scaler while slots are free");
    check(dlss_scaler_for(&c[1], col, dep, mot, out, nil, 960, 540)->sc == sc[1], "the same network and sizes keep their scaler");

    // The game draws with network 1 (used last), then makes one network more than there are slots: the slot of network 2, the one used least recently, is taken
    for (int i = 3; i < N; i++) dlss_scaler_for(&c[i], col, dep, mot, out, nil, 960, 540);
    dlss_scaler_for(&c[1], col, dep, mot, out, nil, 960, 540);
    dlss_scaler *eN = dlss_scaler_for(&c[N], col, dep, mot, out, nil, 960, 540);
    check(eN && eN->sc && dlss_scaler_for(&c[1], col, dep, mot, out, nil, 960, 540)->sc == sc[1], "a network past the slots leaves the network drawn with last its scaler");
    check(held(sc[2]), "the scaler of the network it replaces is held until the submission completes");
    int others = 1; for (int i = 3; i < N; i++) others &= !held(sc[i]);
    check(!held(sc[1]) && others, "the other networks' scalers are not");

    // Dynamic resolution: network 1 steps to new sizes (Fast Fusion's 1920x972 output) and back. The new sizes take a scaler of their own and the first is kept, not
    // dropped; back at the first sizes, the network takes its first scaler again, with a reset (its history is from before the step)
    for (int i = 3; i < N; i++) dlss_scaler_for(&c[i], col, dep, mot, out, nil, 960, 540);   // network N's slot is now the least recently used: the new sizes take it
    dlss_scaler_for(&c[1], col, dep, mot, out, nil, 960, 540)->fresh = 0;
    dlss_scaler *e1 = dlss_scaler_for(&c[1], col, dep, mot, out2, nil, 960, 486);
    id<MTL4FXTemporalScaler> s972 = e1 ? e1->sc : nil;
    check(s972 && s972 != sc[1] && e1->fresh && !held(sc[1]), "new sizes make a new scaler and keep the old one");
    e1->fresh = 0;
    dlss_scaler *b1 = dlss_scaler_for(&c[1], col, dep, mot, out, nil, 960, 540);
    check(b1 && b1->sc == sc[1] && b1->fresh, "back at the first sizes, the first scaler again, with a reset");
    b1->fresh = 0;
    dlss_scaler *s1b = dlss_scaler_for(&c[1], col, dep, mot, out, nil, 960, 540);
    check(s1b == b1 && !s1b->fresh, "staying at the same sizes does not reset again");
    check(dlss_scaler_for(&c[1], col, dep, mot, out2, nil, 960, 486)->sc == s972, "and the second sizes' scaler is kept too");

    // A bias mask (Star Fox's: R8, the input's size): its format is in the scaler's key, so the network takes a scaler made with a reactive mask
    dlss_scaler *em = dlss_scaler_for(&c[3], col, dep, mot, out, tex(MTLPixelFormatR8Unorm, 960, 540), 960, 540);
    check(em && em->sc && em->sc != sc[3] && em->mask && em->fresh, "a bias mask makes a scaler with a reactive mask");

    // Once the fences complete, the held scalers are released
    __weak id<MTL4FXTemporalScaler> w2 = sc[2];
    sc[2] = nil;
    g_done = g_fence + 1; defer_drain();
    check(!held(w2), "a completed fence releases them");
    @autoreleasepool {} check(w2 == nil, "and nothing else keeps them alive");

    // A network made again in the same memory, with the same sizes: another serial, so another scaler, which starts a new history
    int (*net_init)(dlss_net_t *, const dlss_builder_t *) = (int (*)(dlss_net_t *, const dlss_builder_t *))nvn_entry("nvnDlssNetworkInitialize");
    void (*net_fin)(dlss_net_t *) = (void (*)(dlss_net_t *))nvn_entry("nvnDlssNetworkFinalize");
    if (!net_init || !net_fin) { check(0, "nvnDlssNetworkInitialize / Finalize are entry points"); return 1; }
    static uint8_t mem[0x800]; dlss_net_t *n = (dlss_net_t *)mem; const dlss_builder_t b = {.w = 1920, .h = 1080};
    net_init(n, &b); const uint64_t s1 = n->serial; net_init(n, &b); const uint64_t s2 = n->serial;
    check(s1 && s2 && s1 != s2 && n->b.w == 1920, "a network made again in the same memory has a serial of its own");
    p_dlss ca = {.net = s1}, cb = {.net = s2};
    dlss_scaler *ea = dlss_scaler_for(&ca, col, dep, mot, out, nil, 960, 540); id<MTL4FXTemporalScaler> sa = ea ? ea->sc : nil; if (ea) ea->fresh = 0;
    dlss_scaler *eb = dlss_scaler_for(&cb, col, dep, mot, out, nil, 960, 540);
    check(sa && eb && eb->sc && eb->sc != sa && eb->fresh, "and takes a scaler of its own, which resets its history");

    // Finalizing a network retires all of its cached scalers behind the next fence and frees their slots
    dlss_scaler *eb2 = dlss_scaler_for(&cb, col, dep, mot, out2, nil, 960, 486);
    id<MTL4FXTemporalScaler> sb1 = eb ? eb->sc : nil, sb2 = eb2 ? eb2->sc : nil;
    net_fin(n);
    check(n->serial == 0, "nvnDlssNetworkFinalize clears the network serial");
    dlss_retire_drain();
    int left = 0; for (int i = 0; i < DLSS_KEEP; i++) if (g_dlss[i].net == s2) left++;
    check(left == 0 && held(sb1) && held(sb2), "finalizing a network frees its slots and holds its scalers until completion");
    __weak id<MTL4FXTemporalScaler> wb1 = sb1, wb2 = sb2;
    sb1 = sb2 = nil;
    g_done = g_fence + 1; defer_drain();
    @autoreleasepool {} check(wb1 == nil && wb2 == nil, "completed fence releases the finalized network's scalers");

    printf("t_dlss_scaler: %d checks, %s\n", checks, fails ? "FAILED" : "ok");
    return fails != 0;
} }
