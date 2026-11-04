// Bounded, append-only diagnostic events. Closing defines a cutoff, not GPU completion.
#pragma once
#include <pthread.h>
#include <stdint.h>
#include <stddef.h>

enum {
    FT_SYNC_BEGIN = 1, FT_SYNC_END, FT_WAIT_BEGIN, FT_EVENT_RETURN,
    FT_POST_DONE, FT_FALLBACK, FT_WAIT_END, FT_SUBMIT_RECORD,
    FT_COMMIT_BEGIN, FT_COMMIT_END, FT_FEEDBACK, FT_CLOCK_PAIR
};
typedef struct {
    uint64_t cpu_ns, frame, thread, sync, fence, latest, done, value;
    double gpu_begin_s, gpu_end_s;
    uint32_t kind, site;
} ft_event;
typedef struct {
    pthread_mutex_t lock;
    ft_event *events;
    size_t capacity, used;
    uint64_t dropped;
    int closed;
} ft_buffer;
typedef struct { size_t used; uint64_t dropped; } ft_cutoff;

static inline void ft_init(ft_buffer *b, ft_event *events, size_t capacity) {
    pthread_mutex_init(&b->lock, NULL);
    b->events = events; b->capacity = capacity; b->used = 0;
    b->dropped = 0; b->closed = 0;
}
// No ring reuse: a delayed callback cannot overwrite another fence's evidence.
// Events after the explicit cutoff are excluded, rather than silently joined later.
static inline int ft_append(ft_buffer *b, const ft_event *event) {
    int recorded = 0;
    pthread_mutex_lock(&b->lock);
    if (!b->closed) {
        if (b->used < b->capacity) { b->events[b->used++] = *event; recorded = 1; }
        else b->dropped++;
    }
    pthread_mutex_unlock(&b->lock);
    return recorded;
}
// After close returns the event array is immutable and can be exported off-thread.
// Missing begin/end/feedback joins remain missing at this cutoff; no completion is inferred.
static inline ft_cutoff ft_close(ft_buffer *b) {
    pthread_mutex_lock(&b->lock);
    b->closed = 1;
    ft_cutoff result = {b->used, b->dropped};
    pthread_mutex_unlock(&b->lock);
    return result;
}
