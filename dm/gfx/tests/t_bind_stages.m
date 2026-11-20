// Per-stage program binding (bind_stages / prog_stage_mask): single-stage graphics programs bound with all graphics bits (0x1f) keep the vertex stage intact,
// while fragment, geometry, tessellation, NVN2 mesh, compute, unmapped and NULL binds still update or unbind their stages without eager shader_get loading.
// CPU only: no Metal device is needed, so CI runs it.
#define nvn_game_fps nvn_game_fps_mtl_default
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
#define g_ft g_ft_hle
#include "../nvn/hle_nvn.c"
#undef g_ft
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

static NvShader *mk_shader(int stage, int mesh_kind) {
    NvShader *s = [NvShader new];
    s->stage = stage;
    s->mesh_kind = mesh_kind;
    return s;
}
static prog_t mk_prog(NvShader *vs, NvShader *fs, NvShader *gs, NvShader *tcs, NvShader *tes_or_ms, NvShader *cs) {
    prog_t p = {.magic = MAGIC('P', 'R', 'O', 'G'), .serial = nvn_new_serial()};
    NvProg *o = [NvProg new];
    o->sh[0] = vs; o->sh[1] = fs; o->sh[2] = gs; o->sh[3] = tcs; o->sh[4] = tes_or_ms; o->sh[5] = cs;
    serial_put(p.serial, o);
    return p;
}
static void bind_op(const prog_t *p, int stages) {
    p_bind_program bp = {.prog = p, .stages = stages};
    state_op(OP_BIND_PROGRAM, &bp);
}
static void install_key(uint64_t serial, unsigned slot, const uint8_t key[8]) {
    prog_keys_t *pk = pk_get(serial);
    memcpy(pk->key[slot], key, 8);
    pk->ok |= (uint8_t)(1u << slot);
}

int main(void) { @autoreleasepool {
    state_reset();
    NvShader *vs1 = mk_shader(0, 0), *vs2 = mk_shader(0, 0);
    NvShader *fs1 = mk_shader(1, 0), *fs2 = mk_shader(1, 0);
    NvShader *gs1 = mk_shader(2, 0), *ts1 = mk_shader(3, 1), *ms1 = mk_shader(4, 2), *ds1 = mk_shader(4, 0), *cs1 = mk_shader(5, 0);

    prog_t p_vs = mk_prog(vs1, nil, nil, nil, nil, nil);
    prog_t p_ps = mk_prog(nil, fs1, nil, nil, nil, nil);
    prog_t p_vs_fs = mk_prog(vs2, fs2, nil, nil, nil, nil);
    prog_t p_vs_gs_fs = mk_prog(vs1, fs1, gs1, nil, nil, nil);
    prog_t p_mesh_fs = mk_prog(nil, fs1, nil, ts1, ms1, nil);
    prog_t p_mesh_depth = mk_prog(nil, nil, nil, ts1, ms1, nil);
    prog_t p_domain = mk_prog(nil, nil, nil, nil, ds1, nil);
    prog_t p_cs = mk_prog(nil, nil, nil, nil, nil, cs1);

    // 1. Little Busters! pattern: single-stage VS then PS, each bound with 0x1f
    bind_op(&p_vs, 0x1f);
    CHECK(S.bst[0] == &p_vs && S.bst[1] == &p_vs && S.prog == &p_vs, "single-stage VS bound with 0x1f binds VS directly");
    bind_op(&p_ps, 0x1f);
    CHECK(S.bst[0] == &p_vs && S.bst[1] == &p_ps && S.bst[2] == &p_ps, "single-stage PS bound with 0x1f preserves vertex slot");
    CHECK(S.prog && S.prog != &p_vs && S.prog != &p_ps && prog_get(S.prog)->sh[0] == vs1 && prog_get(S.prog)->sh[1] == fs1,
          "single-stage VS then PS with 0x1f yields composite with both stages");

    // 2. Xenoblade Chronicles 2 pattern: VS+FS then VS-only bound with 0x1f must unbind fragment
    bind_op(&p_vs_fs, 0x1f);
    CHECK(S.prog == &p_vs_fs && S.bst[0] == &p_vs_fs && S.bst[1] == &p_vs_fs, "VS+FS bound with 0x1f replaces all graphics stages");
    bind_op(&p_vs, 0x1f);
    CHECK(S.bst[0] == &p_vs && S.bst[1] == &p_vs && S.prog == &p_vs && !prog_get(S.prog)->sh[1],
          "VS-only bound with 0x1f after VS+FS unbinds fragment (no stale FS)");

    // 3. Geometry unbind: VS+GS+FS then VS+FS with 0x1f unbinds GS
    bind_op(&p_vs_gs_fs, 0x1f);
    CHECK(S.prog == &p_vs_gs_fs && prog_get(S.prog)->sh[2] == gs1, "VS+GS+FS binds geometry stage");
    bind_op(&p_vs_fs, 0x1f);
    CHECK(S.prog == &p_vs_fs && !prog_get(S.prog)->sh[2], "VS+FS bound with 0x1f unbinds geometry stage");

    // 4. NVN2 Mario Kart World / Ocarina of Time: VS+FS then task+mesh+FS with 0xff / 0xc3 replaces vertex slot (no bogus composite)
    bind_op(&p_mesh_fs, 0xff);
    CHECK(S.bst[0] == &p_mesh_fs && S.bst[1] == &p_mesh_fs && S.bst[4] == &p_mesh_fs && S.prog == &p_mesh_fs,
          "NVN2 task+mesh+FS bound with 0xff replaces vertex slot without a composite");
    bind_op(&p_vs_fs, 0xff);
    bind_op(&p_mesh_fs, 0xc3);
    CHECK(S.bst[0] == &p_mesh_fs && S.bst[1] == &p_mesh_fs && S.bst[4] == &p_mesh_fs,
          "NVN2 task+mesh+FS bound with 0xc3 replaces vertex slot");

    // 5. NVN2 depth-only task+mesh (t_mesh_first_use / Ocarina of Time) bound with 0xc3 after task+mesh+FS unbinds fragment
    bind_op(&p_mesh_depth, 0xc3);
    CHECK(S.bst[1] == &p_mesh_depth && S.bst[4] == &p_mesh_depth && S.prog && !prog_get(S.prog)->sh[1],
          "NVN2 depth-only task+mesh bound with 0xc3 unbinds fragment");

    // 6. NVN1 domain-only (TES, slot 4 without mesh_kind == 2) bound with 0x1f still preserves vertex slot
    bind_op(&p_vs, 0x1f);
    bind_op(&p_domain, 0x1f);
    CHECK(S.bst[0] == &p_vs && S.bst[4] == &p_domain && S.prog && prog_get(S.prog)->sh[0] == vs1 && prog_get(S.prog)->sh[4] == ds1,
          "NVN1 domain-only program bound with 0x1f preserves vertex slot");

    // 7. NVN2 compute-only program bound with 0xff after VS+FS does not fabricate a composite with old VS
    bind_op(&p_vs_fs, 0xff);
    bind_op(&p_cs, 0xff);
    CHECK(S.cprog == &p_cs && S.bst[0] == &p_cs && S.prog == &p_cs,
          "compute-only program bound with 0xff updates compute and does not keep stale VS");

    // 8. Explicit NULL unbinds still clear every bit they name (Metroid Dread / Scarlet)
    bind_op(NULL, 0x1f);
    bind_op(&p_vs_fs, 0x3);
    bind_op(NULL, 0x2);
    CHECK(S.bst[0] == &p_vs_fs && !S.bst[1] && S.prog && prog_get(S.prog)->sh[0] == vs2 && !prog_get(S.prog)->sh[1],
          "NULL with fragment bit unbinds only fragment");
    bind_op(NULL, 0x7);
    CHECK(!S.bst[0] && !S.bst[1] && !S.bst[2] && !S.prog, "NULL with 0x7 unbinds vertex, fragment and geometry");
    bind_op(&p_vs_fs, 0x1f);
    bind_op(NULL, 0x1f);
    CHECK(!S.bst[0] && !S.bst[1] && !S.bst[4] && !S.prog, "NULL with 0x1f unbinds all graphics stages");

    // 9. Shader-map-only stage resolution: works without calling prog_get/shader_get, handles unloaded stages, and refuses to guess on unmapped stages
    g_shmap = [NSMutableDictionary dictionary];
    g_shmap[@"0102030405060708"] = @"vertex";
    g_shmap[@"1112131415161718"] = @"pixel";
    g_shmap[@"2122232425262728"] = @"mesh";
    const uint8_t k_vs[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    const uint8_t k_ps[8] = {0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18};
    const uint8_t k_ms[8] = {0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28};
    const uint8_t k_unk[8] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00, 0x11};

    prog_t p_lazy_vs = {.magic = MAGIC('P', 'R', 'O', 'G'), .nstage = 1, .serial = nvn_new_serial()};
    install_key(p_lazy_vs.serial, 0, k_vs);
    bind_op(&p_lazy_vs, 0x1f);
    CHECK(S.bst[0] == &p_lazy_vs && S.prog == &p_lazy_vs && serial_get(p_lazy_vs.serial) == nil,
          "unresolved VS program bound with 0x1f reads shader map without triggering prog_get");

    prog_t p_lazy_ms = {.magic = MAGIC('P', 'R', 'O', 'G'), .nstage = 2, .serial = nvn_new_serial()};
    install_key(p_lazy_ms.serial, 0, k_ms);
    install_key(p_lazy_ms.serial, 1, k_ps);
    bind_op(&p_lazy_ms, 0xff);
    CHECK(S.bst[0] == &p_lazy_ms && S.prog == &p_lazy_ms && serial_get(p_lazy_ms.serial) == nil,
          "unresolved NVN2 mesh+PS program bound with 0xff replaces vertex slot without prog_get");

    // Partially unmapped 2-stage program (unknown stage + mapped pixel): must not inherit stale VS
    prog_t p_partial = {.magic = MAGIC('P', 'R', 'O', 'G'), .nstage = 2, .serial = nvn_new_serial()};
    install_key(p_partial.serial, 0, k_unk);
    install_key(p_partial.serial, 1, k_ps);
    bind_op(&p_vs_fs, 0x1f);
    bind_op(&p_partial, 0x1f);
    CHECK(S.bst[0] == &p_partial && S.prog == &p_partial && serial_get(p_partial.serial) == nil,
          "partially unmapped program with pixel stage does not inherit stale vertex slot");

    // Unloaded pixel-only program whose NvProg.stages was recorded by prog_get (o->stages = 0x2, o->sh[1] = nil): still preserves vertex slot
    prog_t p_unloaded_ps = {.magic = MAGIC('P', 'R', 'O', 'G'), .serial = nvn_new_serial()};
    NvProg *up = [NvProg new]; up->stages = 0x2; serial_put(p_unloaded_ps.serial, up);
    bind_op(&p_vs, 0x1f);
    bind_op(&p_unloaded_ps, 0x1f);
    CHECK(S.bst[0] == &p_vs && S.bst[1] == &p_unloaded_ps,
          "pixel-only program with recorded stages bit but unloaded shader still preserves vertex slot");

    printf("t_bind_stages: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
