// Vertex streams as the GPU reads them: the Vulkan port of the behaviours dm/gfx/tests/t_vbuf.m checks on real Metal. A vertex-only program (rasterization off) reads the first
// word of streams 0, 14 and 15 and of an attribute no stream feeds, and writes them to row gl_InstanceIndex of a storage buffer. Checked: the compact binding record
// (nvnCommandBufferBindVertexBuffer, OP_BIND_VBUF1) and a rebinding; a pool replaced while a submission that reads it may still run (its old import released after the GPU);
// a stream at an address 2 mod 4 (Arkham Knight's), which Vulkan cannot fetch, copied aligned: from the CPU when its bytes are current, on the GPU in order when a copy, an
// inline update or a fill earlier in the submission, a submission still running, or a vertex shader's storage write produce them; every nonzero residue; an unrelated update.
// The Metal test's binding-table cache counters and its CPU microbenchmark are Metal's (no counterpart). Checks: each row's words, and the validation check
#include "t_vk_harness.h"
#include "shaders/gen/vcap_vert.h"

static NVNqueue g_q;
static hv_cmds g_c;
static void capture(int row) {   // one point, instance row
    p_draw d; memset(&d, 0, sizeof d); d.prim = NVN_DRAW_PRIMITIVE_POINTS; d.count = 1; d.instances = 1; d.base_instance = row;
    hv_emit(&g_c, OP_DRAW, &d, offsetof(p_draw, ind));
}

int main(void) {
    hv_up("t_vk_vbuf");
    hv_device(0);
    NVNqueueBuilder qb; memset(&qb, 0, sizeof qb); h_nvnQueueBuilderSetDefaults(&qb); h_nvnQueueBuilderSetDevice(&qb, &g_dev); h_nvnQueueInitialize(&g_q, &qb);
    hv_pool cp, dp;   // command memory and the program; the data the streams read (replaced below)
    if (!hv_pool_make(&cp, 1 << 18) || !hv_pool_make(&dp, 16384)) { t("memory pools allocated", 0); return hv_finish(); }
    g_c = (hv_cmds){cp.mem + 65536, 0, 65536};
    uint32_t *mem = (uint32_t *)(void *)dp.mem; memset(dp.mem, 0, 16384);
    mem[0] = 11; mem[1] = 22; mem[2] = 33; mem[3] = 99;
    uint8_t *ub = dp.mem + 64, *producer = dp.mem + 128;
    uint32_t want = 0x87654321u, changed = 0x12345678u, changed2 = 0xa1b2c3d4u;
    memcpy(ub + 2, &want, 4); ub[0] = 0x11; ub[1] = 0x22; ub[6] = 0x33; ub[7] = 0x44;
    memcpy(producer, ub, 8); memcpy(producer + 2, &changed, 4);
    uint32_t *out = (uint32_t *)(void *)(dp.mem + 4096);
    static NVNprogram prog;
    const hv_shader sh[1] = {{"vcap", 0, HV_SPV(vcap_vert_spv), "[{\"kind\":\"sbuf\",\"reg\":0,\"name\":\"O\",\"class\":\"ssbo\",\"set\":1,\"binding\":0,\"rw\":true}]",
                              "[{\"location\":0,\"type\":\"uint\",\"n\":1},{\"location\":1,\"type\":\"uint\",\"n\":1},{\"location\":2,\"type\":\"uint\",\"n\":1},{\"location\":3,\"type\":\"uint\",\"n\":1}]"}};
    hv_program(&prog, &cp, 0, sh, 1);
    tex_t target = hv_tex(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, 1, 1, 1, 1, 0);
    const tex_t *rt[1] = {&target};
    hv_rt(&g_c, 1, rt, NULL); hv_bind(&g_c, &prog, 1);
    p_rast_discard rd = {1}; hv_emit(&g_c, OP_RAST_DISCARD, &rd, sizeof rd);
    const va_st attrs[3] = {{.fmt = NVN_FORMAT_R32UI, .stream = 0}, {.fmt = NVN_FORMAT_R32UI, .stream = 14}, {.fmt = NVN_FORMAT_R32UI, .stream = 15}};
    vs_st streams[16]; for (int i = 0; i < 16; i++) streams[i] = (vs_st){4, 0};
    hv_attrs(&g_c, 3, attrs); hv_streams(&g_c, 16, streams);
    hv_buf(&g_c, OP_BIND_SSBO, 0, 0, out, 512);
    hv_vbuf(&g_c, 0, mem, 4); hv_vbuf(&g_c, 14, mem + 1, 4); hv_vbuf(&g_c, 15, mem + 2, 4);
    capture(0);
    hv_vbuf(&g_c, 0, mem + 3, 4); capture(1);
    hv_submit_nowait(&g_q, &g_c);
    t("pool storage initialized again while a submission reading it may still run", hv_pool_replace(&dp));   // the old import goes when that submission has completed
    capture(2); hv_submit(&g_q, &g_c);
    hv_vbuf(&g_c, 0, ub + 2, 6); capture(3); hv_submit(&g_q, &g_c);                   // unaligned, current on the CPU
    hv_b2b(&g_c, producer, ub, 8); capture(4); hv_submit(&g_q, &g_c);              // written by a copy earlier in the submission: copied on the GPU
    memcpy(producer + 2, &want, 4);
    hv_b2b(&g_c, producer, ub, 8); capture(5); hv_submit(&g_q, &g_c);
    for (int residue = 1; residue < 4; residue++) {                                  // every nonzero residue, a short odd-sized range
        uint32_t value = 0x76543210u + (uint32_t)residue; memcpy(producer + residue, &value, 4);
        hv_vbuf(&g_c, 0, ub + residue, 5); hv_b2b(&g_c, producer, ub, 8); capture(5 + residue); hv_submit(&g_q, &g_c);
    }
    memcpy(producer + 3, &changed2, 4);
    hv_update(&g_c, ub, producer, 8); capture(9); hv_submit(&g_q, &g_c);           // an inline update (ub + 3, from the last residue)
    hv_fill(&g_c, ub, 8, 0x6d6d6d6du); capture(10); hv_submit(&g_q, &g_c);         // a fill
    hv_b2b(&g_c, producer, ub, 8); hv_submit_nowait(&g_q, &g_c);                   // a preceding submission, not waited for
    capture(11); hv_submit(&g_q, &g_c);
    // a vertex shader's storage write produces the stream: the capture shader itself, its output bound at ub (row 0 there: 99, 22, 33, 0), then ub + 2 read as a stream
    hv_vbuf(&g_c, 0, mem + 3, 4); hv_buf(&g_c, OP_BIND_SSBO, 0, 0, ub, 64); capture(0);
    hv_buf(&g_c, OP_BIND_SSBO, 0, 0, out, 512); hv_vbuf(&g_c, 0, ub + 2, 6); capture(12);
    hv_update(&g_c, dp.mem + 1024, producer, 8); capture(13);                      // an unrelated update
    hv_submit(&g_q, &g_c);
    const struct { int row; uint32_t s0; const char *label; } rows[] = {
        {0, 11, "compact stream bindings read streams 0, 14 and 15 and the zero buffer"}, {1, 99, "a rebinding reads the new address"},
        {2, 99, "a replaced pool's new import is read"}, {3, 0x87654321u, "unaligned stream current on the CPU, copied aligned"},
        {4, 0x12345678u, "unaligned stream written by a copy earlier in the submission: copied on the GPU, in order"}, {5, 0x87654321u, "a second copy in a later submission"},
        {6, 0x76543211u, "residue 1 written by a copy"}, {7, 0x76543212u, "residue 2 written by a copy"}, {8, 0x76543213u, "residue 3 written by a copy"},
        {9, 0xa1b2c3d4u, "unaligned stream written by an inline update"}, {10, 0x6d6d6d6du, "unaligned stream written by a fill"},
        {11, 0xa1b2c3d4u, "unaligned stream written by a preceding submission"}, {12, 22u << 16, "unaligned stream written by a vertex shader's storage write"},
        {13, 22u << 16, "an unrelated update leaves the stream as it was"},
    };
    for (unsigned i = 0; i < sizeof rows / sizeof *rows; i++) {
        const uint32_t *r = out + 4 * rows[i].row;
        int ok = r[0] == rows[i].s0 && r[1] == 22 && r[2] == 33 && r[3] == 0;
        if (!ok) fprintf(stderr, "row %d: %#x %u %u %u, expected %#x 22 33 0\n", rows[i].row, r[0], r[1], r[2], r[3], rows[i].s0);
        t(rows[i].label, ok);
    }
    hv_pool_free(&dp); hv_pool_free(&cp);
    return hv_finish();
}
