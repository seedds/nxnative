// Depth-only targets set over an open pass (g_rt_dmask): the pass goes on with its colour writes masked. When that pass ends before the colour set comes back (here a
// mid-pass depth / stencil clear), the depth-only set is what the game has bound: the next pass must not attach the old colour targets again with their colour mask on,
// or the depth-only draws write colour into them. Also: the colour set coming back over the open pass continues it, and every target unbound ends nothing until a draw
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)
static uint8_t *commands; static size_t used;
static void emit(unsigned op, const void *p, size_t n) {
    size_t size = (8 + n + 7) & ~7ull; op_hdr h = {op, (uint32_t)size};
    memcpy(commands + used, &h, 8); if (n) memcpy(commands + used + 8, p, n); used += size;
}
static void run(void) {   // the ops emitted since the last run, in the open submission
    emit(OP_END, NULL, 0); mtl_exec(commands); used = 0;
}
int main(void) { @autoreleasepool {
    setenv("DM_NVN_ENC_THREAD", "0", 1);   // rp_begin below runs on this thread, as the replay's does
    mtl_init();
    void *memory = NULL; if (posix_memalign(&memory, getpagesize(), 65536)) abort(); memset(memory, 0, 65536); mtl_pool_add(memory, 65536); commands = memory;
    tex_t color = {.magic = MAGIC('T', 'E', 'X', 0), .serial = 983010}, depth = {.magic = MAGIC('T', 'E', 'X', 0), .serial = 983020};
    color.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8, .width = 8, .height = 8, .depth = 1, .levels = 1, .samples = 1};
    depth.d = color.d; depth.d.format = NVN_FORMAT_DEPTH32F_STENCIL8;
    p_set_rt both = {.n = 1, .color = {&color}, .depth = &depth}, donly = {.depth = &depth}, none = {0};
    state_reset(); memset(&R, 0, sizeof R);
    mtl_sub_begin(); uint64_t handle = (uint64_t)commands; mtl_sub_index(&handle, 1);
    emit(OP_SET_RT, &both, sizeof both); run();
    rp_begin(); CHECK(g_enc != nil, "a pass opens on the colour and depth targets");
    emit(OP_SET_RT, &donly, sizeof donly); run();
    CHECK(g_enc != nil && g_rt_dmask, "depth-only targets over the open pass: the pass goes on, colour writes masked");
    CHECK(chmask_eff() == 0, "no colour channel is written meanwhile");
    p_clear_ds clear = {.depth = 1, .stencil = 0, .depth_mask = 0, .stencil_mask = 255}; emit(OP_CLEAR_DS, &clear, sizeof clear); run();
    CHECK(g_enc == nil, "the mid-pass depth / stencil clear ends the pass");
    CHECK(R.rt.n == 0 && !R.rt.color[0], "after it the targets are the depth-only set, not the pass's colour targets");
    CHECK(R.rt.depth == &depth && R.has_rt, "the depth target stays bound");
    rp_begin(); CHECK(g_enc != nil && g_pass.cfmt[0] == 0, "the next pass attaches no colour target");
    emit(OP_SET_RT, &both, sizeof both); run();
    CHECK(R.rt.n == 1 && R.rt.color[0] == &color && !g_rt_dmask, "the colour set comes back");
    rp_begin(); emit(OP_SET_RT, &donly, sizeof donly); run(); emit(OP_SET_RT, &both, sizeof both); run();
    CHECK(g_enc != nil && !g_rt_dmask && g_rt_merged && R.rt.n == 1, "depth-only and back over an open pass: one pass, colour writes on again");
    emit(OP_SET_RT, &none, sizeof none); run();
    CHECK(g_enc != nil && g_rt_unbound, "every target unbound over the open pass: its end deferred");
    rp_end(); CHECK(!R.has_rt && !g_rt_unbound, "the pass ending while unbound leaves no targets");
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "GPU submission completes"); CHECK(!mtl_last_commit_error(), "GPU reports no error");
    printf("t_rt_dmask: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
