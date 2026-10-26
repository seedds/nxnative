// Exact, bounded cache of CPU-decoded BC pixels. The caller still uploads every
// dirty candidate: only CPU decoding is reused, so image writes and lost-upload
// retries retain the ordinary upload behavior. Serialized by the upload owner.
#pragma once
#include "nvn_tex.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#ifndef NVN_CPU_CACHE_LOG
#define NVN_CPU_CACHE_LOG(...) do { fprintf(stderr, "[cpu-decode-cache] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#endif
#ifndef NVN_CPU_CACHE_BUDGET
#define NVN_CPU_CACHE_BUDGET (32u * 1024u * 1024u)
#endif
#ifndef NVN_CPU_CACHE_SOURCE_MAX
#define NVN_CPU_CACHE_SOURCE_MAX (8u * 1024u * 1024u)
#endif
#ifndef NVN_CPU_CACHE_DECODED_MAX
#define NVN_CPU_CACHE_DECODED_MAX (16u * 1024u * 1024u)
#endif
enum { CPU_CACHE_SLOTS = 32 };
typedef struct {
    uint64_t serial, offset, source_bytes, decoded_bytes;
    const void *mem, *pool;
    nvn_texdesc desc;
} nvn_cpu_decode_key;
typedef struct {
    nvn_cpu_decode_key key;
    uint8_t *source, *decoded;
    uint64_t used;
    int valid;
} nvn_cpu_decode_entry;
typedef struct {
    nvn_cpu_decode_entry *entry;
    const uint8_t *source;
    uint8_t *decoded;
    int hit;
} nvn_cpu_decode_ticket;
static struct {
    nvn_cpu_decode_entry entries[CPU_CACHE_SLOTS];
    uint64_t bytes, clock, hits, misses, evictions, uncached, compare_ns, copy_ns;
    uint64_t polled_frame, reported_frame;
    int initialized, on;
} g_cpu_decode_cache;

static uint64_t nvn_cpu_cache_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
static void nvn_cpu_decode_drop(nvn_cpu_decode_entry *e) {
    if (!e->source && !e->decoded) return;
    g_cpu_decode_cache.bytes -= e->key.source_bytes + e->key.decoded_bytes;
    free(e->source); free(e->decoded); memset(e, 0, sizeof *e);
}
static void nvn_cpu_decode_clear(void) {
    for (int i = 0; i < CPU_CACHE_SLOTS; i++) nvn_cpu_decode_drop(&g_cpu_decode_cache.entries[i]);
}
static int nvn_cpu_decode_on(uint64_t frame) {
    if (!g_cpu_decode_cache.initialized || g_cpu_decode_cache.polled_frame != frame) {
        g_cpu_decode_cache.initialized = 1; g_cpu_decode_cache.polled_frame = frame;
        const char *e = getenv("DM_NVN_CPU_DECODE_CACHE"); int on = e && !strcmp(e, "1");
        if (on != g_cpu_decode_cache.on) {
            nvn_cpu_decode_clear(); g_cpu_decode_cache.on = on;
            NVN_CPU_CACHE_LOG("[CPU-DECODE-CACHE] %s frame=%llu budget=%u source_cap=%u decoded_cap=%u; exact bytes, every upload retained",
                on ? "enabled" : "disabled", (unsigned long long)frame,
                NVN_CPU_CACHE_BUDGET, NVN_CPU_CACHE_SOURCE_MAX, NVN_CPU_CACHE_DECODED_MAX);
        }
    }
    return g_cpu_decode_cache.on;
}
static int nvn_cpu_decode_same_key(const nvn_cpu_decode_key *a, const nvn_cpu_decode_key *b) {
    return a->serial == b->serial && a->offset == b->offset && a->mem == b->mem && a->pool == b->pool
        && a->source_bytes == b->source_bytes && a->decoded_bytes == b->decoded_bytes
        && !memcmp(&a->desc, &b->desc, sizeof a->desc);
}
static nvn_cpu_decode_entry *nvn_cpu_decode_oldest(nvn_cpu_decode_entry *except) {
    nvn_cpu_decode_entry *old = NULL;
    for (int i = 0; i < CPU_CACHE_SLOTS; i++) {
        nvn_cpu_decode_entry *e = &g_cpu_decode_cache.entries[i];
        if (e != except && e->source && (!old || e->used < old->used)) old = e;
    }
    return old;
}
// Called only on a CPU-BC reupload, after its normal protect + generation stamp.
// The owned source is used for a miss's decode. A subsequent guest write cannot
// make the cached baseline disagree with the bytes actually decoded.
static nvn_cpu_decode_ticket nvn_cpu_decode_begin(uint64_t frame, const nvn_cpu_decode_key *key) {
    nvn_cpu_decode_ticket out = {0};
    if (!nvn_cpu_decode_on(frame)) return out;
    if (!key->serial || !key->mem || !key->source_bytes || !key->decoded_bytes
        || key->source_bytes > NVN_CPU_CACHE_SOURCE_MAX || key->decoded_bytes > NVN_CPU_CACHE_DECODED_MAX
        || key->source_bytes + key->decoded_bytes > NVN_CPU_CACHE_BUDGET) {
        g_cpu_decode_cache.uncached++; return out;
    }
    nvn_cpu_decode_entry *e = NULL;
    for (int i = 0; i < CPU_CACHE_SLOTS; i++)
        if (g_cpu_decode_cache.entries[i].source && g_cpu_decode_cache.entries[i].key.serial == key->serial) { e = &g_cpu_decode_cache.entries[i]; break; }
    if (e && e->valid && nvn_cpu_decode_same_key(&e->key, key)) {
        uint64_t start = nvn_cpu_cache_ns();
        int same = !memcmp(e->source, key->mem, (size_t)key->source_bytes);
        g_cpu_decode_cache.compare_ns += nvn_cpu_cache_ns() - start;
        if (same) {
            e->used = ++g_cpu_decode_cache.clock; g_cpu_decode_cache.hits++;
            return (nvn_cpu_decode_ticket){e, e->source, e->decoded, 1};
        }
    }
    g_cpu_decode_cache.misses++;
    if (!e) for (int i = 0; i < CPU_CACHE_SLOTS; i++)
        if (!g_cpu_decode_cache.entries[i].source) { e = &g_cpu_decode_cache.entries[i]; break; }
    if (!e) { e = nvn_cpu_decode_oldest(NULL); g_cpu_decode_cache.evictions++; }
    if (!e) return out;
    nvn_cpu_decode_drop(e);
    uint64_t need = key->source_bytes + key->decoded_bytes;
    while (g_cpu_decode_cache.bytes + need > NVN_CPU_CACHE_BUDGET) {
        nvn_cpu_decode_entry *old = nvn_cpu_decode_oldest(e);
        if (!old) return out;
        nvn_cpu_decode_drop(old); g_cpu_decode_cache.evictions++;
    }
    uint8_t *source = malloc((size_t)key->source_bytes);
    uint8_t *decoded = calloc(1, (size_t)key->decoded_bytes);
    if (!source || !decoded) { free(source); free(decoded); g_cpu_decode_cache.uncached++; return out; }
    uint64_t start = nvn_cpu_cache_ns(); memcpy(source, key->mem, (size_t)key->source_bytes);
    g_cpu_decode_cache.copy_ns += nvn_cpu_cache_ns() - start;
    e->key = *key; e->source = source; e->decoded = decoded; e->used = ++g_cpu_decode_cache.clock;
    g_cpu_decode_cache.bytes += need;
    return (nvn_cpu_decode_ticket){e, source, decoded, 0};
}
static void nvn_cpu_decode_finish(nvn_cpu_decode_ticket *t, int decoded_ok) {
    if (!t->entry || t->hit) return;
    if (decoded_ok) t->entry->valid = 1; else nvn_cpu_decode_drop(t->entry);
}
static void nvn_cpu_decode_report(uint64_t frame) {
    if (!g_cpu_decode_cache.on || frame / 300 == g_cpu_decode_cache.reported_frame) return;
    g_cpu_decode_cache.reported_frame = frame / 300;
    NVN_CPU_CACHE_LOG("[CPU-DECODE-CACHE] frame=%llu hits=%llu misses=%llu evictions=%llu uncached=%llu retained_bytes=%llu compare_ms=%.3f source_copy_ms=%.3f",
        (unsigned long long)frame, (unsigned long long)g_cpu_decode_cache.hits,
        (unsigned long long)g_cpu_decode_cache.misses, (unsigned long long)g_cpu_decode_cache.evictions,
        (unsigned long long)g_cpu_decode_cache.uncached, (unsigned long long)g_cpu_decode_cache.bytes,
        g_cpu_decode_cache.compare_ns / 1e6, g_cpu_decode_cache.copy_ns / 1e6);
}
