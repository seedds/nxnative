// Compute dispatches ordered by the game's own barriers (nvn_mtl.m dispatch_enc_cs, the default): a barrier where the game issued one, where a copy's
// range meets the dispatch's, and around a dispatch whose reach is no range; none between dispatches the game did not order. Run in both modes (DM_NVN_DISPATCH_BARRIER=full / game):
// the results are the same, the barriers differ.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)
#define EXPECT(label, want, got) do { checks++; long long w_ = (long long)(want), g_ = (long long)(got); if (w_ != g_) { failed++; fprintf(stderr, "FAIL: %s: expected %lld, got %lld\n", label, w_, g_); } } while (0)
enum { N = 64 };
static NvShader *kernel(id<MTLLibrary> lib, NSString *name) {
    NvShader *cs = [NvShader new]; cs->stage = 5; cs->fn = [lib newFunctionWithName:name]; cs->ae = [cs->fn newArgumentEncoderWithBufferIndex:0];
    cs->rf.tg[0] = cs->rf.tg[1] = cs->rf.tg[2] = 1; cs->rf.nb = 2; cs->rf.drv_read[0] = 1;
    cs->rf.b[0] = (nvn_rbind){.kind=NVN_RB_SBUFP, .rw=1, .reg=0, .aid=0};
    cs->rf.b[1] = (nvn_rbind){.kind=NVN_RB_DRV0, .aid=1};
    return cs;
}
static void begin(void) { mtl_sub_begin(); state_reset(); memset(&R, 0, sizeof R); }
static void end(const char *what) { uint64_t fence = mtl_sub_commit(); CHECK(mtl_wait(fence, 5000000000ull), what); mtl_drain(); CHECK(!mtl_last_commit_error(), "no GPU error"); }

int main(void) { @autoreleasepool {
    mtl_init();
    int game = dbar_game();
    NSError *error = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct Args { device uint *buf [[id(0)]]; constant uint *c0 [[id(1)]]; };\n"
        "kernel void chain(constant Args &a [[buffer(0)]], uint3 p [[thread_position_in_grid]]) { if (!any(p != uint3(0))) a.buf[0] = a.buf[0] * 3u + 1u; }\n"
        "kernel void slot(constant Args &a [[buffer(0)]], uint3 p [[thread_position_in_grid]]) { if (!any(p != uint3(0))) a.buf[0] = a.buf[1] + 1u; }\n" options:nil error:&error];
    CHECK(lib != nil, "synthetic compute shaders compile"); if (!lib) return 1;
    prog_t pc = {.serial=970001}, ps = {.serial=970002};
    NvProg *chain = [NvProg new], *slot = [NvProg new]; chain->sh[5] = kernel(lib, @"chain"); slot->sh[5] = kernel(lib, @"slot");
    serial_put(pc.serial, chain); serial_put(ps.serial, slot);
    size_t page = (size_t)getpagesize(), size = 4 * page;
    uint8_t *mem = NULL, *commands = NULL;
    if (posix_memalign((void **)&mem, page, size) || posix_memalign((void **)&commands, page, size)) return 1;
    memset(mem, 0, size); memset(commands, 0, size); mtl_pool_add(mem, size); mtl_pool_add(commands, size);
    uint32_t *word = (uint32_t *)mem;
    p_dispatch one = {.x=1, .y=1, .z=1};
    size_t used = 0;
    #define EMIT(OP, X) do { size_t z = (8 + sizeof(X) + 7) & ~7ull; op_hdr h = {OP, (uint32_t)z}; memcpy(commands + used, &h, 8); memcpy(commands + used + 8, &(X), sizeof(X)); used += z; } while (0)
    #define RUN() do { op_hdr e_ = {OP_END, 8}; memcpy(commands + used, &e_, 8); uint64_t h_ = (uint64_t)commands; mtl_sub_index(&h_, 1); mtl_exec(commands); used = 0; } while (0)

    // 1. A chain on one word, each dispatch behind the game's barrier: the sequential result in both modes
    { word[0] = 5; uint32_t want = 5; for (int i = 0; i < N; i++) want = want * 3u + 1u;
      int32_t bits = NVN_BARRIER_ORDER_PRIMITIVES_BIT;
      for (int i = 0; i < N; i++) { if (i) EMIT(OP_BARRIER, bits); EMIT(OP_DISPATCH, one); }
      begin(); S.cprog = &pc; S.ssbo[5][0] = (p_bind_buf){.addr=(uint64_t)mem, .size=64};
      uint64_t b0 = mtl_blit_barriers(), g0 = g_dbar_game; RUN();
      EXPECT("a barrier before every dispatch the game ordered", N - 1, mtl_blit_barriers() - b0);
      if (game) EXPECT("counted as the game's own", N - 1, g_dbar_game - g0);
      end("the chain completes"); EXPECT("the chain runs in order", want, word[0]); }

    // 2. Dispatches into slots of their own, no game barrier: none in game mode, one each in full mode
    { for (int i = 0; i < N; i++) { word[16 * i] = 0; word[16 * i + 1] = (uint32_t)i; }
      begin(); S.cprog = &ps; uint64_t b0 = mtl_blit_barriers(), f0 = g_dbar_free;
      for (int i = 0; i < N; i++) { S.ssbo[5][0] = (p_bind_buf){.addr=(uint64_t)(mem + 64 * i), .size=64}; dispatch(&one); }
      EXPECT("barriers between dispatches the game did not order", game ? 0 : N - 1, mtl_blit_barriers() - b0);
      if (game) EXPECT("counted as none", N, g_dbar_free - f0);
      end("the slots complete"); int ok = 1; for (int i = 0; i < N; i++) ok &= word[16 * i] == (uint32_t)i + 1;
      CHECK(ok, "every slot's dispatch ran"); }

    // 3. A fill, then a dispatch that reads it: ordered by the copy's range in both modes
    { word[0] = 0;
      begin(); S.cprog = &pc; S.ssbo[5][0] = (p_bind_buf){.addr=(uint64_t)mem, .size=64};
      uint64_t b0 = mtl_blit_barriers(), c0 = g_dbar_copy;
      mtl_sub_fill((uint64_t)mem, 4, 7); dispatch(&one);
      EXPECT("a barrier between a fill and the dispatch that reads it", 1, mtl_blit_barriers() - b0);
      if (game) EXPECT("counted as a copy's range", 1, g_dbar_copy - c0);
      end("the fill and its dispatch complete"); EXPECT("the dispatch sees the fill", 7 * 3 + 1, word[0]); }

    // 4. A dispatch, then a copy of what it wrote: the copy is ordered after it in both modes
    { word[0] = 2; word[512] = 0;
      struct { uint64_t source, destination, bytes; } copy = {(uint64_t)mem, (uint64_t)(mem + 2048), 4};
      EMIT(OP_DISPATCH, one); EMIT(OP_COPY_B2B, copy);
      begin(); S.cprog = &pc; S.ssbo[5][0] = (p_bind_buf){.addr=(uint64_t)mem, .size=64};
      uint64_t b0 = mtl_blit_barriers(); RUN();
      EXPECT("a barrier between a dispatch and the copy that reads what it wrote", 1, mtl_blit_barriers() - b0);
      end("the dispatch and its copy complete"); EXPECT("the copy sees the dispatch", 2 * 3 + 1, word[512]); }

    // 5. A binding whose reach is no range (no size): the full order on both sides, in both modes
    { word[0] = 1;
      begin(); S.cprog = &pc; S.ssbo[5][0] = (p_bind_buf){.addr=(uint64_t)mem, .size=0};
      uint64_t b0 = mtl_blit_barriers(), u0 = g_dbar_unknown;
      dispatch(&one); dispatch(&one); S.ssbo[5][0].size = 64; dispatch(&one);
      EXPECT("barriers around dispatches with no range", 2, mtl_blit_barriers() - b0);
      if (game) EXPECT("counted as no range", 2, g_dbar_unknown - u0);
      end("the rangeless dispatches complete"); EXPECT("they run in order", ((1 * 3 + 1) * 3 + 1) * 3 + 1, word[0]); }

    // 6. What a dispatch wrote orders nothing in the next copy encoder, which starts behind a full barrier of its own: two fills there, the second over the dispatch's range, get no barrier
    { word[0] = 0;
      begin(); S.cprog = &pc; S.ssbo[5][0] = (p_bind_buf){.addr=(uint64_t)mem, .size=64}; dispatch(&one); end("the dispatch completes");
      begin(); uint64_t b0 = mtl_blit_barriers();
      mtl_sub_fill((uint64_t)(mem + 2048), 4, 1); mtl_sub_fill((uint64_t)mem, 4, 9);
      EXPECT("no barrier from a closed encoder's dispatch", 0, mtl_blit_barriers() - b0);
      end("the fills complete"); EXPECT("the second fill lands", 9, word[0]); }

    mtl_pool_del(mem); mtl_pool_del(commands); mtl_drain();
    printf("t_dispatch_barrier (%s): %d checks, %d failed\n", game ? "game" : "full", checks, failed); return failed != 0;
} }
