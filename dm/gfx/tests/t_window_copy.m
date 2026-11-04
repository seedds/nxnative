// Window-copy ownership and fail-stop guards. Entries are seeded deliberately: this
// tests the production retirement/present paths, not wc_try's draw eligibility.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(name, ok) do { int pass = !!(ok); checks++; failed += !pass; printf("%s: %s\n", name, pass ? "PASS" : "FAIL"); } while (0)
static tex_t window(uint64_t serial) { tex_t t = {.magic=MAGIC('T','E','X',0), .serial=serial}; return t; }
static void target(tex_t *w, uint64_t p, int ww, int wh, int pw, int ph) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:pw height:ph mipmapped:NO];
    id<MTLTexture> mt = [g_dev newTextureWithDescriptor:d]; serial_put(p, mt);
    wc_owner *o = calloc(1, sizeof *o); o->serial = p; o->next = g_wc_owners; g_wc_owners = o;
    g_wc[0] = (wc_ent){w->serial, p, p, ww, wh, pw, ph}; atomic_store(&g_wc_n, 1);
}
static void lifecycle(void) {
    tex_t w = window(10001); target(&w, 10002, 100, 50, 200, 100);
    int crop[4] = {10, 5, 40, 20}, out[4];
    uint64_t p = wc_take(w.serial, crop, out);
    CHECK("take selects target and scales crop", p == 10002 && out[0] == 20 && out[1] == 10 && out[2] == 80 && out[3] == 40);
    CHECK("take consumes pending picture", wc_take(w.serial, crop, out) == 0);
    mtl_tex_release(&w);
    CHECK("finalize forgets the window", atomic_load(&g_wc_n) == 0);
    CHECK("taken target survives finalize", tex_get(p) != nil);
    // Reusing the NVN object's storage with a new serial must not recover the old picture.
    w = window(10003); target(&w, 10004, 100, 50, 300, 150);
    CHECK("reused object has a distinct target", wc_take(w.serial, NULL, out) == 10004 && tex_get(p) != nil);
    wc_release(p); CHECK("old target retires after present release", tex_get(p) == nil);
    mtl_tex_release(&w); CHECK("new taken target survives finalize", tex_get(10004) != nil);
    wc_release(10004); CHECK("new target retires after release", tex_get(10004) == nil);
    w = window(10005); target(&w, 10006, 100, 50, 200, 100);
    mtl_tex_release(&w); CHECK("untaken pending target retires on finalize", tex_get(10006) == nil);
    w = window(10007); target(&w, 10008, 100, 50, 200, 100);
    atomic_store(&g_present_discard, 1);
    CHECK("discarded present has no ticket", present_enqueue(&w, 1. / 60, NULL, 0) == 0);
    atomic_store(&g_present_discard, 0); mtl_tex_release(&w);
    CHECK("discarded present releases its target reference", tex_get(10008) == nil);
}
static void guards(void) {
    tex_t w = window(10101); target(&w, 10102, 100, 50, 200, 100);
    g_wc[0].pending = 0; wc_touch(&w, "test write", 0);
    CHECK("write after present does not poison", !atomic_load(&g_wc_off));
    wc_touch(&w, "test read", 1); CHECK("late read poisons even after present", atomic_load(&g_wc_off));
    atomic_store(&g_wc_off, 0); g_wc[0].pending = 10102;
    wc_touch(&w, "test write", 0); CHECK("late write before present poisons", atomic_load(&g_wc_off));
    atomic_store(&g_wc_off, 0); g_wc_cur = &g_wc[0]; g_wc_ndraw = 0; g_stat_draw = 20;
    wc_drawn(20); CHECK("no encoded draw leaves pending unchanged", g_wc_ndraw == 0 && g_wc[0].pending == 10102);
    g_wc[0].pending = 0; g_stat_draw++; wc_drawn(20);
    CHECK("first encoded draw marks picture pending", g_wc[0].pending == 10102 && !atomic_load(&g_wc_off));
    g_stat_draw++; wc_drawn(21); CHECK("second draw poisons", atomic_load(&g_wc_off));
    g_wc_cur = NULL; mtl_tex_release(&w);
}
int main(void) { @autoreleasepool {
    alarm(15); setbuf(stdout, NULL); mtl_init(); lifecycle(); guards();
    printf("window copy: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
