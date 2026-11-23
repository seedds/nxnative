// Exercise exlaunch mod-plugin HLE (hle_exl.c) together with the production SVC dispatcher (hle_svc.c):
//   - svcGetSystemTick (0x1E) returns the 64-bit tick in x0
//   - svcGetInfo(65001) returns dm_exl_process_handle
//   - dm_exl_mod_modules discovers sorted subsdk<N> plugins, inserts before sdk, rejects main/unprepared/clashes/junk
//   - dm_exl_fake_rtld populates the reserved rtld slot below main and QueryMemory reports Code+RX/Code+R/CodeData+RW
//   - MapProcessMemory / UnmapProcessMemory / dm_exl_sync_all validate bounds/regions/state, keep overlapping persistent
//     and short-lived views from clobbering each other, and refuse writes to changed.tsv runs or mismatched build IDs
#include "../dm.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../hle_svc.c"
#include "../hle_exl.c"

static module_t test_mods[3];
module_t *g_mod = test_mods;
int g_nmod;
struct dm_flags g_flag;
static char g_test_dir[1024];
static uint8_t *g_test_rtld_slot;
static size_t g_test_rtld_pg;
static uint64_t g_fake_ticks = 0x123456789abcdef0ull;
static unsigned g_prep_calls;

const char *dm_module_dir(void) { return g_test_dir; }
uint8_t *dm_mod_rtld_slot(size_t *page_out) {
    if (page_out) *page_out = g_test_rtld_pg;
    return g_test_rtld_slot;
}
void hlecount_svc(int num) { (void)num; }
int svc_thread(int num, uint64_t *regs) { (void)num; (void)regs; return 0; }
const hle_readonly_shmem *hle_get_readonly_shmem(uint32_t handle) { (void)handle; return NULL; }
uint32_t hle_new_transfer_memory(hle_transfer_memory memory) { (void)memory; return 0; }
int hle_get_transfer_memory(uint32_t handle, hle_transfer_memory *memory) { (void)handle; (void)memory; return 0; }
void dm_backtrace(uint64_t fp) { (void)fp; }
void guest_clock_read_wait(void) {}
uint64_t guest_ns(void) { return 0; }
uint64_t guest_ticks(void) { return g_fake_ticks; }
uint64_t dm_entropy64(void) { return 0; }
int thr_limit_used(void) { return 0; }
int dm_ue_heap_owns(uint64_t a, uint64_t n) { (void)a; (void)n; return 0; }
void gate_obs_set(int id, void (*fn)(uint64_t *, int)) { (void)id; (void)fn; }
void dm_guest_write_prepare(void *addr, size_t size) { (void)addr; (void)size; g_prep_calls++; }
module_t *module_of(const void *p) {
    uintptr_t a = (uintptr_t)p;
    for (int i = 0; i < g_nmod; ++i) {
        uintptr_t b = (uintptr_t)g_mod[i].base;
        if (a >= b && a - b < g_mod[i].size) return &g_mod[i];
        if (g_mod[i].patch_off < 0 && a >= b + g_mod[i].patch_off && a < b + g_mod[i].patch_off + g_mod[i].patch_len) return &g_mod[i];
    }
    return NULL;
}

static void touch_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    assert(f != NULL);
    if (content) fputs(content, f);
    fclose(f);
}

static void test_mod_discovery(const char *tmp) {
    char mod[1024], romfs[1100], exefs[1100], prep[1200], p[1300];
    snprintf(mod, sizeof mod, "%s/modA", tmp);
    snprintf(romfs, sizeof romfs, "%s/romfs", mod);
    snprintf(exefs, sizeof exefs, "%s/exefs", mod);
    snprintf(prep, sizeof prep, "%s/nxnative", exefs);
    assert(!mkdir(mod, 0755) && !mkdir(romfs, 0755) && !mkdir(exefs, 0755) && !mkdir(prep, 0755));

    // Create subsdk10, subsdk2, subsdk9 (prepared), subsdk7 (unprepared), subsdk9.bak (ignored), main (refused)
    snprintf(p, sizeof p, "%s/subsdk10", exefs); touch_file(p, "");
    snprintf(p, sizeof p, "%s/subsdk2", exefs); touch_file(p, "");
    snprintf(p, sizeof p, "%s/subsdk9", exefs); touch_file(p, "");
    snprintf(p, sizeof p, "%s/subsdk7", exefs); touch_file(p, "");
    snprintf(p, sizeof p, "%s/subsdk9.bak", exefs); touch_file(p, "");
    snprintf(p, sizeof p, "%s/main", exefs); touch_file(p, "");

    const char *prepared[] = {"subsdk2", "subsdk9", "subsdk10"};
    for (int i = 0; i < 3; i++) {
        snprintf(p, sizeof p, "%s/%s.patched.bin", prep, prepared[i]); touch_file(p, "x");
        snprintf(p, sizeof p, "%s/%s.fixups.tsv", prep, prepared[i]); touch_file(p, "# BUILD_ID 0\n");
    }

    setenv("DM_ROMFS_OVERLAY", romfs, 1);
    const char *names[16] = {"main", "subsdk2", "sdk"};   // subsdk2 clashes with existing module
    int n = dm_exl_mod_modules(names, 3, 8);
    assert(n == 5);
    assert(!strcmp(names[0], "main"));
    assert(!strcmp(names[1], "subsdk2"));
    assert(strstr(names[2], "/exefs/nxnative/subsdk9") != NULL);
    assert(strstr(names[3], "/exefs/nxnative/subsdk10") != NULL);   // numeric order: 9 before 10
    assert(!strcmp(names[4], "sdk"));
    free((void *)names[2]);
    free((void *)names[3]);

    // When no sdk is listed (n == 1), plugins append after main rather than before it
    const char *only_main[16] = {"main"};
    int n2 = dm_exl_mod_modules(only_main, 1, 8);
    assert(n2 == 4);
    assert(!strcmp(only_main[0], "main"));
    assert(strstr(only_main[1], "/subsdk2") != NULL);
    assert(strstr(only_main[2], "/subsdk9") != NULL);
    assert(strstr(only_main[3], "/subsdk10") != NULL);
    for (int i = 1; i < 4; i++) free((void *)only_main[i]);

    unsetenv("DM_ROMFS_OVERLAY");
}

typedef struct { uint64_t base, size; uint32_t state, attr, perm, ipc, device, reserved; } mem_info_t;

static mem_info_t query_mem(uint64_t addr) {
    mem_info_t mi = {0};
    uint64_t regs[31] = {0};
    regs[0] = (uintptr_t)&mi;
    regs[2] = addr;
    assert(svc_dispatch(0x06, regs, 0) == 0);
    assert(regs[0] == 0);
    return mi;
}

int main(void) {
    char tmpl[] = "/tmp/t_exl_XXXXXX";
    char *tmp = mkdtemp(tmpl);
    assert(tmp != NULL);
    strlcpy(g_test_dir, tmp, sizeof g_test_dir);

    // 1. svcGetSystemTick returns the 64-bit tick in x0
    {
        uint64_t regs[31] = {0};
        assert(svc_dispatch(0x1E, regs, 0) == 0);
        assert(regs[0] == g_fake_ticks);
    }

    // 2. svcGetInfo(65001) returns dm_exl_process_handle
    {
        uint64_t regs[31] = {0};
        regs[1] = 65001;
        assert(svc_dispatch(0x29, regs, 0) == 0);
        assert(regs[0] == 0 && regs[1] == dm_exl_process_handle);
    }

    // 3. Mod discovery (also increments g_mod_plugins so dm_exl_fake_rtld activates without scanning main)
    test_mod_discovery(tmp);

    // 4. Contiguous layout: [0..3*pg) rtld slot, [3*pg..4*pg) guard, [4*pg..8*pg) main, [9*pg..13*pg) bad-build mod
    size_t pg = (size_t)getpagesize();
    if (pg < 0x4000) pg = 0x4000;
    size_t total = 14 * pg;
    uint8_t *span = mmap(NULL, total, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(span != MAP_FAILED);
    g_test_rtld_slot = span;
    g_test_rtld_pg = pg;

    uint8_t *m0_base = span + 4 * pg;
    assert(!mprotect(m0_base, 4 * pg, PROT_READ | PROT_WRITE));
    uint32_t *text = (uint32_t *)m0_base;
    for (size_t i = 0; i < pg / 4; i++) text[i] = 0xD503201Fu;   // NOPs
    assert(!mprotect(m0_base, pg, PROT_READ | PROT_EXEC));
    assert(!mprotect(m0_base + pg, pg, PROT_READ));
    assert(!mprotect(m0_base + 3 * pg, pg, PROT_READ));   // key page

    strlcpy(g_mod[0].name, "main", sizeof g_mod[0].name);
    g_mod[0].dir = g_test_dir;
    g_mod[0].base = m0_base;
    g_mod[0].size = 4 * pg;
    g_mod[0].text_end = pg;
    g_mod[0].ro_off = pg;
    g_mod[0].data_off = 2 * pg;
    g_mod[0].key_page_off = 3 * pg;
    g_mod[0].key_page_size = pg;
    strlcpy(g_mod[0].build_id, "0123456789abcdef0123456789abcdef", sizeof g_mod[0].build_id);
    g_mod[0].loaded = 1;

    // Second module whose changed.tsv has a mismatched BUILD_ID
    uint8_t *m1_base = span + 9 * pg;
    assert(!mprotect(m1_base, 4 * pg, PROT_READ | PROT_WRITE));
    ((uint32_t *)m1_base)[0] = 0xD503201Fu;
    assert(!mprotect(m1_base, pg, PROT_READ | PROT_EXEC));
    strlcpy(g_mod[1].name, "subsdk9", sizeof g_mod[1].name);
    g_mod[1].dir = g_test_dir;
    g_mod[1].base = m1_base;
    g_mod[1].size = 4 * pg;
    g_mod[1].text_end = pg;
    g_mod[1].ro_off = pg;
    g_mod[1].data_off = 2 * pg;
    g_mod[1].key_page_off = 3 * pg;
    g_mod[1].key_page_size = pg;
    strlcpy(g_mod[1].build_id, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", sizeof g_mod[1].build_id);
    g_mod[1].loaded = 1;
    g_nmod = 2;

    char ch_path[1100];
    snprintf(ch_path, sizeof ch_path, "%s/main.changed.tsv", g_test_dir);
    touch_file(ch_path, "# BUILD_ID 0123456789abcdef0123456789abcdef\n0x10\t0x8\n");
    snprintf(ch_path, sizeof ch_path, "%s/subsdk9.changed.tsv", g_test_dir);
    touch_file(ch_path, "# BUILD_ID bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n");

    // 5. Stand-in rtld and QueryMemory walk
    dm_exl_fake_rtld();
    assert(dm_exl_rtld_base() == (uintptr_t)span);
    assert(dm_exl_rtld_size() == 3 * pg);
    mem_info_t r0 = query_mem((uintptr_t)span);
    mem_info_t r1 = query_mem((uintptr_t)span + pg);
    mem_info_t r2 = query_mem((uintptr_t)span + 2 * pg);
    mem_info_t gap = query_mem((uintptr_t)span + 3 * pg);
    assert(r0.base == (uintptr_t)span && r0.size == pg && r0.state == 3 && r0.perm == 5);
    assert(r1.base == (uintptr_t)span + pg && r1.size == pg && r1.state == 3 && r1.perm == 1);
    assert(r2.base == (uintptr_t)span + 2 * pg && r2.size == pg && r2.state == 4 && r2.perm == 3);
    assert(gap.state == 0 && gap.base == (uintptr_t)span + 3 * pg && gap.size == pg);

    // 6. MapProcessMemory argument and region validation
    mem_init();
    uint64_t aslr_dst1 = (uintptr_t)g_as + 0x10000;
    uint64_t aslr_dst2 = (uintptr_t)g_as + 0x30000;
    uint64_t regs[31] = {0};

    // Wrong process handle -> 0xE401
    regs[0] = aslr_dst1; regs[1] = 0x1234; regs[2] = (uintptr_t)m0_base; regs[3] = 0x1000;
    assert(svc_dispatch(0x74, regs, 0) == 0 && regs[0] == 0xE401);
    // Destination inside heap / stack / alias -> 0xDC01
    regs[0] = (uintptr_t)g_heap; regs[1] = dm_exl_process_handle; regs[2] = (uintptr_t)m0_base; regs[3] = 0x1000;
    assert(svc_dispatch(0x74, regs, 0) == 0 && regs[0] == 0xDC01);
    regs[0] = (uintptr_t)g_stack; regs[1] = dm_exl_process_handle; regs[2] = (uintptr_t)m0_base; regs[3] = 0x1000;
    assert(svc_dispatch(0x74, regs, 0) == 0 && regs[0] == 0xDC01);
    regs[0] = (uintptr_t)g_alias; regs[1] = dm_exl_process_handle; regs[2] = (uintptr_t)m0_base; regs[3] = 0x1000;
    assert(svc_dispatch(0x74, regs, 0) == 0 && regs[0] == 0xDC01);
    // Source overlapping host key page -> 0xD401
    regs[0] = aslr_dst1; regs[1] = dm_exl_process_handle; regs[2] = (uintptr_t)(m0_base + 3 * pg); regs[3] = 0x1000;
    assert(svc_dispatch(0x74, regs, 0) == 0 && regs[0] == 0xD401);
    // Source size overflow -> 0xCA01
    regs[0] = aslr_dst1; regs[1] = dm_exl_process_handle; regs[2] = (uintptr_t)m0_base; regs[3] = UINT64_MAX - 0xfff;
    assert(svc_dispatch(0x74, regs, 0) == 0 && regs[0] == 0xCA01);

    // 7. Overlapping long-lived (v1) and short-lived (v2) views + changed.tsv refusal
    regs[0] = aslr_dst1; regs[1] = dm_exl_process_handle; regs[2] = (uintptr_t)m0_base; regs[3] = 0x1000;
    assert(svc_dispatch(0x74, regs, 0) == 0 && regs[0] == 0);
    mem_info_t vm = query_mem(aslr_dst1);
    assert(vm.base == aslr_dst1 && vm.size == 0x1000 && vm.state == 0xF && vm.perm == 3);

    regs[0] = aslr_dst2; regs[1] = dm_exl_process_handle; regs[2] = (uintptr_t)m0_base; regs[3] = 0x1000;
    assert(svc_dispatch(0x74, regs, 0) == 0 && regs[0] == 0);

    // Short-lived view hooks offset 0x0 and tries to overwrite rewriter-changed offset 0x10
    ((uint32_t *)(uintptr_t)aslr_dst2)[0] = 0x14000010u;   // b +0x40
    ((uint32_t *)(uintptr_t)aslr_dst2)[4] = 0x14000020u;   // offset 0x10 (in main.changed.tsv!)
    regs[0] = aslr_dst2; regs[1] = dm_exl_process_handle; regs[2] = (uintptr_t)m0_base; regs[3] = 0x1000;
    assert(svc_dispatch(0x75, regs, 0) == 0 && regs[0] == 0);
    assert(((const uint32_t *)m0_base)[0] == 0x14000010u);
    assert(((const uint32_t *)m0_base)[4] == 0xD503201Fu);   // refused: stayed original NOP

    // Long-lived view writes offset 0x20 (index 8) while still holding stale NOP at offset 0x0
    ((uint32_t *)(uintptr_t)aslr_dst1)[8] = 0x14000030u;
    dm_exl_sync_all();
    assert(((const uint32_t *)m0_base)[8] == 0x14000030u);
    assert(((const uint32_t *)m0_base)[0] == 0x14000010u);   // v1's stale snapshot must NOT clobber v2's hook!
    assert(g_prep_calls > 0);

    // Unmap long-lived view
    regs[0] = aslr_dst1; regs[1] = dm_exl_process_handle; regs[2] = (uintptr_t)m0_base; regs[3] = 0x1000;
    assert(svc_dispatch(0x75, regs, 0) == 0 && regs[0] == 0);

    // 8. Mismatched BUILD_ID in subsdk9.changed.tsv refuses code write
    regs[0] = aslr_dst1; regs[1] = dm_exl_process_handle; regs[2] = (uintptr_t)m1_base; regs[3] = 0x1000;
    assert(svc_dispatch(0x74, regs, 0) == 0 && regs[0] == 0);
    ((uint32_t *)(uintptr_t)aslr_dst1)[0] = 0x14000040u;
    regs[0] = aslr_dst1; regs[1] = dm_exl_process_handle; regs[2] = (uintptr_t)m1_base; regs[3] = 0x1000;
    assert(svc_dispatch(0x75, regs, 0) == 0 && regs[0] == 0);
    assert(((const uint32_t *)m1_base)[0] == 0xD503201Fu);

    puts("exlaunch HLE and memory SVCs: OK");
    return 0;
}
