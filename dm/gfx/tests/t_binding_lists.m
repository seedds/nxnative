// The per-draw checks of what a draw's shaders read (fb_depth_rule, fb_prepare, ubd_conflict_textures, ubd_conflict_sh) walk two lists made once a shader (sh_lists_make): the
// bindings that name a texture or an image, and those that read a buffer or a handle out of one. Each list must hold every binding kind its walkers act on, in the order of
// rf.b, and nothing else: a kind left out is a check silently not made (NVN_RB_IMGP, added to ubd_conflict_textures while the lists were written, was). A kind added to
// nvn_refl.h is decided here: the assertion on NVN_RB_END below stops the build until it is. The expected lists are written out from the walkers' code, and
// ubd_conflict_textures is run through its list below; fb_prepare and fb_depth_rule need a render pass and act on a subset of the texture list (TEX, IMG, TEXU)
#include "../nvn/nvn_mtl.m"
uint64_t g_t0_ns;
int dm_guest_as_contains(uintptr_t address) { (void)address; return 0; }
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(label, c) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s (line %d)\n", label, __LINE__); } } while (0)
enum { KINDS = NVN_RB_END - 1 };   // the last kind of nvn_refl.h's enum
_Static_assert(NVN_RB_END == 43, "a binding kind was added to nvn_refl.h: decide below whether the per-draw walks act on it, then raise this");
// The kinds the walks act on, written out from their code: ubd_conflict_textures' switch (fb_depth_rule and fb_prepare take TEX, IMG and TEXU of them)
static int is_tex(int k) {
    return k == NVN_RB_TEX || k == NVN_RB_TBUF || k == NVN_RB_TEXU || k == NVN_RB_TBUFU || k == NVN_RB_IMG || k == NVN_RB_IMGU || k == NVN_RB_TEXD || k == NVN_RB_TEXA
        || k == NVN_RB_TEXP || k == NVN_RB_TEXH || k == NVN_RB_IMGP;
}
// and ubd_conflict_sh's loop after the textures
static int is_buf(int k) {
    return k == NVN_RB_CBUF || k == NVN_RB_SBUFP || k == NVN_RB_UBOP || k == NVN_RB_GMEM || k == NVN_RB_TEXU || k == NVN_RB_SAMPU || k == NVN_RB_SBUFU || k == NVN_RB_IMGU || k == NVN_RB_TBUFU;
}
static void check_lists(NvShader *sh, const char *what) {
    const uint8_t *tl, *bl; int nt = sh_tex_list(sh, &tl), nb = sh_buf_list(sh, &bl), wt = 0, wb = 0, ok_t = 1, ok_b = 1;
    for (int i = 0; i < sh->rf.nb; i++) {
        if (is_tex(sh->rf.b[i].kind)) { ok_t &= wt < nt && tl[wt] == i; wt++; }
        if (is_buf(sh->rf.b[i].kind)) { ok_b &= wb < nb && bl[wb] == i; wb++; }
    }
    char label[160];
    snprintf(label, sizeof label, "%s: the texture list holds every texture binding and no other, in rf.b's order (%d of %d)", what, nt, wt); CHECK(label, ok_t && nt == wt);
    snprintf(label, sizeof label, "%s: the buffer list holds every buffer binding and no other, in rf.b's order (%d of %d)", what, nb, wb); CHECK(label, ok_b && nb == wb);
}
int main(void) { @autoreleasepool {
    { int nt = 0, nb = 0; for (int k = 1; k <= KINDS; k++) { nt += is_tex(k); nb += is_buf(k); } CHECK("eleven texture kinds and nine buffer kinds", nt == 11 && nb == 9); }
    // Every kind once, then every kind again backwards: 2 * KINDS bindings, each list twice its kinds
    NvShader *all = [NvShader new]; all->stage = 1;
    for (int k = 1; k <= KINDS; k++) { all->rf.b[k - 1].kind = (uint8_t)k; all->rf.b[2 * KINDS - k].kind = (uint8_t)k; }
    all->rf.nb = 2 * KINDS;
    CHECK("the lists are not made before they are asked for", all->lists_nb == 0);
    check_lists(all, "every kind twice");
    const uint8_t *l; int nt = sh_tex_list(all, &l);
    CHECK("22 texture bindings", nt == 22 && all->tex_n == 22 && all->buf_n == 18 && all->lists_nb == all->rf.nb + 1);
    CHECK("the first is rf.b's first texture binding and the last its last", nt == 22 && all->rf.b[l[0]].kind == NVN_RB_TEX && l[0] == NVN_RB_TEX - 1 && l[nt - 1] == 2 * KINDS - NVN_RB_TEX);
    { int up = 1; for (int i = 1; i < nt; i++) up &= l[i] > l[i - 1]; CHECK("in rising order", up); }
    // Asked again, the lists are the ones made (a list is made once a shader)
    all->tex_idx[0] = 0xee; sh_tex_list(all, &l);
    CHECK("a second call does not make the lists again", l[0] == 0xee);
    // rf.nb changes (the tests build reflections by hand; a shader's own never changes): the lists are made for the bindings there are
    all->rf.nb = KINDS;
    check_lists(all, "every kind once");
    CHECK("half the bindings give half the lists", sh_tex_list(all, &l) == 11 && sh_buf_list(all, &l) == 9 && all->lists_nb == KINDS + 1);
    all->rf.nb = NVN_RB_SAMP;   // CBUF and SAMP
    check_lists(all, "a uniform buffer and a sampler");
    CHECK("no texture binding gives an empty texture list", sh_tex_list(all, &l) == 0);
    CHECK("and its uniform buffer alone in the buffer list", sh_buf_list(all, &l) == 1 && l[0] == 0);
    NvShader *none = [NvShader new];
    CHECK("a shader with no binding has empty lists", sh_tex_list(none, &l) == 0 && sh_buf_list(none, &l) == 0 && none->lists_nb == 1);
    // What a list is for: with an inline update deferred, a buffer texture or image the shader picks on the GPU (a pool or a table of handles: TEXP, TEXH, IMGP) may be the
    // one updated, whatever is bound, so the draw conflicts (ubd_conflict_textures). A kind missing from the texture list never reached that test
    g_nubd = 1;
    static const int picked[] = {NVN_RB_TEXP, NVN_RB_TEXH, NVN_RB_IMGP};
    for (unsigned i = 0; i < sizeof picked / sizeof *picked; i++) {
        NvShader *sh = [NvShader new]; sh->stage = 1; sh->rf.nb = 3;
        sh->rf.b[0].kind = NVN_RB_CBUF; sh->rf.b[1].kind = NVN_RB_SAMP; sh->rf.b[2].kind = (uint8_t)picked[i]; sh->rf.b[2].dtt = MTLTextureTypeTextureBuffer;
        char label[120]; snprintf(label, sizeof label, "a buffer texture picked on the GPU (kind %d) conflicts with a deferred update", picked[i]);
        CHECK(label, ubd_conflict_textures(sh, 1) == 1);
        sh->rf.b[2].dtt = MTLTextureType2D;
        snprintf(label, sizeof label, "a 2D texture picked on the GPU (kind %d) does not", picked[i]);
        CHECK(label, ubd_conflict_textures(sh, 1) == 0);
    }
    all->rf.nb = NVN_RB_SAMP;
    CHECK("a shader with no texture binding conflicts with none", ubd_conflict_textures(all, 1) == 0);
    g_nubd = 0;
    printf("t_binding_lists: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
