// Drawable callbacks and queue-drop paths publish on different threads. Keep
// each sequence number and its timing fields as one coherent snapshot. These
// helpers never acquire renderer/queue locks while holding their private mutex.
#pragma once
#include <pthread.h>
#include <stdint.h>

#define NVN_PRESENT_HISTORY_CAP 1024
typedef struct { uint64_t n; double t; } nvn_present_time;
typedef struct { uint64_t n; double target, lead_ms, gap_ms; int ticks; } nvn_link_time;
typedef struct {
    pthread_mutex_t mutex;
    nvn_present_time presented[NVN_PRESENT_HISTORY_CAP];
    nvn_link_time link[NVN_PRESENT_HISTORY_CAP];
} nvn_present_history;
#define NVN_PRESENT_HISTORY_INIT {.mutex = PTHREAD_MUTEX_INITIALIZER}

static inline void nvn_present_history_put(nvn_present_history *h, uint64_t n, double t) {
    pthread_mutex_lock(&h->mutex);
    nvn_present_time *e = &h->presented[n % NVN_PRESENT_HISTORY_CAP];
    // A delayed callback for an old wrap must not evict a newer generation.
    if (n >= e->n) *e = (nvn_present_time){n, t};
    pthread_mutex_unlock(&h->mutex);
}

static inline double nvn_present_history_get(nvn_present_history *h, uint64_t n) {
    pthread_mutex_lock(&h->mutex);
    nvn_present_time e = h->presented[n % NVN_PRESENT_HISTORY_CAP];
    pthread_mutex_unlock(&h->mutex);
    return e.n == n ? e.t : 0;
}

static inline void nvn_link_history_put(nvn_present_history *h, nvn_link_time value) {
    pthread_mutex_lock(&h->mutex);
    nvn_link_time *e = &h->link[value.n % NVN_PRESENT_HISTORY_CAP];
    if (value.n >= e->n) *e = value;
    pthread_mutex_unlock(&h->mutex);
}

static inline int nvn_link_history_get(nvn_present_history *h, uint64_t n, nvn_link_time *out) {
    pthread_mutex_lock(&h->mutex);
    nvn_link_time e = h->link[n % NVN_PRESENT_HISTORY_CAP];
    pthread_mutex_unlock(&h->mutex);
    if (e.n != n || !e.n) return 0;
    *out = e;
    return 1;
}
