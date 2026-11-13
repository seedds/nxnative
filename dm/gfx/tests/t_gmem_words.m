// The raw pool table words of the argument blocks encoded so far follow the pools at commit (nvn_mtl.m gmem_slot_note, gmem_commit_patch): a pool made after a block was encoded is in the
// table its word points at when the GPU reads it, a word behind the pools or holding an older table is rewritten and counted, and a word that is current is left alone.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(label, c) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s (line %d)\n", label, __LINE__); } } while (0)
// Whether the table the GPU address names has an entry that starts at the guest address (every table is the current one's or an older, still alive one)
static int table_has(uint64_t table_gpu, uint64_t guest) {
    if (!g_gmem_buf || g_gmem_buf.gpuAddress != table_gpu) return 0;
    const uint8_t *t = g_gmem_buf.contents;
    for (unsigned k = 1, n = (unsigned)(g_gmem_buf.length / DMSH_GMEM_MAP_ENTRY); k < n; k++) { uint64_t lo; memcpy(&lo, t + (size_t)k * DMSH_GMEM_MAP_ENTRY, sizeof lo); if (lo == guest) return 1; }
    return 0;
}
int main(void) { @autoreleasepool {
    mtl_init();
    const uint64_t MB = 1ull << 20;
    uint8_t *mem = mmap(NULL, 64 * MB, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0); if (mem == MAP_FAILED) abort();
    uint64_t a = (uint64_t)(uintptr_t)mem;
    mtl_pool_add(mem, 4 * MB); mtl_pool_addressed(mem, 4 * MB);
    mtl_sub_begin();
    uint64_t t0 = gmem_map_gpu();                    // the table two argument blocks were encoded with
    uint64_t w0 = t0, w1 = t0, w2 = 0;
    uint64_t words0, older0, behind0; mtl_gmem_words(&words0, &older0, &behind0);
    gmem_slot_note(&w0, NULL); gmem_slot_note(&w1, NULL);
    gmem_commit_patch();
    uint64_t words, older, behind; mtl_gmem_words(&words, &older, &behind);
    CHECK("an unchanged table leaves the words alone", w0 == t0 && w1 == t0 && g_gmem_buf.gpuAddress == t0);
    CHECK("two words counted, none out of date", words - words0 == 2 && older == older0 && behind == behind0);
    mtl_pool_add(mem + 8 * MB, 4 * MB); mtl_pool_addressed(mem + 8 * MB, 4 * MB);              // a pool made after the blocks were encoded
    CHECK("the table the words hold lacks it", !table_has(t0, a + 8 * MB));
    gmem_slot_note(&w0, NULL); gmem_slot_note(&w1, NULL);
    gmem_commit_patch();
    uint64_t t1 = g_gmem_buf.gpuAddress;
    CHECK("a new table was built, not the old one refilled", t1 != t0);
    CHECK("both words point at it", w0 == t1 && w1 == t1);
    CHECK("it holds the pool made after they were encoded", table_has(t1, a + 8 * MB));
    mtl_gmem_words(&words, &older, &behind);
    CHECK("two words counted as behind the pools", behind - behind0 == 2 && older == older0);
    mtl_pool_add(mem + 16 * MB, 4 * MB); mtl_pool_addressed(mem + 16 * MB, 4 * MB);
    uint64_t t2 = gmem_map_gpu();                    // another block is encoded with the newer table, then the pools change again
    mtl_pool_add(mem + 24 * MB, 4 * MB); mtl_pool_addressed(mem + 24 * MB, 4 * MB);
    w2 = t1;                                         // a block encoded with the older table
    uint64_t w3 = t2;                                // and one with the table that now lags the pools
    gmem_slot_note(&w2, NULL); gmem_slot_note(&w3, NULL);
    gmem_commit_patch();
    uint64_t t3 = g_gmem_buf.gpuAddress;
    CHECK("the words follow the last table", w2 == t3 && w3 == t3 && t3 != t2);
    CHECK("it holds all four added pools", table_has(t3, a + 8 * MB) && table_has(t3, a + 16 * MB) && table_has(t3, a + 24 * MB));
    mtl_gmem_words(&words, &older, &behind);
    CHECK("one word counted as older, one as behind", older - older0 == 1 && behind - behind0 == 3);
    gmem_commit_patch();                             // nothing noted since
    CHECK("a commit with no words changes nothing", g_gmem_buf.gpuAddress == t3);
    // a write log is read against the table the shader searched: when its block's word is repointed at a newer table, the log follows it
    id<MTLBuffer> t3buf = g_gmem_buf;
    uint64_t w4 = t3; (void)gml_take(t3buf); NvGmlLog *lg = g_gml_last;
    CHECK("a log is taken with the table its block holds", lg && lg->map == t3buf);
    mtl_pool_add(mem + 32 * MB, 4 * MB); mtl_pool_addressed(mem + 32 * MB, 4 * MB);             // a pool the block's table lacks
    gmem_slot_note(&w4, (__bridge void *)lg);
    gmem_commit_patch();
    CHECK("its word follows the newer table", w4 == g_gmem_buf.gpuAddress && w4 != t3);
    CHECK("and its log is read against that table, not the older one", lg->map == g_gmem_buf && lg->map != t3buf);
    CHECK("submission completes", mtl_wait(mtl_sub_commit(), 5000000000ull));
    CHECK("GPU has no submission error", !mtl_last_commit_error());
    printf("t_gmem_words: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
