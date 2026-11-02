// The shaders' constants (c1) packed into shared banks as the Metal backend packs them (dm/gfx/hostvk/vk_shader.c c1_make, nvn_mtl.m c1_store; docs/design/vulkan-backend.md,
// step D2, "Constant banks"), on a real device (lavapipe in CTest, under the validation layer: t_vk_harness.h). Each phase is a start of the backend in a child process
// (t_vk_harness.h hv_spawn) with a library of synthetic compute shaders, each the program of a frontend NVNprogram, loaded by key and dispatched through the frontend's records, so
// the c1 descriptor is the one vk_pipeline.c build_set writes for a game's shader:
//   own     no c1_full_vk.tsv beside the shader map: one [RED] line, every c1 gets 64 KiB of its own (the backend before the banks, and now without the table)
//   stale   a c1_full_vk.tsv whose header names other inputs than the library's stamp (inputs.tsv): the same
//   banks   a c1_full_vk.tsv stamped for the library, listing group B (indices not proven bounded): B gets 64 KiB each, every other c1 is packed into the banks
// Shaders, loaded in this order: group A, 300 with 768-byte c1 segments (the size of t_vk_alloc's 300 synthetic shaders, the largest common one); group B, 30 with 512 bytes;
// group C (own and banks), 640 with 6 KiB (the largest segment in the libraries so far), enough to fill the first 4 MiB bank and open a second. own and banks load the same
// shaders and make the same dispatches, so their allocator lines differ by the c1 bytes.
// Checks in each phase:
// - every shader loaded, with its c1 where the phase says (a bank: no range of its own, the bank's buffer, an offset aligned to 256 bytes or minUniformBufferOffsetAlignment, its
//   64 KiB window inside the bank; its own: a range of a block of at least 64 KiB at offset 0); the packed spans (the data rounded up to 256 bytes, plus 256) never overlap;
// - every bank: each shader's bytes where the reflection put them, zeros to the end of its span, and the bank's last 64 KiB zero (read through its CPU pointer);
// - a compute dispatch per shader (c1_read below: invocation i stores c1[i]) reads its constants exactly where the reflection put them and zeros after: through its span when packed
//   (what c1_guard.py proves a packed shader's reads stay inside), through all 64 KiB when it has its own;
// - the host-visible bytes the loads took (the allocator's bytes in use, before and after) are the banks' and the own windows' exactly; the counts (hvk_c1_get_stats) are what the
//   test's own model of the packing expects;
// - the shaders unloaded (hvk_shader_shutdown, which the backend's shutdown calls; a loaded shader is not unloaded before): the line "[hostvk] c1 banks: ..." gives those counts and
//   bytes, and the allocator's bytes in use drop by exactly the c1 bytes;
// - validation clean.
// The parent compares the phases: the same shaders' host-visible c1 bytes without and with the table, and the allocator's shutdown line of each start.
#include "t_vk_harness.h"
#include "vk_backend.h"

// c1_read: glslangValidator 15.1.0 -V --target-env vulkan1.3 (spirv-val clean) of
//   #version 450
//   layout(local_size_x = 16) in;
//   layout(set = 0, binding = 2, std140) uniform C1 { uvec4 c[4096]; } c1;   // c1: uniform slot 1 of translator stage 0 (design 2.3), as dmsh_xlate_vk declares it
//   layout(set = 1, binding = 0, std430) writeonly buffer O { uvec4 o[]; } o;  // storage slot 0
//   void main() { uint i = gl_GlobalInvocationID.x; o.o[i] = c1.c[i]; }
static const uint32_t c1_read_spv[] = {
    0x07230203,0x00010600,0x0008000b,0x00000026,0x00000000,0x00020011,0x00000001,0x0006000b,
    0x00000001,0x4c534c47,0x6474732e,0x3035342e,0x00000000,0x0003000e,0x00000000,0x00000001,
    0x0008000f,0x00000005,0x00000004,0x6e69616d,0x00000000,0x0000000b,0x00000014,0x0000001c,
    0x00060010,0x00000004,0x00000011,0x00000010,0x00000001,0x00000001,0x00030003,0x00000002,
    0x000001c2,0x00040005,0x00000004,0x6e69616d,0x00000000,0x00030005,0x00000008,0x00000069,
    0x00080005,0x0000000b,0x475f6c67,0x61626f6c,0x766e496c,0x7461636f,0x496e6f69,0x00000044,
    0x00030005,0x00000012,0x0000004f,0x00040006,0x00000012,0x00000000,0x0000006f,0x00030005,
    0x00000014,0x0000006f,0x00030005,0x0000001a,0x00003143,0x00040006,0x0000001a,0x00000000,
    0x00000063,0x00030005,0x0000001c,0x00003163,0x00040047,0x0000000b,0x0000000b,0x0000001c,
    0x00040047,0x00000011,0x00000006,0x00000010,0x00030047,0x00000012,0x00000002,0x00040048,
    0x00000012,0x00000000,0x00000019,0x00050048,0x00000012,0x00000000,0x00000023,0x00000000,
    0x00030047,0x00000014,0x00000019,0x00040047,0x00000014,0x00000021,0x00000000,0x00040047,
    0x00000014,0x00000022,0x00000001,0x00040047,0x00000019,0x00000006,0x00000010,0x00030047,
    0x0000001a,0x00000002,0x00050048,0x0000001a,0x00000000,0x00000023,0x00000000,0x00040047,
    0x0000001c,0x00000021,0x00000002,0x00040047,0x0000001c,0x00000022,0x00000000,0x00020013,
    0x00000002,0x00030021,0x00000003,0x00000002,0x00040015,0x00000006,0x00000020,0x00000000,
    0x00040020,0x00000007,0x00000007,0x00000006,0x00040017,0x00000009,0x00000006,0x00000003,
    0x00040020,0x0000000a,0x00000001,0x00000009,0x0004003b,0x0000000a,0x0000000b,0x00000001,
    0x0004002b,0x00000006,0x0000000c,0x00000000,0x00040020,0x0000000d,0x00000001,0x00000006,
    0x00040017,0x00000010,0x00000006,0x00000004,0x0003001d,0x00000011,0x00000010,0x0003001e,
    0x00000012,0x00000011,0x00040020,0x00000013,0x0000000c,0x00000012,0x0004003b,0x00000013,
    0x00000014,0x0000000c,0x00040015,0x00000015,0x00000020,0x00000001,0x0004002b,0x00000015,
    0x00000016,0x00000000,0x0004002b,0x00000006,0x00000018,0x00001000,0x0004001c,0x00000019,
    0x00000010,0x00000018,0x0003001e,0x0000001a,0x00000019,0x00040020,0x0000001b,0x00000002,
    0x0000001a,0x0004003b,0x0000001b,0x0000001c,0x00000002,0x00040020,0x0000001e,0x00000002,
    0x00000010,0x00040020,0x00000021,0x0000000c,0x00000010,0x0004002b,0x00000006,0x00000023,
    0x00000010,0x0004002b,0x00000006,0x00000024,0x00000001,0x0006002c,0x00000009,0x00000025,
    0x00000023,0x00000024,0x00000024,0x00050036,0x00000002,0x00000004,0x00000000,0x00000003,
    0x000200f8,0x00000005,0x0004003b,0x00000007,0x00000008,0x00000007,0x00050041,0x0000000d,
    0x0000000e,0x0000000b,0x0000000c,0x0004003d,0x00000006,0x0000000f,0x0000000e,0x0003003e,
    0x00000008,0x0000000f,0x0004003d,0x00000006,0x00000017,0x00000008,0x0004003d,0x00000006,
    0x0000001d,0x00000008,0x00060041,0x0000001e,0x0000001f,0x0000001c,0x00000016,0x0000001d,
    0x0004003d,0x00000010,0x00000020,0x0000001f,0x00060041,0x00000021,0x00000022,0x00000014,
    0x00000016,0x00000017,0x0003003e,0x00000022,0x00000020,0x000100fd,0x00010038
};
static const char k_bindings[] = "[{\"kind\":\"c1\",\"name\":\"cp_c1\",\"class\":\"ubo\",\"set\":0,\"binding\":2},"
                                 "{\"kind\":\"sbuf\",\"reg\":0,\"name\":\"cp_s0\",\"class\":\"ssbo\",\"set\":1,\"binding\":0,\"rw\":true}]";

enum { NA = 300, NB = 30, NC = 640, NMAX = NA + NB + NC, LA = 768, LB = 512, LC = 6144, WIN = 65536 };
typedef struct { int group; uint32_t len; char key[17]; uint32_t read; size_t out; hvk_shader *sh; } shd;
static shd g_s[NMAX]; static NVNprogram g_prog[NMAX]; static int g_n;
static uint8_t byte_of(int k, uint32_t b) { return (uint8_t)(k * 31 + b * 7 + 1); }   // byte b of shader k's c1
static uint64_t used(void) { hvk_alloc_stats s; hvk_alloc_get_stats(&s); return s.used_bytes; }

// The backend's log: forwarded, with the red lines about the table counted and the two shutdown lines read (the allocator's goes to the results file: it comes after the test ends)
static int g_red_table; static const char *g_res;
static unsigned g_l_packed, g_l_banks, g_l_own, g_l_seen; static unsigned long long g_l_bank_bytes, g_l_own_bytes;
static void on_log(const char *line) {
    fprintf(stderr, "[dm] %s\n", line);
    if (strstr(line, "[RED]") && strstr(line, "c1_full_vk.tsv")) g_red_table++;
    if (sscanf(line, "[hostvk] c1 banks: %u shaders packed in %u banks (%llu bytes), %u with 64 KiB of their own (%llu bytes)", &g_l_packed, &g_l_banks, &g_l_bank_bytes,
               &g_l_own, &g_l_own_bytes) == 5) g_l_seen++;
    const char *p = strstr(line, "device memory at shutdown: "), *q = p ? strstr(p, "peak ") : NULL;
    unsigned blocks; double mib, inuse;
    if (q && g_res && sscanf(q, "peak %u blocks %lf MiB with %lf MiB in use", &blocks, &mib, &inuse) == 3) {
        hv_res_put(g_res, "peak_blocks", blocks); hv_res_put(g_res, "peak_mib", mib); hv_res_put(g_res, "peak_inuse_mib", inuse);
    }
}

static int child(const char *phase, const char *res) {
    static char name[120]; snprintf(name, sizeof name, "t_vk_c1 %s", phase);
    g_res = res; hostvk_set_log(on_log);
    const int banks = !strcmp(phase, "banks"), stale = !strcmp(phase, "stale"), own = !strcmp(phase, "own");
    hv_up(name);
    hv_device(0);
    const hostvk_dev *d = hvk_device();
    NVNqueue q; NVNqueueBuilder qb; memset(&qb, 0, sizeof qb); h_nvnQueueBuilderSetDefaults(&qb); h_nvnQueueBuilderSetDevice(&qb, &g_dev); h_nvnQueueInitialize(&q, &qb);
    // the shaders: each a one-stage compute program (its control section and code unique by name), its module c1_read, its reflection with a c1 segment
    const int ngroup[3] = {NA, NB, banks || own ? NC : 0}; const uint32_t lgroup[3] = {LA, LB, LC};
    g_n = 0;
    for (int g = 0; g < 3; g++) for (int i = 0; i < ngroup[g]; i++) { g_s[g_n].group = g; g_s[g_n].len = lgroup[g]; g_n++; }
    hv_pool cp; const size_t cmd_off = (size_t)g_n * 256, cmd_bytes = 128u << 10;
    if (!hv_pool_make(&cp, (cmd_off + cmd_bytes + 65535) & ~(size_t)65535)) { t("a memory pool for the programs' code", 0); return hv_finish(); }
    hv_cmds c = {cp.mem + cmd_off, 0, cmd_bytes};
    static char json[2 * LC + 1200];
    for (int k = 0; k < g_n; k++) {
        char nm[32]; snprintf(nm, sizeof nm, "c1_%c%d", 'A' + g_s[k].group, k);
        const hv_shader hs = {nm, 5, c1_read_spv, sizeof c1_read_spv, k_bindings, NULL};
        hv_program(&g_prog[k], &cp, (size_t)k * 256, &hs, 1);
        uint8_t md[8]; prog_key((const prog_t *)&g_prog[k], 0, md);
        for (int b = 0; b < 8; b++) snprintf(g_s[k].key + 2 * b, 3, "%02x", md[b]);
        int n = snprintf(json, sizeof json, "{\"key\":\"%s\",\"target\":\"vk\",\"chain\":\"%s\",\"stage\":\"cs\",\"entry\":\"main\",\"threadgroup_size\":[16,1,1],\"bindings\":%s,\"c1\":\"",
                         g_s[k].key, DMSH_CHAIN_VK, k_bindings);
        for (uint32_t b = 0; b < g_s[k].len; b++) n += snprintf(json + n, sizeof json - (size_t)n, "%02x", byte_of(k, b));
        snprintf(json + n, sizeof json - (size_t)n, "\"}");
        char p[800]; snprintf(p, sizeof p, "%s/%s.json", g_libv, g_s[k].key);
        FILE *f = fopen(p, "w"); if (f) { fputs(json, f); fclose(f); }
    }
    // the table beside the shader map, as dmsh_build_vk.py writes it (c1_guard.py: dmsh_chain.py table_head, then the keys): group B listed; the library's stamp says inputs "test"
    if (banks || stale) {
        char p[700]; snprintf(p, sizeof p, "%s/c1_full_vk.tsv", g_lib);
        FILE *f = fopen(p, "w");
        if (f) {
            fprintf(f, "# chain vk-%s inputs %s 2026-11-02T00:00:00+00:00\n", DMSH_CHAIN_VK, banks ? "test" : "other");
            for (int k = 0; k < g_n; k++) if (g_s[k].group == 1) fprintf(f, "%s\n", g_s[k].key);
            fclose(f);
        }
    }
    // loaded by key, group after group: the allocator's bytes in use before and after
    hvk_c1_stats z; hvk_c1_get_stats(&z);
    const VkDeviceSize align = z.align, bank = z.bank_size;
    uint64_t u0 = used(), ug[3] = {0, 0, 0}; int loaded = 0;
    for (int g = 0, k = 0; g < 3; g++) {
        uint64_t before = used();
        for (int i = 0; i < ngroup[g]; i++, k++) { int st = 0; g_s[k].sh = hvk_shader_by_key(g_s[k].key, &st); loaded += g_s[k].sh != NULL; }
        ug[g] = used() - before;
    }
    uint64_t u1 = used();
    char check[400];
    snprintf(check, sizeof check, "%d shaders loaded by key (c1 of %u, %u and %u bytes); one red line about c1_full_vk.tsv %s (%d)", loaded, LA, LB, LC, banks ? "not printed" : "printed",
             g_red_table);
    t(check, loaded == g_n && g_red_table == (banks ? 0 : 1));
    // the test's own model of the packing (nvn_mtl.m c1_store's rule): where each packed shader goes, how many banks
    uint32_t want_banks = 0, want_packed = 0, want_own = 0; VkDeviceSize off = 0; uint64_t want_g[3] = {0, 0, 0};
    for (int k = 0; k < g_n; k++) {
        int packed = banks && g_s[k].group != 1;
        VkDeviceSize span = ((g_s[k].len + 255u) & ~255u) + 256u;
        if (!packed) { want_own++; want_g[g_s[k].group] += WIN; g_s[k].read = WIN; continue; }
        if (!want_banks || off + span + WIN > bank) { want_banks++; off = 0; want_g[g_s[k].group] += bank; }
        off = (off + span + align - 1) / align * align; want_packed++; g_s[k].read = (uint32_t)span;
    }
    hvk_c1_stats s; hvk_c1_get_stats(&s);
    snprintf(check, sizeof check, "the table %s; %u shaders packed in %u banks of %llu bytes, %u with 64 KiB of their own (the model: %u, %u, %u)", s.table ? "taken" : "not taken",
             s.packed, s.banks, (unsigned long long)s.bank_size, s.own, want_packed, want_banks, want_own);
    t(check, s.table == (unsigned)banks && s.packed == want_packed && s.banks == want_banks && s.own == want_own && s.bank_bytes == (uint64_t)want_banks * bank &&
             s.own_bytes == (uint64_t)want_own * WIN);
    snprintf(check, sizeof check, "host-visible bytes the loads took: group A %llu, B %llu, C %llu (the banks' and own windows': %llu, %llu, %llu)", (unsigned long long)ug[0],
             (unsigned long long)ug[1], (unsigned long long)ug[2], (unsigned long long)want_g[0], (unsigned long long)want_g[1], (unsigned long long)want_g[2]);
    t(check, ug[0] == want_g[0] && ug[1] == want_g[1] && ug[2] == want_g[2] && u1 - u0 == s.bank_bytes + s.own_bytes);
    // where each c1 is: a bank (no range of its own, the bank's buffer, aligned, its window inside the bank) or its own window
    int placed = 0, misplaced = 0, overlap = 0, bytes_bad = 0, tail_bad = 0;
    const uint8_t *bmap[8] = {0}; VkBuffer bbuf[8] = {0}; VkDeviceSize bsize[8] = {0}; VkDeviceSize bend[8] = {0};
    for (uint32_t i = 0; i < s.banks && i < 8; i++) bmap[i] = hvk_c1_bank(i, &bbuf[i], &bsize[i]);
    for (int k = 0; k < g_n; k++) {
        const hvk_shader *sh = g_s[k].sh; if (!sh) continue;
        int packed = banks && g_s[k].group != 1, bi = -1;
        for (uint32_t i = 0; i < s.banks && i < 8; i++) if (sh->c1buf == bbuf[i]) bi = (int)i;
        if (packed) {
            int ok = bi >= 0 && !sh->c1mem && sh->c1off % align == 0 && sh->c1off + WIN <= bsize[bi];
            misplaced += !ok; placed += ok;
            if (!ok) continue;
            overlap += sh->c1off < bend[bi];   // the shaders of a bank were loaded in offset order: each starts at or past the end of the span before it
            bend[bi] = sh->c1off + g_s[k].read;
            for (uint32_t b = 0; b < g_s[k].read; b++) bytes_bad += bmap[bi][sh->c1off + b] != (b < g_s[k].len ? byte_of(k, b) : 0);
        } else {
            const uint8_t *m = hvk_mem_map(sh->c1mem);
            int ok = bi < 0 && sh->c1mem && m && !hvk_mem_is_own(sh->c1mem) && hvk_mem_size(sh->c1mem) >= WIN && sh->c1off == 0;
            misplaced += !ok; placed += ok;
            if (ok) for (uint32_t b = 0; b < WIN; b++) bytes_bad += m[b] != (b < g_s[k].len ? byte_of(k, b) : 0);
        }
    }
    for (uint32_t i = 0; i < s.banks && i < 8; i++) {   // past the last span: zeros to the bank's end, at least its last 64 KiB
        if (!bmap[i] || bsize[i] < bend[i] + WIN) { tail_bad++; continue; }
        for (VkDeviceSize b = bend[i]; b < bsize[i]; b++) tail_bad += bmap[i][b] != 0;
    }
    snprintf(check, sizeof check, "%d c1 placed as the phase says (%s), %d not; packed offsets aligned to %llu, windows inside their bank, %d spans overlapping", placed,
             banks ? "group B its own 64 KiB, the others in the banks" : "each its own 64 KiB, a range of a block", misplaced, (unsigned long long)align, overlap);
    t(check, placed == g_n && !misplaced && !overlap);
    snprintf(check, sizeof check, "on the CPU: each c1's bytes where the reflection put them, zeros to the end of its span or window (%d bytes wrong); each bank's end zero, at least 64 KiB (%d wrong)",
             bytes_bad, tail_bad);
    t(check, !bytes_bad && !tail_bad);
    // the GPU: a dispatch per shader through the frontend's records, invocation i storing c1[i] into storage slot 0
    size_t out_bytes = 0; for (int k = 0; k < g_n; k++) { g_s[k].out = out_bytes; out_bytes += g_s[k].read; }
    hv_pool op; int gpu_bad = 0, gpu_ok = 0;
    if (!hv_pool_make(&op, (out_bytes + 65535) & ~(size_t)65535)) t("a memory pool for the dispatches' output", 0);
    else {
        memset(op.mem, 0xA5, op.size);
        for (int k = 0; k < g_n; k++) {
            hv_bind(&c, &g_prog[k], NVN_SHADER_STAGE_COMPUTE_BIT);
            hv_buf(&c, OP_BIND_SSBO, NVN_SHADER_STAGE_COMPUTE, 0, op.mem + g_s[k].out, g_s[k].read);
            p_dispatch dp = {(int32_t)(g_s[k].read / 256), 1, 1, 0, 0}; hv_emit(&c, OP_DISPATCH, &dp, sizeof dp);
            if (k % 64 == 63 || k == g_n - 1) hv_submit(&q, &c);
        }
        for (int k = 0; k < g_n; k++) {
            const uint8_t *o = op.mem + g_s[k].out; int bad = 0;
            for (uint32_t b = 0; b < g_s[k].read; b++) bad += o[b] != (b < g_s[k].len ? byte_of(k, b) : 0);
            gpu_bad += bad; gpu_ok += !bad;
        }
        snprintf(check, sizeof check, "on the GPU: %d of %d dispatches read their c1 where the reflection put it and zeros after (%s); %d bytes wrong", gpu_ok, g_n,
                 banks ? "a packed shader through its span, group B through 64 KiB" : "through 64 KiB", gpu_bad);
        t(check, gpu_ok == g_n && !gpu_bad);
    }
    // unloaded: the line, and the memory back
    uint64_t u2 = used();
    hvk_shader_shutdown();
    uint64_t u3 = used();
    snprintf(check, sizeof check, "the shaders unloaded: the line says %u packed in %u banks (%llu bytes), %u with their own (%llu bytes), as counted",
             g_l_packed, g_l_banks, g_l_bank_bytes, g_l_own, g_l_own_bytes);
    t(check, g_l_seen == 1 && g_l_packed == s.packed && g_l_banks == s.banks && g_l_bank_bytes == s.bank_bytes && g_l_own == s.own && g_l_own_bytes == s.own_bytes);
    snprintf(check, sizeof check, "... and the allocator's bytes in use dropped by %llu, the c1 bytes (%llu)", (unsigned long long)(u2 - u3), (unsigned long long)(s.bank_bytes + s.own_bytes));
    t(check, u2 - u3 == s.bank_bytes + s.own_bytes);
    hvk_c1_get_stats(&z);
    t("... and nothing of c1 left (no bank, no window, the table dropped)", !z.packed && !z.own && !z.banks && !z.table && !hvk_c1_bank(0, NULL, NULL));
    printf("%s: c1 of %d shaders: %u packed in %u banks (%llu bytes), %u with 64 KiB of their own (%llu bytes), %llu bytes of constants; group A (%d shaders of %u bytes) took %llu bytes; "
           "minUniformBufferOffsetAlignment %llu, maxUniformBufferRange %u\n", phase, g_n, s.packed, s.banks, (unsigned long long)s.bank_bytes, s.own, (unsigned long long)s.own_bytes,
           (unsigned long long)s.data_bytes, NA, LA, (unsigned long long)ug[0], (unsigned long long)d->props.limits.minUniformBufferOffsetAlignment, d->props.limits.maxUniformBufferRange);
    hv_res_put(res, "ran", 1); hv_res_put(res, "packed", s.packed); hv_res_put(res, "own", s.own); hv_res_put(res, "banks", s.banks);
    hv_res_put(res, "c1_bytes", (double)(s.bank_bytes + s.own_bytes)); hv_res_put(res, "group_a", (double)ug[0]); hv_res_put(res, "data", (double)s.data_bytes);
    hv_res_put(res, "n", g_n);
    if (op.mem) hv_pool_free(&op);
    hv_pool_free(&cp);
    return hv_finish();
}

int main(int argc, char **argv) {
    char res[800];
    if (argc > 1) {
        snprintf(res, sizeof res, "%s/%s.res", getenv("T_VK_C1_DIR") ? getenv("T_VK_C1_DIR") : ".", argv[1]);
        return child(argv[1], res);
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *tmp = getenv("TMPDIR");
    char dir[600]; snprintf(dir, sizeof dir, "%s/t_vk_c1_XXXXXX", tmp && *tmp ? tmp : "/tmp");
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 2; }
    setenv("T_VK_C1_DIR", dir, 1);
    static const char *const phase[3] = {"own", "stale", "banks"};
    char r[3][900], name[400]; int fail = 0;
    for (int i = 0; i < 3; i++) {
        printf("---- start: %s ----\n", phase[i]);
        int rc = hv_spawn(argv[0], phase[i]);
        snprintf(r[i], sizeof r[i], "%s/%s.res", dir, phase[i]);
        snprintf(name, sizeof name, "%s: the start passed its checks (exit status %d)", phase[i], rc);
        if (rc == 0 && hv_res_get(r[i], "ran", 0) == 0) { printf("== t_vk_c1: skipped (no Vulkan device) ==\n"); rm_dir(dir); return 0; }
        t(name, rc == 0);
        fail |= rc != 0;
    }
    snprintf(name, sizeof name, "own and stale: every c1 its own 64 KiB (%.0f of %.0f, %.0f of %.0f), no bank", hv_res_get(r[0], "own", -1), hv_res_get(r[0], "n", -1),
             hv_res_get(r[1], "own", -1), hv_res_get(r[1], "n", -1));
    t(name, hv_res_get(r[0], "own", -1) == hv_res_get(r[0], "n", -2) && hv_res_get(r[1], "own", -1) == hv_res_get(r[1], "n", -2) && hv_res_get(r[0], "banks", -1) == 0 &&
            hv_res_get(r[1], "banks", -1) == 0);
    double n_own = hv_res_get(r[0], "n", -1), c_own = hv_res_get(r[0], "c1_bytes", -1), c_bank = hv_res_get(r[2], "c1_bytes", -1);
    snprintf(name, sizeof name, "the same %.0f shaders: %.0f bytes of c1 without the table (64 KiB each), %.0f with it (%.1f %%)", n_own, c_own, c_bank, c_own > 0 ? 100.0 * c_bank / c_own : 0.0);
    t(name, n_own == hv_res_get(r[2], "n", -2) && c_own == n_own * WIN && c_bank > 0 && c_bank < c_own);
    double a_own = hv_res_get(r[0], "group_a", -1), a_bank = hv_res_get(r[2], "group_a", -1);
    snprintf(name, sizeof name, "the %d shaders of group A: %.0f bytes of c1 without the table, %.0f with it (%.1f %%)", NA, a_own, a_bank, a_own > 0 ? 100.0 * a_bank / a_own : 0.0);
    t(name, a_own == (double)NA * WIN && a_bank > 0 && a_bank < a_own);
    printf("phase   shaders  packed  banks  own  c1 bytes    allocator at shutdown: peak blocks, MiB, MiB in use\n");
    for (int i = 0; i < 3; i++)
        printf("%-7s %7.0f %7.0f %6.0f %4.0f %10.0f  %4.0f %7.1f %7.1f\n", phase[i], hv_res_get(r[i], "n", 0), hv_res_get(r[i], "packed", 0), hv_res_get(r[i], "banks", 0),
               hv_res_get(r[i], "own", 0), hv_res_get(r[i], "c1_bytes", 0), hv_res_get(r[i], "peak_blocks", 0), hv_res_get(r[i], "peak_mib", 0), hv_res_get(r[i], "peak_inuse_mib", 0));
    for (int i = 0; i < 3; i++) remove(r[i]);
    rm_dir(dir);
    printf("== t_vk_c1: %d checks, %d failed ==\n", g_total, g_fail);
    return g_fail != 0 || fail;
}
