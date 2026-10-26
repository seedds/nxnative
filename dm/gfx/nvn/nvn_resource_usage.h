// Submit-thread per-encoder resource declarations. Dedup is optional; preserving
// the union of access flags is mandatory, including collisions and overflow.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#ifndef NVN_RESOURCE_USAGE_REALLOC
#define NVN_RESOURCE_USAGE_REALLOC realloc
#define NVN_RESOURCE_USAGE_LOCAL_REALLOC 1
#endif
typedef struct { uintptr_t resource; uint64_t usage, epoch; } nvn_resource_usage_entry;
typedef struct {
    nvn_resource_usage_entry fast[512];
    nvn_resource_usage_entry *overflow;
    size_t overflow_n, overflow_cap;
    uint64_t epoch;
} nvn_resource_usage_set;
static inline void nvn_resource_usage_begin(nvn_resource_usage_set *s) {
    if (!++s->epoch) { memset(s->fast, 0, sizeof s->fast); s->epoch = 1; }
    s->overflow_n = 0;
}
static inline void nvn_resource_usage_dispose(nvn_resource_usage_set *s) {
    free(s->overflow); memset(s, 0, sizeof *s);
}
static inline int nvn_resource_usage_add(nvn_resource_usage_set *s, uintptr_t resource,
        uint64_t requested, int dedup, uint64_t *combined, int *skip) {
    nvn_resource_usage_entry *entry = NULL;
    size_t at = (resource >> 4) & 511;
    for (unsigned probe = 0; probe < 16; probe++, at = (at + 1) & 511) {
        nvn_resource_usage_entry *e = &s->fast[at];
        if (e->epoch != s->epoch) {
            *e = (nvn_resource_usage_entry){.resource = resource, .epoch = s->epoch};
            entry = e; break;
        }
        if (e->resource == resource) { entry = e; break; }
    }
    if (!entry) {
        // No eviction: a collision must never forget a previous Write. Slots
        // are not removed within an epoch, so an overflow key remains here.
        for (size_t i = 0; i < s->overflow_n; i++) if (s->overflow[i].resource == resource) {
            entry = &s->overflow[i]; break;
        }
        if (!entry) {
            if (s->overflow_n == s->overflow_cap) {
                size_t cap = s->overflow_cap ? s->overflow_cap * 2 : 32;
                if (cap < s->overflow_cap || cap > SIZE_MAX / sizeof *s->overflow) return 0;
                void *p = NVN_RESOURCE_USAGE_REALLOC(s->overflow, cap * sizeof *s->overflow);
                if (!p) return 0;
                s->overflow = p; s->overflow_cap = cap;
            }
            entry = &s->overflow[s->overflow_n++];
            *entry = (nvn_resource_usage_entry){.resource = resource, .epoch = s->epoch};
        }
    }
    uint64_t previous = entry->usage;
    entry->usage |= requested; *combined = entry->usage;
    *skip = dedup && previous == entry->usage && previous != 0;
    return 1;
}
#ifdef NVN_RESOURCE_USAGE_LOCAL_REALLOC
#undef NVN_RESOURCE_USAGE_LOCAL_REALLOC
#undef NVN_RESOURCE_USAGE_REALLOC
#endif
