// PR #1427's late-pool/immutable-table checks, extended for this branch's dynamic
// entry bitmap and all three commit paths. No long-running game workload needed.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(label, c) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s (line %d)\n", label, __LINE__); } } while (0)
static unsigned entry(id<MTLBuffer> map, const void *p) {
    const uint64_t *v = map.contents;
    for (unsigned i = 1; i < gmem_entries(map); i++) if (v[(size_t)i * DMSH_GMEM_MAP_ENTRY / 8] == (uint64_t)(uintptr_t)p) return i;
    return 0;
}
static void finish(void) { CHECK("submission completes", mtl_wait(mtl_sub_commit(), 5000000000ull)); CHECK("no GPU error", !mtl_last_commit_error()); }
int main(void) { @autoreleasepool {
    mtl_init(); g_gmem_all = 1;
    enum { PG = 16384, MANY = 4200 };
    uint8_t *mem = mmap(NULL, (size_t)(MANY + 8) * 2 * PG, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (mem == MAP_FAILED) abort();
    mtl_pool_add(mem, PG); mtl_sub_begin();
    id<MTLBuffer> first = gmem_map(); uint64_t word = first.gpuAddress;
    gmem_slot_note(&word, NULL, nil); gmem_commit_patch();
    CHECK("unchanged table leaves its word alone", word == first.gpuAddress && g_gmem_buf == first);
    gmem_slot_note(&word, NULL, nil); mtl_pool_add(mem + 2 * PG, PG);
    finish();
    CHECK("normal commit repairs a late pool", word == g_gmem_buf.gpuAddress && entry(g_gmem_buf, mem + 2 * PG));
    CHECK("old table is immutable", g_gmem_buf != first && !entry(first, mem + 2 * PG));
    CHECK("committed words are no longer pending", !g_gmem_nslots);

    mtl_sub_begin(); id<MTLBuffer> small = gmem_map();
    NvGmlReceipt *receipt = nil; uint64_t logword = gml_take_record(small, &receipt); word = small.gpuAddress;
    id<MTLBuffer> small_log = receipt->buffer;
    CHECK("initial log matches small table", small_log.length == gml_bytes(small));
    gmem_slot_note(&word, &logword, receipt);
    for (int i = 2; i < MANY; i++) mtl_pool_add(mem + (size_t)i * 2 * PG, PG);
    gmem_commit_patch();
    CHECK("table grew beyond 4096 entries", gmem_entries(g_gmem_buf) > DMSH_GMEM_MAP_ENTRIES);
    CHECK("table and decoder use the repaired map", word == g_gmem_buf.gpuAddress && receipt->map == g_gmem_buf);
    CHECK("bitmap grows with the map", receipt->buffer.length >= gml_bytes(g_gmem_buf) && receipt->buffer != small_log);
    CHECK("bitmap argument points at replacement", logword == receipt->buffer.gpuAddress);
    CHECK("old table capacity did not change", gmem_entries(small) == DMSH_GMEM_MAP_ENTRIES);
    unsigned i = entry(g_gmem_buf, mem + (size_t)(MANY - 1) * 2 * PG);
    CHECK("last new entry is beyond old bitmap", i >= DMSH_GMEM_MAP_ENTRIES);
    if (i && (size_t)(i / 32 + 1) * 4 <= receipt->buffer.length) ((uint32_t *)receipt->buffer.contents)[i / 32] |= 1u << (i % 32);
    finish();
    int learned = 0; for (int k = 0; k < g_gml_n; k++) learned |= g_gml_lo[k] == (uint64_t)(uintptr_t)(mem + (size_t)(MANY - 1) * 2 * PG);
    CHECK("completion interprets bits against the repaired map", learned);

    mtl_sub_begin(); word = gmem_map().gpuAddress; gmem_slot_note(&word, NULL, nil);
    mtl_pool_add(mem + (size_t)MANY * 2 * PG, PG); g_cut_copy = 1; sub_cut();
    id<MTLBuffer> segment = g_gmem_buf; uint64_t segment_word = word;
    CHECK("segment commit repairs its word", word == segment.gpuAddress && entry(segment, mem + (size_t)MANY * 2 * PG));
    CHECK("segment consumes its word list", !g_gmem_nslots);
    uint64_t later = word; gmem_slot_note(&later, NULL, nil); mtl_pool_add(mem + (size_t)(MANY + 1) * 2 * PG, PG); mdi_wait();
    CHECK("count-wait commit repairs its word", later == g_gmem_buf.gpuAddress && entry(g_gmem_buf, mem + (size_t)(MANY + 1) * 2 * PG));
    CHECK("earlier segment word is not changed", word == segment_word);
    CHECK("earlier segment map is not changed", !entry(segment, mem + (size_t)(MANY + 1) * 2 * PG));
    finish();
    printf("t_gmem_commit: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
