// Anisotropic filtering (DM_ANISO, nvn_mtl.m aniso_floor / aniso_raises): with DM_ANISO=8 the production sampler registry (mtl_reg_samp, samp_by_id) raises the game's
// mipmapped linear samplers that ask for less, and leaves the others as the game made them: one already above the floor, a comparison (shadow) sampler, a sampler without mip
// filtering or with a nearest magnification filter, one held to a single level, a min / max reduction
#include "../nvn/nvn_mtl.m"
uint64_t g_t0_ns;
int dm_guest_as_contains(uintptr_t address) { (void)address; return 0; }
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)
// Registers sp as sampler sid and makes its Metal sampler: how many samplers that raised
static uint64_t made(int sid, samp_par sp) {
    mtl_reg_samp(sid, &sp);
    uint64_t n0 = mtl_aniso_raised();
    os_unfair_lock_lock(&g_rmu); id<MTLSamplerState> s = samp_by_id(sid); os_unfair_lock_unlock(&g_rmu);
    CHECK(s != nil, "sampler %d created", sid);
    return mtl_aniso_raised() - n0;
}
int main(void) { @autoreleasepool {
    setenv("DM_ANISO", "8", 1);
    mtl_init();
    CHECK(aniso_floor() == 8, "DM_ANISO=8: the floor is %d", aniso_floor());
    samp_par mip = {0}; mip.magf = 1; mip.minf = 5; mip.ws = mip.wt = mip.wr = 1; mip.lod_max = 14; mip.aniso = 1;   // Breath of the Wild's material samplers
    samp_par s;
    CHECK(made(20, mip) == 1, "a trilinear sampler at 1x is raised");
    s = mip; s.minf = 3; CHECK(made(21, s) == 1, "a linear sampler with nearest mip selection is raised");
    s = mip; s.aniso = 2; CHECK(made(22, s) == 1, "a sampler at 2x is raised");
    s = mip; s.aniso = 16; CHECK(made(23, s) == 0, "a sampler at 16x keeps the game's");
    s = mip; s.cmp_mode = 1; s.cmp_func = 4; CHECK(made(24, s) == 0, "a comparison sampler keeps the game's");
    s = mip; s.minf = 1; CHECK(made(25, s) == 0, "a sampler without mip filtering keeps the game's");
    s = mip; s.minf = 4; CHECK(made(26, s) == 0, "a nearest minification filter keeps the game's");
    s = mip; s.magf = 0; CHECK(made(27, s) == 0, "a nearest magnification filter keeps the game's");
    s = mip; s.lod_max = 0; CHECK(made(28, s) == 0, "a sampler held to one level keeps the game's");
    s = mip; s.reduction = 1; CHECK(made(29, s) == 0, "a min / max reduction keeps the game's");
    printf("t_aniso: %d checks, %d failed\n", checks, fails);
    return fails != 0;
} }
