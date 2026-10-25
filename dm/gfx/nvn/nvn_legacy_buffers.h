#pragma once
// Buffer-only weak address index. Membership changes under this lock; the
// owning resource table/deferred-release queue controls buffer lifetime.
// Dynamic capacity avoids silently dropping buffers in larger game profiles.
typedef struct {
    uint64_t gpu, length;
    __unsafe_unretained id<MTLBuffer> buffer;
} legacy_buffer_entry;
static legacy_buffer_entry *g_legacy_buffers;
static size_t g_legacy_count, g_legacy_capacity;
static os_unfair_lock g_legacy_lock = OS_UNFAIR_LOCK_INIT;
static void legacy_buffer_add(id<MTLBuffer> b) {
    if (!b) return;
    os_unfair_lock_lock(&g_legacy_lock);
    size_t at = 0;
    while (at < g_legacy_count && g_legacy_buffers[at].gpu < b.gpuAddress) at++;
    if (at < g_legacy_count && g_legacy_buffers[at].buffer == b) {
        os_unfair_lock_unlock(&g_legacy_lock); return;
    }
    if (g_legacy_count == g_legacy_capacity) {
        size_t cap = g_legacy_capacity ? g_legacy_capacity * 2 : 256;
        legacy_buffer_entry *next = realloc(g_legacy_buffers, cap * sizeof *next);
        if (!next) { os_unfair_lock_unlock(&g_legacy_lock); MDIE("【红】Buffer index allocation failed"); }
        g_legacy_buffers = next; g_legacy_capacity = cap;
    }
    memmove(g_legacy_buffers + at + 1, g_legacy_buffers + at,
            (g_legacy_count - at) * sizeof *g_legacy_buffers);
    g_legacy_buffers[at] = (legacy_buffer_entry){b.gpuAddress, b.length, b};
    g_legacy_count++;
    os_unfair_lock_unlock(&g_legacy_lock);
}
static void legacy_buffer_remove(id<MTLBuffer> b) {
    os_unfair_lock_lock(&g_legacy_lock);
    for (size_t i = 0; i < g_legacy_count; i++) if (g_legacy_buffers[i].buffer == b) {
        memmove(g_legacy_buffers + i, g_legacy_buffers + i + 1,
                (g_legacy_count - i - 1) * sizeof *g_legacy_buffers);
        g_legacy_count--; break;
    }
    os_unfair_lock_unlock(&g_legacy_lock);
}
static id<MTLBuffer> legacy_buffer_at(uint64_t gpu, NSUInteger *offset) {
    *offset = 0;
    if (!gpu) return nil;
    os_unfair_lock_lock(&g_legacy_lock);
    size_t lo = 0, hi = g_legacy_count;
    while (lo < hi) { size_t m = lo + (hi - lo) / 2;
        if (g_legacy_buffers[m].gpu <= gpu) lo = m + 1; else hi = m;
    }
    id<MTLBuffer> b = nil;
    if (lo && gpu - g_legacy_buffers[lo - 1].gpu < g_legacy_buffers[lo - 1].length) {
        b = g_legacy_buffers[lo - 1].buffer;
        *offset = (NSUInteger)(gpu - g_legacy_buffers[lo - 1].gpu);
    }
    os_unfair_lock_unlock(&g_legacy_lock);
    return b;
}
