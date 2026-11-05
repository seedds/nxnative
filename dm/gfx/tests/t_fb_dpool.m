// A fragment shader with texture pool bindings (TEXP) reads through a table of every ID registered from the bound base up (nvn_mtl.m pool_gpu). With DM_NVN_FB_DPOOL=1
// (g_fb_dpool_m 1) it counts as a reader of its depth attachment, so that the next depth-writing draw of the pass ends the pass, only when one of those IDs lies over the depth
// attachment's storage or has storage that is not known (fb_pool_meets): the depth texture, a view of it, another texture object over the same memory, or a depth attachment
// whose own storage is not known. A table of other textures, or the depth texture registered below the base, keeps the pass open. The tables checked against are the ones
// production pool_gpu builds. With DM_NVN_FB_DPOOL=0, the default, every pool shader is a reader, as before
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

enum { POOL_BYTES = 1 << 20, BASE = 100, TABLE_IDS = 8 };
static void *g_mem; static pool_t g_pool; static tex_t g_color;
static void make(tex_t *t, uint64_t serial, int format, int64_t off) {
    *t = (tex_t){.magic = MAGIC('T', 'E', 'X', 0), .serial = serial, .pool = &g_pool, .off = off, .mem = (uint8_t *)g_mem + off};
    t->d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = format, .width = 16, .height = 16, .depth = 1, .levels = 1, .samples = 1};
    nvn_tex_layout(&t->d, &t->L); mtl_tex_init(t);
}
static NvProg *pool_prog(int texp) {
    NvProg *pr = [NvProg new]; NvShader *fs = [NvShader new]; pr->sh[1] = fs;
    fs->stage = 1; strcpy(fs->key, "00000000000d9001_b");
    if (texp) fs->rf.b[fs->rf.nb++] = (nvn_rbind){.kind = NVN_RB_TEXP, .aid = 0, .dtt = MTLTextureType2D, .dpf = NVN_DPF_FLOAT};
    fs->rf.b[fs->rf.nb++] = (nvn_rbind){.kind = NVN_RB_SAMPP, .aid = 1};
    fs->rf.pool = fs->rf.bindless = 1;
    return pr;
}
// Two depth-writing draws of the shader in one pass: whether the second one ended it. The depth attachment is named after the pass opens, as fb_prepare reads only R.rt.depth
static int splits(NvProg *pr, const tex_t *depth) {
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin();
    R.has_rt = 1; R.rt.n = 1; R.rt.color[0] = &g_color; S.tex_base = BASE; S.chmask.mask = 0; S.ds.dt = S.ds.dw = 1;
    R.clear_c[0] = 1; R.cc[0] = MTLClearColorMake(0, 0, 0, 1);
    CHECK(rp_begin() != nil, "production render pass opens");
    R.rt.depth = depth;
    fb_prepare(pr); int open = g_enc != nil;
    fb_prepare(pr); int split = open && !g_enc;
    CHECK(open, "the first depth-writing draw stays in the pass");
    R.rt.depth = NULL;
    if (!g_enc) rp_begin();   // the pass the second draw would have opened, which its own read already marked: the next case starts from a clean one
    rp_end();
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "submission completed without error");
    return split;
}
// The IDs from BASE that hold a texture in the production table for the shader's binding (pool_gpu: the dummy elsewhere), a bit each
static unsigned table_real(void) {
    mtl_sub_begin(); os_unfair_lock_lock(&g_rmu);
    __unsafe_unretained id<MTLBuffer> b = nil; pool_gpu(0, MTLTextureType2D, NVN_DPF_FLOAT, BASE, &b);
    const uint32_t n = *(const uint32_t *)b.contents; const uint64_t *e = (const uint64_t *)((const uint8_t *)b.contents + DMSH_POOL_HEAD);
    unsigned real = 0; for (uint32_t i = 0; i < n && i < TABLE_IDS; i++) if (e[i] != e[n]) real |= 1u << i;
    os_unfair_lock_unlock(&g_rmu);
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "pool table submission completed without error");
    return real;
}
int main(void) { @autoreleasepool {
    setenv("DM_NVN_TBDR", "0", 1);
    mtl_init();
    if (posix_memalign(&g_mem, (size_t)getpagesize(), POOL_BYTES)) abort();
    memset(g_mem, 0, POOL_BYTES);
    g_pool = (pool_t){.magic = MAGIC('P', 'O', 'O', 'L'), .mem = g_mem, .size = POOL_BYTES, .flags = NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    mtl_pool_add(g_mem, POOL_BYTES);
    g_color = (tex_t){.magic = MAGIC('T', 'E', 'X', 0), .serial = 99900};
    g_color.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8, .width = 16, .height = 16, .depth = 1, .levels = 1, .samples = 1};
    MTLTextureDescriptor *cd = tex_desc(&g_color, 0); cd.storageMode = MTLStorageModePrivate;
    id<MTLTexture> color = [g_dev newTextureWithDescriptor:cd];
    CHECK(color != nil, "colour target created");
    // NOLINTBEGIN(clang-analyzer-osx.cocoa.RetainCount): g_by_serial owns the colour target (serial_put's CFBridgingRetain) until mtl_tex_release at the end, not a leak
    res_add(color); mtl_tex_init(&g_color); serial_put(g_color.serial, color);
    // NOLINTEND(clang-analyzer-osx.cocoa.RetainCount)
    tex_t depth, again, alias, other, moved, nomem;
    make(&depth, 99901, NVN_FORMAT_DEPTH32F, 0);
    make(&again, 99905, NVN_FORMAT_DEPTH32F, 0);   // the depth texture made again over the same storage (a new object each frame)
    make(&alias, 99902, NVN_FORMAT_R32F, 0);   // another texture over the depth texture's memory, with a description of its own
    make(&other, 99903, NVN_FORMAT_RGBA8, 256 << 10);
    make(&moved, 99904, NVN_FORMAT_RGBA8, 512 << 10); moved.mem = (uint8_t *)g_mem + (768 << 10);   // its pool lies elsewhere than when it was made (a virtual pool's first mapping)
    nomem = depth; nomem.mem = NULL;   // a depth attachment with no storage address
    NvProg *pr = pool_prog(1);
    unsetenv("DM_NVN_FB_DPOOL"); g_fb_dpool_m = -1;
    CHECK(fb_dpool_mode() == 0, "off by default");
    g_fb_dpool_m = 1;

    mtl_reg_tex(BASE + 1, &other, NULL); mtl_reg_tex(BASE - 50, &depth, NULL);   // the depth texture below the base: no ID of the table
    uint64_t kept = g_fb_dpool[0], freed = g_fb_dpool[1], by_pool = g_fb_split_by[2];
    CHECK(table_real() == 1u << 1, "the production table holds the other texture alone");
    CHECK(!splits(pr, &depth), "a table without the depth storage: the next depth-writing draw stays in the pass");
    CHECK(g_fb_dpool[1] == freed + 2 && g_fb_dpool[0] == kept && g_fb_split_by[2] == by_pool, "both draws counted as unable to read the depth attachment, no pass ended");

    mtl_reg_tex(BASE + 2, &depth, NULL);
    CHECK(table_real() == (1u << 1 | 1u << 2), "the production table holds the depth texture once it is registered from the base up");
    kept = g_fb_dpool[0]; by_pool = g_fb_split_by[2];
    CHECK(splits(pr, &depth), "a table with the depth texture: the next depth-writing draw ends the pass");
    CHECK(g_fb_dpool[0] > kept && g_fb_split_by[2] == by_pool + 1, "counted as a pool reader and a pass ended for one");
    view_t level0 = {.set_levels = 1, .base_level = 0, .nlevels = 1};
    mtl_reg_tex(BASE + 2, &depth, &level0);
    CHECK(splits(pr, &depth), "a table with a view of the depth texture: the pass ends");
    mtl_reg_tex(BASE + 2, &again, NULL);
    CHECK(again.serial != depth.serial && splits(pr, &depth), "a table with another object over the same storage (another serial): the pass ends");
    mtl_reg_tex(BASE + 2, &alias, NULL);
    CHECK(table_real() == (1u << 1 | 1u << 2) && splits(pr, &depth), "a table with an alias of the depth memory in another format: the pass ends too");
    mtl_reg_tex(BASE + 2, NULL, NULL); mtl_reg_tex(BASE + 3, &moved, NULL);
    CHECK(splits(pr, &depth), "a table with a texture whose storage is not known: the pass ends");
    mtl_reg_tex(BASE + 3, NULL, NULL);
    CHECK(splits(pr, &nomem), "a depth attachment whose storage is not known: the pass ends");
    CHECK(!splits(pr, &depth), "with those gone, the table of the other texture keeps the pass again");

    mtl_reg_tex(BASE + 2, &depth, NULL);
    CHECK(!splits(pool_prog(0), &depth), "a shader with only a sampler pool reads no texture through it: the pass stays");

    mtl_reg_tex(BASE + 2, NULL, NULL);
    g_fb_dpool_m = 0; by_pool = g_fb_split_by[2];
    CHECK(splits(pr, &depth) && g_fb_split_by[2] == by_pool + 1, "DM_NVN_FB_DPOOL=0: every pool shader is a depth reader, as before");
    g_fb_dpool_m = 1;

    mtl_reg_tex(BASE + 1, NULL, NULL); mtl_reg_tex(BASE - 50, NULL, NULL);
    mtl_tex_release(&depth); mtl_tex_release(&again); mtl_tex_release(&alias); mtl_tex_release(&other); mtl_tex_release(&moved); mtl_tex_release(&g_color);
    mtl_alias_drop_pool(&g_pool); mtl_pool_del(g_mem); mtl_drain(); free(g_mem);
    printf("t_fb_dpool: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
