// Bounded, opt-in inspection of packaged uploads. This never suppresses an
// upload or changes a dirty stamp. Repeated snapshots compare exact guest
// bytes; SHA256 is attribution only, not the equality decision.
#pragma once
#include "nvn_dirty.h"
#include <CommonCrypto/CommonDigest.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#ifndef NVN_UPDIAG_LOG
#define NVN_UPDIAG_LOG(...) do { fprintf(stderr, "[upload-diag] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#endif
enum { UPDIAG_SLOTS = 32, UPDIAG_RECORDS = 128, UPDIAG_MAX_FRAMES = 16 };
#define UPDIAG_MAX_TEXTURE (8u * 1024u * 1024u)
#define UPDIAG_MAX_SNAPSHOTS (32u * 1024u * 1024u)
typedef struct {
    uint64_t frame, serial, offset, bytes, pool_bytes;
    const void *mem, *pool;
    unsigned width, height, depth, format, target, levels, flags;
    int reupload;
} nvn_updiag_meta;
typedef struct {
    nvn_updiag_meta meta;
    int enabled, tracked, stable, baseline, equal, different_pages;
    uint64_t capture_ns, hash_ns, first_different_page;
    const char *path, *uncaptured;
    char digest[CC_SHA256_DIGEST_LENGTH * 2 + 1];
} nvn_updiag_record;
typedef struct {
    uint64_t serial, bytes;
    const void *mem, *pool;
    uint8_t *snapshot;
    int stable;
} nvn_updiag_slot;
static struct {
    char request[64];
    uint64_t polled_frame, first_frame, bytes, total_capture_ns;
    unsigned frames, records, comparisons, unchanged, changed, racy;
    int initialized, active;
    nvn_updiag_slot slots[UPDIAG_SLOTS];
} g_updiag;

static uint64_t nvn_updiag_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
static void nvn_updiag_finish(const char *why) {
    if (g_updiag.active)
        NVN_UPDIAG_LOG("[UPLOAD-DIAG] complete reason=%s records=%u comparisons=%u unchanged=%u changed=%u racy=%u snapshot_bytes=%llu capture_ms=%.3f",
            why, g_updiag.records, g_updiag.comparisons, g_updiag.unchanged,
            g_updiag.changed, g_updiag.racy, (unsigned long long)g_updiag.bytes,
            g_updiag.total_capture_ns / 1e6);
    for (int i = 0; i < UPDIAG_SLOTS; i++) {
        free(g_updiag.slots[i].snapshot);
        memset(&g_updiag.slots[i], 0, sizeof g_updiag.slots[i]);
    }
    g_updiag.bytes = 0; g_updiag.active = 0;
}
static int nvn_updiag_want(uint64_t frame) {
    if (!g_updiag.initialized || frame != g_updiag.polled_frame) {
        g_updiag.initialized = 1; g_updiag.polled_frame = frame;
        const char *e = getenv("DM_NVN_UPLOAD_DIAG_FRAMES");
        char request[sizeof g_updiag.request];
        snprintf(request, sizeof request, "%s", e ? e : "");
        if (strcmp(request, g_updiag.request)) {
            nvn_updiag_finish("request-changed");
            snprintf(g_updiag.request, sizeof g_updiag.request, "%s", request);
            long n = strtol(request, NULL, 10);
            g_updiag.frames = n > UPDIAG_MAX_FRAMES ? UPDIAG_MAX_FRAMES : n > 0 ? (unsigned)n : 0;
            g_updiag.records = g_updiag.comparisons = g_updiag.unchanged = g_updiag.changed = g_updiag.racy = 0;
            g_updiag.total_capture_ns = 0;
            if (g_updiag.frames) {
                g_updiag.first_frame = frame; g_updiag.active = 1;
                NVN_UPDIAG_LOG("[UPLOAD-DIAG] begin frame=%llu frames=%u record_cap=%d snapshot_cap=%u; timing includes diagnostic overhead",
                    (unsigned long long)frame, g_updiag.frames, UPDIAG_RECORDS, UPDIAG_MAX_SNAPSHOTS);
            }
        }
        if (g_updiag.active && frame - g_updiag.first_frame >= g_updiag.frames)
            nvn_updiag_finish("frame-limit");
    }
    if (g_updiag.active && g_updiag.records >= UPDIAG_RECORDS) nvn_updiag_finish("record-limit");
    return g_updiag.active;
}
static void nvn_updiag_begin(nvn_updiag_record *r, const nvn_updiag_meta *meta) {
    memset(r, 0, sizeof *r);
    if (!nvn_updiag_want(meta->frame)) return;
    r->enabled = 1; r->meta = *meta; r->path = "unresolved";
    r->uncaptured = "upload-not-captured"; r->first_different_page = UINT64_MAX;
    g_updiag.records++;
}
// Called immediately after the normal upload has protected its source. The
// generation check rejects a snapshot raced by a write into the covered pages.
static void nvn_updiag_capture(nvn_updiag_record *r) {
    if (!r->enabled) return;
    uint64_t started = nvn_updiag_ns();
    const nvn_updiag_meta *m = &r->meta;
    r->uncaptured = !m->mem || !m->bytes ? "empty" : m->bytes > UPDIAG_MAX_TEXTURE ? "texture-size-cap" : NULL;
    if (r->uncaptured) goto done;
    int slot = -1;
    for (int i = 0; i < UPDIAG_SLOTS; i++) if (g_updiag.slots[i].serial == m->serial) { slot = i; break; }
    if (slot < 0) for (int i = 0; i < UPDIAG_SLOTS; i++) if (!g_updiag.slots[i].snapshot) { slot = i; break; }
    if (slot < 0) { r->uncaptured = "serial-slot-cap"; goto done; }
    nvn_updiag_slot *s = &g_updiag.slots[slot];
    if (g_updiag.bytes - s->bytes + m->bytes > UPDIAG_MAX_SNAPSHOTS) { r->uncaptured = "snapshot-byte-cap"; goto done; }
    uint8_t *current = malloc((size_t)m->bytes);
    if (!current) { r->uncaptured = "allocation-failed"; goto done; }
    const void *span = nvn_dirty_span(m->mem, (size_t)m->bytes);
    r->tracked = span != NULL;
    uint64_t stamp = nvn_dirty_now();
    memcpy(current, m->mem, (size_t)m->bytes);
    r->stable = span && !nvn_dirty_since_span(span, m->mem, (size_t)m->bytes, stamp);
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    uint64_t hash_started = nvn_updiag_ns();
    CC_SHA256(current, (CC_LONG)m->bytes, digest);
    r->hash_ns = nvn_updiag_ns() - hash_started;
    for (unsigned i = 0; i < sizeof digest; i++) snprintf(r->digest + i * 2, 3, "%02x", digest[i]);
    r->baseline = !(s->snapshot && s->serial == m->serial && s->mem == m->mem && s->pool == m->pool && s->bytes == m->bytes);
    if (!r->baseline && r->stable && s->stable) {
        size_t pg = (size_t)getpagesize();
        r->equal = 1; g_updiag.comparisons++;
        for (size_t offset = 0; offset < m->bytes;) {
            size_t n = pg - (((uintptr_t)m->mem + offset) & (pg - 1));
            if (n > m->bytes - offset) n = (size_t)m->bytes - offset;
            if (memcmp(s->snapshot + offset, current + offset, n)) {
                r->equal = 0; r->different_pages++;
                if (r->first_different_page == UINT64_MAX) r->first_different_page = offset;
            }
            offset += n;
        }
        if (r->equal) g_updiag.unchanged++; else g_updiag.changed++;
    } else if (!r->stable || (!r->baseline && !s->stable)) g_updiag.racy++;
    g_updiag.bytes = g_updiag.bytes - s->bytes + m->bytes;
    free(s->snapshot);
    *s = (nvn_updiag_slot){m->serial, m->bytes, m->mem, m->pool, current, r->stable};
done:
    r->capture_ns = nvn_updiag_ns() - started;
    g_updiag.total_capture_ns += r->capture_ns;
}
static void nvn_updiag_end(nvn_updiag_record *r, uint64_t elapsed_ns, int success) {
    if (!r->enabled) return;
    const nvn_updiag_meta *m = &r->meta;
    const char *exact = r->uncaptured ? r->uncaptured : r->baseline ? "baseline" : !r->stable ? "racy" : r->equal ? "unchanged" : r->different_pages ? "changed" : "prior-racy";
    size_t pg = (size_t)getpagesize(); uintptr_t start = (uintptr_t)m->mem & ~(pg - 1);
    uintptr_t end = ((uintptr_t)m->mem + (uintptr_t)m->bytes + pg - 1) & ~(pg - 1);
    NVN_UPDIAG_LOG("[UPLOAD-DIAG] frame=%llu serial=%llu reupload=%d path=%s success=%d mem=%p pool=%p pool_bytes=%llu offset=%#llx storage=%llu pages=%p..%p size=%ux%ux%u format=%#x target=%u levels=%u flags=%#x tracked=%d stable=%d exact=%s different_pages=%d first_different_page_offset=%#llx sha256=%s encoding_ms=%.3f capture_ms=%.3f hash_ms=%.3f",
        (unsigned long long)m->frame, (unsigned long long)m->serial, m->reupload,
        r->path, success, m->mem, m->pool, (unsigned long long)m->pool_bytes,
        (unsigned long long)m->offset, (unsigned long long)m->bytes, (void *)start, (void *)end,
        m->width, m->height, m->depth, m->format, m->target, m->levels, m->flags,
        r->tracked, r->stable, exact, r->different_pages, (unsigned long long)r->first_different_page,
        *r->digest ? r->digest : "-", (elapsed_ns > r->capture_ns ? elapsed_ns - r->capture_ns : 0) / 1e6,
        r->capture_ns / 1e6, r->hash_ns / 1e6);
}
