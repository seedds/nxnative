// ubd_conflict_sh's remembered clear answers (the review of #1142): a stage found clear is taken again only while its shader, its buffer bindings, the deferred updates, the
// submission's pending and storage-buffer writes and the live GPU writes are the same. A storage write between two identical draws into a range a deferred update shadows only in
// part makes the walk report a conflict, and so must the remembered answer
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

enum { LEN = 0x20000 };
static uint8_t *mem;
static void update(uint64_t off, uint32_t value, uint64_t n) { uint32_t v[16]; for (int i = 0; i < 16; i++) v[i] = value; mtl_sub_update(v, (uint64_t)mem + off, n); }
static void bind(int stage, int index, uint64_t off, uint64_t size) { p_bind_buf b = {stage, index, (uint64_t)mem + off, size}; state_op(OP_BIND_UBO, &b); }
static void next_submission(void) { g_nubd = 0; memset(g_ubd_pf, 0, sizeof g_ubd_pf); g_ubd_wide = 0; g_ubd_ver++; pend_reset(); memset(g_ubpc, 0, sizeof g_ubpc); }

int main(void) { @autoreleasepool {
    setenv("DM_NVN_TBDR", "0", 1);
    mtl_init();
    mem = mmap(NULL, LEN, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    CHECK(mem != MAP_FAILED, "memory allocated"); if (mem == MAP_FAILED) return 1;
    mtl_pool_add(mem, LEN);
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin();
    memset(mem, 0x11, LEN);
    NvShader *fs = [NvShader new]; fs->stage = 1; strcpy(fs->key, "0123456789abcdef_b");
    fs->rf.nb = 1; fs->rf.b[0] = (nvn_rbind){.kind = NVN_RB_CBUF, .reg = 0, .aid = 0};

    // A deferred update covering part of the bound block: the rest is read from memory, so the stage is clear, and the next identical draw takes the remembered answer
    bind(1, 0, 0x200, 0x40);
    CHECK(S.ubgen[1] != 0, "a uniform binding numbers its stage's bindings");
    update(0x200, 0xaa, 4);
    uint64_t hits = g_ubdc_hits;
    CHECK(ubd_conflict_sh(fs, 1) == 0, "a partly covered block whose rest memory holds is clear");
    CHECK(ubd_conflict_sh(fs, 1) == 0 && g_ubdc_hits == hits + 1, "the next identical draw takes the remembered answer");

    // A storage write into the rest of the block between two identical draws: the shadow can no longer read it from memory, and the answer is not taken from before
    sbw_note((uint64_t)mem + 0x220, 16, SBW_GPU);
    hits = g_ubdc_hits;
    CHECK(ubd_conflict_sh(fs, 1) == 1 && g_ubdc_hits == hits, "after a storage write into the block the walk runs again and reports the conflict");
    CHECK(ubd_conflict_sh(fs, 1) == 1, "a conflict is not remembered as clear");

    // A new submission and a new binding: clear again, then a binding of the same block elsewhere renumbers the stage, and an update moves the deferred list
    next_submission(); update(0x400, 0xbb, 4); bind(1, 0, 0x400, 0x40);
    CHECK(ubd_conflict_sh(fs, 1) == 0, "clear again in a new submission");
    uint32_t gen = S.ubgen[1]; bind(1, 0, 0x1000, 0x40);
    CHECK(S.ubgen[1] != gen, "binding another block renumbers the stage");
    hits = g_ubdc_hits; ubd_conflict_sh(fs, 1);
    CHECK(g_ubdc_hits == hits, "a renumbered stage is walked again");
    bind(1, 0, 0x400, 0x40); ubd_conflict_sh(fs, 1); hits = g_ubdc_hits;
    update(0x404, 0xcc, 4);
    ubd_conflict_sh(fs, 1);
    CHECK(g_ubdc_hits == hits, "a new deferred update is walked again");

    // A live GPU write noted meanwhile (a segment committed) is part of the key too
    hits = g_ubdc_hits; ubd_conflict_sh(fs, 1); CHECK(g_ubdc_hits == hits + 1, "unchanged: remembered");
    gw_put((uint64_t)mem + 0x8000, (uint64_t)mem + 0x8010, g_done + 1000, g_done);
    hits = g_ubdc_hits; ubd_conflict_sh(fs, 1); CHECK(g_ubdc_hits == hits, "a live GPU write noted since is walked again");

    printf("ubd_conflict_sh remembered answers: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
