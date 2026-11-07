// The store lookahead (DM_NVN_TBDR=1) on draws whose textures are read through handles: an attachment that only a later
// bindless draw samples (a handle in a uniform buffer, TEXU, or an array of them, TEXA / TEXD, or a shader the reflection calls
// bindless or dynamic) must keep its store, while one that nothing reads before its next full clear
// still drops it. Builds a submission's command list, indexes it with the production mtl_sub_index and asks sdx_decide at the
// end of the first pass. No GPU work, no game shaders or files.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

typedef struct { uint8_t p[4096]; size_t n; } chain;
static void emit(chain *c, unsigned op, const void *a, size_t n) {
    size_t sz = (8 + n + 7) & ~7ull; if (c->n + sz > sizeof c->p) abort();
    op_hdr h = {op, (uint32_t)sz}; memcpy(c->p + c->n, &h, 8);
    if (n) memcpy(c->p + c->n + 8, a, n); c->n += sz;
}
static tex_t target(uint64_t serial) {
    tex_t t = {.magic = MAGIC('T','E','X',0), .serial = serial};
    t.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8, .width = 8, .height = 8, .depth = 1, .levels = 1, .samples = 1};
    nvn_tex_layout(&t.d, &t.L); return t;
}
static void set_rt(chain *c, const tex_t *t) { p_set_rt r = {.n = 1}; r.color[0] = t; emit(c, OP_SET_RT, &r, sizeof r); }
static void bind(chain *c, const prog_t *p) { p_bind_program b = {p, 3}; emit(c, OP_BIND_PROGRAM, &b, sizeof b); }
static void tri(chain *c) { p_draw d = {.prim = NVN_DRAW_PRIMITIVE_TRIANGLES, .count = 3, .instances = 1}; emit(c, OP_DRAW, &d, sizeof d); }
static void clear(chain *c) { p_clear_color cl = {.index = 0, .mask = 0xf}; emit(c, OP_CLEAR_COLOR, &cl, sizeof cl); }

// A program whose fragment shader has one binding of the given kind (0: none); flag 1 = reflection bindless, 2 = dynamic
static void program(prog_t *pg, uint64_t serial, int kind, int flag) {
    pg->serial = serial;
    NvProg *p = [NvProg new]; p->sh[0] = [NvShader new]; p->sh[1] = [NvShader new];
    p->sh[0]->stage = 0; p->sh[1]->stage = 1;
    if (kind) { p->sh[1]->rf.nb = 1; p->sh[1]->rf.b[0] = (nvn_rbind){.kind = kind, .reg = 0}; }
    p->sh[1]->rf.bindless = flag == 1; p->sh[1]->rf.dyn = flag == 2;
    serial_put(serial, p);
}   // NOLINT(clang-analyzer-osx.cocoa.RetainCount): g_by_serial owns p (serial_put's CFBridgingRetain) for the life of the test, not a leak

// pass 1 draws A; pass 2 (target B) draws with `reader`; then A is attached again and cleared in full. The store decision of A at
// the end of pass 1: 2 = redundant (dropped), anything else = kept.
static int decide(const prog_t *reader) {
    static uint64_t ser = 600000; tex_t a = target(++ser), b = target(++ser);
    prog_t plain; program(&plain, ++ser, 0, 0);
    chain c = {0};
    set_rt(&c, &a); bind(&c, &plain); tri(&c);         // seq 0 1 2
    set_rt(&c, &b); bind(&c, reader); tri(&c);         // seq 3 4 5: the pass of A ends at 3
    set_rt(&c, &a); clear(&c); tri(&c);                // seq 6 7 8
    emit(&c, OP_END, NULL, 0);
    state_reset(); memset(&R, 0, sizeof R);
    uint64_t h = (uint64_t)c.p; mtl_sub_index(&h, 1);
    CHECK(g_sdx_ok, "the index is available");
    g_exec_cur = g_sdx_op[3]; g_exec_seq = 4;
    g_nsd = 1; g_sd[0] = (sd_att){.ser = a.serial, .idx = 0};
    uint8_t st[SD_ATT_MAX] = {0};
    CHECK(sdx_decide(st), "the decision is made from the index");
    g_exec_cur = NULL; g_exec_seq = 0; g_nsd = 0;
    return st[0];
}

int main(void) {
    @autoreleasepool {
        setenv("DM_NVN_TBDR", "1", 1);
        CHECK(tbdr_on(), "lookahead on");
        prog_t none; program(&none, 700001, 0, 0);
        CHECK(decide(&none) == 2, "nothing reads A before its full clear: its store is dropped");
        prog_t slot; program(&slot, 700002, NVN_RB_TEX, 0);   // reads slot 0, which holds nothing: not A
        CHECK(decide(&slot) == 2, "a slot read of another texture leaves the store dropped");
        const int kinds[] = {NVN_RB_TEXU, NVN_RB_TEXA, NVN_RB_TEXD};
        const char *names[] = {"a handle in a uniform buffer (TEXU) keeps the store", "an array of handles (TEXA) keeps the store",
                               "a dynamic handle array (TEXD) keeps the store"};
        for (int i = 0; i < 3; i++) { prog_t p; program(&p, 700010 + i, kinds[i], 0); CHECK(decide(&p) != 2, names[i]); }
        prog_t bl; program(&bl, 700020, 0, 1); CHECK(decide(&bl) != 2, "a shader the reflection calls bindless keeps the store");
        prog_t dy; program(&dy, 700021, 0, 2); CHECK(decide(&dy) != 2, "a dynamic shader keeps the store");
    }
    printf("LOOKAHEAD_BINDLESS %d checks, %d failed\n", checks, failed);
    return failed != 0;
}
