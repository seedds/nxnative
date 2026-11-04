// NVN2 stage binding and capture regressions without GPU execution or game files.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) {
    return NULL;
}
module_t *module_of(const void *p) {
    (void)p;
    return NULL;
}
module_t *g_mod;
int g_nmod;
void svc_thread_dump(void) {
}
uint64_t now_ns(void) {
    return bp_now();
}

static int checks, failures;
#define CHECK(expr, message)                                                                                           \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(expr)) {                                                                                                 \
            failures++;                                                                                                \
            fprintf(stderr, "FAIL: %s\n", message);                                                                    \
        }                                                                                                              \
    } while (0)

@interface MeshEncoderProbe : NSObject {
  @public
    MTLSize grid, objectThreads, meshThreads;
    int calls;
}
@end
@implementation MeshEncoderProbe
- (void)drawMeshThreadgroups:(MTLSize)g threadsPerObjectThreadgroup:(MTLSize)o threadsPerMeshThreadgroup:(MTLSize)m {
    grid = g;
    objectThreads = o;
    meshThreads = m;
    calls++;
}
@end

int main(void) {
    @autoreleasepool {
        CHECK(stage_of(@"task") == 6 && stage_of(@"mesh") == 7, "native task/mesh stage slots");
        CHECK(c0_ssbo_base(6) == 0x670 && c0_ssbo_base(7) == 0x770, "native task/mesh driver storage-buffer offsets");
        NvProg *mesh = [NvProg new], *pixel = [NvProg new], *compute = [NvProg new];
        NvShader *ts = [NvShader new], *ms = [NvShader new], *fs = [NvShader new], *cs = [NvShader new];
        ts->stage = 6;
        ms->stage = 7;
        fs->stage = 1;
        cs->stage = 5;
        mesh->sh[6] = ts;
        mesh->sh[7] = ms;
        pixel->sh[1] = fs;
        compute->sh[5] = cs;
        prog_t pm = {.serial = nvn_new_serial()}, pf = {.serial = nvn_new_serial()}, pc = {.serial = nvn_new_serial()};
        // NOLINTBEGIN(clang-analyzer-osx.cocoa.RetainCount): serial_put transfers CF retains to the C hash table; fixture teardown below releases every entry.
        serial_put(pm.serial, mesh);
        serial_put(pf.serial, pixel);
        serial_put(pc.serial, compute);
        state_reset();
        // NOLINTEND(clang-analyzer-osx.cocoa.RetainCount)
        p_bind_program bp = {&pm, 0xc0};
        state_op(OP_BIND_PROGRAM, &bp);
        NvProg *combined = prog_get(S.prog);
        CHECK(combined && combined->sh[6] == ts && combined->sh[7] == ms && !combined->sh[0],
              "mesh program remains usable without a vertex shader");
        bp = (p_bind_program){&pf, 2};
        state_op(OP_BIND_PROGRAM, &bp);
        combined = prog_get(S.prog);
        CHECK(combined && combined->sh[6] == ts && combined->sh[7] == ms && combined->sh[1] == fs,
              "separate task/mesh and fragment programs compose");
        const prog_t *graphics = S.prog;
        bp = (p_bind_program){&pc, 0x20};
        state_op(OP_BIND_PROGRAM, &bp);
        CHECK(S.cprog == &pc && S.prog == graphics, "compute binding leaves graphics stages intact");
        bp = (p_bind_program){NULL, 0x40};
        state_op(OP_BIND_PROGRAM, &bp);
        combined = prog_get(S.prog);
        CHECK(combined && !combined->sh[6] && combined->sh[7] == ms && combined->sh[1] == fs,
              "task can be unbound while keeping mesh and fragment");
        mesh->sh[1] = fs;
        state_reset();
        bp = (p_bind_program){&pm, 0xc2};
        state_op(OP_BIND_PROGRAM, &bp);
        CHECK(S.prog == &pm, "one program with all three bound stages keeps the direct fast path");
        bp = (p_bind_program){NULL, 0x40};
        state_op(OP_BIND_PROGRAM, &bp);
        combined = prog_get(S.prog);
        CHECK(combined && !combined->sh[6] && combined->sh[7] == ms && combined->sh[1] == fs,
              "fast path cannot resurrect an explicitly unbound task shader");

        // Upstream's finalized-program handling must also clear the two NVN2 graphics stages.
        bp = (p_bind_program){&pm, 0xc0};
        state_op(OP_BIND_PROGRAM, &bp);
        bp = (p_bind_program){&pf, 2};
        state_op(OP_BIND_PROGRAM, &bp);
        mtl_prog_forget(&pm);
        CHECK(!S.bst[6] && !S.bst[7] && !S.prog && S.bst[1] == &pf,
              "finalizing task/mesh leaves no drawable program but preserves the fragment binding");
        bp = (p_bind_program){&pm, 0xc0};
        state_op(OP_BIND_PROGRAM, &bp);
        combined = prog_get(S.prog);
        CHECK(combined && combined->sh[6] == ts && combined->sh[7] == ms && combined->sh[1] == fs,
              "rebinding task/mesh restores the saved fragment stage");

        for (int stage = 6; stage <= 7; stage++) {
            p_bind_buf u = {stage, 15, 0x10000u + (uint64_t)stage * 0x1000, 256};
            state_op(OP_BIND_UBO, &u);
            state_op(OP_BIND_SSBO, &u);
            CHECK(S.ubo[stage][15].addr == u.addr && S.ssbo[stage][15].addr == u.addr,
                  "task/mesh uniform and storage bindings survive replay");
        }
        uint64_t hashes[NVN_STAGE_COUNT];
        cap_iface(mesh, hashes);
        CHECK(!hashes[0] && hashes[1] && hashes[6] && hashes[7], "capture fingerprints include task and mesh");
        uint64_t old_task = hashes[6], old_mesh = hashes[7];
        ms->rf.tg[0] = 8;
        cap_iface(mesh, hashes);
        CHECK(hashes[6] == old_task && hashes[7] != old_mesh,
              "capture detects changed mesh threadgroup interface independently");
        struct {
            op_hdr h;
            uint32_t dims[4];
            op_hdr end;
        } commands = {{OP_DRAW_MESH, 24}, {0, 1, 1, 1}, {OP_END, 8}};
        ds_st ds = {0};
        g_ds_need = 0;
        CHECK(ds_scan((const uint8_t *)&commands, &ds, 0) == DS_USED,
              "mesh draw conservatively retains depth contents during lookahead");
        // Exercise both the stand-in selector and the encode thread's production decoder without creating a GPU encoder.
        rec_blk block = {0};
        g_rb_cur = &block;
        DmRecEnc *recordingEncoder = [DmRecEnc new];
        [recordingEncoder drawMeshThreadgroups:MTLSizeMake(19, 23, 29)
                   threadsPerObjectThreadgroup:MTLSizeMake(2, 3, 5)
                     threadsPerMeshThreadgroup:MTLSizeMake(7, 11, 13)];
        CHECK(block.n == 1 && block.r[0].op == RO_R_MESH && block.r[0].nobj == 0,
              "deferred mesh call records one mesh operation");
        g_rb_cur = NULL;
        MeshEncoderProbe *probe = [MeshEncoderProbe new];
        g_rx_enc = probe;
        rec_exec(&block.r[0]);
        g_rx_enc = nil;
        CHECK(probe->calls == 1 && probe->grid.width == 19 && probe->grid.height == 23 && probe->grid.depth == 29,
              "deferred replay preserves all three mesh grid dimensions");
        CHECK(probe->objectThreads.width == 2 && probe->objectThreads.height == 3 && probe->objectThreads.depth == 5,
              "deferred replay preserves all three object thread dimensions");
        CHECK(probe->meshThreads.width == 7 && probe->meshThreads.height == 11 && probe->meshThreads.depth == 13,
              "deferred replay preserves all three mesh thread dimensions");
        // No GPU work was submitted: release fixture programs and cached composites directly, without deferring their lifetime to a GPU fence.
        state_reset();
        nvn_hm_clear(&g_by_serial, hm_rel);
        printf("t_mesh_stages: %d checks, %d failed\n", checks, failures);
        return failures != 0;
    }
}
