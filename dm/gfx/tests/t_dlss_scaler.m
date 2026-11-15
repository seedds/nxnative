// DLSS's MetalFX scalers outlive the submissions that encoded them (dlss_scaler_for): a Metal 4 command buffer does not retain what is encoded into it, so a scaler dropped before the
// submission completes is freed under its encoders, and the commit crashes (Fast Fusion, at its 8th network: "-[AGXG17XFamilyMLContext executable]: unrecognized selector" in
// -[_MTL4MachineLearningCommandEncoder encodeToCommandQueue:]). A scaler replaced for new sizes, or taken by a fifth network, is held until the next fence completes; the fifth network
// takes the slot used least recently, not slot 0 (the network the game is drawing with). A network is named by the serial nvnDlssNetworkInitialize gives it, so one made again in
// the same memory takes a scaler of its own, with a new history
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
    p_dlss c[6]; memset(c, 0, sizeof c); for (int i = 1; i <= 5; i++) c[i].net = 0x1000 * i;
    id<MTL4FXTemporalScaler> sc[6] = {nil};
    for (int i = 1; i <= 4; i++) { dlss_scaler *e = dlss_scaler_for(&c[i], col, dep, mot, out, 960, 540); sc[i] = e ? e->sc : nil; }
    check(sc[1] && sc[2] && sc[3] && sc[4], "four networks take four scalers");
    check(dlss_scaler_for(&c[1], col, dep, mot, out, 960, 540)->sc == sc[1], "the same network and sizes keep their scaler");

    // The game draws with network 1 (used last), then makes a fifth: the slot of network 2, the one used least recently, is taken
    dlss_scaler_for(&c[3], col, dep, mot, out, 960, 540); dlss_scaler_for(&c[4], col, dep, mot, out, 960, 540); dlss_scaler_for(&c[1], col, dep, mot, out, 960, 540);
    dlss_scaler *e5 = dlss_scaler_for(&c[5], col, dep, mot, out, 960, 540);
    check(e5 && e5->sc && dlss_scaler_for(&c[1], col, dep, mot, out, 960, 540)->sc == sc[1], "a fifth network leaves the network drawn with last its scaler");
    check(held(sc[2]), "the scaler of the network it replaces is held until the submission completes");
    check(!held(sc[1]) && !held(sc[3]) && !held(sc[4]), "the other networks' scalers are not");

    // New sizes for network 1 (Fast Fusion's 1920x972 output): a new scaler, the old one held
    dlss_scaler *e1 = dlss_scaler_for(&c[1], col, dep, mot, out2, 960, 486);
    check(e1 && e1->sc && e1->sc != sc[1] && held(sc[1]), "new sizes make a new scaler and hold the old one");

    // Once the fences complete, the held scalers are released
    __weak id<MTL4FXTemporalScaler> w1 = sc[1], w2 = sc[2];
    sc[1] = sc[2] = nil;
    g_done = g_fence + 1; defer_drain();
    check(!held(w1) && !held(w2), "a completed fence releases them");
    @autoreleasepool {} check(w1 == nil && w2 == nil, "and nothing else keeps them alive");

    // A network made again in the same memory, with the same sizes: another serial, so another scaler, which starts a new history
    int (*net_init)(dlss_net_t *, const dlss_builder_t *) = (int (*)(dlss_net_t *, const dlss_builder_t *))nvn_entry("nvnDlssNetworkInitialize");
    if (!net_init) { check(0, "nvnDlssNetworkInitialize is an entry point"); return 1; }
    static uint8_t mem[0x800]; dlss_net_t *n = (dlss_net_t *)mem; const dlss_builder_t b = {.w = 1920, .h = 1080};
    net_init(n, &b); const uint64_t s1 = n->serial; net_init(n, &b); const uint64_t s2 = n->serial;
    check(s1 && s2 && s1 != s2 && n->b.w == 1920, "a network made again in the same memory has a serial of its own");
    p_dlss ca = {.net = s1}, cb = {.net = s2};
    dlss_scaler *ea = dlss_scaler_for(&ca, col, dep, mot, out, 960, 540); id<MTL4FXTemporalScaler> sa = ea ? ea->sc : nil; if (ea) ea->fresh = 0;
    dlss_scaler *eb = dlss_scaler_for(&cb, col, dep, mot, out, 960, 540);
    check(sa && eb && eb->sc && eb->sc != sa && eb->fresh, "and takes a scaler of its own, which resets its history");

    printf("t_dlss_scaler: %d checks, %s\n", checks, fails ? "FAILED" : "ok");
    return fails != 0;
} }
